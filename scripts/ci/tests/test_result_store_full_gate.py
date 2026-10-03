"""Pure original-selection counterexamples; no native subprocess calls."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

CI = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("result_store_full", CI / "extract_result_store_full.py")
gate = importlib.util.module_from_spec(spec)
with patch.object(sys, "path", [str(CI), *sys.path]):
    spec.loader.exec_module(gate)


def materialize(build, platform='nt'):
    tests, sections, cases = [], [], []
    for index, (name, binary) in enumerate(gate.REQUIRED.items()):
        command = [f'/build/tests/{binary}', gate.FILTER]
        tests.append({'name': name, 'command': command, 'properties': [{'name': 'TIMEOUT', 'value': 180}]})
        owners = ''
        if platform == 'nt':
            for marker, length in [('target-extended', 340), ('temporary-threshold', 247)]:
                owners += f'[result-store-path] {marker}\n'
                owners += f'[result-store-path-length] {marker} target={length} temporary={length+4}\n'
                owners += '[result-store-fixture] ' + json.dumps({
                    'marker': marker, 'root': f'C:/temp/owner-{index}-{marker}', 'cleanup': 'removed'}) + '\n'
        sections.append(f'{index+1}/3 Testing: {name}\nCommand: "{command[0]}" "{command[1]}"\n'
                        + owners + '[doctest] test cases: 17 | 17 passed | 0 failed\n'
                        + '[doctest] assertions: 40 | 40 passed | 0 failed\nTest Passed.\n')
        cases.append(f'<testcase name="{name}" status="run"/>')
    native = ''.join(sections) + '3/3 Testing: unit.foreign.book\nforeign-output-must-not-upload\n'
    (build / 'Testing/Temporary').mkdir(parents=True)
    (build / 'Testing/Temporary/LastTest.log').write_text(native, encoding='utf-8')
    (build / 'result-store-full-registration.json').write_text(json.dumps({'tests': tests}), encoding='utf-8')
    (build / 'result-store-full-results.xml').write_text(
        '<testsuite>' + ''.join(cases) + '<testcase name="unit.foreign.book" status="run"/></testsuite>', encoding='utf-8')
    return native


class ResultStoreFullGateTests(unittest.TestCase):
    def test_two_actual_commands_sections_and_junit_only(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            materialize(build)
            report = gate.extract(build, 'nt')
            self.assertEqual(report['status'], 'passed')
            self.assertEqual(len(report['details']), 2)
            output = build / 'test-evidence/result-store-full'
            self.assertNotIn('foreign-output', (output/'LastTest.log').read_text())
            self.assertNotIn('unit.foreign.book', (output/'results.xml').read_text())
            self.assertEqual(len({r for d in report['details'].values() for r in d['ownedRoots']}), 4)

    def test_posix_requires_same_source_roster_without_windows_claim(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            materialize(build, 'posix')
            report = gate.extract(build, 'posix')
            self.assertTrue(all(d['nativeCases'] == 17 and not d['ownedRoots'] for d in report['details'].values()))

    def test_missing_input_retains_honest_status(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            materialize(build)
            (build/'result-store-full-results.xml').unlink()
            with self.assertRaisesRegex(RuntimeError, 'input is missing'):
                gate.extract(build, 'nt')
            context = json.loads((build/'test-evidence/result-store-full/context.json').read_text())
            self.assertEqual(context['inputs']['junit']['state'], 'missing')
            self.assertEqual(context['status'], 'failed')
            self.assertTrue((build/'test-evidence/result-store-full/LastTest.log').is_file())

    def test_full_failure_still_retains_native_and_junit(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            native = materialize(build)
            (build/'Testing/Temporary/LastTest.log').write_text(native.replace('17 | 17 passed | 0 failed', '17 | 16 passed | 1 failed'), encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError, '17 successful cases'):
                gate.extract(build, 'nt')
            output = build/'test-evidence/result-store-full'
            self.assertIn('16 passed | 1 failed', (output/'LastTest.log').read_text())
            self.assertTrue((output/'results.xml').is_file())
            self.assertEqual(json.loads((output/'context.json').read_text())['status'], 'failed')

    def test_foreign_checkout_same_binary_command_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            native = materialize(build)
            (build/'Testing/Temporary/LastTest.log').write_text(native.replace('/build/tests/lubancore_sdk_tests', '/foreign/tests/lubancore_sdk_tests'), encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError, 'actual command differs'):
                gate.extract(build, 'nt')

    def test_same_root_between_two_executables_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            native = materialize(build)
            (build/'Testing/Temporary/LastTest.log').write_text(native.replace('owner-1-target-extended', 'owner-0-target-extended'), encoding='utf-8')
            with self.assertRaisesRegex(RuntimeError, 'programs reused'):
                gate.extract(build, 'nt')

    def test_missing_duplicate_native_or_registration_reject(self):
        for mode in ['missing', 'duplicate', 'registration']:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                build = Path(directory)
                native = materialize(build)
                if mode == 'missing':
                    native = native[native.index('2/3 Testing:'):]
                elif mode == 'duplicate':
                    native += native[:native.index('2/3 Testing:')]
                else:
                    path=build/'result-store-full-registration.json'
                    data=json.loads(path.read_text()); data['tests'] += data['tests'][:1]
                    path.write_text(json.dumps(data), encoding='utf-8')
                (build/'Testing/Temporary/LastTest.log').write_text(native, encoding='utf-8')
                with self.assertRaisesRegex(RuntimeError, 'missing or duplicated'):
                    gate.extract(build, 'nt')

    def test_invalid_source_bytes_are_saved_before_decode_error(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            native = materialize(build)
            data=native.encode('utf-8').replace(b'Test Passed.', b'\xffTest Passed.', 1)
            (build/'Testing/Temporary/LastTest.log').write_bytes(data)
            with self.assertRaises(UnicodeDecodeError):
                gate.extract(build, 'nt')
            self.assertIn(b'\xff', (build/'test-evidence/result-store-full/LastTest.log').read_bytes())
