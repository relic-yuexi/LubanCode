"""Relocated SDK header gate fixtures; no configure, build or native execution."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "check_installed_sdk.py"
SPEC = importlib.util.spec_from_file_location("sdk_installed", SCRIPT)
installed = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(installed)

FOCUSED_SPEC = importlib.util.spec_from_file_location("sdk_focused", SCRIPT.with_name("check_sdk_focused.py"))
focused = importlib.util.module_from_spec(FOCUSED_SPEC)
FOCUSED_SPEC.loader.exec_module(focused)


class NativeCommandEvidenceTests(unittest.TestCase):
    def test_posix_full_argument_list_accepts_the_actual_registration(self):
        command = ["/real checkout/build/tests/lubancore_sdk_tests", "--source-file=*test_real.cpp"]
        body = 'Command: "/real checkout/build/tests/lubancore_sdk_tests" "--source-file=*test_real.cpp"\n'
        focused.check_native_command(body, command)
        focused.check_native_command(body.replace("\n", "\r\n"), command)

    def test_windows_spaces_and_backslashes_only_normalize_separators(self):
        command = [r"C:\actual checkout\build\tests\Release\lubancore_sdk_tests.exe",
                   "--source-file=*test_real.cpp"]
        body = 'Command: "C:\\actual checkout\\build\\tests\\Release\\lubancore_sdk_tests.exe" "--source-file=*test_real.cpp"\n'
        focused.check_native_command(body, command)
        focused.check_native_command(body.replace("\\", "/"), command)
        focused.check_native_command(body, [part.replace("\\", "/") for part in command])
        with self.assertRaises(RuntimeError):
            focused.check_native_command(body.replace("actual checkout", "ACTUAL checkout"), command)

    def test_same_basename_from_another_checkout_cannot_replace_the_registered_binary(self):
        command = ["/actual/build/lubancore_sdk_tests", "--source-file=*test_real.cpp"]
        for executable in ("/foreign/build/lubancore_sdk_tests", "lubancore_sdk_tests"):
            body = f'Command: "{executable}" "{command[1]}"\n'
            with self.subTest(executable=executable), self.assertRaises(RuntimeError):
                focused.check_native_command(body, command)

    def test_added_removed_or_changed_arguments_cannot_borrow_a_passing_native_summary(self):
        command = ["/actual/build/lubancore_sdk_tests", "--source-file=*test_real.cpp"]
        variants = [command + ["--test-case=only-one"], command + ["--source-file=*another.cpp"],
                    command[:1], [command[0], "--source-file=*another.cpp"]]
        for changed in variants:
            body = "Command: " + " ".join('"' + part + '"' for part in changed) + "\n"
            body += "[doctest] test cases: 10 | 10 passed | 0 failed\nTest Passed.\n"
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                focused.check_native_command(body, command)

    def test_missing_duplicate_or_decorated_command_is_rejected(self):
        command = ["/actual/build/lubancore_sdk_tests", "--source-file=*test_real.cpp"]
        line = 'Command: "/actual/build/lubancore_sdk_tests" "--source-file=*test_real.cpp"\n'
        for body in ("Test Passed.\n", line + line, "borrowed: " + line, line + 'Command: "foreign"\n'):
            with self.subTest(body=body), self.assertRaises(RuntimeError):
                focused.check_native_command(body, command)

    def test_invalid_registration_or_unparseable_command_is_rejected(self):
        command = ["/actual/build/lubancore_sdk_tests", "--source-file=*test_real.cpp"]
        line = 'Command: "/actual/build/lubancore_sdk_tests" "--source-file=*test_real.cpp"\n'
        for invalid in (None, [], "native", [3, command[1]], [command[0], None], ["", command[1]]):
            with self.subTest(invalid=invalid), self.assertRaises(RuntimeError):
                focused.check_native_command(line, invalid)
        for body in ('Command: "unterminated\n', "Command: \n"):
            with self.subTest(body=body), self.assertRaises(RuntimeError):
                focused.check_native_command(body, command)


class PackageNativeEvidenceTests(unittest.TestCase):
    def evidence(self, count, executable="lubancore_sdk_tests"):
        source = "test_package_manifest.cpp" if count == 15 else "test_lubancore_package_manifest.cpp"
        command = ["C:/actual build/" + executable + ".exe", "--source-file=*" + source]
        body = (f'Command: "{command[0]}" "{command[1]}"\n'
                f'[doctest] test cases: {count} | {count} passed | 0 failed\n'
                '[doctest] assertions: 20 | 20 passed | 0 failed')
        return command, body

    def test_original_and_public_source_filters_cannot_be_replaced_or_reduced(self):
        for source in ("test_package_manifest.cpp", "test_lubancore_package_manifest.cpp"):
            for executable in ("lubancode_tests", "lubancore_sdk_tests"):
                command = ["C:/actual build/" + executable + ".exe", "--source-file=*" + source]
                focused.check_package_registration(command, source, executable)
                for wrong in ([], ["fake", command[1]], [command[0], "--source-file=*other.cpp"],
                              [*command, "--test-case=only-one"]):
                    with self.subTest(source=source, command=wrong), self.assertRaises(RuntimeError):
                        focused.check_package_registration(wrong, source, executable)

    def test_exact_original_and_public_native_case_rosters(self):
        for count in (15, 8):
            for executable in ("lubancode_tests", "lubancore_sdk_tests"):
                command, body = self.evidence(count, executable)
                focused.check_package_native(body, count, command)
                focused.check_package_native(body.replace("\n", "\r\n"), count, command)

    def test_same_basename_foreign_checkout_path_and_extra_actual_filters_reject(self):
        for count in (15, 8):
            command, body = self.evidence(count)
            for bad in (body.replace("C:/actual build/", "D:/foreign checkout/"),
                        body.replace('"--source-file=*', '"--source-file=*other-'),
                        body.replace('"\n[doctest]', '" "--test-case=one"\n[doctest]'),
                        body + '\nCommand: "fake"', body.replace('Command:', 'Unrelated:')):
                with self.subTest(count=count, bad=bad), self.assertRaises(RuntimeError):
                    focused.check_package_native(bad, count, command)

    def test_registration_failure_retains_original_stdout_stderr_and_exitcode(self):
        with tempfile.TemporaryDirectory() as scratch:
            failed = subprocess.CompletedProcess([], 17, b"\xffraw-stdout\n", b"\xffraw-stderr\n")
            with patch("sys.argv", ["check", "--build-dir", scratch]), \
                    patch.object(focused.subprocess, "run", return_value=failed) as mocked:
                with self.assertRaises(subprocess.CalledProcessError):
                    focused.main()
                self.assertEqual(mocked.call_count, 1)
                self.assertNotIn("check", mocked.call_args.kwargs)
            evidence = Path(scratch) / "test-evidence/sdk-focused"
            self.assertEqual((evidence / "registration.stdout").read_bytes(), failed.stdout)
            self.assertEqual((evidence / "tests.json").read_bytes(), failed.stdout)
            self.assertEqual((evidence / "registration.stderr").read_bytes(), failed.stderr)
            result = json.loads((evidence / "registration-result.json").read_text(encoding="utf-8"))
            self.assertEqual(result["returncode"], 17)
            self.assertEqual(result["command"][-1], "--show-only=json-v1")

    def test_missing_duplicate_changed_skipped_and_empty_native_reports_reject(self):
        for count in (15, 8):
            command, body = self.evidence(count)
            for invalid in ('', body + '\n' + body, body.replace(f'{count} passed', '0 passed'),
                            body.replace('20 | 20 passed', '0 | 0 passed'),
                            body.replace('20 passed | 0 failed', '19 passed | 1 failed'),
                            body.replace(f'test cases: {count}', f'test cases: {count + 1}')):
                with self.subTest(count=count, invalid=invalid), self.assertRaises(RuntimeError):
                    focused.check_package_native(invalid, count, command)


class ActionPathGateTests(unittest.TestCase):
    summary = "[doctest] test cases: 10 | 10 passed | 0 failed"
    paths = tuple("[sdk-action-path] " + path for path in focused.ACTION_PATHS)
    private_paths = tuple("[sdk-action-native] " + path for path in
                          ("existing-permission-chain", "summary-stop", "receipt-stop", "binding-opening"))

    def test_public_and_private_actual_paths(self):
        focused.check_action_paths("\n".join(self.paths), native=False)
        focused.check_action_paths("\n".join((self.summary, *self.paths, *self.private_paths)), native=True)

    def test_missing_decorated_duplicate_public_paths_reject(self):
        for marker in self.paths:
            with self.subTest(marker=marker):
                absent = "\n".join(value for value in self.paths if value != marker)
                for body in (absent, absent + "\nother-source: " + marker, "\n".join((*self.paths, marker))):
                    with self.assertRaisesRegex(RuntimeError, "public path did not finish once"):
                        focused.check_action_paths(body, native=False)

    def test_missing_or_duplicated_internal_paths_reject(self):
        for marker in self.private_paths:
            absent = "\n".join((self.summary, *self.paths, *(value for value in self.private_paths if value != marker)))
            for body in (absent, absent + "\nother-source: " + marker,
                         "\n".join((self.summary, *self.paths, *self.private_paths, marker))):
                with self.assertRaisesRegex(RuntimeError, "internal path did not finish once"):
                    focused.check_action_paths(body, native=True)

    def test_zero_changed_failed_or_duplicate_native_summary_rejects(self):
        for summary in ("", "[doctest] test cases: 0 | 0 passed | 0 failed",
                        self.summary.replace("10 passed | 0 failed", "9 passed | 1 failed"),
                        self.summary + "\n" + self.summary):
            with self.assertRaisesRegex(RuntimeError, "10 successful cases"):
                focused.check_action_paths("\n".join((summary, *self.paths, *self.private_paths)), native=True)


class CommandLimitsEvidenceTests(unittest.TestCase):
    command = ["C:/actual build/lubancore_sdk_tests.exe",
               "--source-file=*test_run_command_execution_limits.cpp"]

    def body(self, command=None, platform_name=None):
        import os
        command = self.command if command is None else command
        shells = ("cmd", "powershell") if (platform_name or os.name) == "nt" else ("sh", "bash")
        records = [json.dumps({"shell": shell, "cwd": "/actual/cwd", "exact_command": "probe exact 128",
            "excess_command": "probe excess 129", "timeout_ms": 15000, "max_output_bytes": 128,
            "exact_request_bytes": 128, "excess_request_bytes": 129,
            "exact_outcome": "succeeded", "excess_error_code": "process.output_limit"}) for shell in shells]
        return "\n".join(("Command: " + " ".join('"' + part + '"' for part in command),
            "[doctest] test cases: 6 | 6 passed | 0 failed",
            "[doctest] assertions: 101 | 101 passed | 0 failed",
            *("[command-limits-path] " + path for path in focused.COMMAND_LIMITS_PATHS),
            *("[command-limits-shell] " + record for record in records),
            "Test Passed."))

    def test_actual_sdk_and_cli_full_commands_and_crlf_are_accepted(self):
        for exe in ("C:/actual build/lubancore_sdk_tests.exe", "/actual/build/lubancode_tests"):
            command = [exe, self.command[1]]
            focused.check_command_limits_native(self.body(command), command)
            focused.check_command_limits_native(self.body(command).replace("\n", "\r\n"), command)
        for platform_name in ("nt", "posix"):
            focused.check_command_limits_native(self.body(platform_name=platform_name), self.command, platform_name)

    def test_missing_duplicate_and_foreign_platform_shell_records_reject(self):
        body = self.body(platform_name="nt")
        records = [line for line in body.splitlines() if line.startswith("[command-limits-shell] ")]
        for bad in (body.replace(records[0], ""), body + "\n" + records[0],
                    body.replace(records[1], records[0]), body.replace('"powershell"', '"bash"')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_command_limits_native(bad, self.command, "nt")

    def test_malformed_and_failed_shell_records_reject(self):
        body = self.body(platform_name="nt")
        for bad in (body.replace('"max_output_bytes": 128', '"max_output_bytes": 129'),
                    body.replace('"exact_outcome": "succeeded"', '"exact_outcome": "timed_out"'),
                    body.replace('"excess_error_code": "process.output_limit"', '"excess_error_code": ""'),
                    body.replace('"exact_request_bytes": 128', '"exact_request_bytes": true'),
                    body.replace('[command-limits-shell] {', '[command-limits-shell] invalid{'),
                    body.replace('"cwd": "/actual/cwd"', '"cwd": ""')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_command_limits_native(bad, self.command, "nt")

    def test_foreign_same_basename_and_extra_filter_cannot_borrow_passed_summary(self):
        variants = [["D:/foreign build/lubancore_sdk_tests.exe", self.command[1]],
                    [*self.command, "--test-case=only-one"],
                    [self.command[0], "--source-file=*another.cpp"]]
        for command in variants:
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_command_limits_native(self.body(command), self.command)

    def test_registration_wrong_executable_source_and_extra_arguments_reject(self):
        variants = [[], ["/actual/build/foreign", self.command[1]],
                    [self.command[0], "--source-file=*test_tools.cpp"],
                    [*self.command, "--source-file=*another.cpp"],
                    [*self.command, "--test-case=only-one"]]
        for command in variants:
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_command_limits_registration(command)

    def test_missing_non_list_non_string_and_empty_registration_reject_stably(self):
        for command in (None, "native", (), [], [None, self.command[1]],
                        [self.command[0], 7], ["", self.command[1]]):
            with self.subTest(command=command):
                with self.assertRaises(RuntimeError):
                    focused.check_command_limits_registration(command)
                with self.assertRaises(RuntimeError):
                    focused.check_command_limits_native(self.body(), command)

    def test_missing_duplicate_and_decorated_actual_commands_reject(self):
        body = self.body()
        line = body.splitlines()[0]
        for bad in (body.replace(line, ""), body + "\n" + line,
                    body.replace(line, "another-source: " + line)):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_command_limits_native(bad, self.command)
        for line in ('Command: "unterminated', 'Command: ""', 'Command: '):
            with self.subTest(line=line), self.assertRaises(RuntimeError):
                focused.check_command_limits_native(body.replace(body.splitlines()[0], line), self.command)

    def test_missing_duplicate_changed_zero_and_failed_cases_reject(self):
        body = self.body()
        line = "[doctest] test cases: 6 | 6 passed | 0 failed"
        for replacement in ("", line + "\n" + line,
                            "[doctest] test cases: 0 | 0 passed | 0 failed",
                            "[doctest] test cases: 5 | 5 passed | 0 failed",
                            "[doctest] test cases: 7 | 7 passed | 0 failed",
                            "[doctest] test cases: 6 | 5 passed | 1 failed"):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_command_limits_native(body.replace(line, replacement), self.command)

    def test_nonzero_assertions_and_actual_ctest_success_are_required(self):
        body = self.body()
        line = "[doctest] assertions: 101 | 101 passed | 0 failed"
        for replacement in ("", line + "\n" + line,
                            "[doctest] assertions: 0 | 0 passed | 0 failed",
                            "[doctest] assertions: 101 | 100 passed | 1 failed"):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_command_limits_native(body.replace(line, replacement), self.command)
        for bad in (body.replace("Test Passed.", ""), body + "\nTest Passed.",
                    body.replace("Test Passed.", "Test Failed.")):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_command_limits_native(bad, self.command)

    def test_each_path_marker_is_exact_unique_and_not_borrowed(self):
        body = self.body()
        for path in focused.COMMAND_LIMITS_PATHS:
            marker = "[command-limits-path] " + path
            for bad in (body.replace(marker, ""), body + "\n" + marker,
                        body.replace(marker, "borrowed: " + marker)):
                with self.subTest(path=path, bad=bad), self.assertRaises(RuntimeError):
                    focused.check_command_limits_native(bad, self.command)


class JobStartupGateTests(unittest.TestCase):
    def body(self, command, original=False):
        import shlex
        count = 16 if original else 6
        return "\n".join(("Command: " + " ".join('"' + value + '"' for value in command),
            f"[doctest] test cases: {count} | {count} passed | 0 failed",
            "[doctest] assertions: 91 | 91 passed | 0 failed",
            *("[job-start-path] " + path for path in focused.JOB_START_PATHS)))

    def test_actual_sdk_and_cli_commands_cover_new_and_original_sources(self):
        for exe in ("/build/real/lubancore_sdk_tests", "C:/build real/lubancode_tests.exe"):
            for original in (False, True):
                source = "test_tool_job_coordinator.cpp" if original else "test_tool_job_start_transaction.cpp"
                command = [exe, "--source-file=*" + source]
                focused.check_job_start_native(self.body(command, original), command, original)

    def test_full_command_same_basename_foreign_checkout_and_extra_filter_reject(self):
        command = ["/build/real/lubancore_sdk_tests", "--source-file=*test_tool_job_start_transaction.cpp"]
        for bad in ([command[0].replace("/real/", "/foreign/"), command[1]], command + ["--test-case=one"]):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_job_start_native(self.body(bad), command)

    def test_missing_duplicate_and_decorated_markers_reject(self):
        command = ["/build/real/lubancore_sdk_tests", "--source-file=*test_tool_job_start_transaction.cpp"]
        body = self.body(command)
        for path in focused.JOB_START_PATHS:
            marker = "[job-start-path] " + path
            for bad in (body.replace(marker, ""), body + "\n" + marker, body.replace(marker, "other-source: " + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_job_start_native(bad, command)

    def test_empty_failed_changed_case_and_assertion_counts_reject(self):
        command = ["/build/real/lubancore_sdk_tests", "--source-file=*test_tool_job_start_transaction.cpp"]
        body = self.body(command)
        for bad in (body.replace("6 | 6 passed", "0 | 0 passed"), body.replace("6 | 6 passed", "5 | 5 passed"),
                    body.replace("6 passed | 0 failed", "5 passed | 1 failed"),
                    body.replace("91 | 91 passed", "0 | 0 passed"), body.replace("91 passed | 0 failed", "90 passed | 1 failed"),
                    body + "\n[doctest] test cases: 6 | 6 passed | 0 failed"):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_job_start_native(bad, command)

    def test_bad_source_executable_and_missing_duplicate_command_reject(self):
        command = ["/build/real/lubancore_sdk_tests", "--source-file=*test_tool_job_start_transaction.cpp"]
        for bad in (["/build/real/other", command[1]], [command[0], "--source-file=*test_tool_job.cpp"], [], command + ["extra"]):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_job_start_native(self.body(command), bad)
        body = self.body(command)
        line = body.splitlines()[0]
        for bad in (body.replace(line, ""), body + "\n" + line):
            with self.assertRaises(RuntimeError): focused.check_job_start_native(bad, command)

    def test_registration_failure_preserves_raw_non_utf8_before_throwing(self):
        import subprocess
        import sys
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            result = subprocess.CompletedProcess(["ctest"], 17, b"raw\xffstdout", b"raw\xffstderr")
            with patch.object(focused.subprocess, "run", return_value=result), \
                    patch.object(sys, "argv", ["check", "--build-dir", str(build)]), \
                    self.assertRaises(subprocess.CalledProcessError):
                focused.main()
            evidence = build / "test-evidence" / "sdk-focused"
            self.assertEqual((evidence / "registration.stdout").read_bytes(), b"raw\xffstdout")
            self.assertEqual((evidence / "registration.stderr").read_bytes(), b"raw\xffstderr")
            self.assertIn('"returncode": 17', (evidence / "registration-result.json").read_text())


class JobHoldGateTests(unittest.TestCase):
    def body(self, command):
        return "\n".join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 91 | 91 passed | 0 failed',
            *('[job-hold-path] ' + path for path in focused.JOB_HOLD_PATHS)))

    def test_sdk_cli_full_argv_actual_six_paths(self):
        for executable in ('/build/real/lubancore_sdk_tests', 'C:/build real/lubancode_tests.exe'):
            command = [executable, '--source-file=*test_tool_job_hold_recovery.cpp']
            focused.check_job_hold_native(self.body(command), command)

    def test_foreign_same_basename_extra_filter_and_old_source_reject(self):
        command = ['/build/real/lubancore_sdk_tests', '--source-file=*test_tool_job_hold_recovery.cpp']
        for bad in ([command[0].replace('/real/', '/foreign/'), command[1]],
                    command + ['--test-case=one'], [command[0], '--source-file=*test_tool_job_coordinator.cpp']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_job_hold_native(self.body(bad), command)

    def test_six_paths_counts_and_nonempty_assertions_are_required(self):
        command = ['/build/real/lubancore_sdk_tests', '--source-file=*test_tool_job_hold_recovery.cpp']
        body = self.body(command)
        bad_bodies = [body.replace('6 | 6 passed', '0 | 0 passed'),
                      body.replace('6 | 6 passed', '5 | 5 passed'),
                      body.replace('6 passed | 0 failed', '5 passed | 1 failed'),
                      body.replace('91 | 91 passed', '0 | 0 passed'),
                      body.replace('91 passed | 0 failed', '90 passed | 1 failed')]
        for path in focused.JOB_HOLD_PATHS:
            marker = '[job-hold-path] ' + path
            bad_bodies += [body.replace(marker, ''), body + '\n' + marker,
                           body.replace(marker, 'foreign: ' + marker)]
        for bad in bad_bodies:
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_job_hold_native(bad, command)


class OwnedJobAdmissionGateTests(unittest.TestCase):
    def body(self, command):
        return "\n".join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[owned-job-path] ' + path for path in focused.OWNED_JOB_PATHS)))

    def test_sdk_cli_six_actual_paths_with_full_argv(self):
        for executable in ('/build real/lubancore_sdk_tests', 'C:/build real/lubancode_tests.exe'):
            command = [executable, '--source-file=*test_owned_job_admission.cpp']
            focused.check_owned_job_native(self.body(command), command)

    def test_foreign_same_basename_extra_filter_and_other_source_reject(self):
        command = ['/real/lubancore_sdk_tests', '--source-file=*test_owned_job_admission.cpp']
        for bad in (['/foreign/lubancore_sdk_tests', command[1]], command + ['--test-case=one'],
                    [command[0], '--source-file=*test_tool_job_coordinator.cpp']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_owned_job_native(self.body(bad), command)

    def test_malformed_native_argv_rejects_explicitly(self):
        command = ['/real/lubancore_sdk_tests', '--source-file=*test_owned_job_admission.cpp']
        body = self.body(command)
        for bad in (None, {}, [], command[:1], [7, command[1]], [command[0], 7], tuple(command)):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_owned_job_native(body, bad)

    def test_missing_duplicate_empty_or_failed_evidence_reject(self):
        command = ['/real/lubancore_sdk_tests', '--source-file=*test_owned_job_admission.cpp']
        body = self.body(command)
        bad_bodies = [body.replace('6 | 6 passed', '0 | 0 passed'),
                      body.replace('6 | 6 passed', '5 | 5 passed'),
                      body.replace('6 passed | 0 failed', '5 passed | 1 failed'),
                      body.replace('100 | 100 passed', '0 | 0 passed'),
                      body.replace('100 passed | 0 failed', '99 passed | 1 failed'),
                      body.replace('Test Passed.', ''), body + '\nTest Passed.']
        for path in focused.OWNED_JOB_PATHS:
            marker = '[owned-job-path] ' + path
            bad_bodies += [body.replace(marker, ''), body + '\n' + marker,
                           body.replace(marker, 'other-owner: ' + marker)]
        for bad in bad_bodies:
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_owned_job_native(bad, command)


class PreparedJobGateTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_tool_job_owned_registration.cpp']

    def body(self, command=None):
        command = self.command if command is None else command
        return "\n".join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[job-owned-registration-path] ' + path for path in focused.PREPARED_JOB_PATHS)))

    def test_actual_sdk_cli_full_argv_and_crlf(self):
        for executable in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancode_tests.exe'):
            command = [executable, self.command[1]]
            focused.check_prepared_job_registration(command, executable.replace('\\', '/').split('/')[-1].removesuffix('.exe'))
            for ending in ('\n', '\r\n'):
                focused.check_prepared_job_native(self.body(command).replace('\n', ending), command)

    def test_missing_nonlist_and_nonstring_argv_reject_stably(self):
        for bad in (None, {}, [], self.command[:1], tuple(self.command), [7, self.command[1]], [self.command[0], None]):
            with self.subTest(bad=bad):
                with self.assertRaises(RuntimeError):
                    focused.check_prepared_job_registration(bad)
                with self.assertRaises(RuntimeError):
                    focused.check_prepared_job_native(self.body(), bad)

    def test_foreign_same_basename_full_path_and_other_filter_reject(self):
        for bad in (['/foreign build/lubancore_sdk_tests', self.command[1]],
                    self.command + ['--test-case=one'],
                    [self.command[0], '--source-file=*test_tool_job_coordinator.cpp'],
                    [self.command[0], '--source-file=*test_tool_job_owned_registration_extra.cpp']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_prepared_job_native(self.body(bad), self.command)

    def test_registration_source_and_executable_are_exact(self):
        for bad in ([self.command[0], '--source-file=*test_tool_job_owned_registration_extra.cpp'],
                    ['/real build/unrelated_tests', self.command[1]], self.command + ['--test-case=one']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_prepared_job_registration(bad)

    def test_each_actual_path_must_finish_once(self):
        body = self.body()
        for path in focused.PREPARED_JOB_PATHS:
            marker = '[job-owned-registration-path] ' + path
            for bad in (body.replace(marker, ''), body + '\n' + marker,
                        body.replace(marker, 'foreign-owner: ' + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_prepared_job_native(bad, self.command)

    def test_nonzero_six_cases_assertions_and_pass_receipt_are_required(self):
        body = self.body()
        for bad in (body.replace('6 | 6 passed', '0 | 0 passed'),
                    body.replace('6 | 6 passed', '5 | 5 passed'),
                    body.replace('6 | 6 passed', '7 | 7 passed'),
                    body.replace('6 passed | 0 failed', '5 passed | 1 failed'),
                    body.replace('100 | 100 passed', '0 | 0 passed'),
                    body.replace('100 passed | 0 failed', '99 passed | 1 failed'),
                    body.replace('Test Passed.', ''), body + '\nTest Passed.'):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_prepared_job_native(bad, self.command)

    def test_missing_duplicate_and_bad_quote_command_reject(self):
        body = self.body()
        first = body.splitlines()[0]
        for bad in (body.replace(first, ''), body + '\n' + first,
                    body.replace(first, 'Command: "unterminated')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_prepared_job_native(bad, self.command)


class OwnedJobAdoptionGateTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_tool_job_owned_adoption.cpp']

    def body(self, command=None):
        command = self.command if command is None else command
        return "\n".join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[job-owned-adoption-path] ' + path for path in focused.JOB_ADOPTION_PATHS)))

    def test_actual_sdk_cli_full_argv_and_crlf(self):
        for executable in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancode_tests.exe'):
            command = [executable, self.command[1]]
            focused.check_job_adoption_registration(command, executable.replace('\\', '/').split('/')[-1].removesuffix('.exe'))
            for ending in ('\n', '\r\n'):
                focused.check_job_adoption_native(self.body(command).replace('\n', ending), command)

    def test_missing_nonlist_and_nonstring_argv_reject_stably(self):
        for bad in (None, {}, [], self.command[:1], tuple(self.command), [7, self.command[1]], [self.command[0], None]):
            with self.subTest(bad=bad):
                with self.assertRaises(RuntimeError):
                    focused.check_job_adoption_registration(bad)
                with self.assertRaises(RuntimeError):
                    focused.check_job_adoption_native(self.body(), bad)

    def test_foreign_same_basename_full_path_and_other_filter_reject(self):
        for bad in (['/foreign build/lubancore_sdk_tests', self.command[1]],
                    self.command + ['--test-case=one'],
                    [self.command[0], '--source-file=*test_tool_job_coordinator.cpp'],
                    [self.command[0], '--source-file=*test_tool_job_owned_registration_extra.cpp']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_job_adoption_native(self.body(bad), self.command)

    def test_registration_source_and_executable_are_exact(self):
        for bad in ([self.command[0], '--source-file=*test_tool_job_owned_registration_extra.cpp'],
                    ['/real build/unrelated_tests', self.command[1]], self.command + ['--test-case=one']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_job_adoption_registration(bad)

    def test_each_actual_path_must_finish_once(self):
        body = self.body()
        for path in focused.JOB_ADOPTION_PATHS:
            marker = '[job-owned-adoption-path] ' + path
            for bad in (body.replace(marker, ''), body + '\n' + marker,
                        body.replace(marker, 'foreign-owner: ' + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_job_adoption_native(bad, self.command)

    def test_nonzero_six_cases_assertions_and_pass_receipt_are_required(self):
        body = self.body()
        for bad in (body.replace('6 | 6 passed', '0 | 0 passed'),
                    body.replace('6 | 6 passed', '5 | 5 passed'),
                    body.replace('6 | 6 passed', '7 | 7 passed'),
                    body.replace('6 passed | 0 failed', '5 passed | 1 failed'),
                    body.replace('100 | 100 passed', '0 | 0 passed'),
                    body.replace('100 passed | 0 failed', '99 passed | 1 failed'),
                    body.replace('Test Passed.', ''), body + '\nTest Passed.'):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_job_adoption_native(bad, self.command)

    def test_missing_duplicate_and_bad_quote_command_reject(self):
        body = self.body()
        first = body.splitlines()[0]
        for bad in (body.replace(first, ''), body + '\n' + first,
                    body.replace(first, 'Command: "unterminated')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_job_adoption_native(bad, self.command)


class MiddlewareNativeReceiptGateTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_middleware_native_receipts.cpp']

    def body(self, command=None):
        command = self.command if command is None else command
        return "\n".join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[middleware-native-receipts-path] ' + path for path in focused.MIDDLEWARE_RECEIPT_PATHS)))

    def test_actual_sdk_cli_full_argv_and_crlf(self):
        for executable in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancode_tests.exe'):
            command = [executable, self.command[1]]
            focused.check_middleware_receipt_registration(command, executable.replace('\\', '/').split('/')[-1].removesuffix('.exe'))
            for ending in ('\n', '\r\n'):
                focused.check_middleware_receipt_native(self.body(command).replace('\n', ending), command)

    def test_missing_nonlist_and_nonstring_argv_reject_stably(self):
        for bad in (None, {}, [], self.command[:1], tuple(self.command), [7, self.command[1]], [self.command[0], None]):
            with self.subTest(bad=bad):
                with self.assertRaises(RuntimeError):
                    focused.check_middleware_receipt_registration(bad)
                with self.assertRaises(RuntimeError):
                    focused.check_middleware_receipt_native(self.body(), bad)

    def test_foreign_same_basename_full_path_and_other_filter_reject(self):
        for bad in (['/foreign build/lubancore_sdk_tests', self.command[1]],
                    self.command + ['--test-case=one'],
                    [self.command[0], '--source-file=*test_tool_job_coordinator.cpp'],
                    [self.command[0], '--source-file=*test_middleware_native_receipts_extra.cpp']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_middleware_receipt_native(self.body(bad), self.command)

    def test_registration_source_and_executable_are_exact(self):
        for bad in ([self.command[0], '--source-file=*test_middleware_native_receipts_extra.cpp'],
                    ['/real build/unrelated_tests', self.command[1]], self.command + ['--test-case=one']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_middleware_receipt_registration(bad)

    def test_each_actual_path_must_finish_once(self):
        body = self.body()
        for path in focused.MIDDLEWARE_RECEIPT_PATHS:
            marker = '[middleware-native-receipts-path] ' + path
            for bad in (body.replace(marker, ''), body + '\n' + marker,
                        body.replace(marker, 'foreign-owner: ' + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_middleware_receipt_native(bad, self.command)

    def test_nonzero_six_cases_assertions_and_pass_receipt_are_required(self):
        body = self.body()
        for bad in (body.replace('6 | 6 passed', '0 | 0 passed'),
                    body.replace('6 | 6 passed', '5 | 5 passed'),
                    body.replace('6 | 6 passed', '7 | 7 passed'),
                    body.replace('6 passed | 0 failed', '5 passed | 1 failed'),
                    body.replace('100 | 100 passed', '0 | 0 passed'),
                    body.replace('100 passed | 0 failed', '99 passed | 1 failed'),
                    body.replace('Test Passed.', ''), body + '\nTest Passed.'):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_middleware_receipt_native(bad, self.command)

    def test_missing_duplicate_and_bad_quote_command_reject(self):
        body = self.body()
        first = body.splitlines()[0]
        for bad in (body.replace(first, ''), body + '\n' + first,
                    body.replace(first, 'Command: "unterminated')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_middleware_receipt_native(bad, self.command)


class MiddlewareDispatchCauseGateTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_middleware_dispatch_cause.cpp']

    def body(self, command=None):
        command = self.command if command is None else command
        return "\n".join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[middleware-dispatch-cause-path] ' + path for path in focused.MIDDLEWARE_CAUSE_PATHS)))

    def test_actual_sdk_cli_full_argv_and_crlf(self):
        for executable in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancode_tests.exe'):
            command = [executable, self.command[1]]
            focused.check_middleware_cause_registration(command, executable.replace('\\', '/').split('/')[-1].removesuffix('.exe'))
            for ending in ('\n', '\r\n'):
                focused.check_middleware_cause_native(self.body(command).replace('\n', ending), command)

    def test_missing_nonlist_and_nonstring_argv_reject_stably(self):
        for bad in (None, {}, [], self.command[:1], tuple(self.command), [7, self.command[1]], [self.command[0], None]):
            with self.subTest(bad=bad):
                with self.assertRaises(RuntimeError):
                    focused.check_middleware_cause_registration(bad)
                with self.assertRaises(RuntimeError):
                    focused.check_middleware_cause_native(self.body(), bad)

    def test_foreign_same_basename_full_path_and_other_filter_reject(self):
        for bad in (['/foreign build/lubancore_sdk_tests', self.command[1]],
                    self.command + ['--test-case=one'],
                    [self.command[0], '--source-file=*test_tool_job_coordinator.cpp'],
                    [self.command[0], '--source-file=*test_middleware_dispatch_cause_extra.cpp']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_middleware_cause_native(self.body(bad), self.command)

    def test_registration_source_and_executable_are_exact(self):
        for bad in ([self.command[0], '--source-file=*test_middleware_dispatch_cause_extra.cpp'],
                    ['/real build/unrelated_tests', self.command[1]], self.command + ['--test-case=one']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_middleware_cause_registration(bad)

    def test_each_actual_path_must_finish_once(self):
        body = self.body()
        for path in focused.MIDDLEWARE_CAUSE_PATHS:
            marker = '[middleware-dispatch-cause-path] ' + path
            for bad in (body.replace(marker, ''), body + '\n' + marker,
                        body.replace(marker, 'foreign-owner: ' + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_middleware_cause_native(bad, self.command)

    def test_nonzero_six_cases_assertions_and_pass_receipt_are_required(self):
        body = self.body()
        for bad in (body.replace('6 | 6 passed', '0 | 0 passed'),
                    body.replace('6 | 6 passed', '5 | 5 passed'),
                    body.replace('6 | 6 passed', '7 | 7 passed'),
                    body.replace('6 passed | 0 failed', '5 passed | 1 failed'),
                    body.replace('100 | 100 passed', '0 | 0 passed'),
                    body.replace('100 passed | 0 failed', '99 passed | 1 failed'),
                    body.replace('Test Passed.', ''), body + '\nTest Passed.'):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_middleware_cause_native(bad, self.command)

    def test_missing_duplicate_and_bad_quote_command_reject(self):
        body = self.body()
        first = body.splitlines()[0]
        for bad in (body.replace(first, ''), body + '\n' + first,
                    body.replace(first, 'Command: "unterminated')):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                focused.check_middleware_cause_native(bad, self.command)


class ResultStoreWindowsPathsTests(unittest.TestCase):
    summary = "[doctest] test cases: 17 | 17 passed | 0 failed"
    markers = ("[result-store-path] target-extended", "[result-store-path] temporary-threshold",
               "[result-store-path-length] target-extended target=340 temporary=344",
               "[result-store-path-length] temporary-threshold target=247 temporary=251")
    owners = ('[result-store-fixture] {"marker":"target-extended","root":"C:/temp/owner-target","cleanup":"removed"}',
              '[result-store-fixture] {"marker":"temporary-threshold","root":"C:/temp/owner-temp","cleanup":"removed"}')

    def test_actual_windows_pair_and_original_posix_roster(self):
        focused.check_result_store_native("\n".join((self.summary, *self.markers, *self.owners)), "nt")
        focused.check_result_store_native(self.summary, "posix")

    def test_missing_decorated_and_duplicate_windows_markers_reject(self):
        for marker in self.markers:
            with self.subTest(marker=marker):
                body = "\n".join((self.summary, *(value for value in self.markers if value != marker), *self.owners))
                for changed in (body, body + "\nother-source: " + marker,
                                "\n".join((self.summary, *self.markers, *self.owners, marker))):
                    with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
                        focused.check_result_store_native(changed, "nt")

    def test_wrong_real_path_length_rejects(self):
        body = "\n".join((self.summary, *self.markers, *self.owners)).replace("temporary=251", "temporary=247")
        with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
            focused.check_result_store_native(body, "nt")

    def test_empty_or_changed_native_roster_rejects(self):
        for body in ("", "[doctest] test cases: 0 | 0 passed | 0 failed",
                     self.summary.replace("17 passed | 0 failed", "16 passed | 1 failed"),
                     self.summary + "\n" + self.summary):
            with self.subTest(body=body), self.assertRaisesRegex(RuntimeError, "17 successful cases"):
                focused.check_result_store_native(body, "posix")

    def test_missing_duplicate_decorated_cleanup_records_reject(self):
        body = "\n".join((self.summary, *self.markers, *self.owners))
        for owner in self.owners:
            for bad in (body.replace(owner, ''), body + '\n' + owner,
                        body.replace(owner, 'foreign-source: ' + owner)):
                with self.subTest(bad=bad), self.assertRaisesRegex(RuntimeError, "owned cleanup"):
                    focused.check_result_store_native(bad, 'nt')

    def test_bad_cleanup_owner_or_status_reject(self):
        body = "\n".join((self.summary, *self.markers, *self.owners))
        for bad in (body.replace('"removed"', '"failed"'), body.replace('"root":"C:/temp/owner-target"', '"root":""'),
                    body.replace(self.owners[0], '[result-store-fixture] not-json'),
                    body.replace('"marker":"target-extended"', '"marker":"foreign"')):
            with self.subTest(bad=bad), self.assertRaisesRegex(RuntimeError, "owned cleanup"):
                focused.check_result_store_native(bad, 'nt')

    def test_same_normalized_root_cannot_belong_to_both_paths(self):
        body = "\n".join((self.summary, *self.markers, *self.owners)).replace('C:/temp/owner-temp', 'c:/TEMP/owner-target')
        with self.assertRaisesRegex(RuntimeError, "owned roots were reused"):
            focused.check_result_store_native(body, 'nt')


class MemoryCasWindowsPathsTests(unittest.TestCase):
    markers = ("[memory-cas-path] target-extended", "[memory-cas-path] temporary-threshold")

    def test_windows_requires_both_unique_native_markers_and_posix_requires_neither(self):
        focused.check_memory_cas_paths("\n".join(self.markers) + "\n", "nt")
        focused.check_memory_cas_paths("", "posix")

    def test_windows_rejects_missing_or_decorated_native_markers(self):
        for absent in self.markers:
            with self.subTest(absent=absent):
                body = "\n".join(marker for marker in self.markers if marker != absent)
                with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
                    focused.check_memory_cas_paths(body, "nt")
                with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
                    focused.check_memory_cas_paths(body + "\nother-source: " + absent, "nt")
        with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
            focused.check_memory_cas_paths("", "nt")

    def test_windows_rejects_each_duplicated_native_marker(self):
        body = "\n".join(self.markers) + "\n"
        for duplicate in self.markers:
            with self.subTest(duplicate=duplicate):
                with self.assertRaisesRegex(RuntimeError, "actual Windows path did not finish once"):
                    focused.check_memory_cas_paths(body + duplicate + "\n", "nt")


class RecoverySourceProofTests(unittest.TestCase):
    def section(self, stem):
        prefix = "[session-recovery-path] " if stem == "session_recovery_view" else "[sdk-recovery-path] "
        return ('Command: "native-fixture" "--source-file=*test_' + stem + '.cpp"\n' +
                "\n".join(prefix + path for path in focused.RECOVERY_PATHS[stem]) + "\n")

    def test_two_exact_sources_require_all_their_actual_unique_paths(self):
        for stem, paths in focused.RECOVERY_PATHS.items():
            focused.check_recovery_source("sdk.focused." + stem, self.section(stem), len(paths))
            domain = "unit.trajectory." if stem == "session_recovery_view" else "integration.sdk."
            focused.check_recovery_source(domain + stem, self.section(stem), len(paths))

    def test_empty_reduced_or_extra_case_rosters_are_rejected(self):
        for stem, paths in focused.RECOVERY_PATHS.items():
            for count in (0, len(paths) - 1, len(paths) + 1):
                with self.subTest(stem=stem, count=count), self.assertRaisesRegex(RuntimeError, "roster differs"):
                    focused.check_recovery_source("sdk.focused." + stem, self.section(stem), count)

    def test_wrong_borrowed_or_duplicate_source_filters_are_rejected(self):
        for stem, paths in focused.RECOVERY_PATHS.items():
            for command in ("*test_other.cpp", "*test_" + stem + ".cpp.extra"):
                body = self.section(stem).replace("*test_" + stem + ".cpp", command)
                with self.assertRaisesRegex(RuntimeError, "exact source"):
                    focused.check_recovery_source("sdk.focused." + stem, body, len(paths))
            with self.assertRaisesRegex(RuntimeError, "exact source"):
                focused.check_recovery_source("sdk.focused." + stem, self.section(stem) +
                    'Command: "--source-file=*test_' + stem + '.cpp"\n', len(paths))

    def test_missing_decorated_or_duplicate_paths_cannot_pass(self):
        for stem, paths in focused.RECOVERY_PATHS.items():
            prefix = "[session-recovery-path] " if stem == "session_recovery_view" else "[sdk-recovery-path] "
            marker = prefix + paths[0]
            for body in (self.section(stem).replace(marker + "\n", ""),
                         self.section(stem).replace(marker, "other-source: " + marker),
                         self.section(stem) + marker + "\n"):
                with self.assertRaisesRegex(RuntimeError, "did not finish once"):
                    focused.check_recovery_source("sdk.focused." + stem, body, len(paths))


class PlanRetryEvidenceTests(unittest.TestCase):
    def body(self, platform="posix", executable="lubancore_sdk_tests"):
        count = 25 if platform == "nt" else 22
        paths = focused.PLAN_RETRY_PATHS
        if platform == "nt":
            paths = (*paths, "windows-sharing-recovery")
        return "\n".join((
            f'Command: "C:/actual build/{executable}.exe" "--source-file=*test_atomic_write.cpp"',
            f"[doctest] test cases: {count} | {count} passed | 0 failed | 200 skipped",
            "[doctest] assertions: 120 | 120 passed | 0 failed |",
            *("[sdk-plan-retry] " + path for path in paths), "Test Passed."))

    def test_exact_windows_and_posix_native_and_asan_command(self):
        focused.check_plan_retry_native(self.body("nt"), "nt")
        focused.check_plan_retry_native(self.body(), "posix")
        focused.check_plan_retry_native(self.body(executable="lubancode_tests"), "posix", "lubancode_tests")

    def test_empty_old_wrong_and_failed_native_rosters_reject(self):
        for platform, old in (("nt", 19), ("posix", 16)):
            count = 25 if platform == "nt" else 22
            for bad in (0, old, count - 1, count + 1):
                body = self.body(platform).replace(f"{count} | {count} passed", f"{bad} | {bad} passed")
                with self.subTest(platform=platform, bad=bad), self.assertRaisesRegex(RuntimeError, "native roster"):
                    focused.check_plan_retry_native(body, platform)
            for body in (self.body(platform).replace(f"{count} passed | 0 failed", f"{count - 1} passed | 1 failed"),
                         self.body(platform) + f"\n[doctest] test cases: {count} | {count} passed | 0 failed"):
                with self.assertRaisesRegex(RuntimeError, "native roster"):
                    focused.check_plan_retry_native(body, platform)

    def test_missing_decorated_or_duplicate_path_cannot_borrow_success(self):
        for path in (*focused.PLAN_RETRY_PATHS, "windows-sharing-recovery"):
            marker = "[sdk-plan-retry] " + path
            for body in (self.body("nt").replace(marker, ""),
                         self.body("nt").replace(marker, "other-source: " + marker),
                         self.body("nt") + "\n" + marker):
                with self.subTest(path=path), self.assertRaisesRegex(RuntimeError, "actual path did not finish once"):
                    focused.check_plan_retry_native(body, "nt")

    def test_wrong_source_wrong_binary_and_extra_filter_reject(self):
        for command in ([], ["lubancore_sdk_tests", "--source-file=*test_other.cpp"],
                        ["other_tests", "--source-file=*test_atomic_write.cpp"],
                        ["lubancore_sdk_tests", "--source-file=*test_atomic_write.cpp", "--test-case=one"]):
            with self.subTest(command=command), self.assertRaisesRegex(RuntimeError, "single actual atomic-write source"):
                focused.check_plan_retry_registration(command)
        with self.assertRaisesRegex(RuntimeError, "single actual atomic-write source"):
            focused.check_plan_retry_native(self.body().replace("test_atomic_write.cpp", "test_other.cpp"), "posix")
        with self.assertRaisesRegex(RuntimeError, "identify one actual command"):
            focused.check_plan_retry_native(self.body() + '\nCommand: "lubancore_sdk_tests"', "posix")

    def test_empty_failed_assertions_or_missing_ctest_pass_reject(self):
        for body in (self.body().replace("120 | 120 passed", "0 | 0 passed"),
                     self.body().replace("120 passed | 0 failed", "119 passed | 1 failed"),
                     self.body().replace("Test Passed.", "Test Failed."),
                     self.body() + "\n[doctest] assertions: 120 | 120 passed | 0 failed |"):
            with self.assertRaisesRegex(RuntimeError, "assertions did not actually pass"):
                focused.check_plan_retry_native(body, "posix")

    def test_posix_cannot_claim_windows_native_handle(self):
        with self.assertRaisesRegex(RuntimeError, "cannot claim a Windows sharing probe"):
            focused.check_plan_retry_native(self.body() + "\n[sdk-plan-retry] windows-sharing-recovery", "posix")


class InstalledHeadersTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="sdk-extension-gate-data-")
        self.addCleanup(self.scratch.cleanup)
        self.repo = Path(self.scratch.name)
        self.headers = {
            "include/lubancore/packages.hpp",
            "include/lubancore/api.hpp", "include/lubancore/core.hpp",
            "include/lubancore/extensions.hpp", "include/lubancore/detail/types.hpp",
            "include/lubancore/results.hpp",
            "include/lubancore/skills.hpp",
            "include/lubancore/memory.hpp",
            "include/lubancore/subagents.hpp",
            "include/lubancore/lua.hpp",
        }
        for relative in self.headers:
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#pragma once\n", encoding="utf-8")

    def test_component_and_full_install_accept_all_recursive_public_headers(self):
        for mode in ("component", "full"):
            with self.subTest(mode=mode):
                self.assertEqual(installed.check_public_headers(self.repo, sorted(self.headers), mode),
                                 self.headers)

    def test_both_install_modes_reject_each_missing_public_header(self):
        for mode in ("component", "full"):
            for missing in sorted(self.headers):
                with self.subTest(mode=mode, missing=missing):
                    with self.assertRaisesRegex(RuntimeError, f"{mode} SDK install is missing public headers") as error:
                        installed.check_public_headers(self.repo, sorted(self.headers - {missing}), mode)
                    self.assertIn(missing, str(error.exception))

    def test_missing_extension_or_result_source_cannot_shrink_the_required_install_contract(self):
        for missing in ("include/lubancore/extensions.hpp", "include/lubancore/results.hpp", "include/lubancore/skills.hpp", "include/lubancore/memory.hpp"):
            path = self.repo / missing
            contents = path.read_text(encoding="utf-8")
            path.unlink()
            try:
                for mode in ("component", "full"):
                    with self.subTest(mode=mode, missing=missing):
                        with self.assertRaisesRegex(RuntimeError, "SDK source is missing required public headers") as error:
                            installed.check_public_headers(self.repo, sorted(self.headers - {missing}), mode)
                        self.assertIn(missing, str(error.exception))
            finally:
                path.write_text(contents, encoding="utf-8")


class InstalledSearchResourcesTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="sdk-search-resource-data-")
        self.addCleanup(self.scratch.cleanup)
        self.repo = Path(self.scratch.name) / "repo"
        self.prefix = Path(self.scratch.name) / "relocated"
        self.stage = Path(self.scratch.name) / "stage"
        self.stage.mkdir()
        for binary in ("rg", "rg.exe"):
            (self.stage / binary).write_bytes(b"data fixture, never executed")
        self.pairs = {
            "share/lubancore/libexec/rg": self.stage / "rg",
            "share/lubancore/libexec/rg.exe": self.stage / "rg.exe",
            "share/lubancore/licenses/ripgrep/LICENSE-MIT": self.repo / "third_party/ripgrep/LICENSE-MIT",
            "share/lubancore/ripgrep-manifest.json": self.repo / "third_party/ripgrep/manifest.json",
        }
        for relative, original in self.pairs.items():
            original.parent.mkdir(parents=True, exist_ok=True)
            if not original.exists():
                original.write_bytes(b"nonempty repository resource fixture")
            installed_file = self.prefix / relative
            installed_file.parent.mkdir(parents=True, exist_ok=True)
            installed_file.write_bytes(original.read_bytes())
            installed_file.chmod(0o755)

    def test_relocated_platform_resources_preserve_input_bytes(self):
        for platform, binary in (("win32", "rg.exe"), ("linux", "rg"), ("darwin", "rg")):
            with self.subTest(platform=platform):
                facts = installed.check_search_resources(self.repo, self.prefix, self.stage, platform)
                self.assertEqual(len(facts), 3)
                self.assertIn("share/lubancore/libexec/" + binary, facts)

    def test_missing_or_changed_backend_license_or_manifest_fails(self):
        for relative in self.pairs:
            platform = "win32" if relative.endswith(".exe") else "linux"
            path = self.prefix / relative
            contents = path.read_bytes()
            with self.subTest(resource=relative):
                path.unlink()
                with self.assertRaisesRegex(RuntimeError, "resource is missing"):
                    installed.check_search_resources(self.repo, self.prefix, self.stage, platform)
                path.write_bytes(contents + b"changed")
                path.chmod(0o755)
                with self.assertRaisesRegex(RuntimeError, "differs from"):
                    installed.check_search_resources(self.repo, self.prefix, self.stage, platform)
                path.write_bytes(contents)
                path.chmod(0o755)

    def test_posix_relocation_cannot_lose_executable_permission(self):
        # Pure permission-result injection also exercises this negative gate on
        # Windows; real POSIX installed bytes/mode are checked by remote CI.
        with patch.object(installed.os, "access", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "lost executable permission"):
                installed.check_search_resources(self.repo, self.prefix, self.stage, "linux")


class LuaEvidenceTests(unittest.TestCase):
    def body(self, protected=False, executable="lubancore_sdk_tests"):
        source = "test_lua_protected.cpp" if protected else "test_lubancore_lua.cpp"
        count = 6 if protected else 9
        return "\n".join((
            f'Command: "C:/actual build/{executable}.exe" "--source-file=*{source}"',
            f"[doctest] test cases: {count} | {count} passed | 0 failed | 100 skipped",
            "[doctest] assertions: 32 | 32 passed | 0 failed |",
            *("[sdk-lua-path] " + path for path in focused.LUA_PATHS if not protected),
            "Test Passed.",
        ))

    def test_actual_source_counts_and_paths(self):
        focused.check_lua_native(self.body())
        focused.check_lua_native(self.body(True), protected=True)
        focused.check_lua_native(self.body(executable="lubancode_tests"), executable="lubancode_tests")

    def test_empty_failed_wrong_source_or_borrowed_binary(self):
        body = self.body()
        for changed in (body.replace("9 | 9 passed", "0 | 0 passed"),
                        body.replace("9 | 9 passed | 0 failed", "9 | 8 passed | 1 failed"),
                        body.replace("32 | 32 passed", "0 | 0 passed"),
                        body.replace("test_lubancore_lua.cpp", "test_other.cpp"),
                        body.replace("lubancore_sdk_tests.exe", "another.exe"),
                        body.replace("Test Passed.", "Test Failed.")):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                focused.check_lua_native(changed)

    def test_missing_duplicate_or_foreign_marker_cannot_prove_a_path(self):
        body = self.body()
        for path in focused.LUA_PATHS:
            marker = "[sdk-lua-path] " + path
            for changed in (body.replace(marker, ""), body.replace(marker, marker + "\n" + marker),
                            body.replace(marker, "different-source: " + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_lua_native(changed)


if __name__ == "__main__":
    unittest.main()
