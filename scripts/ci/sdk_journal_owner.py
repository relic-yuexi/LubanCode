"""Journal owner source/receipt/material gates; never execute native code.

Capture keeps actual host files, including partial failed runs. A captured
manifest is not a pass. Material validation checks byte identity and V3 source
bindings; C++ native guards remain the canonical JSON/hash authority.
"""
from __future__ import annotations

import hashlib
import json
import ntpath
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat

try:
    from . import sdk_command_jobs as commands
except ImportError:
    try:
        import sdk_command_jobs as commands
    except ModuleNotFoundError:
        from scripts.ci import sdk_command_jobs as commands

PUBLIC_PATHS = ('roundtrip', 'close-read', 'isolation')
NATIVE_PATHS = ('native-first', 'semantic-first', 'lease-owner-lifetime', 'dropped-lease',
                'lease-close-thread', 'closed-reader', 'capture-fences', 'same-id', 'locked-opening')
SOURCES = {'v3_journal_owner': (7, '[v3-journal-owner-path] ', NATIVE_PATHS),
           'lubancore_journal_owner': (3, '[sdk-journal-owner-path] ', PUBLIC_PATHS),
           'lubancore_journal_owner_guards': (1, '[sdk-journal-owner-guard] ', ('actual-public-source',))}
ENGINE = 'src/trajectory/journal_owner.cpp'
ENGINE_IMPLEMENTATIONS = (ENGINE,)
HELPER = 'examples/sdk-consumer/journal_owner.cpp'
HEADERS = ('include/lubancore/core.hpp', 'include/lubancore/results.hpp')
MAX_ENTRIES = 4096
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_CAPTURE_BYTES = 64 * 1024 * 1024
MAX_LINES = 100000
MAX_LINE_BYTES = 1024 * 1024
HOST_PREFIX = '[sdk-journal-owner-host] '
IDENTITY = re.compile(r'[A-Za-z0-9_-]{1,200}\Z')
HEX = re.compile(r'[0-9a-f]{64}\Z')
WITNESS_FIELDS = ('session', 'operation', 'turn', 'tool_call', 'attempt', 'execution',
                  'persisted', 'result', 'metadata_sha256', 'metadata_bytes',
                  'combined_sha256', 'combined_bytes')
RECEIPT_FIELDS = {'session_id', 'prefix_bytes', 'after_bytes', 'model_calls', 'tool_calls',
                  'old_operation', 'new_operation'} | {
    f'{tag}_{role}_{field}' for tag in ('old', 'new') for role in ('raw', 'formal') for field in WITNESS_FIELDS}


def require(ok, message):
    if not ok:
        raise RuntimeError('Journal owner: ' + message)


def check_registration(command, stem, exe='lubancore_sdk_tests'):
    require(stem in SOURCES, 'unknown native source')
    require(exe in ('lubancore_sdk_tests', 'lubancode_tests'), 'foreign native executable')
    require(isinstance(command, list) and len(command) == 2 and all(isinstance(arg, str) for arg in command) and
            commands.absolute(command[0]) and command[0].replace('\\', '/').split('/')[-1] in (exe, exe + '.exe') and
            command[1] == '--source-file=*test_' + stem + '.cpp', 'requires absolute original single-source argv')


def _hosts(section, paths):
    lines = section.splitlines()
    host_lines = [line for line in lines if line.startswith(HOST_PREFIX)]
    require(len(host_lines) == len(paths), 'actual host facts missing or duplicated')
    facts = []
    observed_path = None
    for line in lines:
        if line.startswith('[sdk-journal-owner-path] '):
            observed_path = line.removeprefix('[sdk-journal-owner-path] ')
        if not line.startswith(HOST_PREFIX):
            continue
        match = re.fullmatch(re.escape(HOST_PREFIX) +
                             r'session=([A-Za-z0-9_-]{1,200}) prefix=([0-9]+) after=([0-9]+) models=4 tools=2', line)
        require(match is not None and observed_path in paths, 'malformed or foreign actual host facts')
        sid, before, after = match.groups()
        before, after = int(before), int(after)
        require(0 < before < after <= MAX_FILE_BYTES, 'host did not append after its original prefix')
        facts.append({'path': observed_path, 'session_id': sid, 'prefix_bytes': before,
                      'after_bytes': after, 'model_calls': 4, 'tool_calls': 2})
        observed_path = None
    require(sorted(item['path'] for item in facts) == sorted(paths) and
            len({item['session_id'] for item in facts}) == len(paths), 'host paths or Session identities duplicate')
    return facts


def check_native(section, command, stem):
    require(isinstance(command, list) and command and isinstance(command[0], str), 'malformed native argv')
    exe = command[0].replace('\\', '/').split('/')[-1].removesuffix('.exe')
    require(exe in ('lubancore_sdk_tests', 'lubancode_tests'), 'foreign native executable')
    check_registration(command, stem, exe)
    commands.actual_command(section, command)
    count, prefix, paths = SOURCES[stem]
    cases = re.findall(r'\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    assertions = re.findall(r'\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    require(cases == [(str(count), str(count), '0')], 'native source did not pass its whole CASE roster')
    require(len(assertions) == 1 and int(assertions[0][0]) > 0 and
            assertions[0][0] == assertions[0][1] and assertions[0][2] == '0', 'native assertions empty or failed')
    paths = commands.check_paths(section, prefix, paths)
    for foreign, (_, other_prefix, _) in SOURCES.items():
        if foreign == stem or (stem == 'lubancore_journal_owner_guards' and foreign == 'lubancore_journal_owner'):
            continue
        require(not any(line.startswith(other_prefix) for line in section.splitlines()), 'borrowed another source path witness')
    host_lines = [line for line in section.splitlines() if line.startswith(HOST_PREFIX)]
    if stem == 'lubancore_journal_owner':
        hosts = _hosts(section, PUBLIC_PATHS)
    elif stem == 'lubancore_journal_owner_guards':
        hosts = _hosts(section, ('close-read',))
        commands.check_paths(section, '[sdk-journal-owner-path] ', ('close-read',))
    else:
        require(not host_lines, 'private owner source borrowed public host facts')
        hosts = []
    return {'source': stem, 'command': command, 'nativeCases': count,
            'nativeAssertions': int(assertions[0][0]), 'paths': paths, 'hosts': hosts,
            'scope': 'actual source argv/completion/CASE/assertions/once paths; source assertions own native authority'}


def check_consumer(section, command):
    require(isinstance(command, list) and len(command) == 3 and all(isinstance(arg, str) for arg in command) and
            commands.absolute(command[0]) and commands.absolute(command[2]) and
            command[0].replace('\\', '/').split('/')[-1] in ('lubancore_consumer', 'lubancore_consumer.exe') and
            command[1] == 'journal-owner', 'installed caller must name absolute executable/state and journal-owner mode')
    commands.actual_command(section, command)
    paths = commands.check_paths(section, '[sdk-journal-owner-path] ', PUBLIC_PATHS)
    require(section.splitlines().count('installed SDK consumer journal-owner passed') == 1,
            'installed consumer did not complete once')
    hosts = _hosts(section, PUBLIC_PATHS)
    return {'command': command, 'paths': paths, 'hosts': hosts,
            'scope': 'actual installed public helper facts; real saved material is checked separately'}


def ownership_violations(targets, testing, with_cli=False):
    owners = {ENGINE: [], HELPER: []}
    for target in targets.values():
        compiled = target.get('luaSources')
        if compiled is None:
            compiled = [item['projectPath'] for item in target.get('sources', [])
                        if item.get('compiled') and item.get('projectPath')]
        for source in owners:
            owners[source].extend([(target['name'], target['type'])] * compiled.count(source))
    references = [('lubancore_sdk_tests', 'EXECUTABLE')] if testing else []
    if testing and with_cli:
        references.append(('lubancode_tests', 'EXECUTABLE'))
    expected = {ENGINE: [('lubancode_engine', 'STATIC_LIBRARY')], HELPER: references}
    return ['Journal owner compiled owner closure differs for ' + source
            for source in owners if sorted(owners[source]) != sorted(expected[source])]


def seal_sources(repo):
    try:
        from .check_sdk_only_boundary import CPP_TOKENS, STANDARD_HEADERS, includes_in, without_comments
    except ImportError:
        from check_sdk_only_boundary import CPP_TOKENS, STANDARD_HEADERS, includes_in, without_comments
    repo = Path(repo)
    records = {}
    for source in (ENGINE, HELPER, *HEADERS):
        path = repo / source
        _safe(path, repo)
        data = path.read_bytes()
        records[source] = {'path': source, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    text = (repo / HELPER).read_text(encoding='utf-8')
    includes = list(includes_in(text))
    directives = CPP_TOKENS.sub(lambda match: re.sub(r'[^\n]', ' ', match.group())
                                if match.group().startswith('R"') else match.group(), without_comments(text))
    require(len(re.findall(r'^\s*#\s*include\b', directives, re.M)) == len(includes), 'dynamic helper include')
    names = [match.group(1) for match in includes]
    public = {'lubancore/core.hpp', 'lubancore/results.hpp'}
    require(set(name for name in names if name.startswith('lubancore/')) == public and
            all(name in STANDARD_HEADERS or name in public for name in names) and
            all(re.match(r'#\s*include\s*<', match.group().lstrip()) for match in includes),
            'helper includes nonpublic implementation')
    records[HELPER]['includes'] = names
    return records


def check_copies(consumer_source, prefix, records, repo, evidence=None):
    copies = {}
    for source, path in ((HELPER, Path(consumer_source) / 'journal_owner.cpp'),
                         *((header, Path(prefix) / header) for header in HEADERS)):
        _safe(path, Path(consumer_source) if source == HELPER else Path(prefix))
        original = (Path(repo) / source).read_bytes()
        data = path.read_bytes()
        record = records[source]
        require(data == original and len(data) == record['bytes'] and
                hashlib.sha256(data).hexdigest() == record['sha256'], 'relocated source/header differs: ' + source)
        if evidence is not None:
            target = Path(evidence) / ('consumer' if source == HELPER else 'installed') / Path(source).name
            target.parent.mkdir(parents=True, exist_ok=True)
            _safe(target.parent, Path(evidence), directory=True)
            require(not target.exists() and not target.is_symlink(), 'copy evidence already exists')
            target.write_bytes(data)
        copies[source] = {'path': str(path.resolve()), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    return copies


def _linked(path):
    return path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction())


def _safe(path, root, directory=False):
    path, root = Path(path).absolute(), Path(root).absolute()
    require(path.is_relative_to(root), 'path leaves owned root')
    for part in (path, *path.parents):
        require(not _linked(part), 'linked path or ancestor refused: ' + str(part))
    require(path.resolve().is_relative_to(root.resolve()), 'resolved path leaves owned root')
    require(path.is_dir() if directory else path.is_file(), 'missing actual ' + ('directory' if directory else 'file'))


def _read(path, root, allowance=MAX_FILE_BYTES):
    _safe(path, root)
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
    fd = os.open(path, flags)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and before.st_size <= allowance, 'actual file exceeds capture byte bound')
        with os.fdopen(fd, 'rb', closefd=False) as stream:
            data = stream.read(allowance + 1)
        after = os.fstat(fd)
        require(len(data) <= allowance and before.st_size == len(data) and
                (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), 'actual file changed during capture')
        _safe(path, root)
        current = Path(path).stat()
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) ==
                (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns), 'actual path changed during capture')
        return data
    finally:
        os.close(fd)


def _selected(relative):
    parts = relative.parts
    if len(parts) == 2 and parts[1] in ('journal-owner-before.jsonl', 'journal-owner-host-receipt.txt'):
        return True
    # Real fixture/state/workspaces/key/sessions/sid/sid.jsonl and artifacts.
    if len(parts) not in (7, 8) or parts[1:3] != ('state', 'workspaces') or parts[4] != 'sessions':
        return False
    if len(parts) == 7:
        return parts[6] == parts[5] + '.jsonl'
    return parts[6] == 'artifacts' and parts[7].startswith(('capture-', 'res-'))


def capture_materials(base, evidence):
    """Copy bounded real files, even from failed native runs; never synthesize a receipt."""
    base, evidence = Path(base).absolute(), Path(evidence).absolute()
    context = {'schemaVersion': 1, 'base': str(base), 'evidence': str(evidence),
               'status': 'capture_failed', 'acceptance': 'not_evaluated', 'fixture_roots': [],
               'files': [], 'issues': [],
               'bounds': {'entries': MAX_ENTRIES, 'file_bytes': MAX_FILE_BYTES, 'total_bytes': MAX_CAPTURE_BYTES}}
    total, entries, destination_ready = 0, 0, False
    try:
        _safe(base, base, directory=True)
        require(not evidence.is_relative_to(base) and not base.is_relative_to(evidence), 'capture source/destination overlap')
        for ancestor in (evidence, *evidence.parents):
            require(not _linked(ancestor), 'linked evidence ancestor')
        require(not evidence.exists() or (evidence.is_dir() and not any(evidence.iterdir())), 'evidence destination is not empty')
        evidence.mkdir(parents=True, exist_ok=True)
        _safe(evidence, evidence, directory=True)
        destination_ready = True
        stack = [base]
        while stack:
            directory = stack.pop()
            _safe(directory, base, directory=True)
            with os.scandir(directory) as listing:
                for entry in listing:
                    entries += 1
                    require(entries <= MAX_ENTRIES, 'actual host root exceeds entry bound')
                    path = Path(entry.path)
                    require(not _linked(path), 'linked actual host entry refused: ' + str(path))
                    if entry.is_dir(follow_symlinks=False):
                        if directory == base:
                            require(re.fullmatch(r'journal-owner-host-[0-9]+-[0-9]+', entry.name) is not None,
                                    'foreign fixture root')
                            context['fixture_roots'].append(entry.name)
                        stack.append(path)
                    elif entry.is_file(follow_symlinks=False):
                        require(directory != base, 'foreign file in actual host root')
                        relative = path.relative_to(base)
                        if not _selected(relative):
                            continue
                        data = _read(path, base, min(MAX_FILE_BYTES, MAX_CAPTURE_BYTES - total))
                        total += len(data)
                        saved = evidence / 'files' / relative
                        saved.parent.mkdir(parents=True, exist_ok=True)
                        _safe(saved.parent, evidence, directory=True)
                        with saved.open('xb') as output:
                            output.write(data)
                        context['files'].append({'relative': relative.as_posix(), 'saved': 'files/' + relative.as_posix(),
                                                 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
                    else:
                        raise RuntimeError('Journal owner: nonregular actual host entry')
        context['fixture_roots'].sort()
        context['files'].sort(key=lambda item: item['relative'])
        context['status'] = 'captured'
    except (OSError, RuntimeError) as error:
        context['issues'].append(str(error))
    # Saving a partial capture is part of the evidence contract. A destination
    # fault remains capture_failed; it must not hide the original native result.
    try:
        require(destination_ready, 'evidence destination was not safely prepared')
        _safe(evidence, evidence, directory=True)
        raw = (json.dumps(context, indent=2, ensure_ascii=False) + '\n').encode('utf-8')
        manifest = evidence / 'capture.json'
        with manifest.open('xb') as output:
            output.write(raw)
        context['manifest'] = {'path': str(manifest), 'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
    except (OSError, RuntimeError) as error:
        context['status'] = 'capture_failed'
        context['issues'].append('manifest preservation failed: ' + str(error))
    return context


def _json(data):
    try:
        return json.loads(data.decode('utf-8'), object_pairs_hook=commands.unique_object,
                          parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (UnicodeError, ValueError, RecursionError) as error:
        raise RuntimeError('Journal owner: malformed saved JSON') from error


def _saved_files(capture):
    require(isinstance(capture, dict) and capture.get('schemaVersion') == 1 and
            capture.get('status') == 'captured' and capture.get('acceptance') == 'not_evaluated' and
            capture.get('issues') == [], 'failed capture cannot establish material eligibility')
    evidence = Path(capture['evidence'])
    manifest = capture.get('manifest')
    require(isinstance(manifest, dict) and manifest.get('path') == str(evidence / 'capture.json'), 'foreign capture manifest')
    raw = _read(evidence / 'capture.json', evidence)
    require(len(raw) == manifest['bytes'] and hashlib.sha256(raw).hexdigest() == manifest['sha256'], 'saved manifest changed')
    require(_json(raw) == {key: value for key, value in capture.items() if key != 'manifest'}, 'capture context differs from saved manifest')
    files = capture.get('files')
    require(isinstance(files, list) and 0 < len(files) <= MAX_ENTRIES, 'empty or oversized capture manifest')
    result, total = {}, 0
    for record in files:
        require(isinstance(record, dict) and set(record) == {'relative', 'saved', 'bytes', 'sha256'}, 'unknown saved record contract')
        relative = record['relative']
        require(isinstance(relative, str) and '\\' not in relative and not any(ch in relative for ch in '\0\r\n') and
                not PurePosixPath(relative).is_absolute() and '..' not in PurePosixPath(relative).parts and
                str(PurePosixPath(relative)) == relative and relative not in result and
                record['saved'] == 'files/' + relative, 'duplicate or unsafe saved relative path')
        data = _read(evidence / record['saved'], evidence)
        require(type(record['bytes']) is int and len(data) == record['bytes'] and
                hashlib.sha256(data).hexdigest() == record['sha256'], 'preserved actual bytes changed: ' + relative)
        total += len(data)
        require(total <= MAX_CAPTURE_BYTES, 'saved material exceeds total byte bound')
        result[relative] = data
    inventory, stack, entries = set(), [evidence], 0
    while stack:
        directory = stack.pop()
        _safe(directory, evidence, directory=True)
        with os.scandir(directory) as listing:
            for entry in listing:
                entries += 1
                require(entries <= MAX_ENTRIES, 'saved evidence exceeds entry bound')
                path = Path(entry.path)
                require(not _linked(path), 'linked saved evidence')
                if entry.is_dir(follow_symlinks=False):
                    stack.append(path)
                elif entry.is_file(follow_symlinks=False):
                    inventory.add(path.relative_to(evidence).as_posix())
                else:
                    raise RuntimeError('Journal owner: nonregular saved evidence')
    require(inventory == {'capture.json'} | {'files/' + relative for relative in result}, 'extra or missing preserved files')
    return result


def _receipt(data):
    try:
        text = data.decode('utf-8')
    except UnicodeError as error:
        raise RuntimeError('Journal owner: host receipt is not UTF-8') from error
    require(text.endswith('\n'), 'host receipt is incomplete')
    result = {}
    for line in text.splitlines():
        key, separator, value = line.partition('=')
        require(separator and key not in result and value and not any(ch in value for ch in '\0\r'), 'duplicate or malformed host receipt')
        result[key] = value
    require(set(result) == RECEIPT_FIELDS, 'host receipt missing or foreign fields')
    for key in result:
        if key in ('prefix_bytes', 'after_bytes', 'model_calls', 'tool_calls') or key.endswith(('_attempt', '_metadata_bytes', '_combined_bytes')):
            require(re.fullmatch(r'[1-9][0-9]*', result[key]) is not None, 'host receipt integer invalid')
            result[key] = int(result[key])
        elif key.endswith('_sha256'):
            require(HEX.fullmatch(result[key]) is not None, 'host digest invalid')
        else:
            require(IDENTITY.fullmatch(result[key]) is not None, 'host identity invalid')
    require(result['model_calls'] == 4 and result['tool_calls'] == 2 and
            0 < result['prefix_bytes'] < result['after_bytes'] <= MAX_FILE_BYTES and
            result['old_operation'] != result['new_operation'], 'host receipt has wrong completion facts')
    return result


def _ledger(data, session):
    require(data and data.endswith(b'\n'), 'saved main ledger is empty or incomplete')
    physical = data.splitlines()
    require(len(physical) <= MAX_LINES, 'saved main ledger exceeds line bound')
    rows, previous, identities, run = [], '0' * 64, set(), None
    for line in physical:
        require(len(line) <= MAX_LINE_BYTES, 'saved main line exceeds byte bound')
        if not line.strip():
            continue
        row = _json(line)
        require(isinstance(row, dict) and type(row.get('schemaVersion')) is int and row['schemaVersion'] == 3 and
                row.get('type') in ('message', 'event') and row.get('sessionId') == session and
                isinstance(row.get('runId'), str) and IDENTITY.fullmatch(row['runId']) and
                isinstance(row.get('timestamp'), str) and row['timestamp'] and
                type(row.get('seq')) is int and row['seq'] == len(rows) + 1 and row.get('prevHash') == previous and
                isinstance(row.get('lineHash'), str) and HEX.fullmatch(row['lineHash']), 'saved main schema/scope/seq/hash linkage differs')
        run = run or row['runId']
        require(row['runId'] == run, 'same-ID continuation changed run identity')
        identity = row.get('eventId') if row['type'] == 'event' else row.get('messageId')
        require(isinstance(identity, str) and IDENTITY.fullmatch(identity) and identity not in identities, 'duplicate or invalid main identity')
        identities.add(identity)
        if row.get('turnId') is not None:
            require(isinstance(row['turnId'], str) and IDENTITY.fullmatch(row['turnId']), 'invalid actual turn identity')
        if row['type'] == 'event':
            require(isinstance(row.get('kind'), str) and isinstance(row.get('payload'), dict), 'event envelope absent')
        else:
            require('turnId' in row and isinstance(row.get('message'), dict), 'message envelope absent')
        rows.append(row)
        previous = row['lineHash']
    require(rows, 'saved main has no actual records')
    require(rows[0]['type'] == 'message' and rows[0]['message'].get('role') == 'system', 'actual main lacks its original system opening')
    return rows


def _events(rows, kind):
    return [row for row in rows if row['type'] == 'event' and row['kind'] == kind]


def _scope(row, turn, action):
    return (row.get('turnId') == turn and row.get('actionId') == action and row['payload'].get('tool_call_id') == action and
            row['payload'].get('attempt') == 1 and type(row['payload'].get('attempt')) is int)


def _invocation(rows, session_dir, files, turn, operation, tag, witness=None):
    starts = [row for row in _events(rows, 'tool.execution.started') if row.get('turnId') == turn]
    require(len(starts) == 1, 'turn did not execute one actual callback')
    start = starts[0]; action = start.get('actionId')
    tool_identity = start['payload'].get('toolIdentity')
    require(isinstance(action, str) and IDENTITY.fullmatch(action) and _scope(start, turn, action) and
            isinstance(tool_identity, dict) and tool_identity.get('logicalName') == 'journal_fixture', 'actual callback identity differs')
    pending = [row for row in _events(rows, 'tool.execution.pending') if _scope(row, turn, action)]
    require(len(pending) == 1 and pending[0]['seq'] < start['seq'] and
            pending[0]['payload'].get('provider_tool_call_id') == 'host-call-' + tag,
            'actual admission did not bind the original host provider call')
    finishes = [row for row in _events(rows, 'tool.execution.finished') if _scope(row, turn, action)]
    require(len(finishes) == 1 and start['seq'] < finishes[0]['seq'], 'actual callback lacks one ordered completion')
    finish = finishes[0]
    persisted = [row for row in _events(rows, 'tool.result.persisted') if row.get('turnId') == turn]
    require(len(persisted) == 2, 'one invocation lacks its two actual raw/formal records')
    materials, used = {}, set()
    for event in persisted:
        payload = event['payload']
        require(_scope(event, turn, action) and payload.get('executionEventRef') == finish['eventId'] and
                finish['seq'] < event['seq'], 'result references a different actual execution')
        refs = payload.get('result_ref')
        require(isinstance(refs, list) and len(refs) == 2, 'plain result must reference metadata and combined bytes')
        by_kind = {}
        for ref in refs:
            require(isinstance(ref, dict) and set(ref) == {'artifactId', 'kind', 'path', 'sha256', 'bytes', 'mediaType'} and
                    ref['kind'] not in by_kind and isinstance(ref['artifactId'], str) and IDENTITY.fullmatch(ref['artifactId']) and
                    isinstance(ref['sha256'], str) and HEX.fullmatch(ref['sha256']) and type(ref['bytes']) is int and ref['bytes'] > 0,
                    'artifact identity missing or duplicated')
            by_kind[ref['kind']] = ref
        require(set(by_kind) == {'result_metadata', 'combined'}, 'plain callback acquired foreign channel')
        metadata_ref, combined_ref = by_kind['result_metadata'], by_kind['combined']
        result_id = metadata_ref['artifactId']
        role = 'raw' if re.fullmatch(r'capture-[0-9]+', result_id) else 'formal' if re.fullmatch(r'res-[0-9]+', result_id) else None
        require(role is not None and role not in materials, 'raw/formal identity swapped or duplicated')
        require(metadata_ref['path'] == 'artifacts/' + result_id + '.json' and metadata_ref['mediaType'] == 'application/json' and
                combined_ref['artifactId'] == result_id + '-combined' and
                combined_ref['path'] == 'artifacts/' + result_id + '.combined.txt' and combined_ref['mediaType'] == 'text/plain',
                'artifact path differs from actual result identity')
        for ref in refs:
            relative = session_dir + '/' + ref['path']
            require(relative in files and relative not in used, 'actual artifact missing or reused')
            actual = files[relative]
            require(len(actual) == ref['bytes'] and hashlib.sha256(actual).hexdigest() == ref['sha256'], 'actual artifact digest/bytes differ')
            used.add(relative)
        meta = _json(files[session_dir + '/' + metadata_ref['path']])
        text = ('JOURNAL_TOOL_' + tag).encode('utf-8')
        require(isinstance(meta, dict) and meta.get('result_id') == result_id and meta.get('tool_call_id') == action and
                type(meta.get('attempt')) is int and meta['attempt'] == 1 and meta.get('execution_event_ref') == finish['eventId'] and
                meta.get('result_kind') == 'text' and meta.get('content') == text.decode() and 'structured_content' not in meta and
                isinstance(meta.get('preview_policy'), dict) and meta['preview_policy'].get('policy') ==
                ('raw-capture-before-post-hook' if role == 'raw' else 'v3-tool-preview'), 'metadata scope/raw/formal provenance differs')
        outputs = meta.get('outputs')
        require(isinstance(outputs, list) and len(outputs) == 1 and isinstance(outputs[0], dict), 'saved combined descriptor missing or duplicated')
        output = outputs[0]
        require(output.get('channel') == 'combined' and output.get('encoding') == 'utf-8' and output.get('capture_complete') is True and
                output.get('capture_reason', '') == '' and output.get('byte_count_kind') == 'exact' and
                type(output.get('captured_bytes')) is int and output['captured_bytes'] == len(text) and
                type(output.get('output_bytes')) is int and output['output_bytes'] == len(text) and
                files[session_dir + '/' + combined_ref['path']] == text, 'combined bytes or capture facts differ')
        nested = output.get('ref')
        expected = {'artifact_id': combined_ref['artifactId'], 'path': combined_ref['path'],
                    'sha256': combined_ref['sha256'], 'bytes': combined_ref['bytes'], 'media_type': combined_ref['mediaType']}
        require(nested == expected, 'combined descriptor substituted its artifact reference')
        facts = {'session': rows[0]['sessionId'], 'operation': operation, 'turn': turn, 'tool_call': action,
                 'attempt': 1, 'execution': finish['eventId'], 'persisted': event['eventId'], 'result': result_id,
                 'metadata_sha256': metadata_ref['sha256'], 'metadata_bytes': metadata_ref['bytes'],
                 'combined_sha256': combined_ref['sha256'], 'combined_bytes': combined_ref['bytes']}
        if witness is not None:
            require(all(witness[f'{tag.lower()}_{role}_{key}'] == value for key, value in facts.items()),
                    'host raw/formal witness differs from actual main or artifact')
        materials[role] = {'facts': facts, 'seq': event['seq']}
    require(set(materials) == {'raw', 'formal'} and materials['raw']['seq'] < materials['formal']['seq'], 'raw/formal order differs')
    selected = [row for row in _events(rows, 'tool.result.selected') if _scope(row, turn, action)]
    require(len(selected) == 1 and selected[0]['seq'] > materials['formal']['seq'] and
            selected[0]['payload'].get('sourceResultEventRefs') == [materials[role]['facts']['persisted'] for role in ('raw', 'formal')] and
            selected[0]['payload'].get('effectiveOutcome') == 'done', 'selected result does not bind both actual immutable sources')
    messages = [row for row in rows if row['type'] == 'message' and row['message'].get('role') == 'tool' and row.get('turnId') == turn]
    require(len(messages) == 1 and messages[0].get('actionId') == action and messages[0]['message'].get('tool_call_id') == action and
            messages[0]['message'].get('content') == 'JOURNAL_TOOL_' + tag and messages[0].get('resultSelectionRef') == selected[0]['eventId'] and
            selected[0]['seq'] < messages[0]['seq'], 'actual tool message did not consume the verified selection')
    return materials, used


def _bindings(rows, count):
    bindings = _events(rows, 'sdk.operation.turn.bound')
    require(len(bindings) == count, 'extra or missing actual operation/turn binding')
    result, turns, inputs = {}, set(), set()
    starts = _events(rows, 'session.started')
    for event in bindings:
        payload = event['payload']; operation = payload.get('operationId'); turn = event.get('turnId')
        require(set(payload) == {'layout', 'version', 'operationId', 'inputId', 'payloadHash'} and
                payload['layout'] == 'sdk_main_operation_turn_v1' and type(payload['version']) is int and payload['version'] == 1 and
                isinstance(operation, str) and IDENTITY.fullmatch(operation) and isinstance(turn, str) and IDENTITY.fullmatch(turn) and
                isinstance(payload['inputId'], str) and IDENTITY.fullmatch(payload['inputId']) and
                isinstance(payload['payloadHash'], str) and HEX.fullmatch(payload['payloadHash']) and
                operation not in result and turn not in turns and payload['inputId'] not in inputs and
                not any(key in event for key in ('stepId', 'actionId', 'requestId', 'parentTurnId', 'compactId',
                                                 'commandId', 'hookDispatchId', 'taskId', 'titleGenerationId', 'effects', 'effectRefs')) and
                min(row['seq'] for row in rows if row.get('turnId') == turn) == event['seq'] and
                len(starts) == 1 and starts[0]['seq'] < event['seq'], 'actual operation binding identity differs')
        result[operation] = event; turns.add(turn); inputs.add(payload['inputId'])
    return result


def _norm(value):
    value = str(value).replace('\\', '/')
    return ntpath.normcase(ntpath.normpath(value)) if ntpath.splitdrive(value)[0] else posixpath.normpath(value)


def check_materials(capture, consumer_receipt):
    """Validate retained actual files, not stdout-created mirrors or Python canonical hashes."""
    require(isinstance(capture, dict) and isinstance(consumer_receipt, dict) and
            set(consumer_receipt) >= {'command', 'paths', 'hosts'}, 'installed caller witness missing')
    command = consumer_receipt['command']
    require(isinstance(command, list) and len(command) == 3 and all(isinstance(arg, str) for arg in command) and
            commands.absolute(command[0]) and command[0].replace('\\', '/').split('/')[-1] in ('lubancore_consumer', 'lubancore_consumer.exe') and
            command[1] == 'journal-owner' and commands.absolute(command[2]) and
            _norm(command[2]) == _norm(capture.get('base', '')), 'saved materials borrowed another actual state root')
    hosts = consumer_receipt['hosts']
    require(isinstance(hosts, list) and len(hosts) == 3 and all(isinstance(item, dict) and
            set(item) == {'path', 'session_id', 'prefix_bytes', 'after_bytes', 'model_calls', 'tool_calls'} and
            item['path'] in PUBLIC_PATHS and isinstance(item['session_id'], str) and IDENTITY.fullmatch(item['session_id']) and
            all(type(item[key]) is int for key in ('prefix_bytes', 'after_bytes', 'model_calls', 'tool_calls')) and
            0 < item['prefix_bytes'] < item['after_bytes'] <= MAX_FILE_BYTES and item['model_calls'] == 4 and item['tool_calls'] == 2
            for item in hosts) and sorted(consumer_receipt['paths']) == sorted(PUBLIC_PATHS) and
            sorted(item.get('path') for item in hosts) == sorted(PUBLIC_PATHS) and
            len({item.get('session_id') for item in hosts}) == 3, 'installed host fixture witness differs')
    files = _saved_files(capture)
    fixtures = capture.get('fixture_roots')
    require(isinstance(fixtures, list) and len(fixtures) == 3 and len(set(fixtures)) == 3 and
            all(isinstance(name, str) and re.fullmatch(r'journal-owner-host-[0-9]+-[0-9]+', name) for name in fixtures),
            'extra, duplicate or missing real host fixture')
    unused = set(files); matched = set(); reports = []
    for fixture in fixtures:
        receipt_path = fixture + '/journal-owner-host-receipt.txt'; before_path = fixture + '/journal-owner-before.jsonl'
        require(receipt_path in files and before_path in files, 'failed host run has no complete real receipt/prefix')
        receipt = _receipt(files[receipt_path]); sid = receipt['session_id']
        facts = [item for item in hosts if item['session_id'] == sid]
        require(len(facts) == 1 and sid not in matched, 'real fixture Session absent or duplicated in installed witness')
        fact = facts[0]; matched.add(sid)
        require(all(receipt[key] == fact[key] for key in ('session_id', 'prefix_bytes', 'after_bytes', 'model_calls', 'tool_calls')),
                'actual host receipt differs from actual consumer stdout facts')
        ledgers = [name for name in files if name.startswith(fixture + '/state/workspaces/') and name.endswith('/' + sid + '/' + sid + '.jsonl')]
        require(len(ledgers) == 1, 'actual main ledger missing or duplicated across workspace roots')
        main = ledgers[0]; session_dir = main.rsplit('/', 1)[0]
        prefix, data = files[before_path], files[main]
        require(len(prefix) == receipt['prefix_bytes'] and len(data) == receipt['after_bytes'] and data.startswith(prefix), 'actual old prefix was rewritten or bytes differ')
        rows = _ledger(data, sid); old_rows = _ledger(prefix, sid); cutoff = len(old_rows)
        require(rows[:cutoff] == old_rows and len(rows) > cutoff and
                len(_events(rows, 'session.started')) == 1 and len(_events(rows, 'session.ended')) == 2 and
                len(_events(old_rows, 'session.ended')) == 1, 'actual Close/recovery lifecycle differs')
        bindings = _bindings(rows, 2)
        require(set(bindings) == {receipt['old_operation'], receipt['new_operation']} and
                len(_events(rows, 'model.request.prepared')) == 4 and len(_events(rows, 'tool.execution.started')) == 2 and
                len(_events(rows, 'tool.execution.pending')) == 2 and
                len(_events(rows, 'tool.execution.finished')) == 2 and len(_events(rows, 'tool.result.persisted')) == 4 and
                len(_events(rows, 'tool.result.selected')) == 2, 'actual model/tool/result source roster differs from 4/2 host calls')
        require(not any(row['kind'].startswith('tool.execution.') and row['kind'] not in
                        ('tool.execution.pending', 'tool.execution.started', 'tool.execution.finished') for row in rows if row['type'] == 'event'),
                'plain callback has extra cancelled/failed/waiting execution records')
        used = {receipt_path, before_path, main}; invocations = {}
        for tag, operation_key in (('OLD', 'old_operation'), ('NEW', 'new_operation')):
            binding = bindings[receipt[operation_key]]; turn = binding['turnId']
            prepared = [event for event in _events(rows, 'model.request.prepared') if event.get('turnId') == turn]
            require(len(prepared) == 2 and len({event.get('requestId') for event in prepared}) == 2 and
                    all(isinstance(event.get('requestId'), str) and IDENTITY.fullmatch(event['requestId']) and
                        binding['seq'] < event['seq'] for event in prepared), 'actual owning turn lacks two model requests')
            require(all((event['seq'] <= cutoff) == (tag == 'OLD') for event in prepared), 'recovery moved model work across original prefix')
            pair, artifacts = _invocation(rows, session_dir, files, turn, receipt[operation_key], tag, receipt)
            require(all((item['seq'] <= cutoff) == (tag == 'OLD') for item in pair.values()), 'raw/formal source crossed original prefix')
            used |= artifacts; invocations[tag.lower()] = pair
        other_ledgers = [name for name in files if name.startswith(fixture + '/state/workspaces/') and name.endswith('.jsonl') and name != main]
        require(len(other_ledgers) == (1 if fact['path'] == 'isolation' else 0), 'extra or missing same-project Session ledger')
        for other in other_ledgers:
            other_sid = PurePosixPath(other).stem
            require(other_sid != sid and PurePosixPath(other).parent.name == other_sid, 'isolated Session identity differs')
            other_rows = _ledger(files[other], other_sid); other_bindings = _bindings(other_rows, 1)
            require(len(_events(other_rows, 'model.request.prepared')) == 2 and len(_events(other_rows, 'tool.execution.started')) == 1 and
                    len(_events(other_rows, 'tool.execution.pending')) == 1 and len(_events(other_rows, 'tool.execution.finished')) == 1 and
                    len(_events(other_rows, 'tool.result.persisted')) == 2 and len(_events(other_rows, 'tool.result.selected')) == 1,
                    'same-project isolated actual invocation differs')
            operation, binding = next(iter(other_bindings.items()))
            _, artifacts = _invocation(other_rows, other.rsplit('/', 1)[0], files, binding['turnId'], operation, 'OTHER')
            used |= artifacts | {other}
        unused -= used
        reports.append({'path': fact['path'], 'fixture': fixture, 'session_id': sid, 'run_id': rows[0]['runId'],
                        'main': main, 'prefix_bytes': len(prefix), 'after_bytes': len(data), 'prefix_rows': cutoff,
                        'main_rows': len(rows), 'model_prepared_records': 4, 'tool_started_records': 2, 'invocations': invocations})
    require(not unused, 'extra or unbound captured main/result files: ' + ', '.join(sorted(unused)))
    return {'status': 'verified_materials', 'command': command, 'fixtures': reports,
            'canonical_hash_recomputed': False, 'native_canonical_guard_required': True,
            'scope': 'actual preserved bytes/digests/record scope/seq/hash linkage/prefix/raw/formal source bindings; C++ owns canonical and prepared-chain authority'}
