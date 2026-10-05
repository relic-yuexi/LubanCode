"""Pure mutations of native publication receipts; no native execution."""
import fnmatch
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

from scripts.ci.sdk_result_immutable import PATHS, SOURCE, check_result_immutable_native, check_result_immutable_registration
from scripts.ci import check_asan_profile as asan


def receipt(executable='/tmp/build/lubancore_sdk_tests'):
    command = [executable, SOURCE]
    section = 'Command: "' + command[0] + '" "' + command[1] + '"\n'
    section += ''.join('[result-immutable-publication-path] ' + path + '\n' for path in PATHS)
    section += '[doctest] test cases: 8 | 8 passed | 0 failed\n'
    section += '[doctest] assertions: 240 | 240 passed | 0 failed\nTest Passed.\n'
    return command, section


class ImmutableResultGateTests(unittest.TestCase):
    def test_both_classifiers_cover_shared_writer_and_new_gate(self):
        root = Path(__file__).resolve().parents[3]
        workflow = (root / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        patterns = re.findall(r'^\s*([^\n]*scripts/ci/sdk_result_immutable\.py[^\n]*)\)\s*$', workflow, re.M)
        self.assertEqual(len(patterns), 2)
        for pattern in patterns:
            for source in ('src/platform/atomic_write.cpp', 'src/platform/atomic_write.hpp',
                           'src/trajectory/v3/result_store.cpp', 'src/trajectory/v3/result_store.hpp',
                           'tests/unit/trajectory_v3/test_v3_result_immutable_publication.cpp',
                           'scripts/ci/sdk_result_immutable.py', 'scripts/ci/tests/test_sdk_result_immutable_gates.py'):
                self.assertTrue(any(fnmatch.fnmatchcase(source, part) for part in pattern.strip().split('|')), source)
            self.assertFalse(any(fnmatch.fnmatchcase('docs/readme.md', part) for part in pattern.strip().split('|')))

    def test_sdk_and_aggregate_exact_native_commands(self):
        for executable in ('/tmp/build/lubancore_sdk_tests', 'D:/build/lubancode_tests.exe'):
            command, section = receipt(executable)
            check_result_immutable_registration(command, executable.rsplit('/', 1)[-1].removesuffix('.exe'))
            self.assertEqual(check_result_immutable_native(section, command), list(PATHS))

    def test_relative_foreign_source_extra_or_invalid_argv_reject(self):
        command, _ = receipt()
        for bad in (None, [], [None, SOURCE], [command[0]], command + ['--test-case=*'],
                    ['native', SOURCE], ['/tmp/foreign', SOURCE], [command[0], SOURCE.replace('.cpp', '_extra.cpp')]):
            with self.subTest(command=bad), self.assertRaises(RuntimeError):
                check_result_immutable_registration(bad)

    def test_missing_duplicate_or_foreign_path_reject(self):
        command, section = receipt()
        marker = '[result-immutable-publication-path] actual\n'
        for bad in (section.replace(marker, ''), section + marker, section.replace(marker, marker.replace('actual', 'invented'))):
            with self.assertRaises(RuntimeError): check_result_immutable_native(bad, command)

    def test_empty_failed_missing_or_duplicate_completion_reject(self):
        command, section = receipt()
        for bad in (section.replace('8 | 8 passed', '0 | 0 passed'),
                    section.replace('8 passed | 0 failed', '7 passed | 1 failed'),
                    section.replace('240 | 240 passed', '0 | 0 passed'),
                    section.replace('240 passed | 0 failed', '239 passed | 1 failed'),
                    section.replace('Test Passed.\n', ''), section + 'Test Passed.\n'):
            with self.assertRaises(RuntimeError): check_result_immutable_native(bad, command)

    def test_actual_command_must_match_registration_once(self):
        command, section = receipt()
        for bad in (section.replace('Command:', 'Skipped Command:'), section.replace(command[0], '/tmp/foreign'),
                    section + section.splitlines()[0] + '\n'):
            with self.assertRaises(RuntimeError): check_result_immutable_native(bad, command)

    def test_aggregate_registration_and_resource_locks_are_wired(self):
        root = Path(__file__).resolve().parents[3]
        cmake = (root / 'cmake/LubanCoreTests.cmake').read_text(encoding='utf-8')
        self.assertIn('"${_lubancore_tests_root}/unit/trajectory_v3/test_v3_result_immutable_publication.cpp"', cmake)
        part = cmake.split('elseif(sdk_basename STREQUAL "test_v3_result_immutable_publication.cpp")', 1)[1].split('elseif(', 1)[0]
        self.assertIn('unit.trajectory_v3.v3_result_immutable_publication', part)
        self.assertEqual(part.count('RESOURCE_LOCK "platform-atomic-write"'), 2)

    def test_asan_execution_cannot_accept_green_counts_without_publication_paths(self):
        name = 'unit.trajectory_v3.v3_result_immutable_publication'
        command, section = receipt('/tmp/build/lubancode_tests')
        manifest = {'selected': [[name], [name]], 'roster': {name: 'tests/unit/trajectory_v3/test_v3_result_immutable_publication.cpp'}}
        registration = {'tests': [{'name': name, 'command': command, 'properties': [{'name': 'TIMEOUT', 'value': 180}]}]}
        junit = ET.Element('testsuite')
        ET.SubElement(junit, 'testcase', name=name, status='run')
        log = '1/1 Testing: ' + name + '\n' + section
        self.assertEqual(asan.check_execution(manifest, [registration, registration], command[0], junit, log),
                         {'status': 'passed', 'sources': 1, 'cases': 8, 'assertions': 240})
        with self.assertRaises(RuntimeError):
            asan.check_execution(manifest, [registration, registration], command[0], junit,
                                 log.replace('[result-immutable-publication-path] actual\n', ''))


if __name__ == '__main__':
    unittest.main()
