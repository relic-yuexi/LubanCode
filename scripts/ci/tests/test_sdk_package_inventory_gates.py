"""Reject incomplete or substituted native PackageInventory evidence; no native runs."""
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'check_sdk_focused.py'
SPEC = importlib.util.spec_from_file_location('sdk_package_inventory_gate', SCRIPT)
focused = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(focused)


class PackageInventoryEvidenceTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_lubancore_package_inventory.cpp']

    def body(self, command=None):
        command = self.command if command is None else command
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 8 | 8 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[sdk-package-inventory-path] ' + path for path in focused.PACKAGE_INVENTORY_PATHS)))

    def test_actual_absolute_commands_and_line_endings(self):
        for exe in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancore_sdk_tests.exe',
                    '/real build/lubancode_tests', 'C:/real build/lubancode_tests.exe'):
            command = [exe, self.command[1]]
            executable = exe.split('/')[-1].removesuffix('.exe')
            focused.check_package_inventory_registration(command, executable)
            for ending in ('\n', '\r\n'):
                focused.check_package_inventory_native(self.body(command).replace('\n', ending), command)

    def test_relative_foreign_or_filtered_registration_is_rejected(self):
        bad = (None, {}, [], tuple(self.command), self.command[:1],
            [7, self.command[1]], [self.command[0], None],
            ['lubancore_sdk_tests', self.command[1]], ['../lubancore_sdk_tests', self.command[1]],
            ['\\real build\\lubancore_sdk_tests', self.command[1]],
            ['C:real build\\lubancore_sdk_tests', self.command[1]],
            ['/real build/another_tests', self.command[1]],
            [self.command[0], '--source-file=*test_lubancore_package_inventory_extra.cpp'],
            [self.command[0], '--source-file=*test_lubancore_session.cpp'],
            self.command + ['--test-case=one'])
        for command in bad:
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_package_inventory_registration(command)
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_package_inventory_native(self.body(), command)

    def test_actual_command_matches_the_registered_path_and_source(self):
        for command in (['/other build/lubancore_sdk_tests', self.command[1]],
                        [self.command[0], '--source-file=*test_lubancore_session.cpp'],
                        self.command + ['--test-case=one']):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_package_inventory_native(self.body(command), self.command)

    def test_each_source_path_finishes_once(self):
        for path in focused.PACKAGE_INVENTORY_PATHS:
            marker = '[sdk-package-inventory-path] ' + path
            for body in (self.body().replace(marker, ''), self.body() + '\n' + marker,
                         self.body().replace(marker, 'different-provider: ' + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_package_inventory_native(body, self.command)

    def test_exact_successful_case_roster(self):
        body = self.body()
        summary = '[doctest] test cases: 8 | 8 passed | 0 failed'
        for replacement in ('', '[doctest] test cases: 0 | 0 passed | 0 failed',
            '[doctest] test cases: 5 | 5 passed | 0 failed',
            '[doctest] test cases: 9 | 9 passed | 0 failed',
            '[doctest] test cases: 8 | 7 passed | 1 failed', summary + '\n' + summary):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_package_inventory_native(body.replace(summary, replacement), self.command)

    def test_nonzero_complete_assertions_and_completion(self):
        body = self.body()
        summary = '[doctest] assertions: 100 | 100 passed | 0 failed'
        for replacement in ('', '[doctest] assertions: 0 | 0 passed | 0 failed',
            '[doctest] assertions: 100 | 99 passed | 1 failed', summary + '\n' + summary):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_package_inventory_native(body.replace(summary, replacement), self.command)
        for replacement in ('', 'Test Failed.', 'Test Passed.\nTest Passed.'):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_package_inventory_native(body.replace('Test Passed.', replacement), self.command)


class PackageInventoryConsumerEvidenceTests(unittest.TestCase):
    command = ['/relocated build/lubancore_consumer', 'package-inventory', '/relocated state/packages']

    def body(self, command=None):
        command = self.command if command is None else command
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in command),
                         '[sdk-package-inventory-consumer] complete', 'Test Passed.'))

    def test_absolute_posix_and_windows_registered_command(self):
        commands = (self.command,
                    ['C:/relocated build/lubancore_consumer.exe', 'package-inventory', 'C:/relocated state/packages'])
        for command in commands:
            for ending in ('\n', '\r\n'):
                focused.check_package_inventory_consumer(self.body(command).replace('\n', ending), command)

    def test_malformed_relative_foreign_or_filtered_command(self):
        invalid = (None, {}, (), [], tuple(self.command), self.command[:2], self.command + ['extra'],
                   [None, *self.command[1:]], [self.command[0], 3, self.command[2]],
                   ['lubancore_consumer', *self.command[1:]],
                   ['../lubancore_consumer', *self.command[1:]],
                   ['C:relocated/lubancore_consumer.exe', *self.command[1:]],
                   ['\\relocated\\lubancore_consumer.exe', *self.command[1:]],
                   ['/relocated build/another_consumer', *self.command[1:]],
                   [self.command[0], 'packages', self.command[2]],
                   [*self.command[:2], 'relative/state'], [*self.command[:2], 'C:relative/state'],
                   [*self.command[:2], '\\relative\\state'], [*self.command[:2], ''],
                   [*self.command[:2], '/state\0tail'])
        for command in invalid:
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_package_inventory_consumer(self.body(), command)

    def test_actual_command_matches_every_registered_argument(self):
        substitutions = (['/other build/lubancore_consumer', *self.command[1:]],
                         [self.command[0], 'packages', self.command[2]],
                         [*self.command[:2], '/other state'], self.command + ['extra'])
        for command in substitutions:
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_package_inventory_consumer(self.body(command), self.command)
        body = self.body()
        for altered in (body.replace('Command: ', 'Unknown command: '), body + '\n' + body,
                        body.replace('Command: ', 'Command: "unterminated ')):
            with self.subTest(altered=altered), self.assertRaises(RuntimeError):
                focused.check_package_inventory_consumer(altered, self.command)

    def test_owned_capture_and_success_finish_once(self):
        body = self.body()
        for marker in ('[sdk-package-inventory-consumer] complete', 'Test Passed.'):
            for altered in (body.replace(marker, ''), body + '\n' + marker,
                            body.replace(marker, 'substituted: ' + marker)):
                with self.subTest(marker=marker), self.assertRaises(RuntimeError):
                    focused.check_package_inventory_consumer(altered, self.command)


if __name__ == '__main__':
    unittest.main()
