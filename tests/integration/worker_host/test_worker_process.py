#!/usr/bin/env python3
"""Real private worker process + loopback model endpoint, standard library only."""
from __future__ import annotations

import argparse
from collections import Counter
import http.server
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time
import traceback

KEY = "worker-fixture-model-key-never-log"
LONG_TEXT = "完整回答🙂" * 5000
TOOL_MARKER = "PRIVATE_TOOL_RESULT_MUST_NOT_ENTER_IPC"
SCENARIOS = (
    "health", "same_cwd_sessions", "operation_idempotency", "approval_routing",
    "cancel_routing", "detach_reattach", "stale_attachment", "eof_cleanup",
    "killed_worker_resume", "wire_payload_boundary", "assistant_full_text",
    "persisted_result_preview", "full_result_policy", "result_restart_frozen",
    "result_record_integrity", "result_query_routing", "result_full_frame_boundary",
    "result_multichannel_preview",
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def until(predicate, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(0.02)
    raise AssertionError("condition did not become true before deadline")


class Model:
    def __init__(self):
        self.lock = threading.Lock()
        self.calls = []
        self.releases = {}
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                require(self.path == "/v1/chat/completions", "wrong model endpoint path")
                require(self.headers.get("Authorization") == "Bearer " + KEY, "model key was not resolved from environment")
                model = body["model"]
                with owner.lock:
                    owner.calls.append(body)
                    release = owner.releases.setdefault(model, threading.Event())
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Connection", "close")
                self.end_headers()
                try:
                    if model.startswith("slow-") or (model == "read-result-live" and body["messages"][-1].get("content") == "hold-live"):
                        deadline = time.monotonic() + 30
                        while not release.wait(0.05) and time.monotonic() < deadline:
                            self.wfile.write(b": waiting\n\n")
                            self.wfile.flush()
                    messages = body["messages"]
                    tool_reply = messages[-1].get("role") == "tool"
                    if model.startswith("write-") and not tool_reply:
                        suffix = model[len("write-"):]
                        delta = {"tool_calls": [{"index": 0, "id": "write-call", "type": "function",
                            "function": {"name": "write_file", "arguments": json.dumps({
                                "path": suffix + ".txt", "content": suffix})}}]}
                        finish = "tool_calls"
                    elif (model == "read-boundary" or model.startswith("read-result-")) and not tool_reply:
                        delta = {"tool_calls": [{"index": 0, "id": "read-call", "type": "function",
                            "function": {"name": "read_file", "arguments": json.dumps({
                                "path": "private.txt" if model == "read-boundary" else "result.txt"})}}]}
                        finish = "tool_calls"
                    elif model == "command-result" and not tool_reply:
                        command = "echo CHANNEL_STDOUT & echo CHANNEL_STDERR 1>&2" if os.name == "nt" else "printf CHANNEL_STDOUT; printf CHANNEL_STDERR >&2"
                        delta = {"tool_calls": [{"index": 0, "id": "command-call", "type": "function",
                            "function": {"name": "run_command", "arguments": json.dumps({
                                "command": command, "shell": "cmd" if os.name == "nt" else "sh", "timeout_ms": 20000})}}]}
                        finish = "tool_calls"
                    else:
                        user = next((m.get("content", "") for m in reversed(messages) if m.get("role") == "user"), "")
                        answer = LONG_TEXT if model == "long" else "safe-answer" if model == "read-boundary" else f"{model}:{user}"
                        delta, finish = {"content": answer}, "stop"
                    frames = [
                        {"id": "fixture", "choices": [{"index": 0, "delta": delta, "finish_reason": None}]},
                        {"id": "fixture", "choices": [{"index": 0, "delta": {}, "finish_reason": finish}]},
                    ]
                    for frame in frames:
                        self.wfile.write(("data: " + json.dumps(frame, ensure_ascii=False) + "\n\n").encode())
                    self.wfile.write(b"data: [DONE]\n\n")
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                    pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server.server_port}/v1"

    def count(self, model):
        with self.lock:
            return sum(c["model"] == model for c in self.calls)

    def release(self, model):
        with self.lock:
            self.releases.setdefault(model, threading.Event()).set()

    def close(self):
        with self.lock:
            for event in self.releases.values():
                event.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)


class Worker:
    def __init__(self, exe, resource, root, model, policy=None, key=KEY):
        self.root, self.model = root, model
        (root / "project").mkdir(parents=True, exist_ok=True)
        self.attachment = None
        self.serial = 0
        self.frames, self.stderr = [], []
        self.queue = queue.Queue()
        env = dict(os.environ, LUBAN_WORKER_TEST_KEY=key)
        self.process = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, cwd=root, env=env, text=True, encoding="utf-8", bufsize=1)

        def read():
            try:
                for line in self.process.stdout:
                    self.frames.append(line)
                    self.queue.put(json.loads(line))
            except Exception as error:
                self.queue.put(error)
            finally:
                self.queue.put(EOFError("worker IPC closed"))

        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        self.error_reader = threading.Thread(target=lambda: self.stderr.extend(self.process.stderr.readlines()), daemon=True)
        self.error_reader.start()
        try:
            params = dict(data_root=str(root / "data"), resource_root=str(resource))
            if policy is not None:
                params["result_policy"] = policy
            self.call("worker.initialize", **params)
            self.attach()
        except Exception:
            self.finish()
            raise

    def raw(self, method, params=None, attachment=None):
        self.serial += 1
        request = {"id": str(self.serial), "method": method, "params": params or {}}
        token = self.attachment if attachment is None else attachment
        if token:
            request["attachment"] = token
        self.process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        response = self.queue.get(timeout=25)
        if isinstance(response, Exception):
            raise response
        require(response["id"] == str(self.serial), "wrong correlation ID")
        return response

    def call(self, method, **params):
        response = self.raw(method, params)
        require("result" in response, f"{method} rejected: {response.get('error', {}).get('code')}")
        return response["result"]

    def attach(self):
        self.attachment = self.call("client.attach", client_id="test-controller")["attachment"]

    def open_params(self, model="echo", tools=(), client="session", resume="", **extra):
        params = dict(client_session_id=client, cwd=str(self.root / "project"), model=model,
            builtin_tools=list(tools), connection=dict(wire="chat_completions", base_url=self.model.url,
                api_key_env="LUBAN_WORKER_TEST_KEY", request_timeout_seconds=30, idle_timeout_seconds=10))
        if resume:
            params["resume_session_id"] = resume
        params.update(extra)
        return params

    def open(self, model="echo", tools=(), client="session", resume="", **extra):
        return self.call("session.open", **self.open_params(model, tools, client, resume, **extra))["session_id"]

    def submit(self, session, key="turn", text="hello"):
        return self.call("operation.submit", session_id=session, client_operation_id=key, text=text)

    def result(self, session, operation):
        def terminal():
            value = self.call("operation.get", session_id=session, operation_id=operation)
            return value if value["state"] not in ("accepted", "running") else None
        return until(terminal)

    def pending(self, session):
        return until(lambda: self.call("approval.list", session_id=session)["approvals"])[0]

    def query(self, session, operation, identity=None, key=None):
        params = dict(session_id=session, operation_id=operation,
            client_query_id=key or "query-" + str(self.serial + 1))
        if identity is not None:
            params["identity"] = identity
        receipt = self.call("result.query.start", **params)
        def complete():
            value = self.call("result.query.get", session_id=session, query_id=receipt["query_id"])
            return value if value["state"] != "pending" else None
        return until(complete)

    def tool_results(self, session, operation):
        queried = self.query(session, operation)
        require(queried["state"] == "ready", "persisted tool index was not ready")
        entries = queried["value"]["results"]
        require(queried["value"]["formal_selected_only"], "capture versions entered supervisor index")
        require(entries and all(item["identity"]["result_id"].startswith("res-") for item in entries), "formal selected result missing")
        require(all(item["identity"]["session_id"] == session and item["identity"]["operation_id"] == operation for item in entries), "index mixed session/operation")
        return entries

    def projection(self, session, operation, identity):
        value = self.query(session, operation, identity)
        require(value["state"] == "ready", "saved result projection failed")
        return value["value"]

    def finish(self):
        if self.process.poll() is None:
            try:
                self.call("worker.shutdown")
                self.process.wait(timeout=15)
            except Exception:
                self.process.kill()
                self.process.wait(timeout=5)
        self.reader.join(5)
        self.error_reader.join(5)
        for pipe in (self.process.stdin, self.process.stdout, self.process.stderr):
            pipe.close()


def scenarios(exe, resource, scratch, model):
    def run(name, function):
        root = scratch / name
        root.mkdir()
        worker = Worker(exe, resource, root, model)
        try:
            function(worker)
        finally:
            worker.finish()
        require(KEY not in "".join(worker.frames + worker.stderr), "resolved model credential entered IPC or logs")

    def health(w):
        info = w.call("worker.status")
        require(info["protocol_version"] == 1 and info["initialized"], "worker not healthy")
        require(info["capabilities"]["tool_results"] == "persisted-selected-pull" and
            info["capabilities"]["tool_result_default"] == "preview", "persisted pull capability is incorrect")
        require(w.raw("job.start")["error"]["code"] == "worker.unsupported_method", "unsupported job silently accepted")
        bad = w.raw("session.open", {"client_session_id": "bad", "api_key": KEY})
        require(bad["error"]["code"] == "worker.unsupported_field", "raw secret field accepted")

    def same_cwd(w):
        a, b = w.open("echo-alpha", client="a"), w.open("echo-beta", client="b")
        require(a != b, "sessions sharing cwd received same identity")
        ar, br = w.submit(a, text="alpha-context"), w.submit(b, text="beta-context")
        require(w.result(a, ar["operation_id"])["state"] == "succeeded", "alpha failed")
        require(w.result(b, br["operation_id"])["state"] == "succeeded", "beta failed")
        with model.lock:
            bodies = {c["model"]: json.dumps(c["messages"]) for c in model.calls if c["model"] in ("echo-alpha", "echo-beta")}
        require("beta-context" not in bodies["echo-alpha"] and "alpha-context" not in bodies["echo-beta"], "model histories crossed sessions")

    def idempotency(w):
        s = w.open("echo-idempotent")
        before = model.count("echo-idempotent")
        first = w.submit(s)
        second = w.submit(s)
        require(second["duplicate"] and first["operation_id"] == second["operation_id"], "repeat submission not deduplicated")
        require(w.result(s, first["operation_id"])["state"] == "succeeded", "operation failed")
        require(model.count("echo-idempotent") == before + 1, "duplicate operation reached model")
        conflict = w.raw("operation.submit", dict(session_id=s, client_operation_id="turn", text="different"))
        require(conflict.get("error", {}).get("sdk_code") == "operation_conflict", "changed duplicate payload did not report operation_conflict")

    def approvals(w):
        a = w.open("write-alpha", ("write_file",), "a")
        b = w.open("write-beta", ("write_file",), "b")
        ar, br = w.submit(a), w.submit(b)
        ap, bp = w.pending(a), w.pending(b)
        require(ap["request_id"] != bp["request_id"], "approval identity crossed sessions")
        foreign = w.raw("approval.resolve", dict(session_id=a, request_id=bp["request_id"], decision="accept"))
        require("error" in foreign, "foreign approval was resolved")
        view = w.call("approval.get", session_id=a, request_id=ap["request_id"])["approvals"][0]
        require(json.loads(view["input_json"]["content"])["path"] == "alpha.txt", "approval arguments missing")
        w.call("approval.resolve", session_id=a, request_id=ap["request_id"], decision="accept")
        w.call("approval.resolve", session_id=b, request_id=bp["request_id"], decision="decline")
        w.result(a, ar["operation_id"])
        w.result(b, br["operation_id"])
        require((w.root / "project" / "alpha.txt").read_text() == "alpha", "approved tool did not execute")
        require(not (w.root / "project" / "beta.txt").exists(), "declined tool executed")

    def cancel(w):
        a, b = w.open("slow-cancel", client="a"), w.open("slow-survivor", client="b")
        ar, br = w.submit(a), w.submit(b)
        until(lambda: model.count("slow-cancel") and model.count("slow-survivor"))
        w.call("operation.cancel", session_id=a, operation_id=ar["operation_id"])
        require(w.result(a, ar["operation_id"])["state"] == "cancelled", "target operation was not cancelled")
        w.call("session.close", session_id=a)
        require(w.call("operation.get", session_id=b, operation_id=br["operation_id"])["state"] == "running", "cancel/close A stopped the still-running B")
        model.release("slow-survivor")
        require(w.result(b, br["operation_id"])["state"] == "succeeded", "cancel crossed session boundary")
        later = w.submit(b, key="survivor-again", text="still open")
        require(w.result(b, later["operation_id"])["state"] == "succeeded", "closing A also closed B admission")

    def detach(w):
        s = w.open("slow-detach")
        receipt = w.submit(s)
        until(lambda: model.count("slow-detach"))
        w.call("client.detach")
        require(w.process.poll() is None and not w.call("worker.status")["attached"], "detach destroyed worker")
        model.release("slow-detach")
        w.attach()
        result = w.result(s, receipt["operation_id"])
        require(result["state"] == "succeeded", "detached operation did not finish")
        require(w.submit(s)["duplicate"], "reattach repeated side effect")

    def stale(w):
        s = w.open()
        previous = w.attachment
        w.attach()
        denied = w.raw("operation.submit", dict(session_id=s, client_operation_id="stale", text="stale"), attachment=previous)
        require(denied["error"]["code"] == "worker.stale_attachment", "old attachment admitted work")
        require(w.call("session.get", session_id=s)["known_operation_ids"] == [], "stale attachment created an operation")

    def eof(w):
        s = w.open("slow-eof")
        receipt = w.submit(s)
        until(lambda: model.count("slow-eof"))
        w.process.stdin.close()
        require(w.process.wait(timeout=15) == 0, "parent EOF failed to drain worker")
        # Reopen the same source to prove Close released the writer and finalized
        # its cancelled turn, rather than merely exiting its input loop.
        next_worker = Worker(exe, resource, w.root, model)
        try:
            resumed = next_worker.open("echo-eof-recovered", resume=s)
            require(resumed == s, "EOF recovery changed session identity")
            saved = next_worker.call("operation.get", session_id=s, operation_id=receipt["operation_id"])
            require(saved["state"] == "cancelled" and saved["result_persisted"], "EOF did not persist cancellation before exit")
            require(model.count("slow-eof") == 1 and model.count("echo-eof-recovered") == 0, "EOF recovery replayed the operation")
        finally:
            next_worker.finish()

    def killed(w):
        s = w.open("slow-kill")
        old_attachment = w.attachment
        receipt = w.submit(s)
        until(lambda: model.count("slow-kill"))
        w.process.kill()
        w.process.wait(timeout=5)
        next_worker = Worker(exe, resource, w.root, model)
        try:
            resumed = next_worker.open("echo-after-kill", resume=s)
            require(resumed == s, "crash recovery forked session identity")
            stale = next_worker.raw("session.close", dict(session_id=s), attachment=old_attachment)
            require(stale["error"]["code"] == "worker.stale_attachment", "previous worker attachment closed recovered session")
            result = next_worker.call("operation.get", session_id=s, operation_id=receipt["operation_id"])
            require(result["state"] == "indeterminate", "dispatched operation was replayed or guessed complete")
            duplicate = next_worker.submit(s)
            require(duplicate["duplicate"] and duplicate["operation_id"] == receipt["operation_id"], "durable dedupe lost on worker restart")
            require(model.count("echo-after-kill") == 0, "uncertain operation was replayed")
            fresh = next_worker.submit(s, "new-turn", "continue")
            require(next_worker.result(s, fresh["operation_id"])["state"] == "succeeded", "recovered session cannot continue")
        finally:
            next_worker.finish()

    def boundary(w):
        (w.root / "project" / "private.txt").write_text(TOOL_MARKER + ":" + KEY + ":" + "x" * 4096, encoding="utf-8")
        s = w.open("read-boundary", ("read_file",))
        receipt = w.submit(s)
        result = w.result(s, receipt["operation_id"])
        require(result["state"] == "succeeded", "real read tool failed")
        for _ in range(10):
            events = w.call("events.poll", session_id=s, max_events=128)["events"]
            if not events:
                break
            require(all("text" not in event and "payload_json" not in event for event in events), "raw event forwarded")
        with model.lock:
            tool_requests = [c for c in model.calls if c["model"] == "read-boundary" and c["messages"][-1]["role"] == "tool"]
        require(tool_requests and TOOL_MARKER in json.dumps(tool_requests[-1]), "actual tool result never reached SDK model history")
        require(TOOL_MARKER not in "".join(w.frames + w.stderr), "raw tool result leaked into IPC or logs")

    def full_text(w):
        s = w.open("long")
        receipt = w.submit(s)
        result = w.result(s, receipt["operation_id"])
        require(result["state"] == "succeeded", "long answer failed")
        chunks, offset = [], 0
        while True:
            page = w.call("operation.get", session_id=s, operation_id=receipt["operation_id"], text_offset=offset, text_limit=32767)["assistant_text"]
            chunks.append(page["content"])
            require(page["offset"] == offset and page["next_offset"] > offset, "text page did not advance")
            offset = page["next_offset"]
            if page["complete"]:
                break
        require("".join(chunks) == LONG_TEXT, "assistant answer was preview-truncated or split inside UTF8")
        require(offset == len(LONG_TEXT.encode()), "assistant byte count differs")

    def read_operation(w, name, body, client="session", **options):
        (w.root / "project" / "result.txt").write_text(body, encoding="utf-8")
        session = w.open(name, ("read_file",), client, **options)
        receipt = w.submit(session)
        require(w.result(session, receipt["operation_id"])["state"] == "succeeded", "actual result read failed")
        entries = w.tool_results(session, receipt["operation_id"])
        # SDK can list both capture-* and res-* versions; Worker exposes only
        # selected formal versions. The fixture performs one tool call.
        require(len(entries) == 1, "one formal tool execution did not have one selected result")
        return session, receipt["operation_id"], entries[0]["identity"]

    def saved_preview(w):
        body = "PREVIEW_VISIBLE:" + KEY + ":" + "中文🙂" * 3000 + ":PREVIEW_PRIVATE_TAIL"
        session, operation, identity = read_operation(w, "read-result-preview", body)
        first = w.projection(session, operation, identity)
        require(first["mode"] == "preview" and first["status"] == "ready" and first["truncated"], "default did not produce a bounded preview")
        require("PREVIEW_VISIBLE" in first["text"] and "PREVIEW_PRIVATE_TAIL" not in first["text"], "preview did not freeze the initial slice")
        require(KEY not in first["text"] and first["redacted"], "persisted preview did not redact the real model key")
        require(len(first["text"].encode()) <= first["previewMaxBytes"] == 4096, "preview escaped the single budget")
        require(first["toolCallId"] == identity["tool_call_id"] and identity["tool_call_id"] != "read-call", "provider ID replaced durable action identity")
        expected = {wire: identity[field] for wire, field in (
            ("sessionId", "session_id"), ("operationId", "operation_id"), ("turnId", "turn_id"),
            ("toolCallId", "tool_call_id"), ("persistedEventId", "persisted_event_id"), ("resultId", "result_id"))}
        require(all(first[key] == value for key, value in expected.items()), "wire projection changed an identity")
        w.call("client.detach")
        w.attach()
        require(w.projection(session, operation, identity) == first, "reattach cut a different preview")
        w.call("session.close", session_id=session)
        require(w.projection(session, operation, identity) == first, "closed handle lost its saved snapshot")
        require("PREVIEW_PRIVATE_TAIL" not in "".join(w.frames), "another channel bypassed the preview budget")

    def full_policy(w):
        denied = w.raw("session.open", w.open_params("read-result-denied", ("read_file",), tool_result_sync="full"))
        require(denied["error"]["code"] == "worker.full_result_sync_disabled", "unpermitted Full was downgraded or accepted")
        require(w.call("worker.status")["session_count"] == 0, "denied Full published a session")
        full_node = Worker(exe, resource, w.root / "permitted", model,
            policy={"allow_full_tool_results": True, "version": "node-full-v1"})
        try:
            body = "FULL_START:" + KEY + ":" + "word" * 4000 + ":FULL_END"
            preview, po, pi = read_operation(full_node, "read-result-node-default", body, client="preview")
            p = full_node.projection(preview, po, pi)
            require(p["mode"] == "preview" and p["truncated"] and "FULL_END" not in p["text"], "Node permission enabled Full without session consent")
            full, fo, fi = read_operation(full_node, "read-result-full", body, client="full", tool_result_sync="full", result_policy_version=7)
            value = full_node.projection(full, fo, fi)
            require(value["mode"] == "full" and value["status"] == "ready" and not value["truncated"], "explicit permitted Full was truncated")
            require("FULL_START" in value["text"] and "FULL_END" in value["text"] and KEY not in value["text"], "Full lost the tail or credential redaction")
            require(value["sessionPolicyVersion"] == 7 and value["nodeAllowsFull"], "Full omitted its frozen double policy")
            full_node.finish()
            resumed = Worker(exe, resource, full_node.root, model,
                policy={"allow_full_tool_results": True, "version": "node-full-v1"})
            try:
                require(resumed.open("echo-full-resume", resume=full) == full, "Full resume changed session identity")
                require(resumed.call("session.get", session_id=full)["tool_result_sync"] == "full", "omitted resume mode silently downgraded Full")
                require(resumed.projection(full, fo, fi) == value, "Full changed across process restart")
            finally:
                resumed.finish()
                require(KEY not in "".join(resumed.frames + resumed.stderr), "resumed Full leaked a model key")
        finally:
            full_node.finish()
            require(KEY not in "".join(full_node.frames + full_node.stderr), "Full entered IPC with a model key")

    def restart_projection(w):
        session, operation, identity = read_operation(w, "read-result-restart", "RESTART_VISIBLE:" + "r" * 9000 + ":RESTART_TAIL")
        first = w.projection(session, operation, identity)
        w.process.kill()
        w.process.wait(timeout=5)
        restarted = Worker(exe, resource, w.root, model)
        try:
            for extra in ({"tool_result_sync": "full"}, {"result_policy_version": 2}):
                rejected = restarted.raw("session.open", restarted.open_params("echo-policy-conflict", resume=session, **extra))
                require(rejected["error"]["code"] == "worker.result_policy_conflict", "resume changed the saved policy tuple")
            require(restarted.open("echo-projection-resume", resume=session) == session, "saved projection resume failed")
            require(restarted.projection(session, operation, identity) == first, "restart projected a new prefix")
            require(model.count("echo-projection-resume") == 0, "result query reran an operation")
        finally:
            restarted.finish()
        for changed in ({"policy": {"preview_max_bytes": 128}}, {"key": "changed-model-key"}):
            rejected_worker = Worker(exe, resource, w.root, model, **changed)
            try:
                rejected = rejected_worker.raw("session.open", rejected_worker.open_params("echo-binding-conflict", resume=session))
                require(rejected["error"]["code"] == "worker.result_policy_conflict", "same-version effective Node/secret change was accepted")
                require(rejected_worker.call("worker.status")["session_count"] == 0, "binding conflict published a session")
            finally:
                rejected_worker.finish()
        require(KEY not in "".join(restarted.frames + restarted.stderr), "restarted result leaked a model key")

    def record_integrity(w):
        session, operation, identity = read_operation(w, "read-result-integrity", "INTEGRITY_VISIBLE:" + "v" * 9000)
        original = w.projection(session, operation, identity)
        # These are Worker supervisor files, not SDK ledger/artifact internals.
        supervisor = w.root / "data" / "worker-result-policies"
        record, = supervisor.rglob("*.projection.json")
        bytes_before = record.read_bytes()
        require(KEY.encode() not in bytes_before, "supervisor persisted an unresolved key")
        record.write_text("{broken", encoding="utf-8")
        corrupt = w.query(session, operation, identity)
        require(corrupt["state"] == "failed" and corrupt["error"]["code"] == "worker.result_projection_failed", "corrupt projection was regenerated")
        require(record.read_text() == "{broken", "corrupt record was replaced with a fresh preview")
        record.write_bytes(bytes_before)
        require(w.projection(session, operation, identity) == original, "original saved projection did not restore")
        record.unlink()
        missing = w.query(session, operation, identity)
        require(missing["state"] == "failed" and missing["error"]["code"] == "worker.result_record_unavailable", "registered missing record was regenerated")
        require(not record.exists(), "missing registered projection gained a new prefix")
        w.finish()
        manifest, = supervisor.rglob("session.json")
        manifest.unlink()
        old = Worker(exe, resource, w.root, model)
        try:
            rejected = old.raw("session.open", old.open_params("echo-policy-missing", resume=session))
            require(rejected["error"]["code"] == "worker.result_policy_missing", "legacy Worker without policy was silently adopted")
        finally:
            old.finish()

    def query_routing(w):
        session, operation, identity = read_operation(w, "read-result-live", "ROUTING_VISIBLE:" + "r" * 5000)
        original = w.projection(session, operation, identity)
        other = w.open("echo-other", client="other")
        foreign = w.raw("result.query.start", dict(session_id=other, operation_id=operation, client_query_id="foreign", identity=identity))
        require(foreign["error"]["code"] == "worker.result_identity_mismatch", "result query crossed sessions")
        for field, value in (("tool_call_id", "read-call"), ("result_id", "capture-hidden"), ("persisted_event_id", "other-event")):
            invalid = dict(identity, **{field: value})
            rejected = w.query(session, operation, invalid)
            require(rejected["state"] == "failed" and rejected["error"]["code"] == "worker.result_not_exposed", "unshown capture or altered identity reached ReadToolResult")
        for field in ("mode", "offset", "limit", "text_offset", "tail", "cwd", "path"):
            rejected = w.raw("result.query.start", dict(session_id=session, operation_id=operation, client_query_id="forbidden", **{field: "full"}))
            require(rejected["error"]["code"] == "worker.unsupported_field", "query accepted a rolling preview/body override")
        params = dict(session_id=session, operation_id=operation, client_query_id="dedupe", identity=identity)
        first = w.call("result.query.start", **params)
        repeat = w.call("result.query.start", **params)
        require(repeat["duplicate"] and first["query_id"] == repeat["query_id"], "query client key did not deduplicate in-process")
        conflict = w.raw("result.query.start", dict(params, identity=dict(identity, result_id="res-other")))
        require(conflict["error"]["code"] == "worker.result_query_conflict", "same query key changed identity")
        previous = w.attachment
        w.attach()
        stale = w.raw("result.query.get", dict(session_id=session, query_id=first["query_id"]), attachment=previous)
        require(stale["error"]["code"] == "worker.stale_attachment", "old attachment read a result query")
        newer = w.submit(session, "hold-turn", "hold-live")
        until(lambda: w.call("operation.get", session_id=session, operation_id=newer["operation_id"])["state"] == "running")
        pending = w.query(session, newer["operation_id"])
        require(pending["state"] == "failed" and pending["error"].get("sdk_code") == "sdk.result.not_ready", "running operation exposed live tool material")
        require(w.projection(session, operation, identity) == original, "new running turn invalidated an old completed result")
        require(w.call("worker.status")["initialized"], "query executor blocked health")
        w.call("operation.cancel", session_id=session, operation_id=newer["operation_id"])
        require(w.result(session, newer["operation_id"])["state"] == "cancelled", "query path obstructed cancellation")
        model.release("read-result-live")

    def frame_boundary(w):
        full_node = Worker(exe, resource, w.root / "full-frame", model,
            policy={"allow_full_tool_results": True, "version": "frame-v1"})
        try:
            session, operation, identity = read_operation(full_node, "read-result-frame", 'FRAME_PREFIX' + '"' * 600000 + 'FRAME_TAIL', tool_result_sync="full")
            failed = full_node.query(session, operation, identity)
            require(failed["state"] == "failed", "oversize escaped Full frame was accepted")
            require(failed["error"]["code"] == "worker.result_frame_too_large" or
                (failed["error"]["code"] == "worker.result_projection_failed" and
                 failed["error"].get("sdk_code") == "result_too_large"), "Full failed for an unrelated reason")
            require("text" not in failed and "value" not in failed, "failed Full emitted a partial body")
            require("FRAME_PREFIX" not in "".join(full_node.frames), "oversize Full escaped through another response")
            require(all(len(line.encode()) <= 1024 * 1024 for line in full_node.frames), "Worker emitted a frame above IPC limit")
            require(full_node.call("worker.status")["initialized"], "oversize result destroyed the Worker")
        finally:
            full_node.finish()

    def multichannel(w):
        command = Worker(exe, resource, w.root / "channels", model,
            policy={"preview_max_bytes": 12, "version": "channels-v1"})
        try:
            session = command.open("command-result", ("run_command",))
            receipt = command.submit(session)
            approval = command.pending(session)
            command.call("approval.resolve", session_id=session, request_id=approval["request_id"], decision="accept")
            require(command.result(session, receipt["operation_id"])["state"] == "succeeded", "real command tool failed")
            entries = command.tool_results(session, receipt["operation_id"])
            value = command.projection(session, receipt["operation_id"], entries[0]["identity"])
            kinds = {channel["kind"] for channel in value["channels"]}
            require({"stdout", "stderr"} <= kinds, "actual stdout/stderr metadata was not preserved")
            require(value["mode"] == "preview" and value["truncated"] and len(value["text"].encode()) <= 12, "each channel received a separate preview allowance")
            require("CHANNEL_STDERR" not in json.dumps(value), "stderr bypassed the aggregate budget")
            require(all("text" not in channel and "path" not in channel and "sha256" not in channel for channel in value["channels"]), "channel metadata became a raw material side channel")
        finally:
            command.finish()

    functions = (health, same_cwd, idempotency, approvals, cancel, detach, stale, eof, killed, boundary, full_text,
        saved_preview, full_policy, restart_projection, record_integrity, query_routing, frame_boundary, multichannel)
    require(len(SCENARIOS) == len(functions), "scenario implementation count differs")
    for name, function in zip(SCENARIOS, functions):
        yield name, lambda name=name, function=function: run(name, function)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    report = {"schemaVersion": 1, "tests": []}
    model = Model()
    try:
        with tempfile.TemporaryDirectory(prefix="worker-process-") as directory:
            for name, action in scenarios(args.worker.resolve(), args.resource_root.resolve(), Path(directory), model):
                try:
                    action()
                    report["tests"].append({"name": name, "status": "passed"})
                    print("PASS " + name, flush=True)
                except Exception:
                    report["tests"].append({"name": name, "status": "failed"})
                    traceback.print_exc()
    finally:
        model.close()
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    require(Counter(t["name"] for t in report["tests"]) == Counter(SCENARIOS), "required scenario set differs")
    require(all(t["status"] == "passed" for t in report["tests"]), "worker process scenarios failed")


if __name__ == "__main__":
    main()
