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
                    if model.startswith("slow-"):
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
                    elif model == "read-boundary" and not tool_reply:
                        delta = {"tool_calls": [{"index": 0, "id": "read-call", "type": "function",
                            "function": {"name": "read_file", "arguments": '{"path":"private.txt"}'}}]}
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
    def __init__(self, exe, resource, root, model):
        self.root, self.model = root, model
        (root / "project").mkdir(parents=True, exist_ok=True)
        self.attachment = None
        self.serial = 0
        self.frames, self.stderr = [], []
        self.queue = queue.Queue()
        env = dict(os.environ, LUBAN_WORKER_TEST_KEY=KEY)
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
            self.call("worker.initialize", data_root=str(root / "data"), resource_root=str(resource))
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

    def open(self, model="echo", tools=(), client="session", resume=""):
        params = dict(client_session_id=client, cwd=str(self.root / "project"), model=model,
            builtin_tools=list(tools), connection=dict(wire="chat_completions", base_url=self.model.url,
                api_key_env="LUBAN_WORKER_TEST_KEY", request_timeout_seconds=30, idle_timeout_seconds=10))
        if resume:
            params["resume_session_id"] = resume
        return self.call("session.open", **params)["session_id"]

    def submit(self, session, key="turn", text="hello"):
        return self.call("operation.submit", session_id=session, client_operation_id=key, text=text)

    def result(self, session, operation):
        def terminal():
            value = self.call("operation.get", session_id=session, operation_id=operation)
            return value if value["state"] not in ("accepted", "running") else None
        return until(terminal)

    def pending(self, session):
        return until(lambda: self.call("approval.list", session_id=session)["approvals"])[0]

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
        require(info["capabilities"]["tool_results"] == "metadata-only", "tool preview claim is incorrect")
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
        require("error" in conflict, "changed duplicate payload accepted")

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
        a, b = w.open("slow-cancel", client="a"), w.open("echo-survivor", client="b")
        ar, br = w.submit(a), w.submit(b)
        until(lambda: model.count("slow-cancel"))
        w.call("operation.cancel", session_id=a, operation_id=ar["operation_id"])
        require(w.result(a, ar["operation_id"])["state"] == "cancelled", "target operation was not cancelled")
        require(w.result(b, br["operation_id"])["state"] == "succeeded", "cancel crossed session boundary")

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
        w.submit(s)
        until(lambda: model.count("slow-eof"))
        w.process.stdin.close()
        require(w.process.wait(timeout=15) == 0, "parent EOF failed to drain worker")
        # Reopen the same source to prove Close released the writer and finalized
        # its cancelled turn, rather than merely exiting its input loop.
        next_worker = Worker(exe, resource, w.root, model)
        try:
            resumed = next_worker.open("echo-eof-recovered", resume=s)
            require(resumed == s, "EOF recovery changed session identity")
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

    functions = (health, same_cwd, idempotency, approvals, cancel, detach, stale, eof, killed, boundary, full_text)
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
