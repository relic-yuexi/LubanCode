"""Pure three-source model-input logs; no native executable is created or run."""
from copy import deepcopy
import ast
import fnmatch
import hashlib
import json
from pathlib import Path
import re
import tempfile
import unittest

from scripts.ci import extract_model_input_full as full
from scripts.ci import sdk_model_input as model
from scripts.ci import check_asan_profile as asan

REPO = Path(__file__).resolve().parents[3]


def native_section(stem, command):
    count, prefix, paths = model.SOURCES[stem]
    lines = ['Command: ' + ' '.join('"' + arg + '"' for arg in command),
             f'[doctest] test cases: {count} | {count} passed | 0 failed | 900 skipped',
             '[doctest] assertions: 101 | 101 passed | 0 failed']
    for path in paths:
        lines.append(prefix + path)
    return '\n'.join([*lines, 'Test Passed.', ''])


class FullModelInputEvidenceTests(unittest.TestCase):
    def fixture(self, build):
        tests, sections, cases = [], [], []
        for number, (name, (stem, executable)) in enumerate(full.REQUIRED.items(), 1):
            command = [(build / executable).as_posix(), '--source-file=*test_' + stem + '.cpp']
            tests.append(dict(name=name, command=command, properties=[dict(name='TIMEOUT', value=full.source_timeout(name))]))
            sections.append(f'{number}/3 Testing: {name}\n' + native_section(stem, command))
            cases.append(f'<testcase name="{name}" status="run"/>')
        return {'registration.json': json.dumps({'tests': tests}).encode(),
                'results.xml': ('<testsuite>' + ''.join(cases) + '</testsuite>').encode(),
                'LastTest.log': ''.join(sections).encode()}

    def write(self, build, data):
        files = {'registration.json': build / 'model-input-full-registration.json',
                 'results.xml': build / 'result-store-full-results.xml',
                 'LastTest.log': build / 'Testing/Temporary/LastTest.log'}
        for name, path in files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data[name])
        return files

    def test_three_actual_sources_use_both_executables_and_keep_original_bytes(self):
        for platform in ('nt', 'posix'):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as directory:
                build = Path(directory).resolve(); raw = self.fixture(build); self.write(build, raw)
                result = full.extract(build, platform)
                self.assertEqual(result['status'], 'passed')
                self.assertEqual(set(result['details']), set(full.REQUIRED))
                self.assertEqual(sum(value['nativeCases'] for value in result['details'].values()), 13)
                output = build / 'test-evidence/model-input-full'
                for name, data in raw.items():
                    self.assertEqual((output / name).read_bytes(), data)
                    self.assertEqual(result['inputs'][name]['sha256'], hashlib.sha256(data).hexdigest())

    def test_registration_preserves_unit180_other300_and_exact_owner_argv(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve(); original = json.loads(self.fixture(build)['registration.json'])
            self.assertEqual(full.source_timeout('unit.app.model_input_wrappers'), 180)
            self.assertEqual([full.source_timeout(name) for name in full.REQUIRED].count(300), 2)
            for number in range(3):
                value = deepcopy(original)
                value['tests'][number]['properties'][0]['value'] = 300 if value['tests'][number]['name'].startswith('unit.') else 180
                with self.subTest(timeout=number), self.assertRaises(RuntimeError):
                    full.check_registration(json.dumps(value), build)
            for mutation in ('missing', 'duplicate', 'extra', 'foreign', 'filtered', 'relative', 'disabled', 'property-duplicate'):
                value = deepcopy(original); test = value['tests'][0]
                if mutation == 'missing': value['tests'].pop()
                if mutation == 'duplicate': value['tests'][1] = deepcopy(test)
                if mutation == 'extra': value['tests'].append(deepcopy(test))
                if mutation == 'foreign': test['command'][0] = (build.parent / 'borrowed' / 'lubancore_sdk_tests').as_posix()
                if mutation == 'filtered': test['command'].append('--test-case=fewer')
                if mutation == 'relative': test['command'][0] = 'lubancore_sdk_tests'
                if mutation == 'disabled': test['properties'].append(dict(name='DISABLED', value=True))
                if mutation == 'property-duplicate': test['properties'].append(deepcopy(test['properties'][0]))
                with self.subTest(mutation=mutation), self.assertRaises(RuntimeError):
                    full.check_registration(json.dumps(value), build)

    def test_incomplete_junit_cases_paths_and_borrowed_sources_fail(self):
        for mutation in ('junit-failed', 'junit-notrun', 'junit-missing', 'junit-duplicate',
                         'argv', 'cases', 'assertions', 'summary-path', 'wrapper-path',
                         'duplicate-path', 'borrowed-source', 'native-duplicate'):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                build = Path(directory).resolve(); raw = self.fixture(build)
                if mutation == 'junit-failed': raw['results.xml'] = raw['results.xml'].replace(b'/>', b'><failure/></testcase>', 1)
                if mutation == 'junit-notrun': raw['results.xml'] = raw['results.xml'].replace(b'status="run"', b'status="notrun"', 1)
                if mutation in ('junit-missing', 'junit-duplicate'):
                    first = re.search(rb'<testcase[^>]+/>', raw['results.xml']).group()
                    raw['results.xml'] = raw['results.xml'].replace(first, b'' if mutation == 'junit-missing' else first * 2, 1)
                if mutation == 'argv': raw['LastTest.log'] = raw['LastTest.log'].replace(b'*test_lubancore_model_input.cpp', b'*test_other.cpp', 1)
                if mutation == 'cases': raw['LastTest.log'] = raw['LastTest.log'].replace(b'6 | 6 passed', b'5 | 5 passed', 1)
                if mutation == 'assertions': raw['LastTest.log'] = raw['LastTest.log'].replace(b'101 | 101 passed', b'0 | 0 passed', 1)
                if mutation == 'summary-path': raw['LastTest.log'] = raw['LastTest.log'].replace(b'[sdk-model-input-path] summary-fail-closed', b'missing', 1)
                if mutation == 'wrapper-path': raw['LastTest.log'] = raw['LastTest.log'].replace(b'[model-input-wrappers-path] spinner-three-state', b'missing', 1)
                if mutation == 'duplicate-path': raw['LastTest.log'] = raw['LastTest.log'].replace(b'[sdk-model-input-path] exact-generate', b'[sdk-model-input-path] exact-generate\n[sdk-model-input-path] exact-generate', 1)
                if mutation == 'borrowed-source': raw['LastTest.log'] = raw['LastTest.log'].replace(b'[sdk-model-input-path] exact-generate', b'[sdk-model-input-path] exact-generate\n[model-input-wrappers-path] spinner-three-state', 1)
                if mutation == 'native-duplicate': raw['LastTest.log'] *= 2
                self.write(build, raw)
                with self.assertRaises((RuntimeError, ValueError)): full.extract(build, 'posix')
                output = build / 'test-evidence/model-input-full'
                self.assertEqual(json.loads((output / 'context.json').read_text())['status'], 'failed')
                self.assertEqual((output / 'LastTest.log').read_bytes(), raw['LastTest.log'])

    def test_missing_or_aborted_originals_never_reuse_a_stale_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory).resolve(); raw = self.fixture(build); inputs = self.write(build, raw)
            full.extract(build, 'posix')
            corrupt = raw['LastTest.log'] + b'\xff\x80\n'; inputs['LastTest.log'].write_bytes(corrupt)
            with self.assertRaises(UnicodeDecodeError): full.extract(build, 'posix')
            output = build / 'test-evidence/model-input-full'
            self.assertEqual((output / 'LastTest.log').read_bytes(), corrupt)
            inputs['results.xml'].unlink()
            with self.assertRaises(RuntimeError): full.extract(build, 'posix')
            result = json.loads((output / 'context.json').read_text())
            self.assertEqual(result['status'], 'failed'); self.assertNotIn('details', result)
            self.assertFalse((output / 'results.xml').exists())
            self.assertEqual((output / 'registration.json').read_bytes(), raw['registration.json'])
            self.assertIn('error', result['inputs']['results.xml'])

    def test_workflow_routes_both_asan_selectors_and_literal_use_real_sources(self):
        text = (REPO / '.github/workflows/ci.yml').read_text()
        patterns = re.findall(r'^\s*([^\n]*scripts/ci/sdk_model_input\.py[^\n]*)\)\s*$', text, re.M)
        self.assertEqual(len(patterns), 2)
        for pattern in patterns:
            for path in ('scripts/ci/sdk_model_input.py', 'scripts/ci/tests/test_sdk_model_input_gates.py',
                         'scripts/ci/extract_model_input_full.py', 'scripts/ci/tests/test_model_input_full_gate.py',
                         'src/api/backend.hpp', 'src/api/model_input_snapshot.hpp',
                         'src/cli/spinner_backend.cpp', 'src/cli/spinner_backend.hpp',
                         'src/runtime/assembly/backend.cpp', 'src/runtime/assembly/backend.hpp',
                         'tests/unit/app/test_model_input_wrappers.cpp'):
                self.assertTrue(any(fnmatch.fnmatchcase(path, part) for part in pattern.strip().split('|')), path)
        self.assertEqual(text.count('> build/model-input-full-registration.json'), 2)
        self.assertEqual(text.count('scripts/ci/extract_model_input_full.py --build-dir build'), 2)
        manifest = asan.make_manifest(REPO)
        for name, stem in full.ORIGINALS.items():
            for selected in manifest['selected']: self.assertIn(name, selected)
            self.assertIn(manifest['roster'][name], manifest['compile_sources'])
        block = re.search(r'^  linux-asan:\n(.*?)(?=^  [A-Za-z][\w-]*:|\Z)', text, re.M | re.S).group(1)
        literal = [value for value in re.findall(r'required = (\{[^}]+\})', block)
                   if isinstance(ast.parse(value, mode='eval').body, ast.Set)]
        self.assertEqual(len(literal), 1)
        self.assertEqual(ast.literal_eval(literal[0]), set(manifest['selected'][0]))


if __name__ == '__main__':
    unittest.main()
