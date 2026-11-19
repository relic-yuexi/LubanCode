"""Pure evidence fixtures; never runs CMake, CTest or a project binary."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("package_manifest_gate", Path(__file__).parents[1] / "check_package_manifest.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def fixtures():
    tests = [{"name": gate.REQUIRED, "command": ["/build/tests/lubancode_tests", gate.SOURCE_FILTER],
              "properties": [{"name": "TIMEOUT", "value": 180},
                             {"name": "LABELS", "value": ["unit", "packages", "unit.packages"]}]}]
    results = f'<testsuite tests="1" failures="0"><testcase name="{gate.REQUIRED}" status="run" /></testsuite>'
    log = (f'1/1 Testing: {gate.REQUIRED}\n'
           f'Command: "/build/tests/lubancode_tests" "{gate.SOURCE_FILTER}"\n'
           '[doctest] test cases: 15 | 15 passed | 0 failed | 999 skipped\n'
           '[doctest] assertions: 50 | 50 passed | 0 failed |\nTest Passed.\n')
    return tests, results, log


class PackageManifestGateTests(unittest.TestCase):
    def test_valid_original_source_evidence(self):
        tests, results, log = fixtures()
        gate.validate_registration(tests)
        self.assertEqual(gate.validate_results(results, log, tests[0]["command"]),
                         {"nativeCases": 15, "nativePassed": 15, "assertionsPassed": 50})

    def test_windows_command_and_crlf_original(self):
        tests, results, log = fixtures()
        tests[0]["command"][0] = r"D:\build\tests\Release\lubancode_tests.exe"
        gate.validate_registration(tests)
        log = log.replace("/build/tests/lubancode_tests", tests[0]["command"][0])
        gate.validate_results(results, log.replace("\n", "\r\n"), tests[0]["command"])

    def test_missing_duplicate_and_wrong_registration(self):
        tests, _, _ = fixtures()
        for bad in ([], tests + tests, [{**tests[0], "name": "unit.packages.other"}]):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_registration(bad)

    def test_wrong_command_or_additional_filter(self):
        tests, _, _ = fixtures()
        for command in ([], ["/build/fake", gate.SOURCE_FILTER], ["/build/lubancode_tests"],
                        ["/build/lubancode_tests", "--source-file=*other.cpp"],
                        ["/build/lubancode_tests", gate.SOURCE_FILTER, "--test-case=one"]):
            bad = copy.deepcopy(tests)
            bad[0]["command"] = command
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                gate.validate_registration(bad)

    def test_disabled_unbounded_invalid_and_duplicate_properties(self):
        tests, _, _ = fixtures()
        for extra in ({"name": "DISABLED", "value": True}, {"name": "TIMEOUT", "value": 0},
                      {"name": "TIMEOUT", "value": 181}, {"name": "TIMEOUT", "value": "bad"}):
            bad = copy.deepcopy(tests)
            bad[0]["properties"].append(extra)
            with self.subTest(extra=extra), self.assertRaises(RuntimeError):
                gate.validate_registration(bad)
        for timeout in (0, 181, "bad", None, float("nan")):
            bad = copy.deepcopy(tests)
            bad[0]["properties"][0]["value"] = timeout
            with self.subTest(timeout=timeout), self.assertRaises(RuntimeError):
                gate.validate_registration(bad)

    def test_wrong_labels_or_missing_timeout(self):
        tests, _, _ = fixtures()
        for props in ([tests[0]["properties"][1]], [{"name": "TIMEOUT", "value": 180}],
                      [{"name": "TIMEOUT", "value": 180}, {"name": "LABELS", "value": ["sdk-focused"]}]):
            with self.subTest(props=props), self.assertRaises(RuntimeError):
                gate.validate_registration([{**tests[0], "properties": props}])

    def test_missing_duplicate_wrong_or_not_run_junit(self):
        tests, results, log = fixtures()
        for bad in ("<testsuite />", results.replace("</testsuite>", results + "</testsuite>"),
                    results.replace(gate.REQUIRED, "unit.packages.other"), results.replace('status="run"', 'status="notrun"')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(bad, log, tests[0]["command"])

    def test_failed_error_skipped_and_aggregate_junit(self):
        tests, results, log = fixtures()
        for tag in ("failure", "error", "skipped"):
            bad = results.replace('status="run" />', f'status="run"><{tag}/></testcase>')
            with self.subTest(tag=tag), self.assertRaises(RuntimeError):
                gate.validate_results(bad, log, tests[0]["command"])
        with self.assertRaises(RuntimeError):
            gate.validate_results(results.replace('failures="0"', 'failures="1"'), log, tests[0]["command"])

    def test_actual_command_cannot_borrow_registration(self):
        tests, results, log = fixtures()
        command = f'Command: "/build/tests/lubancode_tests" "{gate.SOURCE_FILTER}"\n'
        for changed in ("", command + command, command.replace("lubancode_tests", "fake"),
                        command.replace("/build/tests/", "/another-checkout/tests/"),
                        command.replace(gate.SOURCE_FILTER, "--source-file=*other.cpp"),
                        command.rstrip() + ' "--test-case=one"\n'):
            with self.subTest(command=changed), self.assertRaises(RuntimeError):
                gate.validate_results(results, log.replace(command, changed), tests[0]["command"])
        windows = 'Command: "D:\\build dir\\tests\\Release\\lubancode_tests.exe" "' + gate.SOURCE_FILTER + '"\n'
        tests[0]["command"][0] = "D:/build dir/tests/Release/lubancode_tests.exe"
        gate.validate_registration(tests)
        gate.validate_results(results, log.replace(command, windows).replace("\n", "\r\n"), tests[0]["command"])

    def test_missing_duplicate_foreign_or_failed_native_section(self):
        tests, results, log = fixtures()
        for bad in ("", log + log, log.replace(gate.REQUIRED, "unit.packages.other"),
                    log.replace("Test Passed.", "Test Failed."), log.replace("Test Passed.", "")):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad, tests[0]["command"])

    def test_native_wrong_fixed_roster_or_failures(self):
        tests, results, log = fixtures()
        for row in ("0 | 0 passed | 0 failed", "14 | 14 passed | 0 failed", "16 | 16 passed | 0 failed",
                    "15 | 14 passed | 1 failed", "15 | 13 passed | 0 failed"):
            with self.subTest(row=row), self.assertRaises(RuntimeError):
                gate.validate_results(results, log.replace("15 | 15 passed | 0 failed", row), tests[0]["command"])
        with self.assertRaises(RuntimeError):
            gate.validate_results(results, log + "[doctest] test cases: 15 | 15 passed | 0 failed\n", tests[0]["command"])

    def test_empty_failed_missing_or_duplicate_assertions(self):
        tests, results, log = fixtures()
        row = "[doctest] assertions: 50 | 50 passed | 0 failed |\n"
        for bad in (log.replace(row, ""), log + row, log.replace("50 | 50 passed | 0 failed", "0 | 0 passed | 0 failed"),
                    log.replace("50 | 50 passed | 0 failed", "50 | 49 passed | 1 failed")):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad, tests[0]["command"])

    def test_failed_native_run_retains_original_evidence(self):
        tests, results, log = fixtures()
        bad_log = log.replace("Test Passed.", "Test Failed.")
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            evidence = build / "test-evidence/package-manifest"

            def fake_ctest(command, **kwargs):
                if "--show-only=json-v1" in command:
                    return subprocess.CompletedProcess(command, 0, stdout=json.dumps({"tests": tests}).encode("utf-8"), stderr=b"")
                original = build / "Testing/Temporary"
                original.mkdir(parents=True)
                (original / "LastTest.log").write_text(bad_log, encoding="utf-8")
                (original / "LastTestsFailed.log").write_text("1:unit.packages.package_manifest\n", encoding="utf-8")
                Path(command[command.index("--output-junit") + 1]).write_text(results, encoding="utf-8")
                raise subprocess.CalledProcessError(8, command)

            with patch("sys.argv", ["check_package_manifest.py", "--build-dir", str(build)]), \
                    patch.object(gate.subprocess, "run", side_effect=fake_ctest), \
                    self.assertRaises(subprocess.CalledProcessError):
                gate.main()
            self.assertEqual((evidence / "LastTest.log").read_text(encoding="utf-8"), bad_log)
            self.assertEqual((evidence / "results.xml").read_text(encoding="utf-8"), results)
            self.assertTrue((evidence / "tests.json").exists())
            self.assertTrue((evidence / "context.json").exists())
            self.assertTrue((evidence / "LastTestsFailed.log").exists())
            self.assertFalse((evidence / "summary.json").exists())

    def test_registration_process_failure_retains_exact_captured_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            stdout, stderr = b"original stdout\r\n", b"original stderr\r\n"
            registered = subprocess.CompletedProcess(["ctest", "--show-only=json-v1"], 9,
                                                     stdout=stdout, stderr=stderr)
            with patch("sys.argv", ["check_package_manifest.py", "--build-dir", directory]), \
                    patch.object(gate.subprocess, "run", return_value=registered) as fake, \
                    self.assertRaises(subprocess.CalledProcessError) as failure:
                gate.main()
            self.assertEqual(fake.call_count, 1)
            self.assertEqual(failure.exception.returncode, 9)
            self.assertEqual(failure.exception.stdout, stdout)
            self.assertEqual(failure.exception.stderr, stderr)
            evidence = Path(directory) / "test-evidence/package-manifest"
            self.assertEqual((evidence / "registration.stdout").read_bytes(), stdout)
            self.assertEqual((evidence / "registration.stderr").read_bytes(), stderr)
            self.assertEqual((evidence / "tests.json").read_bytes(), stdout)
            self.assertEqual(json.loads((evidence / "registration-result.json").read_text(encoding="utf-8"))["returncode"], 9)
            self.assertTrue((evidence / "context.json").exists())
            self.assertFalse((evidence / "results.xml").exists())
            self.assertFalse((evidence / "summary.json").exists())

    def test_bad_registration_never_runs_native_command(self):
        with tempfile.TemporaryDirectory() as directory:
            registered = subprocess.CompletedProcess([], 0, stdout=b'{"tests": []}', stderr=b"")
            with patch("sys.argv", ["check_package_manifest.py", "--build-dir", directory]), \
                    patch.object(gate.subprocess, "run", return_value=registered) as fake, \
                    self.assertRaises(RuntimeError):
                gate.main()
            self.assertEqual(fake.call_count, 1)
            self.assertIn("--show-only=json-v1", fake.call_args.args[0])
            evidence = Path(directory) / "test-evidence/package-manifest"
            self.assertTrue((evidence / "tests.json").exists())
            self.assertTrue((evidence / "context.json").exists())
            self.assertFalse((evidence / "results.xml").exists())


if __name__ == "__main__":
    unittest.main()
