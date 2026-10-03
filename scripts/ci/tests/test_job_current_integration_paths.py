"""Keep the shared process files in the actual SDK/lifetime CI branches."""

import fnmatch
from pathlib import Path
import unittest


WORKFLOW = Path(__file__).resolve().parents[3] / ".github/workflows/ci.yml"
PROCESS_PATHS = (
    "src/platform/process_posix.cpp",
    "src/platform/process_win.cpp",
    "src/platform/process_diagnostics.hpp",
)


def sdk_classification_branches(text):
    lines = text.splitlines()
    branches = []
    for index, line in enumerate(lines):
        if "include/lubancore/*" not in line or not line.strip().endswith(")"):
            continue
        patterns = line.strip()[:-1].split("|")
        body = []
        for statement in lines[index + 1:]:
            body.append(statement.strip())
            if statement.strip().endswith(";;"):
                break
        else:
            raise ValueError("SDK classification branch has no actual exit")
        branches.append((patterns, body))
    if len(branches) != 2:
        raise ValueError("SDK/lifetime classification must have its two actual branches")
    return branches


class SharedProcessClassificationTests(unittest.TestCase):
    def setUp(self):
        self.text = WORKFLOW.read_text(encoding="utf-8-sig")
        self.branches = sdk_classification_branches(self.text)

    def test_each_process_path_selects_both_actual_branches(self):
        for branch, (patterns, body) in enumerate(self.branches):
            self.assertIn("sdk_tests=$sdk_present", body)
            self.assertIn("cross_platform=true", body)
            for path in PROCESS_PATHS:
                with self.subTest(branch=branch, path=path):
                    self.assertEqual(patterns.count(path), 1)
                    self.assertTrue(any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns))
        self.assertIn("agent_lifetime=true", self.branches[0][1])

    def test_neighboring_process_paths_do_not_select_these_branches(self):
        nearby = (
            "src/platform/process_posix.hpp",
            "src/platform/process_win.hpp",
            "src/platform/process_diagnostics.cpp",
            "src/platform/process_diagnostics.hpp.extra",
            "src/platform/process_posix.cpp.extra",
            "src/platform/nested/process_win.cpp",
            "src/platform/other.cpp",
        )
        for branch, (patterns, _) in enumerate(self.branches):
            for path in nearby:
                with self.subTest(branch=branch, path=path):
                    self.assertFalse(any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns))
            self.assertNotIn("src/platform/*", patterns)
            self.assertNotIn("src/platform/process*", patterns)

    def test_windows_memory_worker_branch_is_preserved(self):
        memory_case = next(line.strip() for line in self.text.splitlines()
                           if line.strip().startswith("src/memory/*|src/platform/process_win.cpp|"))
        self.assertTrue(memory_case.endswith(")"))
        patterns = memory_case[:-1].split("|")
        self.assertIn("src/platform/process_win.cpp", patterns)
        self.assertTrue(any(fnmatch.fnmatchcase(PROCESS_PATHS[1], pattern) for pattern in patterns))
        self.assertNotIn(PROCESS_PATHS[0], patterns)
        self.assertNotIn(PROCESS_PATHS[2], patterns)
        body = self.text.split(memory_case, 1)[1].split("esac", 1)[0]
        self.assertIn("memory_worker=true ;;", body)

    def test_health_and_middleware_paths_remain_in_both_branches(self):
        for patterns, _ in self.branches:
            for path in (
                "src/runtime/middleware_deferred_effects.hpp",
                "tests/unit/runtime/test_middleware_native_receipts.cpp",
                "tests/unit/hooks/test_middleware_dispatch_cause.cpp",
                "tests/unit/runtime/test_agent_thread_lifetime.cpp",
                "scripts/ci/extract_agent_health_full.py",
                "scripts/ci/tests/test_agent_health_full_gate.py",
            ):
                self.assertIn(path, patterns)

    def test_missing_or_duplicate_sdk_branches_are_rejected(self):
        lines = self.text.splitlines()
        index = next(index for index, line in enumerate(lines)
                     if "include/lubancore/*" in line and line.strip().endswith(")"))
        removed = lines[:index] + lines[index + 1:]
        with self.assertRaises(ValueError):
            sdk_classification_branches("\n".join(removed))
        with self.assertRaises(ValueError):
            sdk_classification_branches(self.text + "\n" + lines[index] + "\n                code=true ;;\n")


if __name__ == "__main__":
    unittest.main()
