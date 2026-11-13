"""Pure original-selection proofs, without native subprocess calls."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("child_history_full", Path(__file__).resolve().parents[1] / "extract_child_history_full.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def native_body():
    lines = []
    for case in gate.CASE_IDS:
        prefix = "[child-history-stage] case=" + case
        lines.append(prefix + " rig=0 stage=case.enter")
        for rig in gate.CASE_RIGS[case]:
            stages = ["real-run.enter", "real-run.leave"]
            if not (case == "source-gap" and rig == 2):
                stages += ["check-history.enter", "check-history.leave"]
            for stage in (*stages, "owners.before", "owners.after"):
                lines.append(prefix + f" rig={rig} stage=" + stage)
        lines.extend(("[child-adoption-path] " + case, prefix + " rig=0 stage=case.leave"))
    return ("\n".join(lines) + "\n[doctest] test cases: 8 | 8 passed | 0 failed\n"
            + "[doctest] assertions: 80 | 80 passed | 0 failed\nTest Passed.\n")


def materialize(build, cli_timeout=300):
    tests, sections, cases = [], [], []
    for index, (name, (binary, timeout)) in enumerate(gate.REQUIRED.items()):
        if name == "unit.runtime.child_history_adoption":
            timeout = cli_timeout
        command = [f"/actual build/tests/{binary}", gate.FILTER]
        tests.append({"name": name, "command": command, "properties": [{"name": "TIMEOUT", "value": timeout}]})
        sections.append(f'{index + 1}/3 Testing: {name}\nCommand: "{command[0]}" "{command[1]}"\n' + native_body())
        cases.append(f'<testcase name="{name}" status="run"/>')
    native = "".join(sections) + "3/3 Testing: unit.foreign.book\nforeign-output-must-not-upload\n"
    (build / "Testing/Temporary").mkdir(parents=True)
    (build / "Testing/Temporary/LastTest.log").write_text(native, encoding="utf-8")
    (build / "child-history-full-registration.json").write_text(json.dumps({"tests": tests}), encoding="utf-8")
    (build / "result-store-full-results.xml").write_text("<testsuite>" + "".join(cases) + '<testcase name="unit.foreign.book" status="run"/></testsuite>', encoding="utf-8")
    return native


class ChildHistoryFullGateTests(unittest.TestCase):
    def test_two_actual_sources_keep_their_original_commands_and_no_foreign_output(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            materialize(build)
            report = gate.extract(build, "nt")
            self.assertEqual(report["status"], "passed")
            self.assertEqual(len(report["details"]), 2)
            self.assertTrue(all(detail["nativeCases"] == 8 and detail["nativeAssertions"] > 0 for detail in report["details"].values()))
            output = build / "test-evidence/child-history-full"
            self.assertNotIn("foreign-output", (output / "LastTest.log").read_text())
            self.assertNotIn("unit.foreign.book", (output / "results.xml").read_text())

    def test_posix_uses_same_actual_roster(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            materialize(build, cli_timeout=180)
            self.assertEqual(gate.extract(build, "posix")["status"], "passed")

    def test_posix_cannot_borrow_windows_cli_budget(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            materialize(build, cli_timeout=300)
            with self.assertRaisesRegex(RuntimeError, "changed its timeout"):
                gate.extract(build, "posix")

    def test_sdk_budget_remains_300_on_both_platforms(self):
        for platform, cli_timeout in (("nt", 300), ("posix", 180)):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                materialize(build, cli_timeout=cli_timeout)
                path = build / "child-history-full-registration.json"
                data = json.loads(path.read_text())
                sdk = next(test for test in data["tests"] if test["name"] == "sdk.focused.child_history_adoption")
                sdk["properties"][0]["value"] = 180
                path.write_text(json.dumps(data), encoding="utf-8")
                with self.assertRaisesRegex(RuntimeError, "changed its timeout"):
                    gate.extract(build, platform)

    def test_source_gap_peer_with_no_adoption_check_is_the_real_roster(self):
        body = native_body()
        report = gate.check_native(body)
        self.assertEqual(report["stages"]["source-gap"]["rigOrdinals"], [1, 2])
        self.assertNotIn("case=source-gap rig=2 stage=check-history", body)

    def test_missing_main_check_extra_or_missing_owner_reject(self):
        body = native_body()
        prefix = "[child-history-stage] case=source-gap rig="
        variants = (body.replace(prefix + "1 stage=check-history.enter\n", "").replace(prefix + "1 stage=check-history.leave\n", ""),
                    "\n".join(line for line in body.splitlines() if not line.startswith(prefix + "2 stage=")),
                    body.replace(prefix + "2 stage=", prefix + "3 stage="))
        for variant in variants:
            with self.assertRaises(RuntimeError):
                gate.check_native(variant)

    def test_equal_enter_leave_counts_do_not_accept_reverse_or_mismatched_order(self):
        body = native_body()
        prefix = "[child-history-stage] case=complete rig=1 stage="
        for operation in ("real-run", "check-history"):
            old = prefix + operation + ".enter\n" + prefix + operation + ".leave\n"
            new = prefix + operation + ".leave\n" + prefix + operation + ".enter\n"
            with self.assertRaisesRegex(RuntimeError, "leave precedes"):
                gate.check_native(body.replace(old, new))
        body = body.replace(prefix + "owners.before\n", prefix + "close.leave\n" + prefix + "close.enter\n" + prefix + "owners.before\n")
        with self.assertRaisesRegex(RuntimeError, "leave precedes"):
            gate.check_native(body)

    def test_missing_input_still_saves_native_original(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            materialize(build)
            original_size = (build / "Testing/Temporary/LastTest.log").stat().st_size
            (build / "result-store-full-results.xml").unlink()
            with self.assertRaisesRegex(RuntimeError, "input is missing"):
                gate.extract(build, "nt")
            output = build / "test-evidence/child-history-full"
            self.assertIn("stage=owners.after", (output / "LastTest.log").read_text())
            report = json.loads((output / "context.json").read_text())
            self.assertEqual(report["inputs"]["junit"]["state"], "missing")
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["inputs"]["native"]["bytes"], original_size)

    def test_timeout_retains_actual_reached_stage_and_junit_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            original = materialize(build)
            unit = "unit.runtime.child_history_adoption"
            start = original.index("2/3 Testing: " + unit)
            end = original.index("3/3 Testing:")
            reached = ('2/3 Testing: ' + unit + '\nCommand: "/actual build/tests/lubancode_tests" "' + gate.FILTER + '"\n'
                       + '[child-history-stage] case=complete rig=0 stage=case.enter\n'
                       + '[child-history-stage] case=complete rig=1 stage=real-run.enter\nTest Failed.\n')
            (build / "Testing/Temporary/LastTest.log").write_text(original[:start] + reached + original[end:], encoding="utf-8")
            junit = build / "result-store-full-results.xml"
            junit.write_text(junit.read_text().replace(f'<testcase name="{unit}" status="run"/>',
                f'<testcase name="{unit}" status="run"><failure message="Timeout"/></testcase>'), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "JUnit reports failure"):
                gate.extract(build, "nt")
            output = build / "test-evidence/child-history-full"
            self.assertIn(reached, (output / "LastTest.log").read_text())
            self.assertIn('message="Timeout"', (output / "results.xml").read_text())
            unit_body = (output / "LastTest.log").read_text().split("2/3 Testing:")[1]
            self.assertNotIn("real-run.leave", unit_body)

    def test_actual_command_cannot_borrow_registration_from_another_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            original = materialize(build)
            (build / "Testing/Temporary/LastTest.log").write_text(original.replace("/actual build/tests/lubancode_tests", "/foreign/tests/lubancode_tests"), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "actual command differs"):
                gate.extract(build, "nt")

    def test_wrong_disabled_or_reduced_registration_is_rejected(self):
        for mode in ("wrong", "disabled", "budget", "missing", "duplicate"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                materialize(build)
                path = build / "child-history-full-registration.json"
                data = json.loads(path.read_text())
                if mode == "wrong":
                    data["tests"][0]["command"][1] = "--source-file=*another.cpp"
                elif mode == "disabled":
                    data["tests"][0]["properties"].append({"name": "DISABLED", "value": True})
                elif mode == "budget":
                    data["tests"][1]["properties"][0]["value"] = 180
                elif mode == "missing":
                    data["tests"].pop()
                else:
                    data["tests"].append(data["tests"][0])
                path.write_text(json.dumps(data), encoding="utf-8")
                with self.assertRaises(RuntimeError):
                    gate.extract(build, "nt")

    def test_empty_roster_or_assertions_missing_success_marker_and_skip_reject(self):
        body = native_body()
        for old, new in (("8 | 8 passed | 0 failed", "0 | 0 passed | 0 failed"),
                         ("80 | 80 passed | 0 failed", "0 | 0 passed | 0 failed"),
                         ("[child-adoption-path] complete\n", ""),
                         ("Test Passed.\n", "SKIP: elsewhere\nTest Passed.\n")):
            with self.subTest(old=old), self.assertRaises(RuntimeError):
                gate.check_native(body.replace(old, new))

    def test_owner_retirement_missing_reversed_or_work_after_retirement_reject(self):
        body = native_body()
        prefix = "[child-history-stage] case=complete rig=1 stage="
        variants = (body.replace(prefix + "owners.after\n", ""),
                    body.replace(prefix + "owners.before\n" + prefix + "owners.after\n", prefix + "owners.after\n" + prefix + "owners.before\n"),
                    body.replace(prefix + "owners.after\n", prefix + "owners.after\n" + prefix + "close.enter\n" + prefix + "close.leave\n"))
        for variant in variants:
            with self.assertRaises(RuntimeError):
                gate.check_native(variant)

    def test_duplicate_stage_and_unfixed_payload_reject(self):
        body = native_body()
        stage = "[child-history-stage] case=complete rig=0 stage=case.enter\n"
        for variant in (body.replace(stage, stage * 2), body.replace("stage=real-run.enter", "stage=raw-request-secret", 1)):
            with self.assertRaises(RuntimeError):
                gate.check_native(variant)

    def test_invalid_utf8_native_bytes_are_preserved_before_decode(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            original = materialize(build).encode("utf-8").replace(b"Test Passed.", b"\xffTest Passed.", 1)
            (build / "Testing/Temporary/LastTest.log").write_bytes(original)
            with self.assertRaises(UnicodeDecodeError):
                gate.extract(build, "nt")
            self.assertIn(b"\xff", (build / "test-evidence/child-history-full/LastTest.log").read_bytes())


if __name__ == "__main__":
    unittest.main()
