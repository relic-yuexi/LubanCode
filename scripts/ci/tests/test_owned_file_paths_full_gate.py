"""Full-run data counterexamples; fixture text is never native evidence."""
from copy import deepcopy
import ast
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

from scripts.ci import extract_owned_file_paths_full as full
from scripts.ci import sdk_owned_file_paths as gate
from scripts.ci import check_asan_profile as asan
from scripts.ci.tests.test_sdk_owned_file_paths_gates import HEAD, section, source_bytes, source_tree

REPO = Path(__file__).resolve().parents[3]


@patch.dict(os.environ, {'GITHUB_SHA': HEAD})
class FullOwnedFilePathsTests(unittest.TestCase):
    def fixture(self, build, platform='posix'):
        tests, native, cases = [], [], []
        for index, (name, exe) in enumerate(full.REQUIRED.items(), 1):
            command = [(build / (exe + ('.exe' if platform == 'nt' else ''))).as_posix(), '--source-file=*test_' + gate.STEM + '.cpp']
            tests.append({'name': name, 'command': command, 'properties': [{'name': 'TIMEOUT', 'value': 300}]})
            native.append(f'{index}/2 Testing: {name}\n' + section(command, platform))
            cases.append(f'<testcase name="{name}" status="run"/>')
        return {'registration.json': json.dumps({'tests': tests}).encode(),
                'results.xml': ('<testsuite>' + ''.join(cases) + '</testsuite>').encode(), 'LastTest.log': ''.join(native).encode()}

    def write(self, build, raw):
        paths = {'registration.json': build / 'owned-file-paths-full-registration.json',
                 'results.xml': build / 'result-store-full-results.xml', 'LastTest.log': build / 'Testing/Temporary/LastTest.log'}
        for name, path in paths.items():
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(raw[name])
        return paths

    def test_two_actual_owners_require_source_bytes_head_and_both_platform_kinds(self):
        for platform in ('posix', 'nt'):
            with self.subTest(platform=platform), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); source = source_tree(root / 'source'); build = root / 'build'
                raw = self.fixture(build, platform); self.write(build, raw)
                result = full.extract(build, platform, source); output = build / 'test-evidence/owned-file-paths-full'
                self.assertEqual(result['status'], 'passed'); self.assertEqual(result['githubSha'], HEAD)
                self.assertEqual(sum(row['nativeCases'] for row in result['details'].values()), 12)
                for name, data in raw.items():
                    self.assertEqual((output / name).read_bytes(), data)
                    self.assertEqual(result['inputs'][name]['sha256'], hashlib.sha256(data).hexdigest())
                self.assertEqual((output / gate.SOURCE_COPY).read_bytes(), source_bytes())
                self.assertEqual(result['source']['sha256'], hashlib.sha256(source_bytes()).hexdigest())

    def test_registration_requires_complete_pair_exact_argv_and_original300s(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory); baseline = json.loads(self.fixture(build)['registration.json'])
            for kind in ('missing', 'duplicate', 'extra', 'foreign', 'relative', 'filtered', 'swapped-owner', 'disabled', 'timeout', 'bool-timeout', 'duplicate-property'):
                value = deepcopy(baseline); test = value['tests'][0]
                if kind == 'missing': value['tests'].pop()
                if kind == 'duplicate': value['tests'][1] = deepcopy(test)
                if kind == 'extra': value['tests'].append(deepcopy(test))
                if kind == 'foreign': test['command'][0] = (build.parent / 'foreign/lubancore_sdk_tests').as_posix()
                if kind == 'relative': test['command'][0] = 'lubancore_sdk_tests'
                if kind == 'filtered': test['command'].append('--test-case=one')
                if kind == 'swapped-owner': test['command'][0] = (build / 'lubancode_tests').as_posix()
                if kind == 'disabled': test['properties'].append({'name': 'DISABLED', 'value': True})
                if kind == 'timeout': test['properties'][0]['value'] = 180
                if kind == 'bool-timeout': test['properties'][0]['value'] = True
                if kind == 'duplicate-property': test['properties'].append(deepcopy(test['properties'][0]))
                with self.subTest(kind=kind), self.assertRaises(RuntimeError): full.check_registration(json.dumps(value), build)

    def test_failed_partial_foreign_and_proof_stdout_never_replace_real_full_inputs(self):
        for kind in ('failed-junit', 'notrun-junit', 'missing-junit', 'duplicate-junit', 'foreign-command', 'partial-cases',
                     'zero-assertions', 'missing-path', 'duplicate-path', 'foreign-path', 'wrong-kind', 'missing-kind', 'proof-stdout', 'duplicate-native'):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as directory:
                root = Path(directory); source = source_tree(root / 'source'); build = root / 'build'; raw = self.fixture(build)
                if kind == 'failed-junit': raw['results.xml'] = raw['results.xml'].replace(b'/>', b'><failure/></testcase>', 1)
                if kind == 'notrun-junit': raw['results.xml'] = raw['results.xml'].replace(b'status="run"', b'status="notrun"', 1)
                if kind in ('missing-junit', 'duplicate-junit'):
                    first = re.search(rb'<testcase[^>]+/>', raw['results.xml']).group()
                    raw['results.xml'] = raw['results.xml'].replace(first, b'' if kind == 'missing-junit' else first * 2, 1)
                if kind == 'foreign-command': raw['LastTest.log'] = raw['LastTest.log'].replace(b'*test_lubancore_owned_file_paths.cpp', b'*test_other.cpp', 1)
                if kind == 'partial-cases': raw['LastTest.log'] = raw['LastTest.log'].replace(b'6 | 6 passed', b'5 | 5 passed', 1)
                if kind == 'zero-assertions': raw['LastTest.log'] = raw['LastTest.log'].replace(b'101 | 101 passed', b'0 | 0 passed', 1)
                marker = (gate.PREFIX + gate.PATHS[0]).encode()
                if kind == 'missing-path': raw['LastTest.log'] = raw['LastTest.log'].replace(marker, b'missing', 1)
                if kind == 'duplicate-path': raw['LastTest.log'] = raw['LastTest.log'].replace(marker, marker + b'\n' + marker, 1)
                if kind == 'foreign-path': raw['LastTest.log'] = raw['LastTest.log'].replace(marker, marker + b'\n[sdk-model-input-path] exact-generate', 1)
                if kind == 'wrong-kind': raw['LastTest.log'] = raw['LastTest.log'].replace(b'terminal-file-symlink', b'terminal-directory-junction')
                if kind == 'missing-kind': raw['LastTest.log'] = raw['LastTest.log'].replace((gate.LINK_PREFIX + gate.LINK_KINDS['posix']).encode(), b'missing', 1)
                if kind == 'proof-stdout': raw['LastTest.log'] = raw['LastTest.log'].replace(b'[doctest] test cases:', b'proof: [doctest] test cases:')
                if kind == 'duplicate-native': raw['LastTest.log'] *= 2
                self.write(build, raw)
                with self.assertRaises(RuntimeError): full.extract(build, 'posix', source)
                output = build / 'test-evidence/owned-file-paths-full'
                self.assertEqual(json.loads((output / 'context.json').read_bytes())['status'], 'failed')
                for name, value in raw.items(): self.assertEqual((output / name).read_bytes(), value)
                self.assertEqual((output / gate.SOURCE_COPY).read_bytes(), source_bytes())

    def test_missing_corrupt_source_and_head_failures_preserve_originals_and_clear_old_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = source_tree(root / 'source'); build = root / 'build'; raw = self.fixture(build); paths = self.write(build, raw)
            full.extract(build, 'posix', source); output = build / 'test-evidence/owned-file-paths-full'
            corrupt = raw['LastTest.log'] + b'\xff'; paths['LastTest.log'].write_bytes(corrupt)
            with self.assertRaises(UnicodeDecodeError): full.extract(build, 'posix', source)
            self.assertEqual((output / 'LastTest.log').read_bytes(), corrupt)
            paths['results.xml'].unlink()
            with self.assertRaises(RuntimeError): full.extract(build, 'posix', source)
            report = json.loads((output / 'context.json').read_bytes()); self.assertEqual(report['status'], 'failed'); self.assertNotIn('details', report)
            self.assertFalse((output / 'results.xml').exists()); self.write(build, raw)
            (source / '.git/HEAD').write_text('b' * 40)
            with self.assertRaises(RuntimeError): full.extract(build, 'posix', source)
            self.assertEqual((output / gate.SOURCE_COPY).read_bytes(), source_bytes())
            (source / gate.SOURCE).unlink()
            with self.assertRaises(OSError): full.extract(build, 'posix', source)
            for name, value in raw.items(): self.assertEqual((output / name).read_bytes(), value)
            self.assertFalse((output / gate.SOURCE_COPY).exists())

    def test_workflow_routes_preserve_same_full_run_and_both_asan_selectors(self):
        text = (REPO / '.github/workflows/ci.yml').read_text(encoding='utf-8')
        patterns = re.findall(r'^\s*([^\n]*scripts/ci/sdk_owned_file_paths\.py[^\n]*)\)\s*$', text, re.M)
        self.assertEqual(len(patterns), 2)
        for pattern in patterns:
            for path in ('scripts/ci/sdk_owned_file_paths.py', 'scripts/ci/tests/test_sdk_owned_file_paths_gates.py',
                         'scripts/ci/extract_owned_file_paths_full.py', 'scripts/ci/tests/test_owned_file_paths_full_gate.py',
                         'src/platform/owned_file_path.hpp', gate.SOURCE):
                self.assertTrue(any(fnmatch.fnmatchcase(path, part) for part in pattern.strip().split('|')), path)
        self.assertEqual(text.count('> build/owned-file-paths-full-registration.json'), 2)
        self.assertEqual(text.count('scripts/ci/extract_owned_file_paths_full.py --build-dir build'), 2)
        for selector in asan.selectors_from_workflow(text):
            self.assertRegex('integration.sdk.' + gate.STEM, selector['include'])
            self.assertFalse(selector['exclude'] and re.search(selector['exclude'], 'integration.sdk.' + gate.STEM))
        block = re.search(r'^  linux-asan:\n(.*?)(?=^  [A-Za-z][\w-]*:|\Z)', text, re.M | re.S).group(1)
        literal = [ast.literal_eval(value) for value in re.findall(r'required = (\{[^}]+\})', block)
                   if isinstance(ast.parse(value, mode='eval').body, ast.Set)]
        self.assertEqual(len(literal), 1); self.assertIn('integration.sdk.' + gate.STEM, literal[0])
        self.assertIn('build/asan-owned-file-paths.json', block)


if __name__ == '__main__':
    unittest.main()
