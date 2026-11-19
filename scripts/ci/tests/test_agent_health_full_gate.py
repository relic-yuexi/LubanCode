"""Pure full-run evidence counterexamples; no native process calls."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

CI = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("agent_health_full", CI / "extract_agent_health_full.py")
gate = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(CI), *sys.path]):
    spec.loader.exec_module(gate)


def native_body(command):
    return (f'1/2 Testing: {gate.NAME}\nCommand: "{command[0]}" "{command[1]}"\n'
            '[doctest] test cases: 5 | 5 passed | 0 failed | 120 skipped\n'
            '[doctest] assertions: 42 | 42 passed | 0 failed\nTest Passed.\n')


def materialize(build, *, platform="posix"):
    binary = "lubancode_tests.exe" if platform == "nt" else "lubancode_tests"
    command = [str(build / "tests" / binary).replace("\\", "/"), gate.FILTER]
    registration = {"tests": [{"name": gate.NAME, "command": command,
                              "properties": [{"name": "TIMEOUT", "value": 180}]}]}
    (build / "agent-health-full-registration.json").write_text(json.dumps(registration), encoding="utf-8")
    section = native_body(command)
    (build / "Testing/Temporary").mkdir(parents=True)
    (build / "Testing/Temporary/LastTest.log").write_text(
        section + '2/2 Testing: unit.foreign.source\nforeign-native-must-not-upload\n', encoding="utf-8")
    (build / "result-store-full-results.xml").write_text(
        f'<testsuite><testcase name="{gate.NAME}" status="run"/>'
        '<testcase name="unit.foreign.source" status="run"><system-out>foreign-junit-must-not-upload</system-out>'
        '</testcase></testsuite>', encoding="utf-8")
    return command, section


def registration(build):
    return json.loads((build / "agent-health-full-registration.json").read_text(encoding="utf-8"))


def write_registration(build, data):
    (build / "agent-health-full-registration.json").write_text(json.dumps(data), encoding="utf-8")


class AgentHealthFullGateTests(unittest.TestCase):
    def check_manifest(self, output, status):
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["status"], status)
        self.assertNotIn("manifest.json", manifest["files"])
        for name, facts in manifest["files"].items():
            data = (output / name).read_bytes()
            self.assertEqual(facts["bytes"], len(data))
            self.assertEqual(facts["sha256"], hashlib.sha256(data).hexdigest())
        return manifest

    def test_actual_source_argv_five_cases_and_junit_only(self):
        for platform in ("nt", "posix"):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory(prefix="health full ") as directory:
                build = Path(directory)
                command, section = materialize(build, platform=platform)
                report = gate.extract(build, platform)
                self.assertEqual(report["status"], "passed")
                self.assertEqual(report["stage"], "complete")
                self.assertEqual(report["details"][gate.NAME], {
                    "command": command, "nativeCases": 5, "nativeAssertions": 42})
                output = build / "test-evidence/agent-health-full"
                self.assertEqual((output / "LastTest.log").read_text(encoding="utf-8"), section)
                self.assertNotIn("foreign-junit", (output / "results.xml").read_text(encoding="utf-8"))
                self.assertEqual(report["sourceSha256"], hashlib.sha256(
                    (CI.parents[1] / gate.SOURCE).read_bytes()).hexdigest())
                self.check_manifest(output, "passed")

    def test_public_checks_pair_windows_paths_without_running_native(self):
        command = ["C:\\actual build\\tests\\lubancode_tests.exe", gate.FILTER]
        section = native_body(command)
        self.assertEqual(gate.check_registration(command),
                         ["C:/actual build/tests/lubancode_tests.exe", gate.FILTER])
        result = gate.check_source_native(gate.NAME, section, "nt", command)
        self.assertEqual(result["nativeCases"], 5)
        self.assertEqual(result["nativeAssertions"], 42)
        # ASan already pairs commands; its three-argument source check is the same rule.
        self.assertEqual(gate.check_source_native(gate.NAME, section, "nt"), result)

    def test_registration_requires_two_strings_absolute_binary_exact_source(self):
        valid = ["/build/tests/lubancode_tests", gate.FILTER]
        variants = (None, {}, "command", [], [None, gate.FILTER], [valid[0], 3],
                    [valid[0]], ["lubancode_tests", gate.FILTER],
                    ["/build/tests/another_tests", gate.FILTER],
                    [valid[0], "--source-file=*another.cpp"],
                    [valid[0], gate.FILTER + ",*another.cpp"],
                    [*valid, "--test-case=one"], [*valid, "--no-skip=true"],
                    [valid[0] + "\x00", gate.FILTER])
        for command in variants:
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                gate.check_registration(command)

    def test_actual_foreign_checkout_even_same_basename_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            command, section = materialize(build)
            path = build / "Testing/Temporary/LastTest.log"
            path.write_text(section.replace(command[0], "/foreign/tests/lubancode_tests"), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "differs from the registered"):
                gate.extract(build, "posix")
            self.check_manifest(build / "test-evidence/agent-health-full", "failed")

    def test_registration_and_actual_cannot_both_borrow_foreign_build(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            command, section = materialize(build)
            data = registration(build)
            data["tests"][0]["command"][0] = "/foreign/tests/lubancode_tests"
            write_registration(build, data)
            (build / "Testing/Temporary/LastTest.log").write_text(
                section.replace(command[0], data["tests"][0]["command"][0]), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "foreign build"):
                gate.extract(build, "posix")

    def test_bad_quote_missing_or_duplicate_actual_command_is_rejected(self):
        command = ["/build/tests/lubancode_tests", gate.FILTER]
        body = native_body(command)
        actual_line = f'Command: "{command[0]}" "{command[1]}"\n'
        variants = (body.replace(actual_line, 'Command: "unterminated\n'),
                    body.replace(actual_line, ""), body.replace(actual_line, actual_line * 2),
                    body.replace(actual_line, actual_line.rstrip() + ' "--no-skip=true"\n'))
        for section in variants:
            with self.subTest(section=section), self.assertRaises(RuntimeError):
                gate.check_source_native(gate.NAME, section, "posix", command)

    def test_empty_failed_reduced_or_duplicate_cases_do_not_pass(self):
        command = ["/build/tests/lubancode_tests", gate.FILTER]
        body = native_body(command)
        roster = '[doctest] test cases: 5 | 5 passed | 0 failed | 120 skipped\n'
        for new in ("", '[doctest] test cases: 0 | 0 passed | 0 failed\n',
                    '[doctest] test cases: 4 | 4 passed | 0 failed\n',
                    '[doctest] test cases: 5 | 4 passed | 1 failed\n', roster * 2):
            with self.subTest(new=new), self.assertRaisesRegex(RuntimeError, "five successful cases"):
                gate.check_source_native(gate.NAME, body.replace(roster, new), "posix", command)

    def test_assertions_require_positive_complete_single_summary(self):
        command = ["/build/tests/lubancode_tests", gate.FILTER]
        body = native_body(command)
        assertions = '[doctest] assertions: 42 | 42 passed | 0 failed\n'
        for new in ("", '[doctest] assertions: 0 | 0 passed | 0 failed\n',
                    '[doctest] assertions: 42 | 41 passed | 0 failed\n',
                    '[doctest] assertions: 42 | 41 passed | 1 failed\n', assertions * 2):
            with self.subTest(new=new), self.assertRaises(RuntimeError):
                gate.check_source_native(gate.NAME, body.replace(assertions, new), "posix", command)

    def test_native_failure_timeout_skip_or_duplicate_pass_is_not_a_pass(self):
        command = ["/build/tests/lubancode_tests", gate.FILTER]
        body = native_body(command)
        for status in ("", "Test Failed.\n", "Test Timeout.\n", "Test Passed.\nTest Failed.\n",
                       "Test Passed.\nTest Passed.\n", "SKIP: one case\nTest Passed.\n",
                       "skipped test case\nTest Passed.\n"):
            with self.subTest(status=status), self.assertRaises(RuntimeError):
                gate.check_source_native(gate.NAME, body.replace("Test Passed.\n", status), "posix", command)

    def test_success_junit_cannot_replace_failed_native_source(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            _, section = materialize(build)
            failed = section.replace("5 | 5 passed | 0 failed", "5 | 4 passed | 1 failed").replace(
                "Test Passed.", "Test Failed.")
            (build / "Testing/Temporary/LastTest.log").write_text(failed, encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "native section failed"):
                gate.extract(build, "posix")
            output = build / "test-evidence/agent-health-full"
            self.assertEqual((output / "LastTest.log").read_text(encoding="utf-8"), failed)
            self.assertIn('status="run"', (output / "results.xml").read_text(encoding="utf-8"))
            self.check_manifest(output, "failed")

    def test_junit_failure_skip_error_or_notrun_is_retained_and_rejected(self):
        for child, status in (("failure", "run"), ("error", "run"), ("skipped", "run"), (None, "notrun")):
            with self.subTest(child=child), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                materialize(build)
                detail = f'<{child} message="original failure"/>' if child else ""
                (build / "result-store-full-results.xml").write_text(
                    f'<testsuite><testcase name="{gate.NAME}" status="{status}">{detail}</testcase></testsuite>',
                    encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "JUnit reports failure or skip"):
                    gate.extract(build, "posix")
                output = build / "test-evidence/agent-health-full"
                self.assertTrue((output / "LastTest.log").is_file())
                self.assertIn(f'status="{status}"', (output / "results.xml").read_text(encoding="utf-8"))
                self.check_manifest(output, "failed")

    def test_missing_or_duplicate_native_or_junit_source_rejects_all_matches(self):
        for mode in ("native-missing", "native-duplicate", "junit-missing", "junit-duplicate"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                _, section = materialize(build)
                if mode.startswith("native"):
                    data = section * 2 if mode.endswith("duplicate") else '1/1 Testing: unit.foreign.source\n'
                    (build / "Testing/Temporary/LastTest.log").write_text(data, encoding="utf-8")
                else:
                    case = f'<testcase name="{gate.NAME}" status="run"/>'
                    data = case * 2 if mode.endswith("duplicate") else '<testcase name="unit.foreign.source" status="run"/>'
                    (build / "result-store-full-results.xml").write_text('<testsuite>' + data + '</testsuite>', encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "source is missing or duplicated"):
                    gate.extract(build, "posix")
                output = build / "test-evidence/agent-health-full"
                if mode == "native-duplicate":
                    self.assertEqual((output / "LastTest.log").read_text(encoding="utf-8"), section * 2)
                self.check_manifest(output, "failed")

    def test_registration_missing_duplicate_foreign_disabled_or_wrong_timeout_reject(self):
        for mode in ("missing", "duplicate", "foreign", "disabled", "timeout", "duplicate-timeout", "bad-shape"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                materialize(build)
                data = registration(build)
                test = data["tests"][0]
                if mode == "missing":
                    data["tests"].clear()
                elif mode == "duplicate":
                    data["tests"].append(test)
                elif mode == "foreign":
                    test["name"] = "unit.foreign.source"
                elif mode == "disabled":
                    test["properties"].append({"name": "DISABLED", "value": True})
                elif mode == "timeout":
                    test["properties"][0]["value"] = 300
                elif mode == "duplicate-timeout":
                    test["properties"].append(test["properties"][0])
                else:
                    data = [test]
                write_registration(build, data)
                with self.assertRaises(RuntimeError):
                    gate.extract(build, "posix")
                self.check_manifest(build / "test-evidence/agent-health-full", "failed")

    def test_each_missing_input_retains_remaining_scoped_originals(self):
        for name in ("agent-health-full-registration.json", "result-store-full-results.xml", "Testing/Temporary/LastTest.log"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                materialize(build)
                (build / name).unlink()
                with self.assertRaisesRegex(RuntimeError, "input is missing"):
                    gate.extract(build, "posix")
                output = build / "test-evidence/agent-health-full"
                context = json.loads((output / "context.json").read_text(encoding="utf-8"))
                self.assertEqual(context["status"], "failed")
                self.assertEqual(sum(item["state"] == "missing" for item in context["inputs"].values()), 1)
                self.assertTrue((output / "LastTest.log").is_file() or name.endswith("LastTest.log"))
                self.assertTrue((output / "results.xml").is_file() or name.endswith("results.xml"))
                self.check_manifest(output, "failed")

    def test_invalid_native_utf8_retains_original_bytes_before_decode(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            _, section = materialize(build)
            original = section.encode("utf-8").replace(b'Test Passed.', b'\xffTest Passed.')
            (build / "Testing/Temporary/LastTest.log").write_bytes(original)
            with self.assertRaises(UnicodeDecodeError):
                gate.extract(build, "posix")
            output = build / "test-evidence/agent-health-full"
            self.assertEqual((output / "LastTest.log").read_bytes(), original)
            self.check_manifest(output, "failed")

    def test_malformed_junit_keeps_unparsed_original_and_other_inputs(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            _, section = materialize(build)
            raw = b'<testsuite>\xff<unclosed'
            (build / "result-store-full-results.xml").write_bytes(raw)
            with self.assertRaisesRegex(RuntimeError, "JUnit XML is invalid"):
                gate.extract(build, "posix")
            output = build / "test-evidence/agent-health-full"
            self.assertEqual((output / "junit-unparsed.xml").read_bytes(), raw)
            self.assertEqual((output / "LastTest.log").read_text(encoding="utf-8"), section)
            self.assertEqual(json.loads((output / "context.json").read_text(encoding="utf-8"))["junitScope"],
                             "unparsed-original")
            self.check_manifest(output, "failed")

    def test_invalid_registration_retains_raw_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            materialize(build)
            raw = b'{\xffbad registration'
            (build / "agent-health-full-registration.json").write_bytes(raw)
            with self.assertRaisesRegex(RuntimeError, "registration JSON is invalid"):
                gate.extract(build, "posix")
            output = build / "test-evidence/agent-health-full"
            self.assertEqual((output / "registration.json").read_bytes(), raw)
            self.check_manifest(output, "failed")

    def test_repeated_failed_extraction_cannot_leave_a_prior_success_xml(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            materialize(build)
            gate.extract(build, "posix")
            (build / "result-store-full-results.xml").write_bytes(b'<bad xml')
            with self.assertRaises(RuntimeError):
                gate.extract(build, "posix")
            output = build / "test-evidence/agent-health-full"
            self.assertFalse((output / "results.xml").exists())
            self.check_manifest(output, "failed")


if __name__ == "__main__":
    unittest.main()
