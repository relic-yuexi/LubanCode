"""Pure receipt mutations only; this suite never launches native programs."""
import unittest
import fnmatch
from pathlib import Path
import re

from scripts.ci.sdk_managed_opening import SOURCES, check_managed_opening_native, check_managed_opening_registration


def receipt(stem, executable='/tmp/build/lubancore_sdk_tests'):
    command = [executable, '--source-file=*test_' + stem + '.cpp']
    prefix = '[' + stem.replace('_', '-') + '-path] '
    count = len(SOURCES[stem])
    section = 'Command: "' + command[0] + '" "' + command[1] + '"\n'
    section += ''.join(prefix + path + '\n' for path in SOURCES[stem])
    section += f'[doctest] test cases: {count} | {count} passed | 0 failed\n'
    section += '[doctest] assertions: 200 | 200 passed | 0 failed\nTest Passed.\n'
    return command, section


class ManagedOpeningGateTests(unittest.TestCase):
    def test_large_classifier_keeps_github_inputs_outside_its_literal_script(self):
        workflow = (Path(__file__).resolve().parents[3] / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        blocks = re.findall(r'^      - name: Classify changed files\n(.*?)^      - name: Report selected coverage\n', workflow, re.M | re.S)
        self.assertEqual(len(blocks), 1)
        before, script = blocks[0].split('        run: |\n', 1)
        self.assertIn('        id: c\n', before)
        self.assertNotIn('${{', script)
        self.assertIn('          CLASSIFY_PR_BASE_SHA: ${{ github.event.pull_request.base.sha }}\n', before)
        self.assertIn('          CLASSIFY_BEFORE_SHA: ${{ github.event.before }}\n', before)
        self.assertIn('BASE="$CLASSIFY_PR_BASE_SHA"', script)
        self.assertIn('BASE="$CLASSIFY_BEFORE_SHA"', script)

    def test_both_real_classifiers_cover_shared_owner_sources(self):
        workflow = (Path(__file__).resolve().parents[3] / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        patterns = re.findall(r'^\s*([^\n]*scripts/ci/sdk_managed_opening\.py[^\n]*)\)\s*$', workflow, re.M)
        self.assertEqual(len(patterns), 2)
        sources = ['src/trajectory/managed_session_ownership.cpp', 'src/trajectory/managed_session_ownership.hpp',
                   'src/trajectory/managed_session_reservation.cpp', 'src/trajectory/managed_session_reservation.hpp',
                   'tests/unit/trajectory/test_managed_session_ownership.cpp', 'tests/unit/trajectory/test_managed_session_reservation.cpp',
                   'scripts/ci/sdk_managed_opening.py', 'scripts/ci/tests/test_sdk_managed_opening_gates.py']
        for pattern in patterns:
            for source in sources:
                with self.subTest(source=source):
                    self.assertTrue(any(fnmatch.fnmatchcase(source, item) for item in pattern.strip().split('|')))
            self.assertFalse(any(fnmatch.fnmatchcase('docs/readme.md', item) for item in pattern.strip().split('|')))

    def test_both_actual_native_sources_and_platform_executables(self):
        for stem in SOURCES:
            for executable in ('/tmp/build/lubancore_sdk_tests', 'D:/build/lubancode_tests.exe'):
                command, section = receipt(stem, executable)
                native = executable.rsplit('/', 1)[-1].removesuffix('.exe')
                check_managed_opening_registration(command, stem, native)
                self.assertEqual(check_managed_opening_native(section, command, stem), list(SOURCES[stem]))

    def test_registration_rejects_foreign_relative_or_extra_argv(self):
        stem = 'managed_session_ownership'
        command, _ = receipt(stem)
        for bad in ([command[0]], command + ['--test-case=*'], ['native', command[1]],
                    ['/tmp/other', command[1]], [command[0], '--source-file=*test_other.cpp']):
            with self.assertRaises(RuntimeError):
                check_managed_opening_registration(bad, stem)

    def test_missing_duplicate_and_foreign_paths_reject(self):
        for stem in SOURCES:
            command, section = receipt(stem)
            line = '[' + stem.replace('_', '-') + '-path] actual\n'
            for bad in (section.replace(line, ''), section + line, section.replace(line, line.replace('actual', 'invented'))):
                with self.assertRaises(RuntimeError):
                    check_managed_opening_native(bad, command, stem)

    def test_case_and_assertion_failure_or_empty_run_reject(self):
        for stem in SOURCES:
            command, section = receipt(stem)
            count = len(SOURCES[stem])
            for bad in (section.replace(f'{count} | {count} passed', '0 | 0 passed'),
                        section.replace(f'{count} passed | 0 failed', f'{count-1} passed | 1 failed'),
                        section.replace('200 | 200 passed | 0 failed', '200 | 199 passed | 1 failed'),
                        section.replace('200 | 200 passed', '0 | 0 passed')):
                with self.assertRaises(RuntimeError):
                    check_managed_opening_native(bad, command, stem)

    def test_completion_and_actual_command_must_be_single_and_match(self):
        for stem in SOURCES:
            command, section = receipt(stem)
            for bad in (section.replace('Test Passed.\n', ''), section + 'Test Passed.\n',
                        section.replace('Command:', 'Skipped Command:'),
                        section.replace(command[0], '/tmp/foreign'),
                        section + section.splitlines()[0] + '\n'):
                with self.assertRaises(RuntimeError):
                    check_managed_opening_native(bad, command, stem)


if __name__ == '__main__':
    unittest.main()
