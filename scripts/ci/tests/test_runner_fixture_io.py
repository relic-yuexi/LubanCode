"""Pure data checks. Importing the fixture does not start a native process."""
import errno
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[3] / "tests/runner/test_runner_process.py"
SPEC = importlib.util.spec_from_file_location("runner_fixture_io", SCRIPT)
fixture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixture)


class RunnerFixtureIoTests(unittest.TestCase):
    def test_boot_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            endpoint = Path(directory) / "endpoint.json"
            self.assertFalse(fixture.endpoint_ready(endpoint, "old"))
            endpoint.write_text(json.dumps({"boot_id": "old"}), encoding="utf-8")
            self.assertFalse(fixture.endpoint_ready(endpoint, "old"))
            endpoint.write_text(json.dumps({"boot_id": "new"}), encoding="utf-8")
            self.assertTrue(fixture.endpoint_ready(endpoint, "old"))

    def test_windows_crt_denial_is_bounded_by_caller(self):
        errors = []
        with patch.object(fixture, "read_json", side_effect=PermissionError(errno.EACCES, "拒读")):
            self.assertFalse(fixture.endpoint_ready("unused", "old", windows=True, errors=errors))
        self.assertIn("PermissionError", errors[0])

    def test_windows_sharing_codes(self):
        for code in (32, 33):
            error = PermissionError(errno.EACCES, "busy")
            error.winerror = code
            with self.subTest(code=code), patch.object(fixture, "read_json", side_effect=error):
                self.assertFalse(fixture.endpoint_ready("unused", "old", windows=True))

    def test_other_permission_errors_are_hard_failures(self):
        for windows, winerror in ((False, None), (True, 5)):
            error = PermissionError(errno.EACCES, "permanent")
            if winerror is not None:
                error.winerror = winerror
            with self.subTest(windows=windows, winerror=winerror), patch.object(fixture, "read_json", side_effect=error):
                with self.assertRaises(PermissionError):
                    fixture.endpoint_ready("unused", "old", windows=windows)

    def test_invalid_json_is_not_a_readiness_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            endpoint = Path(directory) / "endpoint.json"
            endpoint.write_text("broken", encoding="utf-8")
            with self.assertRaises(json.JSONDecodeError):
                fixture.endpoint_ready(endpoint, None, windows=True)

    def test_timeout_keeps_last_read_diagnostic(self):
        with self.assertRaisesRegex(AssertionError, "PermissionError witness"):
            fixture.eventually(lambda: False, timeout=0, failure_detail=lambda: "PermissionError witness")

    def test_cp1252_display_does_not_lose_raw_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            state = base / "runner-state"
            state.mkdir()
            raw = "真实诊断\n".encode("utf-8") + b"\xff"
            (base / "service-one.log").write_bytes(raw)
            (state / "jobs.json").write_text("{}", encoding="utf-8")
            (state / "identity.json").write_text("secret", encoding="utf-8")
            report = base / "output/process-results.json"
            buffer = io.BytesIO()
            stream = io.TextIOWrapper(buffer, encoding="cp1252")
            with patch.object(fixture.sys, "stdout", stream):
                fixture.preserve_failure(types.SimpleNamespace(base=base, root=state), report, "failed", "原错")
            saved = report.parent / "failures/failed"
            self.assertEqual((saved / "service-one.log").read_bytes(), raw)
            self.assertEqual((saved / "traceback.txt").read_text(encoding="utf-8"), "原错")
            self.assertFalse((saved / "identity.json").exists())
            self.assertIn(b"\\u", buffer.getvalue())
            fixture.write_report(report, {"tests": [{"name": "failed", "status": "failed", "errors": ["原错"]}]})
            self.assertEqual(json.loads(report.read_text(encoding="utf-8"))["tests"][0]["status"], "failed")

    def test_console_failure_keeps_files(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "service-one.log").write_bytes(b"original")
            report = base / "out/process-results.json"
            with patch.object(fixture, "safe_output", return_value=False):
                fixture.preserve_failure(types.SimpleNamespace(base=base, root=base), report, "failed", "original error")
            self.assertEqual((report.parent / "failures/failed/service-one.log").read_bytes(), b"original")
            stream = io.StringIO()
            stream.close()
            self.assertFalse(fixture.safe_output("original error", stream=stream))

    def test_case_cleanup_uses_owned_bytes_and_always_finishes_its_report(self):
        real_directory = tempfile.TemporaryDirectory
        real_capture = fixture.capture_failure_records
        raw = "原始服务字节\n".encode("utf-8") + b"\xff"
        tail = "关场尾字\n".encode("utf-8")
        for mode in ("cleanup", "close", "body_cleanup", "passed"):
            with self.subTest(mode=mode), real_directory() as directory:
                report_path = Path(directory) / "out/process-results.json"
                report = {"schemaVersion": 1, "tests": []}
                calls = []
                source = []

                class Directory:
                    def __init__(self, **options):
                        self.owned = real_directory(**options)
                        self.name = self.owned.name
                        source.append(Path(self.name))

                    def cleanup(self):
                        calls.append("cleanup")
                        self.owned.cleanup()
                        # Deliberately lose every original before raising. The
                        # receipt must use the pre-cleanup owned bytes.
                        if mode in ("cleanup", "body_cleanup"):
                            raise PermissionError(errno.EACCES, "目录清理原错")

                class Runner:
                    def __init__(self, binary, base):
                        self.base = Path(base)
                        self.root = self.base / "runner-state"
                        self.root.mkdir()
                        (self.base / "service-one.log").write_bytes(raw)
                        (self.root / "jobs.json").write_bytes(b'{"jobs":[]}')
                        (self.root / "endpoint.json").write_bytes(b'{"boot_id":"synthetic"}')
                        (self.root / "identity.json").write_bytes(b"private identity")
                        (self.root / "task.log").write_bytes(b"private task output")

                    def start_service(self):
                        calls.append("start")

                    def close(self):
                        calls.append("close")
                        with (self.base / "service-one.log").open("ab") as stream:
                            stream.write(tail)
                        if mode == "close":
                            raise RuntimeError("关场原错")

                def capture(runner):
                    calls.append("capture")
                    return real_capture(runner)

                def check(_):
                    calls.append("check")
                    if mode == "body_cleanup":
                        raise RuntimeError("场内原错")

                with patch.object(fixture, "Runner", Runner), \
                     patch.object(fixture.tempfile, "TemporaryDirectory", Directory), \
                     patch.object(fixture, "capture_failure_records", side_effect=capture), \
                     patch.object(fixture, "safe_output", return_value=False):
                    status = fixture.run_case("never executed", report_path, report, mode, check)
                expected = "passed" if mode == "passed" else "failed"
                self.assertEqual(status, expected)
                self.assertEqual(calls, ["start", "check", "close", "capture", "cleanup"])
                self.assertFalse(source[0].exists())
                saved_report = json.loads(report_path.read_text(encoding="utf-8"))
                self.assertEqual(saved_report, report)
                self.assertEqual(len(report["tests"]), 1)
                self.assertEqual(report["tests"][0]["status"], expected)
                saved = report_path.parent / "failures" / mode
                if mode == "passed":
                    self.assertFalse(saved.exists())
                    self.assertNotIn("errors", report["tests"][0])
                else:
                    self.assertEqual((saved / "service-one.log").read_bytes(), raw + tail)
                    self.assertEqual((saved / "jobs.json").read_bytes(), b'{"jobs":[]}')
                    self.assertEqual((saved / "endpoint.json").read_bytes(), b'{"boot_id":"synthetic"}')
                    self.assertFalse((saved / "identity.json").exists())
                    self.assertFalse((saved / "task.log").exists())
                    error = (saved / "traceback.txt").read_text(encoding="utf-8")
                    self.assertEqual(error, "\n".join(report["tests"][0]["errors"]))
                    self.assertIn("关场原错" if mode == "close" else "目录清理原错", error)
                    if mode == "body_cleanup":
                        self.assertIn("场内原错", error)

                # The next scenario must still run after cleanup failed.
                with patch.object(fixture, "Runner", Runner), \
                     patch.object(fixture.tempfile, "TemporaryDirectory", Directory), \
                     patch.object(fixture, "safe_output", return_value=False):
                    fixture.run_case("never executed", report_path, report, mode + "-next", lambda _: None)
                self.assertEqual(len(json.loads(report_path.read_text(encoding="utf-8"))["tests"]), 2)

    def test_response_bytes_are_retained_before_invalid_json_rejection(self):
        result = types.SimpleNamespace(stdout=b"not-json\r\n\xff", stderr=b"original\r\n", returncode=1)
        original = []
        with patch.object(fixture.subprocess, "run", return_value=result):
            with self.assertRaises(UnicodeDecodeError):
                fixture.request("never executed", "unused", "fixture-id", {"method": "job.start"},
                                on_response=original.append)
        self.assertEqual(original[0].stdout, b"not-json\r\n\xff")
        self.assertEqual(original[0].stderr, b"original\r\n")

    def test_failure_time_state_receipt_logs_and_marker_are_owned_and_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            state = base / "runner-state"
            project = base / "project"
            state.mkdir(); project.mkdir()
            job_id = "a" * 32
            logs = state / "jobs" / job_id
            logs.mkdir(parents=True)
            before = b'{"state":"failed","error_code":"runner.launch_failed"}'
            (state / "jobs.json").write_bytes(before)
            (state / "identity.json").write_bytes(b"auth token never retained")
            (logs / "stdout.log").write_bytes(b"own fixture stdout")
            (logs / "stderr.log").write_bytes(b"own fixture stderr")
            marker = project / "case.jsonl"
            marker.write_bytes(b'{"event":"started"}\n')
            runner = types.SimpleNamespace(base=base, root=state, project=project, handles=[],
                fixture_markers={marker}, start_receipts=[{"stdout": b'{"ok":true}\r\n', "stderr": b"",
                                                         "returncode": 0, "job_id": job_id}])
            original = fixture.capture_failure_observation(runner)
            (state / "jobs.json").write_bytes(b'{"state":"cancelled"}')
            after, errors = fixture.capture_failure_records(runner)
            self.assertFalse(errors)
            after.update(original)
            report = base / "report/results.json"
            fixture.preserve_failure(runner, report, "failure", "original", records=after)
            saved = report.parent / "failures/failure"
            self.assertEqual((saved / "failure-time/jobs.json").read_bytes(), before)
            self.assertEqual((saved / "jobs.json").read_bytes(), b'{"state":"cancelled"}')
            self.assertEqual((saved / "failure-time/receipts/start-00.stdout").read_bytes(), b'{"ok":true}\r\n')
            self.assertEqual((saved / ("failure-time/jobs/" + job_id + "/stderr.log")).read_bytes(), b"own fixture stderr")
            self.assertEqual((saved / "failure-time/markers/case.jsonl").read_bytes(), marker.read_bytes())
            self.assertFalse(any(path.name == "identity.json" for path in saved.rglob("*")))

    def test_missing_not_regular_and_io_error_are_distinct(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "jobs.json").mkdir()
            runner = types.SimpleNamespace(base=base, root=base)
            saved = fixture.capture_failure_observation(runner)
            entries = {e["name"]: e for e in json.loads(saved["failure-time/evidence.json"])["entries"]}
            # Windows refuses opening a directory; POSIX opens then rejects fstat.
            self.assertIn(entries["jobs.json"]["state"], {"not_regular", "read_error"})
            self.assertEqual(entries["endpoint.json"]["state"], "absent")
            with patch.object(fixture, "read_evidence_bytes", side_effect=PermissionError(errno.EACCES, "real error")):
                saved = fixture.capture_failure_observation(runner)
            entry = json.loads(saved["failure-time/evidence.json"])["entries"][0]
            self.assertEqual(entry["state"], "read_error")
            self.assertIn("real error", entry["error"])

    def test_file_total_and_entry_caps_are_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "jobs.json").write_bytes(b"0123456789")
            (base / "endpoint.json").write_bytes(b"abcdefghij")
            runner = types.SimpleNamespace(base=base, root=base)
            with patch.object(fixture, "EVIDENCE_FILE_LIMIT", 6), patch.object(fixture, "EVIDENCE_TOTAL_LIMIT", 8):
                records = fixture.capture_failure_observation(runner)
            manifest = json.loads(records["failure-time/evidence.json"])
            self.assertEqual(records["failure-time/jobs.json"], b"012345")
            self.assertEqual(records["failure-time/endpoint.json"], b"ab")
            self.assertEqual(manifest["capturedBytes"], 8)
            self.assertEqual([entry["actualBytes"] for entry in manifest["entries"][:2]], [10, 10])
            self.assertTrue(all(entry["truncated"] for entry in manifest["entries"][:2]))
            with patch.object(fixture, "EVIDENCE_ENTRY_LIMIT", 1):
                records = fixture.capture_failure_observation(runner)
            manifest = json.loads(records["failure-time/evidence.json"])
            self.assertEqual(len(manifest["entries"]), 1)
            self.assertTrue(manifest["entryLimitReached"])

    def test_diagnostic_read_descriptor_is_closed_on_read_error(self):
        with patch.object(fixture.os, "open", return_value=77), \
             patch.object(fixture.os, "fstat", return_value=types.SimpleNamespace(st_mode=0o100600)), \
             patch.object(fixture.os, "read", side_effect=OSError("read failed")), \
             patch.object(fixture.os, "close") as close:
            with self.assertRaisesRegex(OSError, "read failed"):
                fixture.read_evidence_bytes("never opened", 10)
        close.assert_called_once_with(77)


if __name__ == "__main__":
    unittest.main()
