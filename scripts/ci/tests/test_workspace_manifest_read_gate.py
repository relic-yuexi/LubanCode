"""Pure-data checks: evidence omissions must not turn a native failure green."""
import importlib.util
import io
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
from pathlib import Path

spec = importlib.util.spec_from_file_location("workspace_read_gate", Path(__file__).resolve().parents[1] / "check_workspace_manifest_reads.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def sample(platform="windows-msvc"):
    registration = {"tests": [{"name": name, "command": ["native-tests", "--source-file=" + file]} for name, file in gate.SOURCES.items()]}
    junit = ET.Element("testsuite")
    logs = []
    for number, name in enumerate(gate.SOURCES, 1):
        ET.SubElement(junit, "testcase", name=name, status="run")
        count = (13 if platform == "windows-msvc" else 12) if name.startswith("unit.") else (5 if platform == "windows-msvc" else 4)
        logs.append(f"test {number}\n    Start {number}: {name}\n{number}: [doctest] test cases: {count} | {count} passed | 0 failed\n{number}: [doctest] assertions: 30 | 30 passed | 0 failed\n")
    return registration, junit, "".join(logs), platform


class WorkspaceManifestReadGateTests(unittest.TestCase):
    def test_success_for_each_platform(self):
        for platform in ("windows-msvc", "macos-clang", "linux-manylinux"):
            with self.subTest(platform=platform):
                self.assertEqual(len(gate.validate(*sample(platform))), 2)

    def test_disabled_source_rejected(self):
        registration, *rest = sample()
        registration["tests"][0]["properties"] = [{"name": "DISABLED", "value": True}]
        with self.assertRaises(ValueError):
            gate.validate(registration, *rest)

    def test_wrong_source_filter_rejected(self):
        registration, *rest = sample()
        registration["tests"][0]["command"] = ["native-tests", "--source-file=unrelated.cpp"]
        with self.assertRaises(ValueError):
            gate.validate(registration, *rest)

    def test_skipped_or_failed_junit_rejected(self):
        for status in ("skipped", "failure"):
            registration, junit, native, platform = sample()
            ET.SubElement(junit[0], status)
            with self.subTest(status=status), self.assertRaises(ValueError):
                gate.validate(registration, junit, native, platform)

    def test_native_zero_cases_rejected(self):
        registration, junit, native, platform = sample()
        native = native.replace("13 | 13 passed | 0 failed", "0 | 0 passed | 0 failed")
        with self.assertRaises(ValueError):
            gate.validate(registration, junit, native, platform)

    def test_native_failed_assertion_rejected(self):
        registration, junit, native, platform = sample()
        native = native.replace("30 | 30 passed | 0 failed", "30 | 29 passed | 1 failed", 1)
        with self.assertRaises(ValueError):
            gate.validate(registration, junit, native, platform)

    def test_omitted_native_source_rejected(self):
        registration, junit, native, platform = sample()
        native = native.split("test 2", 1)[0]
        with self.assertRaises(ValueError):
            gate.validate(registration, junit, native, platform)

    def test_wrong_checkout_is_rejected_and_saved(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory).resolve()
            diagnostic = repo / "checkout-error.txt"
            result = subprocess.CompletedProcess([], 0, stdout="a" * 40 + "\n", stderr="")
            with patch.object(gate.subprocess, "run", return_value=result) as run, patch.object(gate.sys, "stderr", io.StringIO()) as stderr:
                with self.assertRaisesRegex(ValueError, "actual checkout differs from GITHUB_SHA"):
                    gate.checkout_head(repo, "b" * 40, diagnostic)
                self.assertIn("actual=" + "a" * 40, stderr.getvalue())
            self.assertIn("expected=" + "b" * 40, diagnostic.read_text(encoding="utf-8"))
            self.assertIn("actual=" + "a" * 40, diagnostic.read_text(encoding="utf-8"))
            self.assertEqual(run.call_args.args[0], ["git", "-c", "safe.directory=" + repo.as_posix(), "rev-parse", "HEAD"])
            self.assertEqual(run.call_args.kwargs["cwd"], repo)
            self.assertTrue(run.call_args.kwargs["check"])

    def test_git_failure_keeps_stderr_and_stays_failed(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory).resolve()
            diagnostic = repo / "checkout-error.txt"
            detail = "fatal: detected dubious ownership in repository at /fixture/repo\n"
            error = subprocess.CalledProcessError(128, ["git"], stderr=detail)
            with patch.object(gate.subprocess, "run", side_effect=error) as run, patch.object(gate.sys, "stderr", io.StringIO()) as stderr:
                with self.assertRaises(subprocess.CalledProcessError) as rejected:
                    gate.checkout_head(repo, "b" * 40, diagnostic)
                self.assertIs(rejected.exception, error)
                self.assertIn(detail, stderr.getvalue())
            self.assertEqual(diagnostic.read_text(encoding="utf-8"), detail)
            self.assertEqual(run.call_args.args[0], ["git", "-c", "safe.directory=" + repo.as_posix(), "rev-parse", "HEAD"])
            self.assertEqual(run.call_args.kwargs["cwd"], repo)

    def test_stderr_encoding_failure_cannot_hide_git_or_wrong_head_error(self):
        class BrokenStderr(io.StringIO):
            def write(self, text):
                raise UnicodeEncodeError("cp1252", text or "x", 0, 1, "fixture rejects diagnostic")
        for failed_git in (False, True):
            with self.subTest(failed_git=failed_git), tempfile.TemporaryDirectory() as directory:
                repo = Path(directory).resolve()
                diagnostic = repo / "checkout-error.txt"
                detail = "fatal: 仓库拒绝\n"
                error = subprocess.CalledProcessError(128, ["git"], stderr=detail)
                result = subprocess.CompletedProcess([], 0, stdout="a" * 40 + "\n", stderr="")
                with patch.object(gate.subprocess, "run", side_effect=error if failed_git else None, return_value=result), patch.object(gate.sys, "stderr", BrokenStderr()):
                    if failed_git:
                        with self.assertRaises(subprocess.CalledProcessError) as rejected:
                            gate.checkout_head(repo, "b" * 40, diagnostic)
                        self.assertIs(rejected.exception, error)
                    else:
                        with self.assertRaisesRegex(ValueError, "actual checkout differs from GITHUB_SHA"):
                            gate.checkout_head(repo, "b" * 40, diagnostic)
                saved = diagnostic.read_text(encoding="utf-8")
                if failed_git:
                    self.assertEqual(saved, detail)
                else:
                    self.assertIn("actual=" + "a" * 40, saved)


if __name__ == "__main__":
    unittest.main()
