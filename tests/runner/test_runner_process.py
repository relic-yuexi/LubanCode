"""Real installed Runner processes; no model, GPU, or network dependency."""
from __future__ import annotations

import argparse
import ctypes
import errno
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


def eventually(check, timeout=10, failure_detail=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(0.04)
    detail = failure_detail() if failure_detail is not None else ""
    raise AssertionError("condition did not become true" + (": " + detail if detail else ""))


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def endpoint_ready(path, old_boot, *, windows=WINDOWS, errors=None):
    try:
        endpoint = read_json(path)
    except FileNotFoundError as error:
        if errors is not None:
            errors[:] = [repr(error)]
        return False
    except PermissionError as error:
        # Python's Windows CRT may retain only EACCES, without a Win32 code.
        # A bounded retry is not evidence of the underlying sharing mode.
        if not windows or error.errno != errno.EACCES or getattr(error, "winerror", None) not in (None, 32, 33):
            raise
        if errors is not None:
            errors[:] = [repr(error)]
        return False
    return endpoint.get("boot_id") != old_boot


def safe_output(value, *, stream=None):
    stream = sys.stdout if stream is None else stream
    try:
        encoding = getattr(stream, "encoding", None) or "utf-8"
        display = value.encode(encoding, errors="backslashreplace").decode(encoding)
        print(display, file=stream, flush=True)
        return True
    except (UnicodeError, OSError, ValueError):
        # Console failure must not replace the stored fixture exception.
        return False


def capture_failure_records(runner):
    records = {}
    errors = []
    for service_log in runner.base.glob("service-*.log"):
        try:
            records[service_log.name] = service_log.read_bytes()
        except Exception:
            errors.append("reading service evidence failed:\n" + traceback.format_exc())
    # Synthetic fixture records only. Never archive identity/auth or task logs.
    for filename in ("jobs.json", "endpoint.json"):
        snapshot = runner.root / filename
        try:
            if snapshot.is_file():
                records[filename] = snapshot.read_bytes()
        except Exception:
            errors.append("reading fixture records failed:\n" + traceback.format_exc())
    return records, errors


def preserve_failure(runner, report_path, name, error, *, records=None):
    if records is None:
        records, errors = capture_failure_records(runner)
        if errors:
            error += "\n" + "\n".join(errors)
    evidence = Path(report_path).parent / "failures" / name
    evidence.mkdir(parents=True, exist_ok=True)
    (evidence / "traceback.txt").write_text(error, encoding="utf-8")
    for filename, raw in records.items():
        (evidence / filename).write_bytes(raw)
    # All original files land before console presentation starts.
    for filename, raw in records.items():
        if filename.startswith("service-") and filename.endswith(".log"):
            safe_output(raw.decode("utf-8", errors="replace")[-4000:])


def write_report(path, report):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")


def run_case(binary, report_path, report, name, check):
    temporary = None
    runner = None
    status = "failed"
    errors = []
    records = {}
    try:
        temporary = tempfile.TemporaryDirectory(prefix="luban-runner-test-")
        runner = Runner(binary, temporary.name)
        runner.start_service()
        check(runner)
        status = "passed"
    except Exception:
        errors.append(traceback.format_exc())
    finally:
        if runner:
            try:
                runner.close()
            except Exception:
                status = "failed"
                errors.append("closing fixture failed:\n" + traceback.format_exc())
            # Cache only fixture-owned records while the source directory still
            # exists. A later cleanup failure can remove some or all originals.
            try:
                records, read_errors = capture_failure_records(runner)
                if read_errors:
                    status = "failed"
                    errors.extend(read_errors)
            except Exception:
                status = "failed"
                errors.append("reading fixture evidence failed:\n" + traceback.format_exc())
        if temporary:
            try:
                temporary.cleanup()
            except Exception:
                status = "failed"
                errors.append("removing fixture directory failed:\n" + traceback.format_exc())
        if errors:
            try:
                preserve_failure(runner, report_path, name, "\n".join(errors), records=records)
            except Exception:
                status = "failed"
                errors.append("preserving evidence failed:\n" + traceback.format_exc())
        # Success discards the memory-only cache. It never publishes service or
        # identity material. Report state includes both service and dir cleanup.
        report["tests"].append({"name": name, "status": status, **({"errors": errors} if errors else {})})
        write_report(report_path, report)
    if errors:
        safe_output("\n".join(errors), stream=sys.stderr)
    safe_output(f"{name}: {status}")
    return status


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
    try:
        reply = json.loads(result.stdout)
    except ValueError as error:
        raise AssertionError(f"request exited {result.returncode}: stdout={result.stdout[:2000]!r}, "
                             f"stderr={result.stderr[:2000]!r}") from error
    assert result.returncode == (0 if reply.get("ok") else 1), result.stderr
    return reply


def events(path):
    if not Path(path).exists():
        return []
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    return [json.loads(line) for line in lines if line.endswith("}")]


class WindowsReplacementBlock:
    """Hold a real readable file handle without FILE_SHARE_DELETE."""
    def __init__(self, path):
        assert WINDOWS
        from ctypes import wintypes
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.api.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                        ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        self.api.CreateFileW.restype = wintypes.HANDLE
        self.api.CloseHandle.argtypes = [wintypes.HANDLE]
        self.api.CloseHandle.restype = wintypes.BOOL
        self.handle = self.api.CreateFileW(str(Path(path).resolve()), 0x80000000, 0x1 | 0x2,
                                          None, 3, 0x80, None)  # READ, share READ|WRITE, OPEN_EXISTING
        if self.handle == ctypes.c_void_p(-1).value:
            self.handle = None
            raise ctypes.WinError(ctypes.get_last_error())

    def close(self):
        if self.handle is not None:
            if not self.api.CloseHandle(self.handle):
                raise ctypes.WinError(ctypes.get_last_error())
            self.handle = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def store_diagnostic(runner, event, file):
    rows = [row for row in events(runner.log_path) if row.get("event") == event and row.get("file") == file]
    if not rows:
        return None
    assert len(rows) == 1, rows
    row = rows[0]
    assert row["atomic_code"] == "atomic.replace_failed", row
    assert row["failure_kind"] == "TransientReject" and row["outcome"] == "NotCommitted", row
    assert isinstance(row["message"], str) and row["message"], row
    assert isinstance(row["attempt"], int) and 1 <= row["attempt"] <= 51, row
    assert set(row) == {"event", "file", "atomic_code", "failure_kind", "outcome", "attempt", "message"}, row
    return row


def fixture_job(marker, nonce, duration, child_marker=None):
    if child_marker and child_marker != "-":
        subprocess.Popen([sys.executable, "-u", THIS, "_job", child_marker, nonce, "20", "-"],
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
        self.log_path = None

    def start_service(self, on_spawn=None):
        old_boot = read_json(self.root / "endpoint.json")["boot_id"] if (self.root / "endpoint.json").exists() else None
        self.log_path = self.base / ("service-" + uuid.uuid4().hex + ".log")
        self.stream = open(self.log_path, "wb")
        env = dict(os.environ, RUNNER_TEST_SECRET=self.secret)
        self.process = subprocess.Popen([self.binary, "serve", "--state-root", str(self.root)],
            stdin=subprocess.DEVNULL, stdout=self.stream, stderr=subprocess.STDOUT, env=env,
            start_new_session=not WINDOWS, creationflags=NO_WINDOW)
        if on_spawn is not None:
            on_spawn()
        readiness_errors = []
        def ready():
            assert self.process.poll() is None, "Runner exited before readiness"
            endpoint = self.root / "endpoint.json"
            return endpoint_ready(endpoint, old_boot, errors=readiness_errors)
        eventually(ready, failure_detail=lambda: readiness_errors[-1] if readiness_errors else "")
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
        assert book["schemaVersion"] == 1 and len(book["records"]) == 1, book
        record = next(iter(book["records"].values()))
        assert record["state"] == "running" and record["exit_code"] is None, record
        assert record["session_id"] == handle["session_id"] and record["job_id"] == handle["job_id"], record
        assert record["job_nonce"] == handle["job_nonce"] == record["process_nonce"], record
        assert record["runner_id"] == runner.runner_id and record["pid"] > 0, record
        assert record["owner_boot_id"] == read_json(runner.root / "endpoint.json")["boot_id"], record
        assert not record["cancel_requested"] and not record["cancel_delivered"], record
        canonical = json.dumps(book["records"], sort_keys=True, ensure_ascii=False, separators=(",", ":"))
        assert book["sha256"] == hashlib.sha256(canonical.encode("utf-8")).hexdigest(), book
        record["pid"] = 0 if starting else sentinel.pid
        if starting:
            record["state"] = "starting"
        canonical = json.dumps(book["records"], sort_keys=True, ensure_ascii=False, separators=(",", ":"))
        book["sha256"] = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
        write_json(book_path, book)
        if WINDOWS and not starting:
            # Exercise both startup writes: recovery saves jobs before publishing endpoint.
            for filename in ("jobs.json", "endpoint.json"):
                runner.kill_service()
                write_json(book_path, book)  # Replay the same valid unfinished snapshot.
                target = runner.root / filename
                before = target.read_bytes()
                endpoint_before = (runner.root / "endpoint.json").read_bytes()
                with WindowsReplacementBlock(target) as blocker:
                    def release_after_rejection():
                        rejected = eventually(lambda: store_diagnostic(runner, "runner.store_retry", filename))
                        assert rejected["attempt"] == 1, rejected
                        assert runner.process.poll() is None, "Runner died during a retryable short rejection"
                        assert target.read_bytes() == before, "Short rejection changed the old target"
                        assert (runner.root / "endpoint.json").read_bytes() == endpoint_before
                        blocker.close()
                    runner.start_service(on_spawn=release_after_rejection)
                assert (runner.root / "endpoint.json").read_bytes() != endpoint_before
                assert store_diagnostic(runner, "runner.store_failed", filename) is None
                saved = read_json(book_path)
                assert saved["records"][next(iter(book["records"]))]["state"] == "indeterminate"
                assert runner.job_call("job.get", handle)["job"]["state"] == "indeterminate"
                assert sentinel.poll() is None
                print(f"windows_{Path(filename).stem}_short_reject_recovery: passed (real handle; persisted indeterminate)",
                      flush=True)
        else:
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
        if WINDOWS and not starting:
            print("windows_store_short_reject_idempotency: passed (same key not rerun; sentinel untouched)", flush=True)
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
    if WINDOWS:
        current_id = runner.runner_id
        runner.kill_service()
        endpoint = runner.root / "endpoint.json"
        before = endpoint.read_bytes()
        book_before = (runner.root / "jobs.json").read_bytes()
        with WindowsReplacementBlock(endpoint):
            def observe_budget_exhaustion():
                assert runner.process.wait(timeout=10) == 1, "Blocked Runner did not fail closed"
                retry = store_diagnostic(runner, "runner.store_retry", "endpoint.json")
                failure = store_diagnostic(runner, "runner.store_failed", "endpoint.json")
                assert retry is not None and retry["attempt"] == 1, retry
                assert failure is not None and 2 <= failure["attempt"] <= 51, failure
                assert {"ok": False, "error": {"code": "runner.store_failed"}} in events(runner.log_path)
                assert endpoint.read_bytes() == before, "Exhausted retry changed the old endpoint"
                assert (runner.root / "jobs.json").read_bytes() == book_before, "Exhausted retry changed the book"
            try:
                runner.start_service(on_spawn=observe_budget_exhaustion)
            except AssertionError as error:
                assert str(error) == "Runner exited before readiness", str(error)
            else:
                raise AssertionError("Permanently held endpoint unexpectedly became ready")
        runner.kill_service()  # Close the failed service log before a new launch.
        runner.start_service()
        assert runner.runner_id == current_id
        print("windows_endpoint_short_reject_budget: passed (bounded failure; old bytes kept; release restarts)", flush=True)


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
    assert final["state"] == "succeeded" and final["exit_code"] == 0, final
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

    # Natural leader exit also cleans a still-running contained descendant.
    entry, _ = runner.start("entry-exit", duration=2, child=True)
    child = runner.project / "entry-exit-child.jsonl"
    eventually(lambda: len(events(child)) > 2)
    result = runner.terminal(entry)
    assert result["state"] == "succeeded" and result["exit_code"] == 0, result
    time.sleep(0.2)
    count = len(events(child))
    time.sleep(0.25)
    assert len(events(child)) == count


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
        run_case(str(Path(args.runner).resolve()), args.report, report, name, check)
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
