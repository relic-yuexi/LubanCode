"""Pure evidence validation; never invokes CMake, CTest or product binaries."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("memory_commit_gate", Path(__file__).parents[1] / "check_memory_commit.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def fixtures(count=12, project_memory_count=47, busy_markers=1):
    names = sorted(gate.REQUIRED)
    tests = [{"name": name, "properties": [{"name": "TIMEOUT", "value": 180}]} for name in names]
    results = "<testsuite>" + "".join(f'<testcase name="{name}" status="run" />' for name in names) + "</testsuite>"
    log = "".join(f'{index}/2 Testing: {name}\n[doctest] test cases: {count if "memory_project_commit" in name else project_memory_count}\n' +
                  ((gate.BUSY_MARKER + "\n") * busy_markers if name == "unit.memory.project_memory" else "")
                  for index, name in enumerate(names, 1))
    return tests, results, log


class MemoryCommitGateTests(unittest.TestCase):
    def test_valid_original_evidence(self):
        tests, results, log = fixtures()
        gate.validate_registration(tests)
        self.assertEqual(gate.validate_results(results, log)["unit.memory.memory_project_commit"], 12)
        self.assertEqual(gate.validate_results(results, log)["unit.memory.project_memory"], 47)

    def test_missing_and_disabled_registration(self):
        tests, _, _ = fixtures()
        with self.assertRaises(RuntimeError):
            gate.validate_registration(tests[:1])
        tests[0]["properties"].append({"name": "DISABLED", "value": True})
        with self.assertRaises(RuntimeError):
            gate.validate_registration(tests)

    def test_skipped_or_failed_junit(self):
        _, results, log = fixtures()
        for tag in ("skipped", "failure", "error"):
            bad = results.replace('status="run" />', f'status="run"><{tag}/></testcase>', 1)
            with self.assertRaises(RuntimeError):
                gate.validate_results(bad, log)

    def test_wrong_fixed_native_roster(self):
        for count in (0, 11, 13):
            _, results, log = fixtures(count)
            with self.assertRaises(RuntimeError):
                gate.validate_results(results, log)

    def test_missing_or_duplicated_native_sections(self):
        _, results, log = fixtures()
        for bad in ("", log + log):
            with self.assertRaises(RuntimeError):
                gate.validate_results(results, bad)

    def test_wrong_project_memory_roster(self):
        for count in (0, 46, 48):
            with self.subTest(count=count):
                _, results, log = fixtures(project_memory_count=count)
                with self.assertRaises(RuntimeError):
                    gate.validate_results(results, log)

    def test_missing_or_duplicated_busy_path(self):
        for count in (0, 2):
            with self.subTest(count=count):
                _, results, log = fixtures(busy_markers=count)
                with self.assertRaises(RuntimeError):
                    gate.validate_results(results, log)

    def test_busy_marker_must_belong_to_project_memory_source(self):
        _, results, log = fixtures(busy_markers=0)
        wrong_source = log.replace("[doctest] test cases: 12\n",
                                   "[doctest] test cases: 12\n" + gate.BUSY_MARKER + "\n")
        with self.assertRaises(RuntimeError):
            gate.validate_results(results, wrong_source)


if __name__ == "__main__":
    unittest.main()
