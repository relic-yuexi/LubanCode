"""Pure original-log fixtures; never configure or run a native executable."""
from copy import deepcopy
import ast
import fnmatch
import hashlib
import json
from pathlib import Path
import re
import tempfile
import unittest

from scripts.ci import extract_named_results_full as full
from scripts.ci import sdk_named_results as named
from scripts.ci import check_asan_profile as asan

REPO = Path(__file__).resolve().parents[3]


def summary():
    return dict(session_id='session', tool_call_id='action', summary_event_id='summary',
                source_revision=3, model_calls=2, raw_results=1, formal_results=1,
                local_named_mirrors=0, summary_candidates=2, selected=True,
                subsequent_request_verified=True)


def native_section(stem, command):
    count, prefix, paths = named.SOURCES[stem]
    lines = ['Command: ' + ' '.join('"' + arg + '"' for arg in command),
             f'[doctest] test cases: {count} | {count} passed | 0 failed',
             '[doctest] assertions: 91 | 91 passed | 0 failed',
             *(prefix + path for path in paths)]
    if stem == 'lubancore_named_results':
        lines.append('[sdk-named-results-summary] ' + json.dumps(summary()))
    return '\n'.join([*lines, 'Test Passed.', ''])


class FullEvidenceTests(unittest.TestCase):
    def fixture(self, build):
        tests, sections, cases = [], [], []
        for number, (name, (stem, executable)) in enumerate(full.REQUIRED.items(), 1):
            command = [(build / executable).as_posix(), '--source-file=*test_' + stem + '.cpp']
            tests.append(dict(name=name, command=command, properties=[dict(name='TIMEOUT', value=300)]))
            sections.append(f'{number}/4 Testing: {name}\n' + native_section(stem, command))
            cases.append(f'<testcase name="{name}" status="run"/>')
        return {'registration.json': json.dumps({'tests': tests}).encode(),
                'results.xml': ('<testsuite>' + ''.join(cases) + '</testsuite>').encode(),
                'LastTest.log': ''.join(sections).encode()}

    def write(self, build, data):
        originals = {'registration.json': build / 'named-results-full-registration.json',
                     'results.xml': build / 'result-store-full-results.xml',
                     'LastTest.log': build / 'Testing/Temporary/LastTest.log'}
        for name, path in originals.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data[name])
        return originals

    def test_original_and_focused_quartet_preserves_exact_input_bytes(self):
        for platform in ('nt', 'posix'):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as directory:
                build = Path(directory).resolve()
                raw = self.fixture(build); self.write(build, raw)
                result = full.extract(build, platform)
                self.assertEqual(result['status'], 'passed')
                self.assertEqual(set(result['details']), set(full.REQUIRED))
                self.assertEqual(sum(item['nativeCases'] for item in result['details'].values()), 36)
                output = build / 'test-evidence/named-results-full'
                for name, value in raw.items():
                    self.assertEqual((output / name).read_bytes(), value)
                    self.assertEqual(result['inputs'][name]['sha256'], hashlib.sha256(value).hexdigest())

    def test_registration_cannot_change_source_owner_roster_or_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve(); original = json.loads(self.fixture(build)['registration.json'])
            for mutation in ('missing', 'duplicate', 'extra', 'foreign', 'filtered', 'relative',
                             'disabled', 'timeout', 'duplicate-property'):
                value = deepcopy(original); test = value['tests'][0]
                if mutation == 'missing': value['tests'].pop()
                if mutation == 'duplicate': value['tests'][1] = deepcopy(test)
                if mutation == 'extra': value['tests'].append(deepcopy(test))
                if mutation == 'foreign': test['command'][0] = (build.parent / 'other-build' / 'lubancore_sdk_tests').as_posix()
                if mutation == 'filtered': test['command'].append('--test-case=less')
                if mutation == 'relative': test['command'][0] = 'lubancore_sdk_tests'
                if mutation == 'disabled': test['properties'].append(dict(name='DISABLED', value=True))
                if mutation == 'timeout': test['properties'][0]['value'] = 301
                if mutation == 'duplicate-property': test['properties'].append(deepcopy(test['properties'][0]))
                with self.subTest(mutation=mutation), self.assertRaises(RuntimeError):
                    full.check_registration(json.dumps(value), build)

    def test_failed_junit_or_incomplete_native_source_never_becomes_passed(self):
        for mutation in ('junit-failed', 'junit-skipped', 'junit-missing', 'junit-duplicate',
                         'native-source', 'native-cases', 'native-assertions', 'native-path',
                         'native-summary', 'native-duplicate', 'summary-bool'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                build = Path(directory).resolve(); raw = self.fixture(build)
                if mutation == 'junit-failed': raw['results.xml'] = raw['results.xml'].replace(b'/>', b'><failure/></testcase>', 1)
                if mutation == 'junit-skipped': raw['results.xml'] = raw['results.xml'].replace(b'status="run"', b'status="notrun"', 1)
                if mutation in ('junit-missing', 'junit-duplicate'):
                    first = re.search(rb'<testcase[^>]+/>', raw['results.xml']).group()
                    raw['results.xml'] = raw['results.xml'].replace(first, b'' if mutation == 'junit-missing' else first * 2, 1)
                if mutation == 'native-source': raw['LastTest.log'] = raw['LastTest.log'].replace(b'*test_lubancore_named_results.cpp', b'*test_other.cpp', 1)
                if mutation == 'native-cases': raw['LastTest.log'] = raw['LastTest.log'].replace(b'10 | 10 passed', b'9 | 9 passed', 1)
                if mutation == 'native-assertions': raw['LastTest.log'] = raw['LastTest.log'].replace(b'91 | 91 passed', b'0 | 0 passed', 1)
                if mutation == 'native-path': raw['LastTest.log'] = raw['LastTest.log'].replace(b'[sdk-named-results-path] close-read', b'missing', 1)
                if mutation == 'native-summary': raw['LastTest.log'] = re.sub(rb'^\[sdk-named-results-summary\].*\n', b'', raw['LastTest.log'], count=1, flags=re.M)
                if mutation == 'native-duplicate': raw['LastTest.log'] *= 2
                if mutation == 'summary-bool': raw['LastTest.log'] = raw['LastTest.log'].replace(b'"source_revision": 3', b'"source_revision": true', 1)
                self.write(build, raw)
                with self.assertRaises((RuntimeError, ValueError)):
                    full.extract(build, 'posix')
                output = build / 'test-evidence/named-results-full'
                self.assertEqual(json.loads((output / 'context.json').read_text())['status'], 'failed')
                self.assertEqual((output / 'LastTest.log').read_bytes(), raw['LastTest.log'])

    def test_abort_and_invalid_utf8_preserve_originals_and_remove_stale_success(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve(); raw = self.fixture(build); files = self.write(build, raw)
            full.extract(build, 'posix')
            broken = raw['LastTest.log'] + b'\xff\x80\n'; files['LastTest.log'].write_bytes(broken)
            with self.assertRaises(UnicodeDecodeError): full.extract(build, 'posix')
            output = build / 'test-evidence/named-results-full'
            self.assertEqual((output / 'LastTest.log').read_bytes(), broken)
            files['results.xml'].unlink()
            with self.assertRaises(RuntimeError): full.extract(build, 'posix')
            report = json.loads((output / 'context.json').read_text())
            self.assertEqual(report['status'], 'failed')
            self.assertNotIn('details', report)
            self.assertFalse((output / 'results.xml').exists())
            self.assertIn('error', report['inputs']['results.xml'])
            self.assertEqual((output / 'registration.json').read_bytes(), raw['registration.json'])

    def test_actual_workflow_routes_and_asan_literal_cover_both_sources(self):
        text = (REPO / '.github/workflows/ci.yml').read_text()
        classifiers = re.findall(r'^\s*([^\n]*scripts/ci/sdk_named_results\.py[^\n]*)\)\s*$', text, re.M)
        self.assertEqual(len(classifiers), 2)
        dependencies = ('scripts/ci/sdk_named_results.py', 'scripts/ci/extract_named_results_full.py',
                        'scripts/ci/tests/test_named_results_full_gate.py', 'scripts/ci/tests/test_sdk_named_results_gates.py',
                        *named.ENGINE_IMPLEMENTATIONS, named.ADAPTER, named.HEADER, named.HELPER,
                        'src/runtime/action_summary.cpp')
        for pattern in classifiers:
            for path in dependencies:
                self.assertTrue(any(fnmatch.fnmatchcase(path, item) for item in pattern.strip().split('|')), path)
        self.assertEqual(text.count('> build/named-results-full-registration.json'), 2)
        self.assertEqual(text.count('scripts/ci/extract_named_results_full.py --build-dir build'), 2)
        manifest = asan.make_manifest(REPO)
        for stem in named.SOURCES:
            for selected in manifest['selected']:
                self.assertIn('integration.sdk.' + stem, selected)
            self.assertIn('tests/integration/sdk/test_' + stem + '.cpp', manifest['compile_sources'])
        for path in (named.ADAPTER, named.HELPER):
            self.assertEqual(manifest['attachments'].count(path), 1)
        block = re.search(r'^  linux-asan:\n(.*?)(?=^  [A-Za-z][\w-]*:|\Z)', text, re.M | re.S).group(1)
        literal = [value for value in re.findall(r'required = (\{[^}]+\})', block)
                   if isinstance(ast.parse(value, mode='eval').body, ast.Set)]
        self.assertEqual(len(literal), 1)
        self.assertEqual(ast.literal_eval(literal[0]), set(manifest['selected'][0]))


if __name__ == '__main__':
    unittest.main()
