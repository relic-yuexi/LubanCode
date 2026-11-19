"""Pure evidence fixtures; never invokes CMake, CTest or product binaries."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("updater_lock_gate", Path(__file__).parents[1] / "check_updater_lock.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def fixtures():
    tests = [{"name": gate.REQUIRED,
              "command": ["/build/tests/lubancode_tests", gate.SOURCE_FILTER],
              "properties": [{"name": "TIMEOUT", "value": 300},
                             {"name": "LABELS", "value": ["integration", "updater", "integration.updater"]}]}]
    results = f'<testsuite tests="1" failures="0"><testcase name="{gate.REQUIRED}" status="run" /></testsuite>'
    log = (f'1/1 Testing: {gate.REQUIRED}\n{gate.READY_MARKER}\n'
           '[doctest] test cases: 8 | 8 passed | 0 failed | 694 skipped\n'
           '[doctest] assertions: 58 | 58 passed | 0 failed |\nTest Passed.\n')
    return tests, results, log


class UpdaterLockGateTests(unittest.TestCase):
    def test_exact_original_source_and_nonzero_native(self):
        tests, results, log = fixtures()
        gate.validate_registration(tests)
        self.assertEqual(gate.validate_results(results, log),
                         {"nativeCases": 8, "nativePassed": 8, "assertionsPassed": 58, "readyPaths": 1})

    def test_missing_duplicate_or_wrong_registration(self):
        tests, _, _ = fixtures()
        for bad in ([], tests + tests, [{**tests[0], "name": "unit.updater.updater_lock"}]):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                gate.validate_registration(bad)

    def test_exact_filter_and_real_cli_command(self):
        tests, _, _ = fixtures()
        for command in (["another", gate.SOURCE_FILTER],
                        ["lubancode_tests", "--source-file=*test_updater*.cpp"],
                        ["lubancode_tests", gate.SOURCE_FILTER, "--test-case=*alive*"], []):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                gate.validate_registration([{**tests[0], "command": command}])
        gate.validate_registration([{**tests[0], "command": [r'C:\build\Release\lubancode_tests.exe', gate.SOURCE_FILTER]}])

    def test_disabled_bad_timeout_or_labels(self):
        tests, _, _ = fixtures()
        props = tests[0]["properties"]
        bad_values = [props + [{"name": "DISABLED", "value": True}], props + [props[0]],
                      [{"name": "TIMEOUT", "value": 300}]]
        bad_values += [[{"name": "TIMEOUT", "value": value}, props[1]] for value in (0, 301, "bad")]
        for bad in bad_values:
            with self.subTest(properties=bad), self.assertRaises(RuntimeError):
                gate.validate_registration([{**tests[0], "properties": bad}])

    def test_skipped_failed_and_not_run_junit(self):
        _, results, log = fixtures()
        bad_values = [results.replace('status="run"', 'status="notrun"')]
        for kind in ("failure", "error", "skipped"):
            bad_values += [results.replace('status="run" />', f'status="run"><{kind}/></testcase>')]
        bad_values += [results.replace('failures="0"', 'failures="1"')]
        for bad in bad_values:
            with self.subTest(junit=bad), self.assertRaises(RuntimeError):
                gate.validate_results(bad, log)

    def test_missing_duplicate_or_foreign_junit(self):
        _, results, log = fixtures()
        for bad in ("<testsuite/>", results.replace(gate.REQUIRED, "unit.config.update_checker"),
                    results.replace('</testsuite>', f'<testcase name="{gate.REQUIRED}" status="run" /></testsuite>')):
            with self.subTest(junit=bad), self.assertRaises(RuntimeError):
                gate.validate_results(bad, log)

    def test_wrong_native_roster_and_empty_assertions(self):
        _, results, log = fixtures()
        for bad in (log.replace('8 | 8 passed', '0 | 0 passed'),
                    log.replace('8 | 8 passed', '7 | 7 passed'),
                    log.replace('8 | 8 passed', '9 | 9 passed'),
                    log.replace('8 | 8 passed | 0 failed', '8 | 7 passed | 1 failed'),
                    log.replace('58 | 58 passed', '0 | 0 passed'),
                    log.replace('58 | 58 passed | 0 failed', '58 | 57 passed | 1 failed')):
            with self.subTest(native=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_missing_duplicate_or_failed_native_sections(self):
        _, results, log = fixtures()
        for bad in ("", log + log, log.replace(gate.REQUIRED, "unit.config.update_checker"),
                    log.replace('Test Passed.', 'Test Failed.')):
            with self.subTest(native=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_missing_duplicate_or_decorated_ready_marker(self):
        _, results, log = fixtures()
        for bad in (log.replace(gate.READY_MARKER, ''),
                    log.replace(gate.READY_MARKER, gate.READY_MARKER + '\n' + gate.READY_MARKER),
                    log.replace(gate.READY_MARKER, 'other source: ' + gate.READY_MARKER)):
            with self.subTest(native=bad), self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_mock_run_retains_original_registration_and_native_evidence(self):
        tests, results, log = fixtures()
        with tempfile.TemporaryDirectory() as root:
            build = Path(root)
            temporary = build / 'Testing' / 'Temporary'
            temporary.mkdir(parents=True)
            def run(command, **kwargs):
                if '--show-only=json-v1' in command:
                    return subprocess.CompletedProcess(command, 0, json.dumps({"tests": tests}))
                self.assertIn(gate.SOURCE_FILTER.split('=')[1], tests[0]['command'][1])
                Path(command[command.index('--output-junit') + 1]).write_text(results, encoding='utf-8')
                (temporary / 'LastTest.log').write_text(log, encoding='utf-8')
                return subprocess.CompletedProcess(command, 0)
            with patch.object(gate.subprocess, 'run', side_effect=run):
                self.assertEqual(gate.run_gate(build, 'Release')['nativeCases'], 8)
            evidence = build / 'test-evidence' / 'updater-lock'
            self.assertEqual((evidence / 'LastTest.log').read_text(encoding='utf-8'), log)
            self.assertEqual(json.loads((evidence / 'tests.json').read_text(encoding='utf-8'))['tests'], tests)
            self.assertTrue((evidence / 'context.json').is_file())
            self.assertTrue((evidence / 'summary.json').is_file())

    def test_mock_native_failure_retains_originals_without_acceptance(self):
        tests, results, log = fixtures()
        with tempfile.TemporaryDirectory() as root:
            build = Path(root)
            temporary = build / 'Testing' / 'Temporary'
            temporary.mkdir(parents=True)
            failed = log.replace('Test Passed.', 'Test Failed.')
            def run(command, **kwargs):
                if '--show-only=json-v1' in command:
                    return subprocess.CompletedProcess(command, 0, json.dumps({"tests": tests}))
                Path(command[command.index('--output-junit') + 1]).write_text(results, encoding='utf-8')
                (temporary / 'LastTest.log').write_text(failed, encoding='utf-8')
                (temporary / 'LastTestsFailed.log').write_text('1:' + gate.REQUIRED, encoding='utf-8')
                raise subprocess.CalledProcessError(8, command)
            with patch.object(gate.subprocess, 'run', side_effect=run), self.assertRaises(subprocess.CalledProcessError):
                gate.run_gate(build, 'Release')
            evidence = build / 'test-evidence' / 'updater-lock'
            self.assertEqual((evidence / 'LastTest.log').read_text(encoding='utf-8'), failed)
            self.assertTrue((evidence / 'LastTestsFailed.log').is_file())
            self.assertTrue((evidence / 'tests.json').is_file())
            self.assertFalse((evidence / 'summary.json').exists())


if __name__ == '__main__':
    unittest.main()
