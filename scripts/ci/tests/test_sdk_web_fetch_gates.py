"""WebFetch native/installed/full evidence and ownership data; no native execution."""
from copy import deepcopy
import fnmatch
import hashlib
import json
import re
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

from scripts.ci import check_sdk_focused as focused
from scripts.ci import check_installed_sdk as installed
from scripts.ci import check_sdk_build_closure as closure
from scripts.ci import check_sdk_only_boundary as boundary
from scripts.ci import sdk_web_fetch_fixture as fixture
from scripts.ci.sdk_lua_profile import consumer_roster, focused_roster
from scripts.ci.tests import test_sdk_only_boundary as graph_fixtures
from scripts.ci.tests.test_job_current_integration_paths import sdk_classification_branches

CI = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(CI))
from scripts.ci import extract_web_fetch_full as full


def native(command):
    records = [{"path": name, "quiescent": True, "requests": count, "stop_elapsed_ms": 2}
               for name, count in focused.WEB_FETCH_REQUEST_COUNTS.items()]
    return ('Command: ' + ' '.join('"' + value + '"' for value in command) + '\n' +
            '[sdk-web-fetch-decoder] {"gzip_decoding":true,"outcome":"download_limit"}\n' +
            '\n'.join('[sdk-web-fetch-path] ' + name for name in focused.WEB_FETCH_REQUEST_COUNTS) + '\n' +
            '\n'.join('[sdk-web-fetch-fixture] ' + json.dumps(record) for record in records) + '\n' +
            '[doctest] test cases: 6 | 6 passed | 0 failed | 23 skipped\n'
            '[doctest] assertions: 54 | 54 passed | 0 failed |\nTest Passed.\n')


def materialize(build, source_root):
    tests, logs = [], []
    for index, (name, (executable, timeout)) in enumerate(full.REQUIRED.items(), 1):
        command = [str(build / 'tests' / executable), full.FILTER]
        tests.append({'name': name, 'command': command, 'properties': [
            {'name': 'TIMEOUT', 'value': timeout}, {'name': 'ENVIRONMENT', 'value': [
                'LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0', 'NO_PROXY=127.0.0.1', 'no_proxy=127.0.0.1']}]})
        logs.append(f'{index}/2 Testing: {name}\n' + native(command))
    (build / 'Testing/Temporary').mkdir(parents=True)
    (build / 'web-fetch-full-registration.json').write_text(json.dumps({'tests': tests}), encoding='utf-8')
    (build / 'Testing/Temporary/LastTest.log').write_text(''.join(logs), encoding='utf-8')
    (build / 'result-store-full-results.xml').write_text('<testsuite>' + ''.join(
        '<testcase name="' + name + '" status="run" />' for name in full.REQUIRED) + '</testsuite>', encoding='utf-8')
    source = source_root / full.SOURCE
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text('pure fake source fixture; never compiled', encoding='utf-8')
    return tests


class WebFetchNativeTests(unittest.TestCase):
    def test_exact_source_and_owned_fixture_receipts_pass_for_both_executables(self):
        for executable in ('lubancore_sdk_tests', 'lubancode_tests'):
            for root in ('/actual/build', 'D:/actual/build'):
                command = [root + '/' + executable, '--source-file=*test_lubancore_web_fetch.cpp']
                focused.check_web_fetch_native(native(command), command)
                focused.check_web_fetch_native(native(command).replace('\n', '\r\n'), command)

    def test_argv_rejects_foreign_relative_duplicate_or_filtered_calls(self):
        good = ['/actual/lubancore_sdk_tests', '--source-file=*test_lubancore_web_fetch.cpp']
        for bad in (None, [], ['lubancore_sdk_tests', good[1]], ['/actual/foreign', good[1]],
                    [good[0], '--source-file=*fetch.cpp'], good + ['--test-case=one']):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError): focused.check_web_fetch_native(native(good), bad)
        with self.assertRaises(RuntimeError): focused.check_web_fetch_native(native(good).replace('/actual/', '/foreign/'), good)

    def test_fixture_bool_count_time_and_complete_paths_are_required(self):
        command = ['/actual/lubancore_sdk_tests', '--source-file=*test_lubancore_web_fetch.cpp']
        good = native(command)
        for old, new in (('"quiescent": true', '"quiescent": 1'), ('"quiescent": true', '"quiescent": false'),
                         ('"requests": 1', '"requests": true'), ('"requests": 1', '"requests": 2'),
                         ('"stop_elapsed_ms": 2', '"stop_elapsed_ms": -1'),
                         ('"stop_elapsed_ms": 2', '"stop_elapsed_ms": 3000'),
                         ('"stop_elapsed_ms": 2', '"stop_elapsed_ms": 1.0'),
                         ('"path": "content"', '"path": "admission"'),
                         ('[sdk-web-fetch-path] admission', '[sdk-web-fetch-path] admission decorated'),
                         ('6 | 6 passed', '0 | 0 passed'), ('54 | 54 passed', '0 | 0 passed'),
                         ('Test Passed.', 'Test Failed.')):
            with self.subTest(new=new), self.assertRaises(RuntimeError):
                focused.check_web_fetch_native(good.replace(old, new, 1), command)
        with self.assertRaises(RuntimeError): focused.check_web_fetch_native(good + good, command)
        with self.assertRaises(RuntimeError): focused.check_web_fetch_native(good.replace('[sdk-web-fetch-fixture]', '[other]'), command)

    def test_native_environment_keeps_both_explicit_loopback_exceptions(self):
        values = ['LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0', 'NO_PROXY=127.0.0.1', 'no_proxy=127.0.0.1']
        focused.check_web_fetch_environment([{'name': 'ENVIRONMENT', 'value': values}])
        for bad in ([], values[:-1], values + ['NO_PROXY=other'], ['NO_PROXY=*', 'no_proxy=127.0.0.1']):
            with self.assertRaises(RuntimeError): focused.check_web_fetch_environment([{'name': 'ENVIRONMENT', 'value': bad}])

    def test_decoder_capability_is_typed_and_selects_one_exact_outcome(self):
        for capability, outcome in ((True, 'download_limit'), (False, 'unsupported_encoding')):
            receipt = '[sdk-web-fetch-decoder] ' + json.dumps({'gzip_decoding': capability, 'outcome': outcome})
            focused.check_web_fetch_decoder(receipt)
            with self.assertRaises(RuntimeError): focused.check_web_fetch_decoder(receipt + '\n' + receipt)
        for capability, outcome in ((1, 'download_limit'), ('true', 'download_limit'),
                                    (True, 'unsupported_encoding'), (False, 'download_limit'),
                                    (False, 'network_error'), (True, 'ok')):
            with self.assertRaises(RuntimeError): focused.check_web_fetch_decoder(
                '[sdk-web-fetch-decoder] ' + json.dumps({'gzip_decoding': capability, 'outcome': outcome}))
        with self.assertRaises(RuntimeError): focused.check_web_fetch_decoder('')


class WebFetchFullTests(unittest.TestCase):
    def scenario(self, mutate=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); build = root / 'build'; build.mkdir()
            tests = materialize(build, root)
            if mutate: mutate(build, tests)
            with patch.object(full, '__file__', str(root / 'scripts/ci/extract_web_fetch_full.py')):
                if mutate:
                    with self.assertRaises((RuntimeError, ValueError)): full.extract(build, 'posix')
                    self.assertEqual(json.loads((build / 'test-evidence/web-fetch-full/manifest.json').read_text())['status'], 'failed')
                else:
                    result = full.extract(build, 'posix')
                    self.assertEqual(result['status'], 'passed')
                    self.assertEqual(set(result['details']), set(full.REQUIRED))
            output = build / 'test-evidence/web-fetch-full'
            self.assertEqual((output / 'registration.json').read_bytes(), (build / 'web-fetch-full-registration.json').read_bytes())
            return

    def test_both_actual_full_sources_keep_original_registration_and_log(self):
        self.scenario()

    def test_missing_duplicate_foreign_argv_or_unbounded_registration_rejected(self):
        def alter(kind):
            def mutate(build, tests):
                if kind == 'missing': tests.pop()
                if kind == 'duplicate': tests.append(deepcopy(tests[0]))
                if kind == 'foreign': tests[0]['command'][0] = str(build.parent / 'foreign/lubancore_sdk_tests')
                if kind == 'timeout': tests[0]['properties'][0]['value'] = 600
                if kind == 'proxy': tests[0]['properties'][1]['value'] = []
                (build / 'web-fetch-full-registration.json').write_text(json.dumps({'tests': tests}), encoding='utf-8')
            return mutate
        for kind in ('missing', 'duplicate', 'foreign', 'timeout', 'proxy'):
            with self.subTest(kind=kind): self.scenario(alter(kind))

    def test_failed_or_unparseable_junit_keeps_failure_native_bytes(self):
        for bad_xml in (b'<broken', b'<testsuite><testcase name="sdk.focused.lubancore_web_fetch" status="run"><failure/></testcase></testsuite>'):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory); build = root / 'build'; build.mkdir(); materialize(build, root)
                (build / 'result-store-full-results.xml').write_bytes(bad_xml)
                log = build / 'Testing/Temporary/LastTest.log'; raw = log.read_bytes().replace(b'Test Passed.', b'Test Failed.')
                log.write_bytes(raw)
                with patch.object(full, '__file__', str(root / 'scripts/ci/extract_web_fetch_full.py')):
                    with self.assertRaises(RuntimeError): full.extract(build, 'posix')
                output = build / 'test-evidence/web-fetch-full'
                self.assertEqual((output / 'LastTest.log').read_bytes(), raw)
                if bad_xml == b'<broken': self.assertEqual((output / 'junit-unparsed.xml').read_bytes(), bad_xml)


class WebFetchOwnershipTests(unittest.TestCase):
    def test_actual_helper_owners_must_match_testing_and_cli_modes(self):
        for testing in (False, True):
            for cli in (False, True):
                names = [] if not testing else ['lubancore_sdk_tests'] + (['lubancode_tests'] if cli else [])
                targets = {name: {'name': name, 'type': 'EXECUTABLE', 'luaSources': [fixture.HELPER]} for name in names}
                self.assertEqual(closure.web_fetch_reference_ownership_violations(targets, testing, cli), [])
                for mutation in ('missing', 'duplicate', 'foreign', 'static'):
                    bad = deepcopy(targets)
                    if mutation == 'missing' and names: del bad[names[0]]
                    elif mutation == 'duplicate' and names: bad[names[0]]['luaSources'].append(fixture.HELPER)
                    elif mutation == 'static' and names: bad[names[0]]['type'] = 'STATIC_LIBRARY'
                    else: bad['foreign'] = {'name': 'lubancore_sdk', 'type': 'SHARED_LIBRARY', 'luaSources': [fixture.HELPER]}
                    self.assertTrue(closure.web_fetch_reference_ownership_violations(bad, testing, cli))
        target = {'name': 'lubancore_sdk_tests', 'type': 'EXECUTABLE', 'sources': [{'compiled': True, 'projectPath': fixture.HELPER}]}
        self.assertEqual(boundary.web_fetch_consumer_ownership_violations({'sdk': target}, True), [])
        self.assertTrue(boundary.web_fetch_consumer_ownership_violations({'sdk': target}, False))
        self.assertTrue(boundary.web_fetch_consumer_ownership_violations({}, True))

    def test_on_off_rosters_and_public_header_remain_required(self):
        for enabled in (False, True):
            self.assertIn('sdk.focused.lubancore_web_fetch', focused_roster(focused.REQUIRED, enabled))
            self.assertIn('sdk.consumer.web_fetch', consumer_roster(installed.REQUIRED_TESTS, enabled))
        self.assertIn('include/lubancore/web_fetch.hpp', installed.REQUIRED_PUBLIC_HEADERS)


class WebFetchActualGraphTests(unittest.TestCase):
    def setUp(self):
        self.fixture = graph_fixtures.BoundaryTests('test_clean_graph_and_recursive_headers_are_recorded')
        self.fixture.setUp(); self.addCleanup(self.fixture.doCleanups)
        self.fixture.flags['BUILD_TESTING'] = 'ON'
        self.fixture.source_file(fixture.HELPER, '#include <lubancore/core.hpp>\n')
        self.target = {'id': 'web', 'name': 'lubancore_sdk_tests', 'type': 'EXECUTABLE',
                       'sources': [{'path': fixture.HELPER, 'compileGroupIndex': 0}], 'compileGroups': [{}]}
        self.fixture.targets.append(self.target)

    def test_actual_file_api_scans_helper_and_its_public_header(self):
        report = self.fixture.check(testing=True)
        self.assertEqual(report['status'], 'passed', report['violations'])
        self.assertIn(fixture.HELPER, report['scannedProjectFiles'])
        self.assertIn({'from': fixture.HELPER, 'to': 'include/lubancore/core.hpp'}, report['projectIncludeEdges'])

    def test_missing_duplicate_foreign_and_non_testing_owner_rejected(self):
        original = deepcopy(self.target)
        for change in ({'sources': []}, {'sources': original['sources'] * 2}, {'name': 'foreign'}, {'type': 'STATIC_LIBRARY'}):
            self.target.update(deepcopy(original)); self.target.update(change)
            self.assertEqual(self.fixture.check(testing=True)['status'], 'failed')
        self.target.update(original); self.fixture.flags['BUILD_TESTING'] = 'OFF'
        self.assertEqual(self.fixture.check()['status'], 'failed')

    def test_nearby_source_or_private_header_cannot_replace_public_helper(self):
        self.fixture.source_file('examples/sdk-consumer/web_fetch_extra.cpp', 'int extra;\n')
        self.target['sources'][0]['path'] = 'examples/sdk-consumer/web_fetch_extra.cpp'
        self.assertEqual(self.fixture.check(testing=True)['status'], 'failed')
        self.target['sources'][0]['path'] = fixture.HELPER
        self.fixture.source_file('src/app/turn_runner.hpp', '#pragma once\n')
        self.fixture.source_file(fixture.HELPER, '#include "app/turn_runner.hpp"\n')
        self.fixture.assert_rejected(self.fixture.check(testing=True), 'reverse host include')


class WebFetchWorkflowTests(unittest.TestCase):
    def test_both_actual_classifiers_cover_sources_fixture_and_exact_gate_paths(self):
        text = (CI.parents[1] / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        exact = ('src/net/http_transport.cpp', 'src/net/http_transport.hpp', 'tests/support/fake_http_server.cpp',
                 'tests/support/fake_http_server.hpp', 'tests/fixtures/sdk_web_fetch_http.py',
                 'scripts/ci/sdk_web_fetch_fixture.py', 'scripts/ci/extract_web_fetch_full.py',
                 'scripts/ci/tests/test_sdk_web_fetch_gates.py', 'scripts/ci/tests/test_sdk_web_fetch_fixture.py')
        for patterns, body in sdk_classification_branches(text):
            self.assertIn('sdk_tests=$sdk_present', body); self.assertIn('cross_platform=true', body)
            for path in exact:
                self.assertEqual(patterns.count(path), 1)
            for path in (*exact, fixture.HELPER, 'include/lubancore/web_fetch.hpp',
                         'tests/integration/sdk/test_lubancore_web_fetch.cpp'):
                self.assertTrue(any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns), path)
            self.assertNotIn('src/net/*', patterns)

    def test_full_originals_are_registered_before_test_and_retained_after_success_or_failure(self):
        text = (CI.parents[1] / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        for start, end in (('  build-test:\n', '  linux-manylinux:\n'), ('  linux-manylinux:\n', '  linux-asan:\n')):
            job = text.split(start, 1)[1].split(end, 1)[0]
            self.assertEqual(job.count('> build/web-fetch-full-registration.json'), 1)
            extract = job.split('      - name: Extract actual full web-fetch source pair\n', 1)[1].split('      - name: ', 1)[0]
            self.assertIn('always()', extract); self.assertIn("steps.test.outcome == 'success' || steps.test.outcome == 'failure'", extract)
            self.assertIn('scripts/ci/extract_web_fetch_full.py --build-dir build', extract)
            upload = job.split('      - name: Upload actual full web-fetch source pair\n', 1)[1].split('      - name: ', 1)[0]
            self.assertIn('path: build/test-evidence/web-fetch-full/', upload); self.assertIn('if-no-files-found: error', upload)
        asan = text.split('  linux-asan:\n', 1)[1]
        from scripts.ci.check_asan_profile import selectors_from_workflow
        # Check actual selector membership rather than requiring WebFetch to
        # remain the last alternative whenever another SDK module is added.
        for selector in selectors_from_workflow(text):
            for name in ('integration.sdk.lubancore_agentic_rag', 'integration.sdk.lubancore_web_fetch'):
                self.assertIsNotNone(re.search(selector['include'], name))
                self.assertFalse(selector['exclude'] and re.search(selector['exclude'], name))
        self.assertIn("'integration.sdk.lubancore_web_fetch',", asan)
        self.assertIn('check_web_fetch_native(sections[0], commands[0])', asan)
        self.assertIn("check_web_fetch_environment(test.get('properties', []))", asan)


if __name__ == '__main__':
    unittest.main()
