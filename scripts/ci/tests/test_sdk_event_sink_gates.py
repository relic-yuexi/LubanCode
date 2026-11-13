"""Reject incomplete or substituted native EventSink evidence; no native runs."""
import importlib.util
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'check_sdk_focused.py'
SPEC = importlib.util.spec_from_file_location('sdk_event_sink_gate', SCRIPT)
focused = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(focused)


class EventSinkEvidenceTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_lubancore_event_sink.cpp']

    def body(self, command=None):
        command = self.command if command is None else command
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[sdk-event-sink-path] ' + path for path in focused.EVENT_SINK_PATHS)))

    def test_actual_absolute_commands_and_line_endings(self):
        for exe in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancore_sdk_tests.exe',
                    '/real build/lubancode_tests', 'C:/real build/lubancode_tests.exe'):
            command = [exe, self.command[1]]
            executable = exe.split('/')[-1].removesuffix('.exe')
            focused.check_event_sink_registration(command, executable)
            for ending in ('\n', '\r\n'):
                focused.check_event_sink_native(self.body(command).replace('\n', ending), command)

    def test_relative_foreign_or_filtered_registration_is_rejected(self):
        bad = (None, {}, [], tuple(self.command), self.command[:1],
            [7, self.command[1]], [self.command[0], None],
            ['lubancore_sdk_tests', self.command[1]], ['../lubancore_sdk_tests', self.command[1]],
            ['\\real build\\lubancore_sdk_tests', self.command[1]],
            ['C:real build\\lubancore_sdk_tests', self.command[1]],
            ['/real build/another_tests', self.command[1]],
            [self.command[0], '--source-file=*test_lubancore_event_sink_extra.cpp'],
            [self.command[0], '--source-file=*test_lubancore_session.cpp'],
            self.command + ['--test-case=one'])
        for command in bad:
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_event_sink_registration(command)
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_event_sink_native(self.body(), command)

    def test_actual_command_matches_the_registered_path_and_source(self):
        for command in (['/other build/lubancore_sdk_tests', self.command[1]],
                        [self.command[0], '--source-file=*test_lubancore_session.cpp'],
                        self.command + ['--test-case=one']):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_event_sink_native(self.body(command), self.command)

    def test_each_source_path_finishes_once(self):
        for path in focused.EVENT_SINK_PATHS:
            marker = '[sdk-event-sink-path] ' + path
            for body in (self.body().replace(marker, ''), self.body() + '\n' + marker,
                         self.body().replace(marker, 'different-provider: ' + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_event_sink_native(body, self.command)

    def test_exact_successful_case_roster(self):
        body = self.body()
        summary = '[doctest] test cases: 6 | 6 passed | 0 failed'
        for replacement in ('', '[doctest] test cases: 0 | 0 passed | 0 failed',
            '[doctest] test cases: 5 | 5 passed | 0 failed',
            '[doctest] test cases: 7 | 7 passed | 0 failed',
            '[doctest] test cases: 6 | 5 passed | 1 failed', summary + '\n' + summary):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_event_sink_native(body.replace(summary, replacement), self.command)

    def test_nonzero_complete_assertions_and_completion(self):
        body = self.body()
        summary = '[doctest] assertions: 100 | 100 passed | 0 failed'
        for replacement in ('', '[doctest] assertions: 0 | 0 passed | 0 failed',
            '[doctest] assertions: 100 | 99 passed | 1 failed', summary + '\n' + summary):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_event_sink_native(body.replace(summary, replacement), self.command)
        for replacement in ('', 'Test Failed.', 'Test Passed.\nTest Passed.'):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_event_sink_native(body.replace('Test Passed.', replacement), self.command)


class RelocatedEventSinkEvidenceTests(unittest.TestCase):
    command = ['/relocated consumer/build/lubancore_consumer', 'event-sink', '/relocated consumer/build/state-event-sink']

    def body(self, command=None):
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in (command or self.command)),
                          '[sdk-event-sink-consumer] actual-owned-queue', 'Test Passed.'))

    def test_actual_relocated_command_and_line_endings(self):
        for executable in ('/relocated consumer/build/lubancore_consumer', 'C:/relocated consumer/build/lubancore_consumer.exe'):
            command = [executable, *self.command[1:]]
            for ending in ('\n', '\r\n'):
                focused.check_event_sink_consumer(self.body(command).replace('\n', ending), command)

    def test_foreign_binary_arguments_and_registration_are_rejected(self):
        for changed in (['/foreign/build/lubancore_consumer', *self.command[1:]],
                        [self.command[0], 'smoke', self.command[2]], self.command + ['extra'],
                        self.command[:2]):
            with self.subTest(command=changed), self.assertRaises(RuntimeError):
                focused.check_event_sink_consumer(self.body(changed), self.command)
        for invalid in (None, [], ['lubancore_consumer', *self.command[1:]],
                        ['C:build/lubancore_consumer.exe', *self.command[1:]],
                        ['/real/other_consumer', *self.command[1:]]):
            with self.subTest(command=invalid), self.assertRaises(RuntimeError):
                focused.check_event_sink_consumer(self.body(), invalid)

    def test_missing_duplicate_and_decorated_success_are_rejected(self):
        for marker in ('[sdk-event-sink-consumer] actual-owned-queue', 'Test Passed.'):
            for changed in ('', marker + '\n' + marker, 'borrowed: ' + marker):
                with self.subTest(marker=marker, changed=changed), self.assertRaises(RuntimeError):
                    focused.check_event_sink_consumer(self.body().replace(marker, changed), self.command)


if __name__ == '__main__':
    unittest.main()
