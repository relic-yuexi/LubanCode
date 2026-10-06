"""Journal evidence checker counterexamples. All fixtures are pure test data.

Synthetic JSONL/logs here test parsers and rejection rules, never native results
or C++ canonical validation. No configure/build/native consumer is invoked.
"""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts.ci import sdk_journal_owner as gate

REPO = Path(__file__).resolve().parents[3]


def argv(stem='v3_journal_owner', exe='lubancore_sdk_tests', windows=False):
    return [('C:/data/' if windows else '/data/') + exe + ('.exe' if windows else ''),
            '--source-file=*test_' + stem + '.cpp']


def original_command(command):
    return 'Command: ' + ' '.join('"' + item + '"' for item in command)


def hosts(paths=gate.PUBLIC_PATHS):
    return [{'path': path, 'session_id': 'pure-scene-' + str(index), 'prefix_bytes': 100,
             'after_bytes': 200, 'model_calls': 4, 'tool_calls': 2} for index, path in enumerate(paths)]


def host_lines(facts):
    result = []
    for fact in facts:
        result += ['[sdk-journal-owner-path] ' + fact['path'],
                   '[sdk-journal-owner-host] session={session_id} prefix={prefix_bytes} after={after_bytes} models={model_calls} tools={tool_calls}'.format(**fact)]
    return result


def native(stem='v3_journal_owner', command=None):
    command = command or argv(stem)
    count, prefix, paths = gate.SOURCES[stem]
    lines = [original_command(command)]
    if stem == 'lubancore_journal_owner':
        lines += host_lines(hosts())
    else:
        if stem == 'lubancore_journal_owner_guards':
            lines += host_lines(hosts(('close-read',)))
        lines += [prefix + path for path in paths]
    # Filtered-out other source files legitimately count as framework skipped.
    lines += [f'[doctest] test cases: {count} | {count} passed | 0 failed | 1818 skipped',
              '[doctest] assertions: 300 | 300 passed | 0 failed', 'Test Passed.']
    return '\n'.join(lines) + '\n'


def consumer(command, facts=None):
    return '\n'.join([original_command(command), *host_lines(facts or hosts()),
                      'installed SDK consumer journal-owner passed', 'Test Passed.']) + '\n'


class LogGateTests(unittest.TestCase):
    def test_original_source_rosters_and_two_executable_names(self):
        for windows in (False, True):
            for exe in ('lubancore_sdk_tests', 'lubancode_tests'):
                for stem in gate.SOURCES:
                    command = argv(stem, exe, windows)
                    report = gate.check_native(native(stem, command), command, stem)
                    self.assertEqual(report['nativeCases'], gate.SOURCES[stem][0])
                    self.assertGreater(report['nativeAssertions'], 0)

    def test_registration_rejects_relative_extra_foreign_and_selected_case(self):
        command = argv()
        for changed in (None, [], [None, command[1]], [command[0]], [*command, '--test-case=one'],
                        ['lubancore_sdk_tests', command[1]], [command[0], '--source-file=*journal*'],
                        [command[0], '--source-file=*test_lubancore_journal_owner.cpp']):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError):
                gate.check_registration(changed, 'v3_journal_owner')

    def test_original_command_and_completion_cannot_be_borrowed(self):
        text = native(); command = argv()
        for changed in (text.replace(command[0], '/other/lubancore_sdk_tests'), text + 'Command: other\n',
                        text.replace('Test Passed.', 'Test Failed.'), text + 'Test Passed.\n',
                        text + 'SKIPPED: native unavailable\n', text.replace('Test Passed.', 'Test Not Run.')):
            with self.assertRaises(RuntimeError):
                gate.check_native(changed, command, 'v3_journal_owner')

    def test_exact_cases_positive_assertions_and_once_source_markers(self):
        for stem, (count, prefix, paths) in gate.SOURCES.items():
            text = native(stem)
            for changed in (text.replace(f'{count} | {count} passed', f'{count - 1} | {count - 1} passed'),
                            text.replace('300 | 300 passed', '0 | 0 passed'),
                            text.replace('300 | 300 passed', '300 | 299 passed'),
                            text.replace(prefix + paths[0] + '\n', ''), text + prefix + paths[0] + '\n',
                            text + prefix + 'foreign\n', text + '[doctest] assertions: 1 | 1 passed | 0 failed\n'):
                with self.subTest(stem=stem), self.assertRaises(RuntimeError):
                    gate.check_native(changed, argv(stem), stem)
        with self.assertRaises(RuntimeError):
            gate.check_native(native() + '[sdk-journal-owner-path] roundtrip\n', argv(), 'v3_journal_owner')

    def test_consumer_requires_three_absolute_args_and_all_real_facts(self):
        command = ['/second/build/lubancore_consumer', 'journal-owner', '/second/state-journal-owner']
        self.assertEqual(len(gate.check_consumer(consumer(command), command)['hosts']), 3)
        for changed in (command[:-1], [*command, 'extra'], [command[0], 'other', command[2]],
                        ['lubancore_consumer', *command[1:]], [*command[:2], 'state']):
            with self.assertRaises(RuntimeError):
                gate.check_consumer(consumer(command), changed)
        text = consumer(command)
        for changed in (text + '[sdk-journal-owner-path] roundtrip\n', text.replace('models=4', 'models=3', 1),
                        text.replace('tools=2', 'tools=0', 1), text.replace('prefix=100 after=200', 'prefix=200 after=200', 1),
                        text.replace('session=pure-scene-1', 'session=pure-scene-0'),
                        text.replace('installed SDK consumer journal-owner passed', ''),
                        text + 'installed SDK consumer journal-owner passed\n', text + host_lines(hosts())[1] + '\n'):
            with self.assertRaises(RuntimeError):
                gate.check_consumer(changed, command)


class OwnershipAndCopyTests(unittest.TestCase):
    def graph(self, testing=True, cli=False, file_api=False):
        graph = {'engine': {'name': 'lubancode_engine', 'type': 'STATIC_LIBRARY', 'luaSources': [gate.ENGINE]}}
        if testing:
            graph['sdk_test'] = {'name': 'lubancore_sdk_tests', 'type': 'EXECUTABLE', 'luaSources': [gate.HELPER]}
            if cli:
                graph['aggregate'] = {'name': 'lubancode_tests', 'type': 'EXECUTABLE', 'luaSources': [gate.HELPER]}
        if file_api:
            for target in graph.values():
                target['sources'] = [{'projectPath': path, 'compiled': True} for path in target.pop('luaSources')]
        return graph

    def test_actual_compiled_owner_counts_for_every_host_profile(self):
        for testing in (False, True):
            for cli in (False, True):
                for file_api in (False, True):
                    self.assertEqual(gate.ownership_violations(self.graph(testing, cli, file_api), testing, cli), [])
        for source in (gate.ENGINE, gate.HELPER):
            for mutation in ('duplicate', 'rogue', 'missing', 'declaration-only'):
                graph = self.graph(); target = next(item for item in graph.values() if source in item['luaSources'])
                if mutation == 'duplicate': target['luaSources'].append(source)
                elif mutation == 'rogue': graph['rogue'] = {'name': 'rogue', 'type': 'STATIC_LIBRARY', 'luaSources': [source]}
                elif mutation == 'missing': target['luaSources'].remove(source)
                else: target['sources'] = [{'projectPath': path, 'compiled': False} for path in target.pop('luaSources')]
                with self.subTest(source=source, mutation=mutation):
                    self.assertTrue(gate.ownership_violations(graph, True))

    def test_actual_helper_headers_and_engine_are_sealed_and_relocated(self):
        records = gate.seal_sources(REPO)
        self.assertEqual(set(records), {gate.ENGINE, gate.HELPER, *gate.HEADERS})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); caller = root / 'caller'; caller.mkdir(); prefix = root / 'prefix'
            destinations = [(gate.HELPER, caller / 'journal_owner.cpp'), *[(header, prefix / header) for header in gate.HEADERS]]
            for source, target in destinations:
                target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes((REPO / source).read_bytes())
            self.assertEqual(len(gate.check_copies(caller, prefix, records, REPO, root / 'evidence')), 3)
            for source, target in destinations:
                raw = target.read_bytes(); target.write_bytes(raw + b'changed\n')
                with self.assertRaises(RuntimeError): gate.check_copies(caller, prefix, records, REPO)
                target.write_bytes(raw)
            bad = deepcopy(records); bad[gate.HEADERS[0]]['sha256'] = '0' * 64
            with self.assertRaises(RuntimeError): gate.check_copies(caller, prefix, bad, REPO)


def encoded(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':')).encode('utf-8')


class PureLedger:
    """Data fixture only: hashes are labels, not claims of C++ canonical output."""
    def __init__(self, session_dir, sid):
        self.session_dir, self.sid, self.rows = session_dir, sid, []
        self.materials = {}
        self.add('message', messageId='msg-system', turnId=None, message={'role': 'system', 'content': 'pure'}, purpose='conversation', origin='session-runtime')
        self.event('session.started', 'event-start', payload={'context': {'fixture_float': 0.0000001}})

    def add(self, record_type, **fields):
        row = {'type': record_type, 'schemaVersion': 3, 'sessionId': self.sid, 'runId': 'run-pure',
               'seq': len(self.rows) + 1, 'timestamp': '2026-10-06T00:00:00.000Z',
               'prevHash': self.rows[-1]['lineHash'] if self.rows else '0' * 64, **fields}
        row['lineHash'] = hashlib.sha256(encoded(row)).hexdigest()
        self.rows.append(row); return row

    def event(self, kind, identity, payload=None, **fields):
        return self.add('event', kind=kind, eventId=identity, payload=payload or {}, **fields)

    def turn(self, tag, number):
        turn, action, operation = 'turn-' + tag, 'action-' + tag, 'operation-' + tag
        self.event('sdk.operation.turn.bound', 'bound-' + tag, {'layout': 'sdk_main_operation_turn_v1', 'version': 1,
                   'operationId': operation, 'inputId': 'input-' + tag, 'payloadHash': 'a' * 64}, turnId=turn)
        self.event('model.request.prepared', 'prepared-' + tag + '-1', turnId=turn, requestId='request-' + tag + '-1')
        payload = {'tool_call_id': action, 'attempt': 1}
        self.event('tool.execution.pending', 'pending-' + tag, {**payload, 'provider_tool_call_id': 'host-call-' + tag, 'reason': 'queued'}, turnId=turn, actionId=action)
        self.event('tool.execution.started', 'started-' + tag, {**payload, 'toolIdentity': {'logicalName': 'journal_fixture'}}, turnId=turn, actionId=action)
        execution = 'finished-' + tag
        self.event('tool.execution.finished', execution, payload, turnId=turn, actionId=action)
        witnesses, sources = {}, []
        for role, prefix in (('raw', 'capture-'), ('formal', 'res-')):
            rid = prefix + f'{number:06}'; text = ('JOURNAL_TOOL_' + tag).encode()
            channel = {'artifactId': rid + '-combined', 'kind': 'combined', 'path': 'artifacts/' + rid + '.combined.txt',
                       'sha256': hashlib.sha256(text).hexdigest(), 'bytes': len(text), 'mediaType': 'text/plain'}
            meta = {'result_id': rid, 'tool_call_id': action, 'attempt': 1, 'execution_event_ref': execution, 'result_kind': 'text',
                    'content': text.decode(), 'capture_limits': None,
                    'preview_policy': {'policy': 'raw-capture-before-post-hook' if role == 'raw' else 'v3-tool-preview'},
                    'outputs': [{'channel': 'combined', 'encoding': 'utf-8', 'capture_complete': True, 'byte_count_kind': 'exact',
                                 'captured_bytes': len(text), 'output_bytes': len(text),
                                 'ref': {'artifact_id': channel['artifactId'], 'path': channel['path'], 'sha256': channel['sha256'],
                                         'bytes': channel['bytes'], 'media_type': channel['mediaType']}}]}
            metadata = encoded(meta)
            mref = {'artifactId': rid, 'kind': 'result_metadata', 'path': 'artifacts/' + rid + '.json',
                    'sha256': hashlib.sha256(metadata).hexdigest(), 'bytes': len(metadata), 'mediaType': 'application/json'}
            for ref, data in ((mref, metadata), (channel, text)):
                target = self.session_dir / ref['path']; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
            persisted = 'persisted-' + tag + '-' + role
            self.event('tool.result.persisted', persisted, {**payload, 'executionEventRef': execution, 'result_ref': [mref, channel]}, turnId=turn, actionId=action)
            witnesses[role] = dict(session=self.sid, operation=operation, turn=turn, tool_call=action, attempt=1, execution=execution,
                                   persisted=persisted, result=rid, metadata_sha256=mref['sha256'], metadata_bytes=mref['bytes'],
                                   combined_sha256=channel['sha256'], combined_bytes=len(text))
            sources.append(persisted)
        selected = 'selected-' + tag
        self.event('tool.result.selected', selected, {**payload, 'sourceResultEventRefs': sources, 'effectiveOutcome': 'done'}, turnId=turn, actionId=action)
        self.add('message', messageId='tool-message-' + tag, turnId=turn, actionId=action, resultSelectionRef=selected,
                 message={'role': 'tool', 'tool_call_id': action, 'content': 'JOURNAL_TOOL_' + tag})
        self.event('model.request.prepared', 'prepared-' + tag + '-2', turnId=turn, requestId='request-' + tag + '-2')
        self.event('session.ended', 'ended-' + tag)
        self.materials[tag] = witnesses
        return operation

    def bytes(self):
        return b'\n'.join(encoded(row) for row in self.rows) + b'\n'

    def save(self):
        self.session_dir.mkdir(parents=True, exist_ok=True)
        (self.session_dir / (self.sid + '.jsonl')).write_bytes(self.bytes())


class PureHosts:
    def __init__(self, root):
        self.base = root / 'state-journal-owner'; self.base.mkdir()
        self.command = [str(root / 'build' / 'lubancore_consumer'), 'journal-owner', str(self.base)]
        self.facts, self.fixtures = [], []
        for index, path in enumerate(gate.PUBLIC_PATHS):
            fixture = self.base / f'journal-owner-host-{100 + index}-{index + 1}'; sid = 'scene-' + str(index)
            ledger = PureLedger(fixture / 'state' / 'workspaces' / 'workspace' / 'sessions' / sid, sid)
            old = ledger.turn('OLD', 1); prefix = ledger.bytes(); new = ledger.turn('NEW', 2); ledger.save()
            (fixture / 'journal-owner-before.jsonl').write_bytes(prefix)
            receipt = {'session_id': sid, 'prefix_bytes': len(prefix), 'after_bytes': len(ledger.bytes()), 'model_calls': 4,
                       'tool_calls': 2, 'old_operation': old, 'new_operation': new}
            for tag in ('OLD', 'NEW'):
                for role, values in ledger.materials[tag].items():
                    receipt.update({f'{tag.lower()}_{role}_{key}': value for key, value in values.items()})
            self.write_receipt(fixture, receipt)
            if path == 'isolation':
                other = PureLedger(fixture / 'state' / 'workspaces' / 'workspace' / 'sessions' / 'other-scene', 'other-scene')
                other.turn('OTHER', 1); other.save()
            self.facts.append({'path': path, **{key: receipt[key] for key in ('session_id', 'prefix_bytes', 'after_bytes', 'model_calls', 'tool_calls')}})
            self.fixtures.append((fixture, ledger, receipt))

    @staticmethod
    def write_receipt(fixture, values):
        (fixture / 'journal-owner-host-receipt.txt').write_text(''.join(f'{key}={value}\n' for key, value in values.items()), encoding='utf-8', newline='\n')

    def receipt(self):
        return gate.check_consumer(consumer(self.command, self.facts), self.command)


class SavedMaterialTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.world = PureHosts(self.root)

    def capture(self, name='evidence'):
        return gate.capture_materials(self.world.base, self.root / name)

    def check(self, capture=None):
        return gate.check_materials(capture or self.capture(), self.world.receipt())

    def test_preserves_exact_actual_test_data_and_checks_all_three_fixture_pairs(self):
        capture = self.capture(); report = self.check(capture)
        self.assertEqual(capture['acceptance'], 'not_evaluated')
        self.assertEqual(report['status'], 'verified_materials'); self.assertEqual(len(report['fixtures']), 3)
        self.assertFalse(report['canonical_hash_recomputed']); self.assertTrue(report['native_canonical_guard_required'])
        for item in capture['files']:
            self.assertEqual((self.world.base / item['relative']).read_bytes(), (Path(capture['evidence']) / item['saved']).read_bytes())

    def test_saved_artifact_or_main_tampering_is_rejected_without_rereading_source(self):
        for category in ('.combined.txt', '.jsonl', '.json'):
            capture = self.capture('evidence-' + category.replace('.', '_'))
            item = next(item for item in capture['files'] if item['relative'].endswith(category))
            saved = Path(capture['evidence']) / item['saved']; saved.write_bytes(saved.read_bytes() + b'tamper')
            with self.subTest(category=category), self.assertRaises(RuntimeError): self.check(capture)

    def test_live_source_can_retire_after_capture_and_cannot_replace_saved_snapshot(self):
        capture = self.capture()
        fixture, ledger, _ = self.world.fixtures[0]
        (ledger.session_dir / (ledger.sid + '.jsonl')).write_bytes(b'changed live source')
        self.assertEqual(self.check(capture)['status'], 'verified_materials')

    def test_wrong_old_prefix_and_mismatched_host_facts_are_rejected(self):
        fixture, _, _ = self.world.fixtures[0]
        path = fixture / 'journal-owner-before.jsonl'; raw = path.read_bytes(); path.write_bytes(b'X' + raw[1:])
        with self.assertRaises(RuntimeError): self.check()
        path.write_bytes(raw)
        receipt = self.world.receipt(); receipt['hosts'][0]['after_bytes'] += 1
        with self.assertRaises(RuntimeError): gate.check_materials(self.capture('fresh'), receipt)

    def test_main_scope_sequence_run_and_actual_count_mismatches_are_rejected(self):
        fixture, ledger, receipt = self.world.fixtures[0]
        original = deepcopy(ledger.rows)
        for field, value in (('sessionId', 'foreign-scene'), ('runId', 'foreign-run'), ('seq', 100), ('prevHash', 'f' * 64)):
            ledger.rows = deepcopy(original); ledger.rows[-1][field] = value; ledger.save()
            receipt['after_bytes'] = len(ledger.bytes()); self.world.facts[0]['after_bytes'] = len(ledger.bytes()); self.world.write_receipt(fixture, receipt)
            with self.subTest(field=field), self.assertRaises(RuntimeError): self.check(self.capture('changed-' + field))
        ledger.rows = deepcopy(original)
        next(row for row in ledger.rows if row.get('kind') == 'model.request.prepared' and row.get('turnId') == 'turn-NEW')['kind'] = 'model.request.sent'
        ledger.save(); receipt['after_bytes'] = len(ledger.bytes()); self.world.facts[0]['after_bytes'] = len(ledger.bytes()); self.world.write_receipt(fixture, receipt)
        with self.assertRaises(RuntimeError): self.check(self.capture('wrong-count'))

    def test_raw_formal_swaps_wrong_attempt_execution_and_missing_artifacts_reject(self):
        fixture, ledger, receipt = self.world.fixtures[0]
        path = ledger.session_dir / 'artifacts' / 'capture-000002.json'; original = path.read_bytes()
        for field, value in (('result_id', 'res-000002'), ('attempt', 2), ('execution_event_ref', 'foreign-execution'), ('tool_call_id', 'foreign-action')):
            meta = json.loads(original); meta[field] = value; path.write_bytes(encoded(meta))
            with self.subTest(field=field), self.assertRaises(RuntimeError): self.check(self.capture('metadata-' + field))
        path.write_bytes(original)
        formal = ledger.session_dir / 'artifacts' / 'res-000002.combined.txt'; formal.unlink()
        with self.assertRaises(RuntimeError): self.check(self.capture('missing-artifact'))
        self.world.write_receipt(fixture, {**receipt, 'old_raw_result': receipt['old_formal_result'], 'old_formal_result': receipt['old_raw_result']})
        with self.assertRaises(RuntimeError): self.check(self.capture('swapped-witness'))

    def test_coherent_artifact_digests_do_not_launder_wrong_scope_or_raw_provenance(self):
        fixture, ledger, receipt = self.world.fixtures[0]
        path = ledger.session_dir / 'artifacts' / 'capture-000002.json'
        original_bytes, original_rows, original_receipt = path.read_bytes(), deepcopy(ledger.rows), deepcopy(receipt)
        for field, value in (('tool_call_id', 'foreign-action'), ('execution_event_ref', 'foreign-execution'),
                             ('attempt', 2), ('preview_policy', {'policy': 'v3-tool-preview'}),
                             ('content', 'FORGED_CALLBACK_TEXT')):
            ledger.rows = deepcopy(original_rows); receipt.clear(); receipt.update(original_receipt)
            metadata = json.loads(original_bytes); metadata[field] = value; material = encoded(metadata); path.write_bytes(material)
            # Make descriptor, saved-file digest and host witness mutually
            # consistent. This still cannot change the real main action/terminal
            # source, raw provenance, or the known callback bytes. Fixture line
            # hashes remain labels; no C++ canonical pass is asserted here.
            event = next(row for row in ledger.rows if row.get('eventId') == 'persisted-NEW-raw')
            ref = event['payload']['result_ref'][0]; ref['sha256'] = hashlib.sha256(material).hexdigest(); ref['bytes'] = len(material)
            receipt['new_raw_metadata_sha256'] = ref['sha256']; receipt['new_raw_metadata_bytes'] = ref['bytes']
            ledger.save(); receipt['after_bytes'] = len(ledger.bytes()); self.world.facts[0]['after_bytes'] = len(ledger.bytes())
            self.world.write_receipt(fixture, receipt)
            with self.subTest(field=field), self.assertRaisesRegex(RuntimeError, 'metadata scope/raw/formal provenance'):
                self.check(self.capture('coherent-' + field))

    def test_material_reference_escape_and_wrong_actual_selection_cannot_pass(self):
        fixture, ledger, receipt = self.world.fixtures[0]
        original = deepcopy(ledger.rows)
        for mutation in ('path-escape', 'selection-swap', 'selection-omission'):
            ledger.rows = deepcopy(original)
            if mutation == 'path-escape':
                event = next(row for row in ledger.rows if row.get('eventId') == 'persisted-NEW-raw')
                event['payload']['result_ref'][0]['path'] = 'artifacts/../../outside.json'
            else:
                event = next(row for row in ledger.rows if row.get('eventId') == 'selected-NEW')
                if mutation == 'selection-swap': event['payload']['sourceResultEventRefs'].reverse()
                else: event['payload']['sourceResultEventRefs'].pop()
            ledger.save(); receipt['after_bytes'] = len(ledger.bytes()); self.world.facts[0]['after_bytes'] = len(ledger.bytes())
            self.world.write_receipt(fixture, receipt)
            with self.subTest(mutation=mutation), self.assertRaises(RuntimeError): self.check(self.capture(mutation))

    def test_duplicate_receipt_fields_extra_artifacts_and_duplicate_main_root_reject(self):
        fixture, ledger, receipt = self.world.fixtures[0]
        receipt_file = fixture / 'journal-owner-host-receipt.txt'; original = receipt_file.read_bytes()
        receipt_file.write_bytes(original + b'session_id=scene-0\n')
        with self.assertRaises(RuntimeError): self.check(self.capture('duplicate-receipt'))
        receipt_file.write_bytes(original)
        extra = ledger.session_dir / 'artifacts' / 'res-999999.json'; extra.write_bytes(b'{}')
        with self.assertRaises(RuntimeError): self.check(self.capture('extra-artifact'))
        extra.unlink()
        duplicate = fixture / 'state' / 'workspaces' / 'different-root' / 'sessions' / ledger.sid / (ledger.sid + '.jsonl')
        duplicate.parent.mkdir(parents=True); duplicate.write_bytes(ledger.bytes())
        with self.assertRaises(RuntimeError): self.check(self.capture('duplicate-main'))

    def test_capture_failure_retains_real_partial_files_and_never_becomes_green(self):
        capture = self.capture()
        fixture, _, _ = self.world.fixtures[1]
        (fixture / 'journal-owner-host-receipt.txt').unlink()
        partial = self.capture('partial')
        self.assertTrue(partial['files']); self.assertEqual(partial['acceptance'], 'not_evaluated')
        with self.assertRaises(RuntimeError): self.check(partial)
        with patch.object(gate, 'MAX_ENTRIES', 1):
            failed = self.capture('bounded')
        self.assertEqual(failed['status'], 'capture_failed'); self.assertEqual(failed['acceptance'], 'not_evaluated')
        self.assertTrue(Path(failed['manifest']['path']).is_file())
        with self.assertRaises(RuntimeError): self.check(failed)

    def test_capture_file_and_total_byte_bounds_leave_not_evaluated_manifest(self):
        for setting in ('MAX_FILE_BYTES', 'MAX_CAPTURE_BYTES'):
            with patch.object(gate, setting, 1):
                failed = self.capture('bounded-' + setting)
            self.assertEqual(failed['status'], 'capture_failed'); self.assertEqual(failed['acceptance'], 'not_evaluated')
            self.assertTrue(Path(failed['manifest']['path']).is_file())
            with self.assertRaises(RuntimeError): self.check(failed)

    def test_changed_context_manifest_foreign_root_and_duplicate_saved_path_reject(self):
        capture = self.capture(); receipt = self.world.receipt(); receipt['command'][2] = str(self.root / 'foreign-state')
        with self.assertRaises(RuntimeError): gate.check_materials(capture, receipt)
        copied = deepcopy(capture); copied['files'].append(copied['files'][0])
        with self.assertRaises(RuntimeError): self.check(copied)
        manifest = Path(capture['manifest']['path']); manifest.write_bytes(manifest.read_bytes() + b' ')
        with self.assertRaises(RuntimeError): self.check(capture)

    def test_additional_saved_files_are_not_hidden_outside_the_manifest(self):
        capture = self.capture(); extra = Path(capture['evidence']) / 'files' / 'unbound.json'; extra.write_bytes(b'{}')
        with self.assertRaisesRegex(RuntimeError, 'extra or missing preserved files'): self.check(capture)

    def test_symlink_escape_and_overlapping_destination_cannot_write_source(self):
        destination = self.world.base / 'nested-evidence'
        failed = gate.capture_materials(self.world.base, destination)
        self.assertEqual(failed['status'], 'capture_failed'); self.assertFalse(destination.exists())
        fixture, ledger, _ = self.world.fixtures[0]
        path = ledger.session_dir / 'artifacts' / 'res-000002.combined.txt'; original = path.read_bytes(); path.unlink()
        foreign = self.root / 'foreign'; foreign.write_bytes(original)
        try:
            path.symlink_to(foreign)
        except OSError:
            self.skipTest('OS does not permit symlink creation in this pure checker fixture')
        failed = self.capture('linked')
        self.assertEqual(failed['status'], 'capture_failed'); self.assertEqual(failed['acceptance'], 'not_evaluated')
        with self.assertRaises(RuntimeError): self.check(failed)


if __name__ == '__main__':
    unittest.main()
