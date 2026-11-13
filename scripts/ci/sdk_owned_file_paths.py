"""Owned-file-path source and actual native evidence; no program is executed."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re

try:
    from . import sdk_command_jobs as commands
    from .check_sdk_only_boundary import CPP_TOKENS, code_only
except ImportError:
    import sdk_command_jobs as commands
    from check_sdk_only_boundary import CPP_TOKENS, code_only

STEM = 'lubancore_owned_file_paths'
SOURCE = 'tests/integration/sdk/test_' + STEM + '.cpp'
PREFIX = '[sdk-owned-file-path] '
LINK_PREFIX = '[sdk-owned-file-link-kind] '
LINK_KINDS = {'nt': 'terminal-directory-junction', 'posix': 'terminal-file-symlink'}
PATHS = ('public-roundtrip', 'file-relative', 'plan-terminal', 'root-link', 'policy-link', 'artifact-links')
CASES = 6
SOURCE_COPY = 'owned-file-paths-source.cpp'
SOURCE_RECORD = 'owned-file-paths-source.json'


def require(ok, message):
    if not ok:
        raise RuntimeError('Owned file paths: ' + message)


def checkout_head(repo):
    """Read normal/worktree Git metadata only; never invoke git or a build tool."""
    repo = Path(repo).resolve()
    directory = repo / '.git'
    if directory.is_file():
        value = directory.read_text(encoding='utf-8').strip()
        require(value.startswith('gitdir: '), 'invalid checkout metadata')
        path = Path(value.removeprefix('gitdir: '))
        directory = (path if path.is_absolute() else repo / path).resolve()
    head = (directory / 'HEAD').read_text(encoding='ascii').strip()
    if head.startswith('ref: '):
        ref = head.removeprefix('ref: ')
        require(re.fullmatch(r'refs/[A-Za-z0-9_./-]+', ref) is not None and '..' not in Path(ref).parts,
                'unsafe symbolic checkout ref')
        common = directory
        if (directory / 'commondir').is_file():
            path = Path((directory / 'commondir').read_text(encoding='utf-8').strip())
            common = (path if path.is_absolute() else directory / path).resolve()
        loose = next((root / ref for root in (directory, common) if (root / ref).is_file()), None)
        if loose is not None:
            head = loose.read_text(encoding='ascii').strip()
        else:
            packed = common / 'packed-refs'
            rows = [line.split() for line in packed.read_text(encoding='ascii').splitlines()
                    if line and not line.startswith(('#', '^'))] if packed.is_file() else []
            values = [row[0] for row in rows if len(row) == 2 and row[1] == ref]
            require(len(values) == 1, 'checkout ref is missing or ambiguous')
            head = values[0]
    require(re.fullmatch(r'[0-9a-f]{40}', head) is not None, 'checkout head is not an exact commit')
    return head


def source_contract(raw):
    text = raw.decode('utf-8')
    code = code_only(text)
    require(len(re.findall(r'\bTEST_CASE\s*\(', code)) == CASES, 'source does not declare exactly six CASEs')
    require(not re.search(r'\b(?:SUBCASE|TEST_CASE_TEMPLATE|DOCTEST_SKIP|SKIP)\s*\(', code),
            'source changes the complete six-CASE contract')
    literals = CPP_TOKENS.sub(lambda match: re.sub(r'[^\n]', ' ', match.group())
                              if match.group().startswith(('//', '/*', 'R"')) else match.group(), text)
    paths = re.findall(r'\bMark\s*\(\s*"([^"\r\n]+)"\s*\)\s*;', literals)
    require(sorted(paths) == sorted(PATHS) and len(re.findall(r'\bMark\s*\(', code)) == CASES + 1,
            'source paths are missing, repeated, foreign or dynamic')
    require(literals.count('"' + PREFIX + '"') == 1, 'source output prefix differs')
    require(len(re.findall(r'\bTerminalLink\s*\(', code)) == 4 and
            len(re.findall(r'for\s*\(\s*const\s+bool\s+dangling\s*:\s*\{\s*false\s*,\s*true\s*\}\s*\)', code)) == 3,
            'source terminal-link call/real dangling-pair contract differs')
    require(all(literals.count('"' + LINK_PREFIX + kind + '\\n"') == 1 for kind in LINK_KINDS.values()),
            'source platform link-kind facts differ')
    return {'cases': CASES, 'paths': list(PATHS), 'prefix': PREFIX, 'terminalLinkFacts': 6, 'linkKinds': LINK_KINDS}


def capture_source(repo, output):
    """Preserve original bytes before validating their source/checkout contract."""
    repo, output = Path(repo).resolve(), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    (output / SOURCE_RECORD).unlink(missing_ok=True)
    (output / SOURCE_COPY).unlink(missing_ok=True)
    raw = (repo / SOURCE).read_bytes()
    (output / SOURCE_COPY).write_bytes(raw)
    record = {'schemaVersion': 1, 'source': SOURCE, 'bytes': len(raw),
              'sha256': hashlib.sha256(raw).hexdigest(), 'status': 'captured'}
    try:
        record['checkoutHead'] = checkout_head(repo)
        expected = os.environ.get('GITHUB_SHA')
        if expected is not None:
            require(expected == record['checkoutHead'], 'actual checkout differs from GITHUB_SHA')
        record.update(source_contract(raw), status='source_verified')
    except Exception as error:
        record.update(status='failed', error=str(error))
        raise
    finally:
        (output / SOURCE_RECORD).write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    return record


def check_source_copy(raw, record, expected_head):
    require(isinstance(record, dict) and set(record) == {'schemaVersion', 'source', 'bytes', 'sha256', 'status',
            'checkoutHead', 'cases', 'paths', 'prefix', 'terminalLinkFacts', 'linkKinds'} and record.get('status') == 'source_verified' and
            type(record.get('schemaVersion')) is int and record['schemaVersion'] == 1 and record.get('source') == SOURCE,
            'source record is incomplete or foreign')
    require(isinstance(expected_head, str) and re.fullmatch(r'[0-9a-f]{40}', expected_head) is not None and
            record.get('checkoutHead') == expected_head, 'source head differs from this run')
    require(type(record.get('bytes')) is int and len(raw) == record['bytes'] and
            hashlib.sha256(raw).hexdigest() == record.get('sha256'), 'preserved original source bytes changed')
    contract = source_contract(raw)
    require(all(record.get(key) == value for key, value in contract.items()), 'source contract differs from original bytes')
    return record


def read_source_evidence(repo, output):
    repo, output = Path(repo).resolve(), Path(output)
    raw = (output / SOURCE_COPY).read_bytes()
    record = json.loads((output / SOURCE_RECORD).read_bytes(), object_pairs_hook=commands.unique_object)
    head = checkout_head(repo)
    require(os.environ.get('GITHUB_SHA', head) == head, 'actual checkout differs from this run')
    check_source_copy(raw, record, head)
    require((repo / SOURCE).read_bytes() == raw, 'source changed after original preservation')
    return {'record': record, 'bytes': raw, 'head': head}


def check_registration(command, executable='lubancore_sdk_tests'):
    require(executable in ('lubancore_sdk_tests', 'lubancode_tests'), 'foreign executable owner')
    require(isinstance(command, list) and len(command) == 2 and all(isinstance(arg, str) for arg in command) and
            commands.absolute(command[0]) and command[0].replace('\\', '/').split('/')[-1] in (executable, executable + '.exe') and
            command[1] == '--source-file=*test_' + STEM + '.cpp', 'requires the absolute two-argument original source command')


def check_native(section, command, source_record, source_bytes, expected_head, platform_name=None):
    require(isinstance(command, list) and command and isinstance(command[0], str), 'malformed native command')
    check_registration(command, command[0].replace('\\', '/').split('/')[-1].removesuffix('.exe'))
    check_source_copy(source_bytes, source_record, expected_head)
    commands.actual_command(section, command)
    cases = re.findall(r'^\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed(?:\s*\|\s*\d+ skipped)?\s*$', section, re.M)
    assertions = re.findall(r'^\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed\s*\|?\s*$', section, re.M)
    require(cases == [('6', '6', '0')], 'whole six-CASE source did not pass')
    require(len(assertions) == 1 and int(assertions[0][0]) > 0 and assertions[0][0] == assertions[0][1] and
            assertions[0][2] == '0', 'assertions absent, duplicated, empty or failed')
    paths = commands.check_paths(section, PREFIX, PATHS)
    require(not any(re.match(r'^\[(?:sdk-[^]]+-path|v3-journal-owner-path|model-input-wrappers-path)\] ', line)
                    and not line.startswith(PREFIX) for line in section.splitlines()), 'borrowed another source path witness')
    platform_name = os.name if platform_name is None else platform_name
    require(platform_name in LINK_KINDS, 'unknown native platform')
    kinds = [line.removeprefix(LINK_PREFIX) for line in section.splitlines() if line.startswith(LINK_PREFIX)]
    require(kinds == [LINK_KINDS[platform_name]] * 6, 'terminal-link facts missing, duplicated or borrowed from another platform')
    return {'source': SOURCE, 'sourceHead': expected_head, 'sourceSha256': source_record['sha256'],
            'command': command, 'nativeCases': CASES, 'nativeAssertions': int(assertions[0][0]), 'paths': paths, 'terminalLinkKinds': kinds,
            'scope': 'original source bytes/head and actual argv/complete CASE/assertion/once-path evidence; source assertions own native semantics'}


def ownership_violations(targets, testing, with_cli=False):
    owners = []
    for target in targets.values():
        sources = target.get('luaSources', target.get('projectSources'))
        if sources is None:
            sources = [item['projectPath'] for item in target.get('sources', []) if item.get('compiled') and item.get('projectPath')]
        owners.extend([(target['name'], target['type'])] * sources.count(SOURCE))
    expected = [('lubancore_sdk_tests', 'EXECUTABLE')] if testing else []
    if testing and with_cli:
        expected.append(('lubancode_tests', 'EXECUTABLE'))
    return [] if sorted(owners) == sorted(expected) else ['Owned file paths source requires exactly its configured native reference owners']
