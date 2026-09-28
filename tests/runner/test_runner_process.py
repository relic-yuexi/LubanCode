"""Real installed Runner processes; no model, GPU, or network dependency."""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import traceback
import uuid

THIS = str(Path(__file__).resolve())
WINDOWS = os.name == "nt"
NO_WINDOW = subprocess.CREATE_NO_WINDOW if WINDOWS else 0
TERMINAL = {"succeeded", "failed", "cancelled", "indeterminate"}


def eventually(check, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.04)
    raise AssertionError("condition did not become true")


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, data):
    path = Path(path)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    temporary.replace(path)


def request(binary, root, runner_id, body):
    result = subprocess.run(
        [binary, "request", "--state-root", str(root), "--runner-id", runner_id],
        input=json.dumps(body) + "\n", text=True, encoding="utf-8", capture_output=True,
        timeout=12, creationflags=NO_WINDOW,
    )
    reply = json.loads(result.stdout)
    assert result.returncode == (0 if reply.get("ok") else 1), result.stderr
    return reply


def events(path):
    if not Path(path).exists():
        return []
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    return [json.loads(line) for line in lines if line.endswith("}")]


def fixture_job(marker, nonce, duration, child_marker=None):
    if child_marker and child_marker != "-":
        subprocess.Popen([sys.executable, "-u", THIS, "_job", child_marker, nonce, duration, "-"],
                         creationflags=NO_WINDOW)
    started = time.monotonic()
    with open(marker, "a", encoding="utf-8") as sink:
        sink.write(json.dumps({"event": "started", "nonce": nonce, "pid": os.getpid(), "cwd": os.getcwd()}) + "\n")
        sink.flush()
        print("experiment-started", flush=True)
        print(os.environ.get("RUNNER_TEST_SECRET", "no-secret"), flush=True)
        print("stderr-witness", file=sys.stderr, flush=True)
        while time.monotonic() - started < float(duration):
            sink.write(json.dumps({"event": "tick", "nonce": nonce}) + "\n")
            sink.flush()
            time.sleep(0.06)
    print("experiment-complete", flush=True)


def fixture_worker(binary, root, runner_id, payload, receipt, gate):
    eventually(lambda: Path(gate).exists())
    reply = request(binary, root, runner_id, read_json(payload))
    write_json(receipt, {"reply": reply, "worker_pid": os.getpid()})
    while True:
        time.sleep(0.1)


class Domain:
    """A disposable Node/Worker kill domain, separate from the Runner."""
    def __init__(self, command, gate):
        self.job = None
        self.process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL, start_new_session=not WINDOWS,
                                        creationflags=NO_WINDOW)
        if WINDOWS:
            from ctypes import wintypes
            class Basic(ctypes.Structure):
                _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                            ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                            ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                            ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD),
                            ("SchedulingClass", wintypes.DWORD)]
            class Io(ctypes.Structure):
                _fields_ = [(name, ctypes.c_ulonglong) for name in
                            ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                             "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]
            class Extended(ctypes.Structure):
                _fields_ = [("BasicLimitInformation", Basic), ("IoInfo", Io), ("ProcessMemoryLimit", ctypes.c_size_t),
                            ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                            ("PeakJobMemoryUsed", ctypes.c_size_t)]
            api = ctypes.WinDLL("kernel32", use_last_error=True)
            api.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
            api.CreateJobObjectW.restype = wintypes.HANDLE
            api.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
            api.SetInformationJobObject.restype = wintypes.BOOL
            api.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
            api.AssignProcessToJobObject.restype = wintypes.BOOL
            api.CloseHandle.argtypes = [wintypes.HANDLE]
            api.CloseHandle.restype = wintypes.BOOL
            self.api = api
            self.job = api.CreateJobObjectW(None, None)
            limits = Extended()
            limits.BasicLimitInformation.LimitFlags = 0x2000  # KILL_ON_JOB_CLOSE
            if not self.job or not api.SetInformationJobObject(self.job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
                if self.job:
                    api.CloseHandle(self.job)
                    self.job = None
                self.process.kill()
                self.process.wait(timeout=10)
                raise ctypes.WinError(ctypes.get_last_error())
            if not api.AssignProcessToJobObject(self.job, int(self.process._handle)):
                api.CloseHandle(self.job)
                self.job = None
                self.process.kill()
                self.process.wait(timeout=10)
                raise ctypes.WinError(ctypes.get_last_error())
        Path(gate).touch()  # Child may only create work after assignment.

    def kill(self):
        if self.job:
            assert self.api.CloseHandle(self.job)
            self.job = None
        elif self.process.poll() is None:
            if WINDOWS:
                self.process.kill()
            else:
                os.killpg(self.process.pid, signal.SIGKILL)
        self.process.wait(timeout=10)


class Runner:
    def __init__(self, binary, base):
        self.binary = binary
        self.base = Path(base)
        self.root = self.base / "runner-state"
        self.project = self.base / "shared project"
        self.project.mkdir()
        self.secret = "local-secret-" + uuid.uuid4().hex
        self.handles = []
        self.process = None
        self.stream = None

    def start_service(self):
        old_boot = read_json(self.root / "endpoint.json")["boot_id"] if (self.root / "endpoint.json").exists() else None
        self.stream = open(self.base / ("service-" + uuid.uuid4().hex + ".log"), "wb")
        env = dict(os.environ, RUNNER_TEST_SECRET=self.secret)
        self.process = subprocess.Popen([self.binary, "serve", "--state-root", str(self.root)],
            stdin=subprocess.DEVNULL, stdout=self.stream, stderr=subprocess.STDOUT, env=env,
            start_new_session=not WINDOWS, creationflags=NO_WINDOW)
        def ready():
            assert self.process.poll() is None, "Runner exited before readiness"
            endpoint = self.root / "endpoint.json"
            return endpoint.exists() and read_json(endpoint).get("boot_id") != old_boot
        eventually(ready)
        self.runner_id = read_json(self.root / "identity.json")["runner_id"]
        info = self.call({"method": "runner.info"})
        assert info["ok"] and info["capabilities"]["worker_exit_survival"] == "independent_domain_required"
        assert not info["capabilities"]["arbitrary_service_restart_survival"]

    def call(self, body, runner_id=None):
        return request(self.binary, self.root, runner_id or self.runner_id, body)

    def spec(self, name, duration=20, child=False):
        marker = self.project / (name + ".jsonl")
        child_marker = self.project / (name + "-child.jsonl") if child else "-"
        return {"argv": [sys.executable, "-u", THIS, "_job", str(marker), name, str(duration), str(child_marker)],
                "cwd": str(self.project), "env_refs": ["RUNNER_TEST_SECRET"]}

    def start(self, key, session="session-a", duration=20, child=False):
        body = {"method": "job.start", "session_id": session, "client_job_id": key,
                "spec": self.spec(key, duration, child)}
        reply = self.call(body)
        assert reply["ok"], reply
        self.handles.append(reply["job"])
        return reply["job"], body

    def job_call(self, method, handle):
        return self.call({"method": method, **{key: handle[key] for key in ("session_id", "job_id", "job_nonce")}})

    def terminal(self, handle):
        def done():
            reply = self.job_call("job.get", handle)
            assert reply["ok"], reply
            return reply["job"] if reply["job"]["state"] in TERMINAL else None
        return eventually(done, 15)

    def cancel(self, handle):
        reply = self.job_call("job.cancel", handle)
        assert reply["ok"], reply
        result = self.terminal(handle)
        assert result["state"] == "cancelled" and result["exit_code"] is not None, result
        return result

    def kill_service(self):
        if self.process and self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=10)
        if self.stream:
            self.stream.close()
            self.stream = None

    def close(self):
        if self.process and self.process.poll() is None:
            for handle in self.handles:
                try:
                    self.job_call("job.cancel", handle)
                except Exception:
                    pass
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        if self.stream:
            self.stream.close()


def survival(runner, node):
    key = "node-job" if node else "worker-job"
    payload = runner.base / "payload.json"
    receipt = runner.base / "receipt.json"
    gate = runner.base / "worker-gate"
    body = {"method": "job.start", "session_id": "session-a", "client_job_id": key, "spec": runner.spec(key, 20)}
    write_json(payload, body)
    args = [sys.executable, "-u", THIS, "_node" if node else "_worker", runner.binary,
            str(runner.root), runner.runner_id, str(payload), str(receipt), str(gate)]
    domain = Domain(args, gate)
    try:
        eventually(lambda: receipt.exists())
        response = read_json(receipt)["reply"]
        assert response["ok"], response
        handle = response["job"]
        runner.handles.append(handle)
        marker = runner.project / (key + ".jsonl")
        eventually(lambda: len(events(marker)) > 2)
        domain.kill()
        count = len(events(marker))
        eventually(lambda: len(events(marker)) > count + 1)
        # A new, independent CLI client reconnects by handle, never by Worker PID.
        current = runner.job_call("job.get", handle)
        assert current["ok"] and current["job"]["state"] == "running"
        assert current["job"]["job_id"] == handle["job_id"]
        runner.cancel(handle)
    finally:
        if domain.process.poll() is None or domain.job:
            domain.kill()


def same_project(runner):
    first, _ = runner.start("shared-a", "session-a")
    second, _ = runner.start("shared-b", "session-b")
    for name in ("shared-a", "shared-b"):
        marker = runner.project / (name + ".jsonl")
        eventually(lambda marker=marker: len(events(marker)) > 2)
        assert os.path.samefile(events(marker)[0]["cwd"], runner.project)
    runner.cancel(first)
    marker = runner.project / "shared-b.jsonl"
    count = len(events(marker))
    eventually(lambda: len(events(marker)) > count + 1)
    runner.cancel(second)


def idempotency(runner):
    handle, body = runner.start("same-key")
    replay = runner.call(body)
    assert replay["ok"] and replay["job"]["job_id"] == handle["job_id"]
    assert replay["job"]["job_nonce"] == handle["job_nonce"]
    changed = json.loads(json.dumps(body))
    changed["spec"]["argv"][-2] = "19"
    assert runner.call(changed)["error"]["code"] == "runner.idempotency_conflict"
    marker = runner.project / "same-key.jsonl"
    eventually(lambda: len(events(marker)) > 2)
    assert sum(row["event"] == "started" for row in events(marker)) == 1
    runner.cancel(handle)

    # Deliver a real request, discard its receipt, then reconnect with the same key.
    body = {"method": "job.start", "session_id": "session-a", "client_job_id": "lost-receipt",
            "spec": runner.spec("lost-receipt")}
    request_id = uuid.uuid4().hex
    identity = read_json(runner.root / "identity.json")
    endpoint = read_json(runner.root / "endpoint.json")
    envelope = {"schemaVersion": 1, "runner_id": runner.runner_id, "boot_id": endpoint["boot_id"],
                "auth_token": identity["auth_token"], "request_id": request_id,
                "expires_at_ms": int(time.time() * 1000) + 15000, "body": body}
    write_json(runner.root / "requests" / (request_id + ".json"), envelope)
    receipt = runner.root / "responses" / (request_id + ".json")
    eventually(lambda: receipt.exists())
    receipt.unlink()  # The first caller never consumes its job handle.
    replay = runner.call(body)
    assert replay["ok"], replay
    handle = replay["job"]
    runner.handles.append(handle)
    marker = runner.project / "lost-receipt.jsonl"
    eventually(lambda: len(events(marker)) > 2)
    assert sum(row["event"] == "started" for row in events(marker)) == 1
    assert runner.call(body)["job"]["job_nonce"] == handle["job_nonce"]
    runner.cancel(handle)


def fencing(runner):
    handle, _ = runner.start("fenced")
    for field, value in (("session_id", "session-b"), ("job_nonce", "0" * 32)):
        wrong = dict(handle, **{field: value})
        for method in ("job.get", "job.cancel"):
            assert runner.job_call(method, wrong)["error"]["code"] == "runner.job_not_owned"
    assert runner.job_call("job.get", handle)["job"]["state"] == "running"
    runner.cancel(handle)


def crash(runner, starting=False):
    handle, body = runner.start("uncertain", duration=2)
    marker = runner.project / "uncertain.jsonl"
    eventually(lambda: len(events(marker)) > 2)
    runner.kill_service()
    sentinel = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(20)"], creationflags=NO_WINDOW)
    try:
        # Replay a valid interrupted ledger snapshot with an unrelated live PID.
        book_path = runner.root / "jobs.json"
        book = read_json(book_path)
        record = next(iter(book["records"].values()))
        record["pid"] = 0 if starting else sentinel.pid
        if starting:
            record["state"] = "starting"
        canonical = json.dumps(book["records"], sort_keys=True, ensure_ascii=False, separators=(",", ":"))
        book["sha256"] = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
        write_json(book_path, book)
        runner.start_service()
        current = runner.job_call("job.get", handle)
        assert current["ok"] and current["job"]["state"] == "indeterminate"
        assert runner.job_call("job.cancel", handle)["error"]["code"] == "runner.job_indeterminate"
        replay = runner.call(body)
        assert replay["ok"] and replay["job"]["job_id"] == handle["job_id"]
        assert replay["job"]["state"] == "indeterminate"
        assert sentinel.poll() is None
        time.sleep(2.2)  # POSIX orphan is allowed to finish; no resume is promised.
        assert sum(row["event"] == "started" for row in events(marker)) == 1
    finally:
        sentinel.terminate()
        sentinel.wait(timeout=10)


def cancel_tree(runner):
    handle, _ = runner.start("tree", child=True)
    child = runner.project / "tree-child.jsonl"
    eventually(lambda: len(events(child)) > 2)
    runner.cancel(handle)
    time.sleep(0.2)
    count = len(events(child))
    time.sleep(0.25)
    assert len(events(child)) == count


def state_fencing(runner):
    old_id = runner.runner_id
    runner.kill_service()
    runner.root.rename(runner.base / "old-state")
    runner.start_service()
    assert runner.runner_id != old_id
    assert runner.call({"method": "runner.info"}, runner_id=old_id)["error"]["code"] == "runner.identity_mismatch"


def launch_failure(runner):
    spec = runner.spec("bad-exe")
    spec["argv"][0] = str(runner.project / "missing-executable")
    body = {"method": "job.start", "session_id": "session-a", "client_job_id": "bad-exe", "spec": spec}
    first = runner.call(body)
    assert first["ok"], first
    result = runner.terminal(first["job"])
    assert result["state"] == "failed"
    again = runner.call(body)
    assert again["ok"] and again["job"]["job_id"] == first["job"]["job_id"]
    runner.kill_service()
    runner.start_service()
    recovered = runner.call(body)
    assert recovered["ok"] and recovered["job"]["job_id"] == first["job"]["job_id"]
    assert recovered["job"]["job_nonce"] == first["job"]["job_nonce"]
    assert recovered["job"]["state"] == "failed"
    assert recovered["job"]["exit_code"] == result["exit_code"]


def results_and_logs(runner):
    handle, body = runner.start("finished", duration=0.2)
    final = runner.terminal(handle)
    assert final["state"] == "succeeded" and final["exit_code"] == 0
    logs = runner.root / "jobs" / handle["job_id"]
    assert "experiment-complete" in (logs / "stdout.log").read_text(encoding="utf-8")
    assert runner.secret in (logs / "stdout.log").read_text(encoding="utf-8")
    assert "stderr-witness" in (logs / "stderr.log").read_text(encoding="utf-8")
    assert runner.secret not in (runner.root / "jobs.json").read_text(encoding="utf-8")
    assert runner.secret not in json.dumps(final)
    assert "experiment-complete" not in json.dumps(final)
    assert final["logs"]["stdout"]["bytes"] > 0
    assert runner.call({"method": "job.logs"})["error"]["code"] == "runner.full_log_sync_disabled"
    runner.kill_service()
    runner.start_service()
    recovered = runner.job_call("job.get", handle)
    assert recovered["ok"], recovered
    assert recovered["job"]["state"] == "succeeded" and recovered["job"]["exit_code"] == 0
    assert recovered["job"]["logs"] == final["logs"]
    replay = runner.call(body)
    assert replay["ok"] and replay["job"]["job_id"] == handle["job_id"]
    assert replay["job"]["state"] == "succeeded"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--report", required=True)
    args = parser.parse_args()
    cases = [
        ("worker_exit_survival", lambda r: survival(r, False)),
        ("node_exit_survival", lambda r: survival(r, True)),
        ("same_project_sessions", same_project),
        ("idempotent_start", idempotency),
        ("session_nonce_fencing", fencing),
        ("runner_crash_indeterminate", lambda r: crash(r, False)),
        ("starting_window_indeterminate", lambda r: crash(r, True)),
        ("cancel_process_group", cancel_tree),
        ("state_directory_fencing", state_fencing),
        ("launch_failure_durable", launch_failure),
        ("exit_status_and_logs", results_and_logs),
    ]
    report = {"schemaVersion": 1, "tests": []}
    for name, check in cases:
        runner = None
        status = "failed"
        with tempfile.TemporaryDirectory(prefix="luban-runner-test-") as directory:
            try:
                runner = Runner(str(Path(args.runner).resolve()), directory)
                runner.start_service()
                check(runner)
                status = "passed"
            except Exception:
                traceback.print_exc()
            finally:
                if runner:
                    runner.close()
        report["tests"].append({"name": name, "status": status})
        print(f"{name}: {status}", flush=True)
    Path(args.report).parent.mkdir(parents=True, exist_ok=True)
    Path(args.report).write_text(json.dumps(report, indent=2), encoding="utf-8")
    return 0 if all(case["status"] == "passed" for case in report["tests"]) else 1


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "_job":
        fixture_job(*sys.argv[2:])
    elif len(sys.argv) > 1 and sys.argv[1] == "_worker":
        fixture_worker(*sys.argv[2:])
    elif len(sys.argv) > 1 and sys.argv[1] == "_node":
        eventually(lambda: Path(sys.argv[-1]).exists())
        subprocess.Popen([sys.executable, "-u", THIS, "_worker", *sys.argv[2:]], creationflags=NO_WINDOW)
        while True:
            time.sleep(0.1)
    else:
        raise SystemExit(main())
