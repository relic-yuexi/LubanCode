"""Counterexamples for result-extension binding, not native acceptance."""
from copy import deepcopy
import unittest
from scripts.ci import sdk_journal_owner as gate


def fixture():
    observation = {'provider_namespace': 'lubancore.backend.legacy', 'anomalies': [],
                   'extensions': [], 'fields': [{} for _ in range(5)]}
    rows = [{'type': 'message', 'sessionId': 'session', 'runId': 'run'},
            {'type': 'event', 'kind': 'model.request.prepared', 'requestId': 'request',
             'turnId': 'turn', 'stepId': 'writer-step', 'seq': 1,
             'payload': {'model': 'model', 'producerStepId': 'producer-step', 'prefixAccount': {'cacheEpoch': 1}}},
            {'type': 'event', 'kind': 'model.request.sent', 'requestId': 'request',
             'turnId': 'turn', 'stepId': 'writer-step', 'seq': 2, 'payload': {}},
            {'type': 'event', 'kind': 'model.usage.observed', 'requestId': 'request',
             'turnId': 'turn', 'stepId': 'writer-step', 'seq': 3,
             'payload': {'numbers': [2,1,0,0,0], 'observation': observation,
                         'reportedByProvider': True, 'incomplete': False, 'providerResponseId': None}}]
    def summary(count, total):
        return {'samples': count, 'total': total, 'counter_overflow': False, 'fields': [
            {'observed': count if i < 2 else 0, 'valid': count if i < 2 else 0,
             'missing': 0 if i < 2 else count, 'inferred': 0, 'anomalous': 0,
             'included': count, 'omitted': 0, 'arithmetic_overflow': False} for i in range(5)]}
    saved = {'usage': {'version': 2, 'attempts_complete': True, 'attempts': [{
        'numbers': [2,1,0,0,0], 'observation': deepcopy(observation),
        'source_session_id': 'session', 'source_run_id': 'run', 'trajectory_request_id': 'request',
        'provider_response_id': '', 'model': 'model', 'step_id': 'producer-step', 'turn_id': 'turn',
        'purpose': 'main_turn', 'cache_epoch': 1, 'reported_by_provider': True,
        'subordinate': False, 'incomplete': False}],
        'direct': summary(1, [2,1,0,0,0]), 'subordinate': summary(0, [0,0,0,0,0])}}
    return saved, rows


class UsageBinding(unittest.TestCase):
    def test_actual_request_relation_and_legacy_absence(self):
        saved, rows = fixture()
        gate._ordinary_usage(saved, rows, 'turn')
        gate._ordinary_usage({}, rows, 'turn')

    def test_foreign_identity_and_observation_are_rejected(self):
        for key, value in [('source_session_id', 'other'), ('source_run_id', 'other'),
                           ('trajectory_request_id', 'other'), ('model', 'other'),
                           ('step_id', 'writer-step'), ('cache_epoch', True),
                           ('provider_response_id', 'invented'), ('numbers', [2,2,0,0,0])]:
            with self.subTest(key=key):
                saved, rows = fixture()
                saved['usage']['attempts'][0][key] = value
                with self.assertRaises(RuntimeError): gate._ordinary_usage(saved, rows, 'turn')

    def test_prepared_sent_and_counter_prefix_cannot_be_forged(self):
        for fault in range(5):
            with self.subTest(fault=fault):
                saved, rows = fixture()
                if fault == 0: rows.pop(2)
                if fault == 1: rows[2]['seq'] = 4
                if fault == 2: saved['usage']['attempts'].append(deepcopy(saved['usage']['attempts'][0]))
                if fault == 3: saved['usage']['direct']['total'][0] = 3
                if fault == 4: saved['usage']['direct']['fields'][0]['included'] = True
                with self.assertRaises(RuntimeError): gate._ordinary_usage(saved, rows, 'turn')
