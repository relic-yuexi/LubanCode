"""Named-result source ownership and actual native/installed evidence; data only."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

try:
    from . import sdk_command_jobs as commands
except ImportError:
    try:
        import sdk_command_jobs as commands
    except ModuleNotFoundError:
        from scripts.ci import sdk_command_jobs as commands

PUBLIC_PATHS = ('roundtrip', 'jobs', 'summary', 'publication', 'identity', 'isolation',
                'close-read', 'close-write', 'opening', 'bindings')
GUARD_PATHS = ('shared-partial', 'file-receipts', 'file-compatibility', 'opening-envelope',
               'claim-bounds', 'owner-retirement', 'inventory', 'ancestor')
SOURCES = {'lubancore_named_results': (10, '[sdk-named-results-path] ', PUBLIC_PATHS),
           'lubancore_named_result_guards': (8, '[sdk-named-result-guards-path] ', GUARD_PATHS)}
HEADER = 'include/lubancore/named_results.hpp'
HELPER = 'examples/sdk-consumer/named_results.cpp'
ADAPTER = 'src/sdk/named_results.cpp'
ENGINE_IMPLEMENTATIONS = ('src/trajectory/named_result_blobs.cpp', 'src/trajectory/named_result_opening.cpp')
SUMMARY_KEYS = {'session_id', 'tool_call_id', 'summary_event_id', 'source_revision', 'model_calls',
                'raw_results', 'formal_results', 'local_named_mirrors', 'summary_candidates',
                'selected', 'subsequent_request_verified'}


def require(ok, message):
    if not ok:
        raise RuntimeError('Named results: ' + message)


def check_registration(command, stem, executable='lubancore_sdk_tests'):
    require(stem in SOURCES, 'unknown native source')
    require(isinstance(command, list) and len(command) == 2 and all(isinstance(arg, str) for arg in command) and commands.absolute(command[0]) and
            command[0].replace('\\', '/').split('/')[-1] in (executable, executable + '.exe') and
            command[1] == '--source-file=*test_' + stem + '.cpp', 'requires absolute original single-source argv')


def check_summary(section, required):
    prefix = '[sdk-named-results-summary] '
    raw = [line.removeprefix(prefix) for line in section.splitlines() if line.startswith(prefix)]
    require(len(raw) == (1 if required else 0), 'summary witness missing, duplicated or emitted by another source')
    if not required:
        return None
    try:
        fact = json.loads(raw[0], object_pairs_hook=commands.unique_object,
                          parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (ValueError, TypeError) as error:
        raise RuntimeError('Named results: malformed actual summary JSON') from error
    require(isinstance(fact, dict) and set(fact) == SUMMARY_KEYS, 'summary witness field contract differs')
    for key in ('session_id', 'tool_call_id', 'summary_event_id'):
        value = fact[key]
        require(isinstance(value, str) and 0 < len(value.encode('utf-8')) <= 200 and
                not any(char in value for char in '\0\r\n'), 'summary identity is empty, unsafe or oversized')
    for key in ('source_revision', 'model_calls', 'summary_candidates'):
        require(type(fact[key]) is int and fact[key] > 0, 'summary needs actual revision, calls and candidates')
    for key, value in (('raw_results', 1), ('formal_results', 1), ('local_named_mirrors', 0)):
        require(type(fact[key]) is int and fact[key] == value, 'summary must use real raw/formal external materials')
    require(fact['selected'] is True and fact['subsequent_request_verified'] is True,
            'summary was not adopted into the verified subsequent model request')
    return fact


def check_native(section, command, stem):
    require(isinstance(command, list) and command and isinstance(command[0], str), 'malformed actual native argv')
    executable = command[0].replace('\\', '/').split('/')[-1].removesuffix('.exe')
    require(executable in ('lubancore_sdk_tests', 'lubancode_tests'), 'foreign native executable')
    check_registration(command, stem, executable)
    commands.actual_command(section, command)
    count, prefix, paths = SOURCES[stem]
    cases = re.findall(r'\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    assertions = re.findall(r'\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    require(cases == [(str(count), str(count), '0')], 'native source did not pass its complete CASE roster')
    require(len(assertions) == 1 and int(assertions[0][0]) > 0 and
            assertions[0][0] == assertions[0][1] and assertions[0][2] == '0', 'native assertions empty or failed')
    observed = commands.check_paths(section, prefix, paths)
    summary = check_summary(section, stem == 'lubancore_named_results')
    return {'source': stem, 'command': command, 'nativeCases': count, 'nativeAssertions': int(assertions[0][0]),
            'paths': observed, 'summary': summary,
            'scope': 'actual whole-source argv/completion/CASE/assertions/paths; private native receipts checked by source assertions'}


def check_consumer(section, command, probe):
    require(isinstance(command, list) and len(command) == 4 and all(isinstance(arg, str) for arg in command) and
            commands.absolute(command[0]) and commands.absolute(command[2]) and commands.absolute(command[3]) and
            command[0].replace('\\', '/').split('/')[-1] in ('lubancore_consumer', 'lubancore_consumer.exe') and
            command[1] == 'named-results', 'installed caller must explicitly name state and real relocated probe')
    commands.actual_command(section, command)
    require(command[3].replace('\\', '/') == probe['copy']['path'].replace('\\', '/'), 'borrowed a foreign probe')
    paths = commands.check_paths(section, '[sdk-named-results-path] ', PUBLIC_PATHS)
    check_summary(section, False)  # Installed helper has no internal Journal reader.
    require(section.splitlines().count('[sdk-named-results-consumer] complete') == 1 and
            section.splitlines().count('installed SDK consumer named-results passed') == 1, 'installed paths did not complete')
    return {'command': command, 'paths': paths,
            'scope': 'installed SDK/STL helper and real second-root bytes; internal V3 summary witness belongs to native source'}


def ownership_violations(targets, testing, with_cli=False):
    owners = {}
    for target in targets.values():
        compiled = target.get('luaSources')
        if compiled is None:
            compiled = [item['projectPath'] for item in target.get('sources', []) if item.get('compiled') and item.get('projectPath')]
        for source in (ADAPTER, HELPER, *ENGINE_IMPLEMENTATIONS):
            owners.setdefault(source, []).extend([(target['name'], target['type'])] * compiled.count(source))
    references = [('lubancore_sdk_tests', 'EXECUTABLE')] if testing else []
    if testing and with_cli:
        references.append(('lubancode_tests', 'EXECUTABLE'))
    errors = []
    for source, actual in owners.items():
        expected = [('lubancode_engine', 'STATIC_LIBRARY')] if source in ENGINE_IMPLEMENTATIONS else (
            references if source == HELPER else references + [('lubancore_sdk', 'SHARED_LIBRARY')])
        if sorted(actual) != sorted(expected):
            errors.append('Named results compiled owner closure differs for ' + source)
    return errors


def seal_sources(repo):
    repo = Path(repo)
    records = {}
    for path in (HEADER, HELPER, ADAPTER, *ENGINE_IMPLEMENTATIONS):
        data = (repo / path).read_bytes()
        records[path] = {'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    includes = re.findall(r'^#include\s*[<"]([^>"]+)[>"]', (repo / HELPER).read_text(encoding='utf-8'), re.M)
    standard = {'algorithm', 'atomic', 'chrono', 'condition_variable', 'cstdint', 'exception', 'filesystem', 'fstream',
                'functional', 'iostream', 'iterator', 'map', 'memory', 'mutex', 'stdexcept', 'string', 'thread', 'utility', 'vector'}
    require(set(includes) <= standard | {'lubancore/core.hpp', 'lubancore/named_results.hpp'}, 'helper includes nonpublic implementation')
    require({'lubancore/core.hpp', 'lubancore/named_results.hpp'} <= set(includes), 'helper public includes missing')
    return records


def check_copies(consumer_source, prefix, records, repo, evidence=None):
    copies = {}
    for source, path in ((HELPER, Path(consumer_source) / 'named_results.cpp'),
                         (HEADER, Path(prefix) / HEADER)):
        record = records[source]
        original = (Path(repo) / source).read_bytes()
        data = path.read_bytes()
        require(data == original and len(data) == record['bytes'] and
                hashlib.sha256(data).hexdigest() == record['sha256'], 'installed/source bytes changed: ' + source)
        if evidence is not None:
            target = Path(evidence) / ('consumer' if source == HELPER else 'installed') / Path(source).name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        copies[source] = {'path': str(path.resolve()), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    return copies
