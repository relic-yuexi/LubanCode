"""Pure evidence fixtures; never runs CMake, CTest or a project binary."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("release_query_gate", Path(__file__).parents[1] / "check_release_query.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def fixtures():
    tests = [{"name": gate.REQUIRED, "command": ["/build/tests/lubancode_tests", gate.SOURCE_FILTER],
              "properties": [{"name": "TIMEOUT", "value": 180},
                             {"name": "LABELS", "value": ["unit", "config", "unit.config"]}]}]
    results = f'<testsuite tests="1" failures="0"><testcase name="{gate.REQUIRED}" status="run" /></testsuite>'
    log = (f'1/1 Testing: {gate.REQUIRED}\n'
           '[doctest] test cases: 10 | 10 passed | 0 failed | 999 skipped\n'
           '[doctest] assertions: 50 | 50 passed | 0 failed |\nTest Passed.\n')
    return tests, results, log


class ReleaseQueryGateTests(unittest.TestCase):
    def test_valid_original_source_evidence(self):
        tests, results, log = fixtures()
        gate.validate_registration(tests)
        self.assertEqual(gate.validate_results(results, log),
                         {"nativeCases": 10, "nativePassed": 10, "assertionsPassed": 50})

    def test_windows_command_and_crlf_original(self):
        tests, results, log = fixtures()
        tests[0]["command"][0] = r"D:\build\tests\Release\lubancode_tests.exe"
        gate.validate_registration(tests)
        gate.validate_results(results, log.replace("\n", "\r\n"))

    def test_missing_duplicate_and_wrong_registration(self):
        tests, _, _ = fixtures()
        for bad in ([], tests + tests, [{**tests[0], "name": "unit.config.other"}]):
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
        _, results, log = fixtures()
        for bad in ("<testsuite />", results.replace("</testsuite>", results + "</testsuite>"),
                    results.replace(gate.REQUIRED, "unit.config.other"), results.replace('status="run"', 'status="notrun"')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(bad, log)

    def test_failed_error_skipped_and_aggregate_junit(self):
        _, results, log = fixtures()
        for tag in ("failure", "error", "skipped"):
            bad = results.replace('status="run" />', f'status="run"><{tag}/></testcase>')
            with self.subTest(tag=tag), self.assertRaises(RuntimeError):
                gate.validate_results(bad, log)
        with self.assertRaises(RuntimeError):
            gate.validate_results(results.replace('failures="0"', 'failures="1"'), log)

    def test_missing_duplicate_foreign_or_failed_native_section(self):
        _, results, log = fixtures()
        for bad in ("", log + log, log.replace(gate.REQUIRED, "unit.config.other"),
                    log.replace("Test Passed.", "Test Failed."), log.replace("Test Passed.", "")):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_native_wrong_fixed_roster_or_failures(self):
        _, results, log = fixtures()
        for row in ("0 | 0 passed | 0 failed", "9 | 9 passed | 0 failed", "11 | 11 passed | 0 failed",
                    "10 | 9 passed | 1 failed", "10 | 8 passed | 0 failed"):
            with self.subTest(row=row), self.assertRaises(RuntimeError):
                gate.validate_results(results, log.replace("10 | 10 passed | 0 failed", row))
        with self.assertRaises(RuntimeError):
            gate.validate_results(results, log + "[doctest] test cases: 10 | 10 passed | 0 failed\n")

    def test_empty_failed_missing_or_duplicate_assertions(self):
        _, results, log = fixtures()
        row = "[doctest] assertions: 50 | 50 passed | 0 failed |\n"
        for bad in (log.replace(row, ""), log + row, log.replace("50 | 50 passed | 0 failed", "0 | 0 passed | 0 failed"),
                    log.replace("50 | 50 passed | 0 failed", "50 | 49 passed | 1 failed")):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_failed_native_run_retains_original_evidence(self):
        tests, results, log = fixtures()
        bad_log = log.replace("Test Passed.", "Test Failed.")
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            evidence = build / "test-evidence/release-query"

            def fake_ctest(command, **kwargs):
                if "--show-only=json-v1" in command:
                    return subprocess.CompletedProcess(command, 0, stdout=json.dumps({"tests": tests}))
                original = build / "Testing/Temporary"
                original.mkdir(parents=True)
                (original / "LastTest.log").write_text(bad_log, encoding="utf-8")
                (original / "LastTestsFailed.log").write_text("1:unit.config.update_checker\n", encoding="utf-8")
                Path(command[command.index("--output-junit") + 1]).write_text(results, encoding="utf-8")
                raise subprocess.CalledProcessError(8, command)

            with patch("sys.argv", ["check_release_query.py", "--build-dir", str(build)]), \
                    patch.object(gate.subprocess, "run", side_effect=fake_ctest), \
                    self.assertRaises(subprocess.CalledProcessError):
                gate.main()
            self.assertEqual((evidence / "LastTest.log").read_text(encoding="utf-8"), bad_log)
            self.assertEqual((evidence / "results.xml").read_text(encoding="utf-8"), results)
            self.assertTrue((evidence / "tests.json").exists())
            self.assertTrue((evidence / "context.json").exists())
            self.assertTrue((evidence / "LastTestsFailed.log").exists())
            self.assertFalse((evidence / "summary.json").exists())

    def test_bad_registration_never_runs_native_command(self):
        with tempfile.TemporaryDirectory() as directory:
            registered = subprocess.CompletedProcess([], 0, stdout='{"tests": []}')
            with patch("sys.argv", ["check_release_query.py", "--build-dir", directory]), \
                    patch.object(gate.subprocess, "run", return_value=registered) as fake, \
                    self.assertRaises(RuntimeError):
                gate.main()
            self.assertEqual(fake.call_count, 1)
            self.assertIn("--show-only=json-v1", fake.call_args.args[0])
            evidence = Path(directory) / "test-evidence/release-query"
            self.assertTrue((evidence / "tests.json").exists())
            self.assertTrue((evidence / "context.json").exists())
            self.assertFalse((evidence / "results.xml").exists())


if __name__ == "__main__":
    unittest.main()
