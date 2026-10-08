"""Reject substituted or partial startup evidence; run on CI, no native execution."""
import unittest

from scripts.ci.check_sdk_focused import check_opening_start_native


class OpeningStartEvidenceTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_lubancore_lifecycle.cpp']
    paths = ('standard', 'nonstandard', 'repeated', 'isolation', 'shutdown')

    def body(self, command=None):
        command = self.command if command is None else command
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in command),
                          '[doctest] test cases: 12 | 12 passed | 0 failed',
                          '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
                          *('[sdk-opening-start-path] ' + path for path in self.paths)))

    def test_native_platform_commands(self):
        for executable in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancore_sdk_tests.exe',
                           '/real build/lubancode_tests'):
            command = [executable, self.command[1]]
            for ending in ('\n', '\r\n'):
                check_opening_start_native(self.body(command).replace('\n', ending), command)

    def test_each_actual_path_must_finish_once(self):
        for path in self.paths:
            marker = '[sdk-opening-start-path] ' + path
            for body in (self.body().replace(marker, ''), self.body() + '\n' + marker):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    check_opening_start_native(body, self.command)

    def test_original_cases_and_assertions_cannot_be_skipped(self):
        for old, new in (('12 | 12 passed', '8 | 8 passed'),
                         ('100 | 100 passed | 0 failed', '100 | 99 passed | 1 failed'),
                         ('Test Passed.', 'Test Failed.')):
            with self.assertRaises(RuntimeError):
                check_opening_start_native(self.body().replace(old, new), self.command)

    def test_foreign_relative_and_filtered_commands(self):
        for command in (['lubancore_sdk_tests', self.command[1]],
                        ['/real build/foreign_tests', self.command[1]],
                        [self.command[0], '--source-file=*test_other.cpp'],
                        self.command + ['--test-case=*startup*']):
            with self.assertRaises(RuntimeError):
                check_opening_start_native(self.body(command), command)


if __name__ == '__main__':
    unittest.main()
