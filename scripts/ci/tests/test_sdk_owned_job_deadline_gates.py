"""Reject substituted commands, invented budgets and workers not retired."""
import json
from pathlib import Path
import re
import unittest
from scripts.ci import sdk_owned_job_deadline as gate


class OwnedDeadlineEvidenceTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_lubancore_owned_job_deadline.cpp']

    def facts(self):
        return [dict(path=path, quiescent=True, global_running=0,
            started_intent=path not in ('registered-queue', 'authorization'),
            command_not_invoked=path in ('late-worker', 'strict-recovery'),
            command_calls=1 if path in ('runtime-budget', 'startup-close') else 0,
            effective_timeout_ms=150 if path in ('runtime-budget', 'startup-close') else None)
            for path in gate.PATHS]

    def queue_observation(self, state='queued', elapsed=100):
        return dict(unlimited_budget_ms=0, unlimited_queued=True, unlimited_started=False,
                    unlimited_final_state='cancelled', registration_budget_ms=2000,
                    immediate_state=state, elapsed_before_register_ms=elapsed, global_running=1)

    def body(self, facts=None, command=None):
        command = self.command if command is None else command
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            '[owned-job-deadline-queue-observation] ' + json.dumps(self.queue_observation()),
            *('[owned-job-deadline-path] ' + path for path in gate.PATHS),
            *('[owned-job-deadline-fact] ' + json.dumps(fact) for fact in (self.facts() if facts is None else facts))))

    def reject(self, body=None, command=None):
        with self.assertRaises(RuntimeError):
            gate.check_owned_job_deadline_native(self.body() if body is None else body,
                self.command if command is None else command)

    def test_absolute_native_commands_and_line_endings(self):
        for exe in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancore_sdk_tests.exe',
                    '/real build/lubancode_tests', 'C:/real build/lubancode_tests.exe'):
            command = [exe, self.command[1]]
            for ending in ('\n', '\r\n'):
                facts = gate.check_owned_job_deadline_native(self.body(command=command).replace('\n', ending), command)
                self.assertEqual(facts, self.facts())

    def test_relative_foreign_filtered_or_malformed_argv(self):
        for command in (None, {}, [], tuple(self.command), self.command[:1],
                [False, self.command[1]], ['lubancore_sdk_tests', self.command[1]],
                ['C:relative/lubancore_sdk_tests', self.command[1]],
                ['/real\nbuild/lubancore_sdk_tests', self.command[1]],
                ['/real/foreign_tests', self.command[1]],
                [self.command[0], '--source-file=*test_lubancore_job_operations.cpp'],
                self.command + ['--test-case=one']):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                gate.check_owned_job_deadline_native(self.body(), command)
        self.reject(self.body(command=['/other/lubancore_sdk_tests', self.command[1]]))

    def test_case_assertion_and_completion_are_nonempty_exact(self):
        body = self.body()
        for original, replacement in (
            ('6 | 6 passed | 0 failed', '5 | 5 passed | 0 failed'),
            ('6 | 6 passed | 0 failed', '6 | 5 passed | 1 failed'),
            ('100 | 100 passed | 0 failed', '0 | 0 passed | 0 failed'),
            ('100 | 100 passed | 0 failed', '100 | 99 passed | 1 failed'),
            ('Test Passed.', ''), ('Test Passed.', 'Test Failed.'),
            ('Test Passed.', 'Test Passed.\nTest Passed.')):
            self.reject(body.replace(original, replacement))

    def test_all_paths_and_facts_exist_exactly_once(self):
        for path in gate.PATHS:
            marker = '[owned-job-deadline-path] ' + path
            self.reject(self.body().replace(marker, ''))
            self.reject(self.body() + '\n' + marker)
        self.reject(self.body() + '\n[owned-job-deadline-path] foreign')
        self.reject(self.body(self.facts()[:-1]))
        facts = self.facts()
        facts[-1]['path'] = facts[0]['path']
        self.reject(self.body(facts))
        self.reject(self.body(self.facts() + [self.facts()[0]]))

    def test_terminal_facts_require_actual_quiescence_and_types(self):
        for index in range(len(gate.PATHS)):
            for key, value in (('quiescent', False), ('quiescent', 1), ('global_running', 1),
                    ('global_running', False), ('started_intent', 1), ('command_not_invoked', 0),
                    ('command_calls', True), ('command_calls', -1), ('command_calls', 0.0)):
                facts = self.facts()
                facts[index][key] = value
                self.reject(self.body(facts))

    def test_uninvoked_commands_cannot_invent_started_or_runtime(self):
        for index in (0, 1):
            facts = self.facts()
            facts[index]['started_intent'] = True
            self.reject(self.body(facts))
        for index in (0, 1, 3, 5):
            for value in (0, 10, False):
                facts = self.facts()
                facts[index]['effective_timeout_ms'] = value
                self.reject(self.body(facts))
        for index in (3, 5):
            for key in ('started_intent', 'command_not_invoked'):
                facts = self.facts()
                facts[index][key] = False
                self.reject(self.body(facts))

    def test_real_commands_cannot_have_zero_or_inconsistent_budget(self):
        for index in (2, 4):
            for key, value in (('effective_timeout_ms', None), ('effective_timeout_ms', 0),
                    ('effective_timeout_ms', True), ('effective_timeout_ms', 1.5),
                    ('effective_timeout_ms', -1), ('command_not_invoked', True),
                    ('started_intent', False), ('command_calls', 0)):
                facts = self.facts()
                facts[index][key] = value
                self.reject(self.body(facts))

    def test_strict_json_shape_and_duplicate_fields(self):
        for value in (None, [], 'text', dict(self.facts()[0], foreign=True)):
            facts = self.facts()
            facts[0] = value
            self.reject(self.body(facts))
        facts = self.facts()
        del facts[0]['quiescent']
        self.reject(self.body(facts))
        self.reject(self.body().replace('"quiescent": true', '"quiescent": true, "quiescent": true', 1))
        self.reject(self.body().replace('"command_calls": 0', '"command_calls": NaN', 1))

    def test_focus_and_both_asan_selectors_are_wired(self):
        from scripts.ci.check_sdk_focused import REQUIRED
        from scripts.ci.check_asan_profile import selectors_from_workflow
        root = Path(__file__).resolve().parents[3]
        self.assertIn('sdk.focused.lubancore_owned_job_deadline', REQUIRED)
        workflow = (root / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        selectors = selectors_from_workflow(workflow)
        name = 'integration.sdk.lubancore_owned_job_deadline'
        for selector in selectors:
            self.assertIsNotNone(re.search(selector['include'], name))
            self.assertFalse(selector['exclude'] and re.search(selector['exclude'], name))
        self.assertIn("check_owned_job_deadline_registration(test.get('command', []), 'lubancode_tests')", workflow)
        self.assertIn('check_owned_job_deadline_native(sections[0], commands[0])', workflow)
        cmake = (root / 'cmake/LubanCoreTests.cmake').read_text(encoding='utf-8')
        self.assertIn('set(sdk_original_test "integration.sdk.lubancore_owned_job_deadline")', cmake)
        self.assertEqual(cmake.count('PROPERTIES RESOURCE_LOCK "sdk-owned-job-deadline"'), 2)

    def test_queue_observation_accepts_elapsed_deadline_without_fabricating_a_queue(self):
        for state, elapsed in (('queued', 0), ('queued', 1999), ('cancelled', 1999), ('cancelled', 8000)):
            value = self.queue_observation(state, elapsed)
            section = '[owned-job-deadline-queue-observation] ' + json.dumps(value)
            self.assertEqual(gate.check_queue_observation(section), value)
        self.reject(self.body().replace('"immediate_state": "queued"', '"immediate_state": "cancelled"'))

    def test_queue_observation_rejects_missing_duplicate_and_changed_claims(self):
        prefix = '[owned-job-deadline-queue-observation] '
        original = self.queue_observation()
        raw = prefix + json.dumps(original)
        self.reject(self.body().replace(raw, ''))
        self.reject(self.body() + '\n' + raw)
        for key, replacement in (('unlimited_budget_ms', False), ('unlimited_budget_ms', 1),
                ('registration_budget_ms', 2001), ('registration_budget_ms', True),
                ('global_running', 0), ('global_running', True), ('unlimited_queued', False),
                ('unlimited_queued', 1), ('unlimited_started', True), ('unlimited_started', 0),
                ('unlimited_final_state', 'succeeded'), ('immediate_state', 'running'),
                ('immediate_state', []), ('elapsed_before_register_ms', True),
                ('elapsed_before_register_ms', -1), ('elapsed_before_register_ms', 1999.5)):
            value = dict(original); value[key] = replacement
            with self.subTest(key=key, replacement=replacement):
                self.reject(self.body().replace(raw, prefix + json.dumps(value)))
        for value in (None, [], 'text', dict(original, foreign=True)):
            self.reject(self.body().replace(raw, prefix + json.dumps(value)))
        incomplete = dict(original); del incomplete['unlimited_queued']
        self.reject(self.body().replace(raw, prefix + json.dumps(incomplete)))
        self.reject(self.body().replace('"unlimited_budget_ms": 0', '"unlimited_budget_ms": 0, "unlimited_budget_ms": 0'))
        self.reject(self.body().replace('"elapsed_before_register_ms": 100', '"elapsed_before_register_ms": NaN'))


if __name__ == '__main__':
    unittest.main()
