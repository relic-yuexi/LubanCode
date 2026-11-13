"""Pure WebFetch fixture/consumer receipts, with all subprocesses mocked."""
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts.ci import sdk_web_fetch_fixture as fixture
from scripts.ci import check_sdk_focused as focused


def context():
    root = '/actual/scratch/web-fetch-fixture'
    value = {'schemaVersion': 1, 'status': 'closed', 'pid': 123, 'stop_written': True, 'forced': False,
             'returncode': 0, 'alive_after_wait': False, 'early_exit': False,
             'environment': deepcopy(fixture.ENVIRONMENT), 'cwd': root, 'caller_cwd': '/actual/scratch',
             'fixture_source': root + '/sdk_web_fetch_http.py', 'fixture_sha256': 'a' * 64,
             'ready_file': root + '/ready.json', 'requests_file': root + '/requests.tsv', 'stop_file': root + '/stop',
             'consumer_build': '/actual/scratch/build', 'consumer_junit': '/actual/evidence/results.xml',
             'consumer_executable': '/actual/scratch/build/lubancore_consumer'}
    value['ready'] = {'schemaVersion': 1, 'pid': 123, 'base_url': 'http://127.0.0.1:23881',
                      'requests_file': value['requests_file'], 'stop_file': value['stop_file']}
    value['argv'] = ['/tools/python', value['fixture_source'], '--ready-file', value['ready_file'],
                     '--requests-file', value['requests_file'], '--stop-file', value['stop_file']]
    value['callers'] = []
    for phase, tail in [('registration', ['--show-only=json-v1']),
                        ('execute', ['--output-on-failure', '--no-tests=error', '--output-junit', value['consumer_junit']])]:
        value['callers'].append({'phase': phase, 'status': 'completed', 'returncode': 0,
            'environment': deepcopy(fixture.ENVIRONMENT), 'cwd': value['caller_cwd'],
            'argv': ['/tools/ctest', '--test-dir', value['consumer_build'], '-C', 'Release', *tail],
            'stdout_bytes': 12, 'stdout_sha256': 'b' * 64})
    return value


def request_bytes():
    return ''.join(target + '\t' + agent + '\n' for (target, agent), count in fixture.EXPECTED_REQUESTS.items()
                   for _ in range(count)).encode('ascii')


class ReceiptTests(unittest.TestCase):
    def test_loopback_request_ledger_proves_all_thirty_real_requests(self):
        raw = request_bytes()
        self.assertEqual(fixture.check_requests(raw)['records'], 30)
        # The four overlapping sessions may arrive in a different order.
        self.assertEqual(fixture.check_requests(b'\n'.join(reversed(raw.rstrip(b'\n').split(b'\n'))) + b'\n')['records'], 30)
        for bad in (b'', raw[:-1], raw + raw.splitlines(keepends=True)[0], b''.join(raw.splitlines(keepends=True)[1:]),
                    raw.replace(b'/admission/plain', b'/admission/never'), raw.replace(b'isolation-1', b'other-agent'),
                    raw.replace(b'\n', b'\r\n'), raw.replace(b'\n', b'\x1e', 1),
                    raw + b'non-ascii-\xff\n'):
            with self.assertRaises(RuntimeError): fixture.check_requests(bad)

    def test_typed_lifecycle_rejects_unjoined_foreign_or_forced_results(self):
        good = context(); fixture.check_caller_receipts(good)
        for field, value in [('schemaVersion', True), ('pid', True), ('returncode', False), ('returncode', 1),
                             ('forced', True), ('stop_written', False), ('alive_after_wait', True), ('early_exit', True),
                             ('start_error', 'failed'), ('cleanup_error', 'failed'), ('fixture_sha256', ''),
                             ('requests_file', '/foreign/requests.tsv'), ('caller_cwd', 'relative')]:
            bad = deepcopy(good); bad[field] = value
            with self.subTest(field=field), self.assertRaises(RuntimeError): fixture.check_caller_receipts(bad)
        for mutate in ('foreign-port', 'foreign-pid', 'argv', 'environment', 'caller-filter', 'caller-zero', 'caller-relative'):
            bad = deepcopy(good)
            if mutate == 'foreign-port': bad['ready']['base_url'] = 'http://example.test:80'
            if mutate == 'foreign-pid': bad['ready']['pid'] += 1
            if mutate == 'argv': bad['argv'].append('--hidden-option')
            if mutate == 'environment': bad['environment']['NO_PROXY'] = '*'
            if mutate == 'caller-filter': bad['callers'][1]['argv'].extend(['-R', 'only-one'])
            if mutate == 'caller-zero': bad['callers'][1]['stdout_bytes'] = 0
            if mutate == 'caller-relative': bad['callers'][0]['argv'][0] = 'ctest'
            with self.subTest(mutate=mutate), self.assertRaises(RuntimeError): fixture.check_caller_receipts(bad)

    def test_consumer_full_argv_binds_actual_fixture_and_installed_target(self):
        ctx = context()
        command = [ctx['consumer_executable'], 'web-fetch', ctx['consumer_build'] + '/state-web-fetch',
                   ctx['ready']['base_url'], ctx['requests_file']]
        section = ('Command: ' + ' '.join('"' + arg + '"' for arg in command) + '\n' +
                   '[sdk-web-fetch-decoder] {"gzip_decoding":true,"outcome":"download_limit"}\n' +
                   '\n'.join('[sdk-web-fetch-path] ' + path for path in focused.WEB_FETCH_REQUEST_COUNTS) +
                   '\n[sdk-web-fetch-consumer] complete\nTest Passed.\n')
        focused.check_web_fetch_consumer(section, command, ctx)
        for index, value in ((0, '/foreign/lubancore_consumer'), (1, 'smoke'), (2, 'relative'),
                             (3, 'http://127.0.0.1:23882'), (4, '/foreign/requests.tsv')):
            bad = list(command); bad[index] = value
            with self.subTest(index=index), self.assertRaises(RuntimeError): focused.check_web_fetch_consumer(section, bad, ctx)
        with self.assertRaises(RuntimeError): focused.check_web_fetch_consumer(section, command + ['extra'], ctx)
        with self.assertRaises(RuntimeError): focused.check_web_fetch_consumer(section.replace('[sdk-web-fetch-path] cancel', 'cancel skipped'), command, ctx)

    def test_source_and_header_copies_expose_only_public_sdk_and_stl(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); repo = root / 'repo'; source = root / 'copy'; prefix = root / 'installed'
            helper = repo / fixture.HELPER; helper.parent.mkdir(parents=True)
            helper.write_text('#include <lubancore/core.hpp>\n#include <string>\n', encoding='utf-8')
            source.mkdir(); (source / 'web_fetch.cpp').write_bytes(helper.read_bytes())
            seal = fixture.seal_helper(repo); fixture.check_helper_copy(source, seal)
            header = repo / fixture.HEADER; header.parent.mkdir(parents=True)
            header.write_text('#include <lubancore/api.hpp>\n#include <cstdint>\n#include <string>\n', encoding='utf-8')
            copied = prefix / fixture.HEADER; copied.parent.mkdir(parents=True); copied.write_bytes(header.read_bytes())
            fixture.check_installed_header(repo, prefix)
            copied.write_text('#include <cpr/cpr.h>\n', encoding='utf-8')
            with self.assertRaises(RuntimeError): fixture.check_installed_header(repo, prefix)
            (source / 'web_fetch.cpp').write_text('#include "sdk/core.hpp"\n', encoding='utf-8')
            with self.assertRaises(RuntimeError): fixture.check_helper_copy(source, seal)
            helper.write_text('#include <lubancore/core.hpp>\n#include <cpr/cpr.h>\n', encoding='utf-8')
            with self.assertRaises(RuntimeError): fixture.seal_helper(repo)


class FakeProcess:
    def __init__(self, argv, *, malformed=False, early=False, force=False, **kwargs):
        self.argv = argv; self.pid = 811; self.returncode = 9 if early else None
        self.force = force; self.terminated = False; self.waited = False
        ready, requests, stop = (Path(argv[index]) for index in (3, 5, 7))
        self.stop = stop
        requests.write_bytes(b'')
        value = {'schemaVersion': 1, 'pid': 812 if malformed else self.pid, 'base_url': 'http://127.0.0.1:23991',
                 'requests_file': str(requests), 'stop_file': str(stop)}
        ready.write_text(json.dumps(value) + '\n', encoding='utf-8')

    def poll(self): return self.returncode

    def wait(self, timeout):
        if self.force and not self.terminated: raise subprocess.TimeoutExpired(self.argv, timeout)
        self.waited = True
        if self.returncode is None:
            if not self.stop.is_file(): raise AssertionError('fixture owner did not publish stop before wait')
            self.returncode = 0
        return self.returncode

    def terminate(self): self.terminated = True; self.returncode = -15
    def kill(self): self.terminated = True; self.returncode = -9


class FixtureOwnerTests(unittest.TestCase):
    def setUp(self):
        # These are mocked receipt cases, not executed CTest/HTTP commands.
        silence = patch('builtins.print')
        silence.start()
        self.addCleanup(silence.stop)

    def prepare(self, directory):
        root = Path(directory); repo = root / 'repo'; scratch = root / 'scratch'; evidence = root / 'evidence'
        source = repo / fixture.SERVER; source.parent.mkdir(parents=True)
        source.write_text('# copied fixture data only; subprocess calls are mocked\n', encoding='utf-8')
        scratch.mkdir()
        return fixture.FixtureOwner(repo, scratch, evidence, {'PATH': '', 'HTTP_PROXY': 'must-not-survive'}), evidence

    def test_actual_owner_wiring_publishes_stop_and_records_callers_without_launching_any_process(self):
        with tempfile.TemporaryDirectory() as directory:
            owner, evidence = self.prepare(directory)
            instances = []
            def launch(argv, **kwargs):
                self.assertEqual(kwargs['stdin'], subprocess.DEVNULL)
                self.assertNotIn('HTTP_PROXY', kwargs['env']); self.assertEqual(kwargs['env']['NO_PROXY'], '127.0.0.1')
                process = FakeProcess(argv, **kwargs); instances.append(process); return process
            with patch.object(fixture.subprocess, 'Popen', side_effect=launch), patch.object(fixture.subprocess, 'run',
                    side_effect=lambda argv, **kwargs: subprocess.CompletedProcess(argv, 0, b'original caller output\n', b'')):
                with owner as live:
                    build = live.scratch / 'build'; junit = evidence / 'results.xml'
                    live.caller('registration', ['/tools/ctest', '--test-dir', str(build), '-C', 'Release', '--show-only=json-v1'], build, junit)
                    live.caller('execute', ['/tools/ctest', '--test-dir', str(build), '-C', 'Release', '--output-on-failure',
                        '--no-tests=error', '--output-junit', str(junit)], build, junit)
            self.assertEqual(len(instances), 1); self.assertTrue(instances[0].waited)
            fixture.check_caller_receipts(owner.context)
            self.assertEqual((evidence / 'caller-execute.stdout').read_bytes(), b'original caller output\n')
            self.assertTrue((evidence / 'ready.json').is_file()); self.assertTrue((evidence / 'requests.tsv').is_file())
            self.assertEqual((evidence / 'stop').read_bytes(), b'stop\n')

    def test_bad_ready_and_early_exit_still_retire_child_and_keep_original_ready(self):
        for failure in ('malformed', 'early'):
            with tempfile.TemporaryDirectory() as directory:
                owner, evidence = self.prepare(directory); created = []
                def launch(argv, **kwargs):
                    process = FakeProcess(argv, malformed=failure == 'malformed', early=failure == 'early', **kwargs)
                    created.append(process); return process
                with patch.object(fixture.subprocess, 'Popen', side_effect=launch):
                    with self.assertRaises(RuntimeError): owner.start()
                self.assertTrue(created[0].waited); self.assertIsNotNone(created[0].poll())
                self.assertTrue((evidence / 'context.json').is_file()); self.assertTrue((evidence / 'ready.json').is_file())

    def test_forced_retirement_fails_and_does_not_mask_an_existing_caller_error(self):
        with tempfile.TemporaryDirectory() as directory:
            owner, evidence = self.prepare(directory)
            with patch.object(fixture.subprocess, 'Popen', side_effect=lambda argv, **kwargs: FakeProcess(argv, force=True, **kwargs)):
                with self.assertRaisesRegex(RuntimeError, 'original caller failed'):
                    with owner: raise RuntimeError('original caller failed')
            self.assertIs(owner.context['forced'], True)
            self.assertIs(owner.context['alive_after_wait'], False)
            self.assertIn('cleanup_error', json.loads((evidence / 'context.json').read_text()))
            with self.assertRaises(RuntimeError): fixture.check_closed_context(owner.context)

    def test_caller_spawn_error_is_recorded_and_owned_server_still_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            owner, evidence = self.prepare(directory)
            with patch.object(fixture.subprocess, 'Popen', side_effect=lambda argv, **kwargs: FakeProcess(argv, **kwargs)), \
                 patch.object(fixture.subprocess, 'run', side_effect=OSError('caller missing')):
                with self.assertRaises(OSError):
                    with owner as live:
                        live.caller('registration', ['/tools/ctest', '--test-dir', str(live.scratch / 'build'), '-C', 'Release',
                            '--show-only=json-v1'], live.scratch / 'build', evidence / 'results.xml')
            self.assertEqual(owner.context['callers'][0]['status'], 'spawn-failed')
            self.assertEqual(owner.context['returncode'], 0)


if __name__ == '__main__':
    unittest.main()
