"""Synthetic parser/source/graph counterexamples only; never native evidence."""
from copy import deepcopy
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts.ci import sdk_owned_file_paths as gate

HEAD = 'a' * 40


def source_bytes():
    lines = ['void Mark(const char* value) { std::cout << "' + gate.PREFIX + '" << value; }',
             'void TerminalLink() {', '#ifdef _WIN32',
             'std::cout << "' + gate.LINK_PREFIX + gate.LINK_KINDS['nt'] + '\\n";', '#else',
             'std::cout << "' + gate.LINK_PREFIX + gate.LINK_KINDS['posix'] + '\\n";', '#endif', '}']
    for index, path in enumerate(gate.PATHS):
        loop = 'for (const bool dangling : {false, true}) { TerminalLink(); }' if index in (2, 4, 5) else ''
        lines.append('TEST_CASE("parser fixture ' + str(index) + '") { ' + loop + ' Mark("' + path + '"); }')
    return ('\n'.join(lines) + '\n').encode()


def source_record(raw=None):
    raw = source_bytes() if raw is None else raw
    return {'schemaVersion': 1, 'source': gate.SOURCE, 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest(),
            'status': 'source_verified', 'checkoutHead': HEAD, **gate.source_contract(raw)}


def source_tree(root):
    root = Path(root); path = root / gate.SOURCE; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(source_bytes())
    (root / '.git').mkdir(); (root / '.git/HEAD').write_text(HEAD + '\n')
    return root


def argv(exe='lubancore_sdk_tests', windows=False):
    return [('C:/actual/' if windows else '/actual/') + exe + ('.exe' if windows else ''), '--source-file=*test_' + gate.STEM + '.cpp']


def section(command, platform_name='posix'):
    return '\n'.join(['Command: ' + ' '.join('"' + arg + '"' for arg in command),
        *(gate.PREFIX + path for path in gate.PATHS), *([gate.LINK_PREFIX + gate.LINK_KINDS[platform_name]] * 6),
        '[doctest] test cases: 6 | 6 passed | 0 failed | 500 skipped',
        '[doctest] assertions: 101 | 101 passed | 0 failed |', 'Test Passed.']) + '\n'


@patch.dict(os.environ, {'GITHUB_SHA': HEAD})
class OwnedFilePathsGateTests(unittest.TestCase):
    def test_real_argv_platform_kind_and_source_contract(self):
        for platform in ('posix', 'nt'):
            for executable in ('lubancore_sdk_tests', 'lubancode_tests'):
                command = argv(executable, platform == 'nt')
                value = gate.check_native(section(command, platform), command, source_record(), source_bytes(), HEAD, platform)
                self.assertEqual(value['nativeCases'], 6); self.assertEqual(value['nativeAssertions'], 101)
                self.assertEqual(value['terminalLinkKinds'], [gate.LINK_KINDS[platform]] * 6)

    def test_marker_alone_and_proof_stdout_cannot_replace_execution(self):
        command = argv(); valid = section(command)
        for changed in ('\n'.join(gate.PREFIX + p for p in gate.PATHS),
                        json.dumps({'proof': valid, 'status': 'passed', 'cases': 6}),
                        valid.replace('[doctest] test cases:', 'proof: [doctest] test cases:'),
                        valid.replace('[doctest] assertions:', 'proof: [doctest] assertions:'),
                        valid.replace('Command: ', 'proof: Command: '),
                        valid.replace('Test Passed.', 'proof: Test Passed.')):
            with self.subTest(changed=changed[:50]), self.assertRaises(RuntimeError):
                gate.check_native(changed, command, source_record(), source_bytes(), HEAD, 'posix')

    def test_partial_failed_skipped_duplicate_and_borrowed_logs_rejected(self):
        command = argv(); valid = section(command)
        for changed in (valid.replace('6 | 6 passed', '5 | 5 passed'), valid.replace('6 passed | 0 failed', '5 passed | 1 failed'),
                        valid.replace('101 | 101 passed', '0 | 0 passed'), valid.replace('101 | 101 passed', '101 | 100 passed'),
                        valid + '[doctest] assertions: 1 | 1 passed | 0 failed\n', valid + '[doctest] test cases: 6 | 6 passed | 0 failed\n',
                        valid.replace('Test Passed.', 'Test Failed.'), valid + 'Test Passed.\n', valid + 'Test Not Run.\n', valid + 'SKIPPED: unavailable\n',
                        valid + 'Command: other\n', valid.replace(command[0], '/borrowed/lubancore_sdk_tests'),
                        valid.replace(gate.PREFIX + gate.PATHS[0], 'missing'), valid + gate.PREFIX + gate.PATHS[0] + '\n',
                        valid + gate.PREFIX + 'foreign\n', valid + '[sdk-model-input-path] exact-generate\n'):
            with self.subTest(changed=changed[-70:]), self.assertRaises(RuntimeError):
                gate.check_native(changed, command, source_record(), source_bytes(), HEAD, 'posix')

    def test_terminal_kind_requires_six_real_platform_facts(self):
        command = argv(); valid = section(command); line = gate.LINK_PREFIX + gate.LINK_KINDS['posix'] + '\n'
        for changed in (valid.replace(line, '', 1), valid + line,
                        valid.replace(gate.LINK_KINDS['posix'], gate.LINK_KINDS['nt']), valid.replace(line, gate.LINK_PREFIX + 'proof\n', 1)):
            with self.subTest(changed=changed[-50:]), self.assertRaises(RuntimeError):
                gate.check_native(changed, command, source_record(), source_bytes(), HEAD, 'posix')

    def test_registration_refuses_foreign_filter_arguments_and_owner(self):
        command = argv()
        for changed in (None, [], [None, command[1]], [command[0]], [*command, '--test-case=one'],
                        ['lubancore_sdk_tests', command[1]], ['/actual/other', command[1]],
                        ['/actual/lubancode_tests', command[1]], [command[0], '--source-file=*other.cpp']):
            with self.subTest(changed=changed), self.assertRaises(RuntimeError): gate.check_registration(changed)

    def test_source_hash_head_contract_and_extra_fields_rejected(self):
        raw = source_bytes(); record = source_record()
        for key, value in [('sha256', '0' * 64), ('checkoutHead', 'b' * 40), ('source', 'tests/foreign.cpp'),
                           ('bytes', len(raw) + 1), ('bytes', True), ('schemaVersion', True), ('status', 'captured'),
                           ('paths', []), ('cases', 5), ('terminalLinkFacts', 1)]:
            changed = deepcopy(record); changed[key] = value
            with self.subTest(key=key), self.assertRaises(RuntimeError): gate.check_source_copy(raw, changed, HEAD)
        changed = {**record, 'stdoutProof': 'passed'}
        with self.assertRaises(RuntimeError): gate.check_source_copy(raw, changed, HEAD)
        with self.assertRaises(RuntimeError): gate.check_source_copy(raw + b'// drift\n', record, HEAD)

    def test_source_comments_strings_missing_or_dynamic_paths_do_not_count(self):
        raw = source_bytes()
        for changed in (raw.replace(b'Mark("public-roundtrip");', b'// Mark("public-roundtrip");'),
                        raw.replace(b'Mark("public-roundtrip");', b'Mark(dynamic);'),
                        raw + b'Mark("public-roundtrip");\n', raw.replace(b'TEST_CASE(', b'// TEST_CASE(', 1),
                        raw.replace(b'false, true', b'false', 1),
                        raw.replace(b'void TerminalLink()', b'void Other()')):
            with self.subTest(changed=changed[-60:]), self.assertRaises(RuntimeError): gate.source_contract(changed)
        self.assertEqual(gate.source_contract(raw + b'// TEST_CASE(fake) Mark("fake");\n')['cases'], 6)

    def test_capture_retains_original_bytes_and_failed_head_does_not_reuse_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            root = source_tree(Path(directory) / 'source'); output = Path(directory) / 'evidence'
            record = gate.capture_source(root, output)
            self.assertEqual(record['checkoutHead'], HEAD)
            self.assertEqual(gate.read_source_evidence(root, output)['bytes'], source_bytes())
            (root / '.git/HEAD').write_text('b' * 40)
            with self.assertRaises(RuntimeError): gate.capture_source(root, output)
            self.assertEqual((output / gate.SOURCE_COPY).read_bytes(), source_bytes())
            self.assertEqual(json.loads((output / gate.SOURCE_RECORD).read_bytes())['status'], 'failed')

    def test_checkout_symbolic_worktree_and_packed_refs_are_read_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = source_tree(Path(directory) / 'source'); gitdir = root / '.git'
            (gitdir / 'HEAD').write_text('ref: refs/heads/fixture\n'); (gitdir / 'packed-refs').write_text(HEAD + ' refs/heads/fixture\n')
            self.assertEqual(gate.checkout_head(root), HEAD)
            worktree = Path(directory) / 'other'; worktree.mkdir(); admin = gitdir / 'worktrees/test'; admin.mkdir(parents=True)
            (worktree / '.git').write_text('gitdir: ' + str(admin)); (admin / 'HEAD').write_text('ref: refs/heads/fixture\n')
            (admin / 'commondir').write_text('../..')
            self.assertEqual(gate.checkout_head(worktree), HEAD)

    def test_configured_source_owners_reject_missing_duplicate_foreign_and_testing_off(self):
        def target(name, kind='EXECUTABLE', paths=None): return {'name': name, 'type': kind, 'projectSources': [gate.SOURCE] if paths is None else paths}
        sdk = {'sdk': target('lubancore_sdk_tests')}
        self.assertEqual(gate.ownership_violations(sdk, True), [])
        combined = {**sdk, 'cli': target('lubancode_tests')}
        self.assertEqual(gate.ownership_violations(combined, True, True), [])
        self.assertEqual(gate.ownership_violations({}, False), [])
        for targets, testing, cli in (({}, True, False), (sdk, False, False), (sdk, True, True), (combined, True, False),
                ({'wrong': target('another_test')}, True, False), ({'sdk': target('lubancore_sdk_tests', 'STATIC_LIBRARY')}, True, False),
                ({'sdk': target('lubancore_sdk_tests', paths=[gate.SOURCE, gate.SOURCE])}, True, False),
                ({'sdk': target('lubancore_sdk_tests', paths=[gate.SOURCE.replace('.cpp', '_other.cpp')])}, True, False)):
            with self.subTest(targets=targets):
                self.assertNotEqual(gate.ownership_violations(targets, testing, cli), [])


if __name__ == '__main__':
    unittest.main()
