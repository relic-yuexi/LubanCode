"""Pure evidence counterexamples; never invokes CMake, CTest or native code."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("runner_release_gate", Path(__file__).parents[1] / "check_runner_release.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def fixtures():
    tests = [{"name": gate.REQUIRED, "command": ["/build/tests/luban_runner_release_tests", gate.SOURCE_FILTER],
              "properties": [{"name": "TIMEOUT", "value": 180},
                             {"name": "LABELS", "value": ["unit", "job_runner", "unit.job_runner"]}]}]
    results = f'<testsuite tests="1" failures="0"><testcase name="{gate.REQUIRED}" status="run" /></testsuite>'
    log = (f'1/1 Testing: {gate.REQUIRED}\nCommand: "/build/tests/luban_runner_release_tests" "{gate.SOURCE_FILTER}"\n{gate.READY_MARKER}\n'
           '[doctest] test cases: 1 | 1 passed | 0 failed | 694 skipped\n'
           '[doctest] assertions: 8 | 8 passed | 0 failed |\nTest Passed.\n')
    return tests, results, log


class RunnerReleaseGateTests(unittest.TestCase):
    def test_one_actual_source(self):
        tests, results, log = fixtures()
        gate.validate_registration(tests)
        self.assertEqual(gate.validate_results(results, log)["nativeCases"], 1)

    def test_missing_duplicate_or_foreign_registration(self):
        tests, _, _ = fixtures()
        for bad in ([], tests + tests, [{**tests[0], "name": "runner.process.lifecycle"}]):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_registration(bad)

    def test_exact_cli_filter(self):
        tests, _, _ = fixtures()
        for command in ([], ["luban-runner", gate.SOURCE_FILTER], ["lubancode_tests", gate.SOURCE_FILTER],
                        ["luban_runner_release_tests", "--source-file=*runner*.cpp"],
                        ["luban_runner_release_tests", gate.SOURCE_FILTER, "--test-case=*Release*"]):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                gate.validate_registration([{**tests[0], "command": command}])

    def test_enabled_bounded_labels(self):
        tests, _, _ = fixtures()
        properties = tests[0]["properties"]
        for bad in (properties + [properties[0]], properties + [{"name": "DISABLED", "value": True}],
                    [{"name": "TIMEOUT", "value": 0}, properties[1]], [properties[0]]):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_registration([{**tests[0], "properties": bad}])

    def test_junit_failure_skip_foreign_or_empty(self):
        _, results, log = fixtures()
        values = ("<testsuite/>", results.replace('status="run"', 'status="notrun"'),
                  results.replace(gate.REQUIRED, "runner.process.lifecycle"),
                  results.replace('failures="0"', 'failures="1"'),
                  results.replace('status="run" />', 'status="run"><skipped/></testcase>'))
        for bad in values:
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(bad, log)

    def test_native_roster_and_assertions_nonzero(self):
        _, results, log = fixtures()
        for bad in (log.replace('1 | 1 passed', '0 | 0 passed'), log.replace('1 | 1 passed', '2 | 2 passed'),
                    log.replace('1 | 1 passed | 0 failed', '1 | 0 passed | 1 failed'),
                    log.replace('8 | 8 passed', '0 | 0 passed')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_platform_marker_once_from_this_source(self):
        _, results, log = fixtures()
        foreign = "[runner-release-path] " + ("posix-owned-gate-rejection" if gate.os.name == "nt"
                                               else "windows-normal-release")
        for bad in (log.replace(gate.READY_MARKER, ''), log.replace(gate.READY_MARKER, foreign),
                    log.replace(gate.READY_MARKER, gate.READY_MARKER + '\n' + gate.READY_MARKER),
                    log.replace(gate.READY_MARKER, 'foreign-source: ' + gate.READY_MARKER)):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_native_failed_duplicate_foreign_or_missing_section(self):
        _, results, log = fixtures()
        for bad in ('', log + log, log.replace(gate.REQUIRED, 'runner.process.lifecycle'),
                    log.replace('Test Passed.', 'Test Failed.')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_native_command_cannot_borrow_registration(self):
        _, results, log = fixtures()
        for bad in (log.replace('/build/tests/luban_runner_release_tests', '/another/binary'),
                    log.replace('/build/tests/luban_runner_release_tests', '/build/tests/lubancode_tests'),
                    log.replace(gate.SOURCE_FILTER, '--source-file=*test_other.cpp'),
                    log + '\nCommand: "luban_runner_release_tests"',
                    log.replace(f'Command: "/build/tests/luban_runner_release_tests" "{gate.SOURCE_FILTER}"\n', '')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_actual_ctest_failure_retains_lasttest(self):
        tests, _, log = fixtures()
        import json
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            temporary = build / "Testing/Temporary"
            temporary.mkdir(parents=True)
            (temporary / "LastTest.log").write_text(log.replace('Test Passed.', 'Test Failed.'), encoding='utf-8')
            failure = subprocess.CalledProcessError(1, ['ctest'])
            listed = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": tests}))
            with patch.object(gate.subprocess, 'run', side_effect=[listed, failure]):
                with self.assertRaises(subprocess.CalledProcessError):
                    gate.run_gate(build, 'Release')
            self.assertEqual((build / 'test-evidence/runner-release/LastTest.log').read_text(encoding='utf-8'),
                             log.replace('Test Passed.', 'Test Failed.'))
