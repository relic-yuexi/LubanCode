"""Pure receipt mutations; never launches a native test or HTTP fixture."""
import unittest
import fnmatch
from pathlib import Path
import re

from scripts.ci.sdk_memory_handoff import PATHS, SOURCE, check_memory_handoff_native, check_memory_handoff_registration


def receipt(executable='/tmp/build/lubancore_sdk_tests'):
    command = [executable, SOURCE]
    section = 'Command: "' + command[0] + '" "' + command[1] + '"\n'
    section += ''.join('[memory-project-handoff-path] ' + path + '\n' for path in PATHS)
    section += '[doctest] test cases: 6 | 6 passed | 0 failed\n'
    section += '[doctest] assertions: 200 | 200 passed | 0 failed\nTest Passed.\n'
    return command, section


class MemoryHandoffGateTests(unittest.TestCase):
    def test_both_real_classifiers_cover_shared_commit_sources(self):
        workflow = (Path(__file__).resolve().parents[3] / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        patterns = re.findall(r'^\s*([^\n]*scripts/ci/sdk_memory_handoff\.py[^\n]*)\)\s*$', workflow, re.M)
        self.assertEqual(len(patterns), 2)
        for pattern in patterns:
            for source in ('src/memory/project_commit.cpp', 'src/memory/project_commit.hpp',
                           'tests/unit/memory/test_memory_project_commit_handoff.cpp',
                           'scripts/ci/sdk_memory_handoff.py', 'scripts/ci/tests/test_sdk_memory_handoff_gates.py'):
                with self.subTest(source=source):
                    self.assertTrue(any(fnmatch.fnmatchcase(source, item) for item in pattern.strip().split('|')))
            self.assertFalse(any(fnmatch.fnmatchcase('docs/readme.md', item) for item in pattern.strip().split('|')))

    def test_sdk_and_aggregate_actual_commands(self):
        for executable in ('/tmp/build/lubancore_sdk_tests', 'D:/build/lubancode_tests.exe'):
            command, section = receipt(executable)
            native = executable.rsplit('/', 1)[-1].removesuffix('.exe')
            check_memory_handoff_registration(command, native)
            self.assertEqual(check_memory_handoff_native(section, command), list(PATHS))

    def test_foreign_relative_source_or_extra_argv_reject(self):
        command, _ = receipt()
        for bad in ([command[0]], command + ['--test-case=*'], ['native', SOURCE],
                    ['/tmp/other', SOURCE], [command[0], '--source-file=*test_other.cpp']):
            with self.assertRaises(RuntimeError):
                check_memory_handoff_registration(bad)

    def test_absent_duplicate_or_foreign_actual_paths_reject(self):
        command, section = receipt()
        line = '[memory-project-handoff-path] same-directory\n'
        for bad in (section.replace(line, ''), section + line, section.replace(line, line.replace('same-directory', 'invented'))):
            with self.assertRaises(RuntimeError):
                check_memory_handoff_native(bad, command)

    def test_failed_empty_or_incomplete_native_receipts_reject(self):
        command, section = receipt()
        for bad in (section.replace('6 | 6 passed', '0 | 0 passed'),
                    section.replace('6 passed | 0 failed', '5 passed | 1 failed'),
                    section.replace('200 | 200 passed | 0 failed', '200 | 199 passed | 1 failed'),
                    section.replace('200 | 200 passed', '0 | 0 passed'),
                    section.replace('Test Passed.\n', ''), section + 'Test Passed.\n'):
            with self.assertRaises(RuntimeError):
                check_memory_handoff_native(bad, command)

    def test_actual_native_command_must_match_once(self):
        command, section = receipt()
        for bad in (section.replace('Command:', 'Skipped Command:'),
                    section.replace(command[0], '/tmp/foreign'), section + section.splitlines()[0] + '\n'):
            with self.assertRaises(RuntimeError):
                check_memory_handoff_native(bad, command)


if __name__ == '__main__':
    unittest.main()
