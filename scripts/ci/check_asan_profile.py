#!/usr/bin/env python3
"""Private ASan compile closure and actual execution evidence; no native work in prepare.

Only ``register`` runs CTest, with --show-only, in remote CI. All other modes
read source/JSON/XML/logs. The existing workflow selectors remain authoritative.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import xml.etree.ElementTree as ET

try:
    from .check_sdk_only_boundary import CLIENT, prepare, read_reply
    from .check_sdk_focused import check_native_command
except ImportError:
    from check_sdk_only_boundary import CLIENT, prepare, read_reply
    from check_sdk_focused import check_native_command


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def selectors_from_workflow(text):
    blocks = re.findall(r'^  linux-asan:\n(.*?)(?=^  [A-Za-z][\w-]*:|\Z)', text, re.M | re.S)
    require(len(blocks) == 1, 'expected one actual linux-asan job')
    lines = re.sub(r'\\\n\s*', ' ', blocks[0]).splitlines()
    commands = [shlex.split(line.strip()) for line in lines if line.strip().startswith('ctest ')]
    require(len(commands) == 2, 'expected the original two ASan CTest selector commands')
    selectors = []
    for index, command in enumerate(commands):
        require(command.count('-R') == 1 and command.count('-E') <= 1,
                'ambiguous ASan selector')
        include = command[command.index('-R') + 1]
        exclude = command[command.index('-E') + 1] if '-E' in command else ''
        re.compile(include)
        re.compile(exclude)
        require(('--show-only=json-v1' in command) == (index == 0),
                'ASan selector order changed')
        selectors.append({'include': include, 'exclude': exclude})
    return selectors


def source_roster(source):
    sources, basenames = {}, set()
    paths = sorted(path for kind in ('unit', 'integration')
                   for path in (source / 'tests' / kind).rglob('test_*.cpp'))
    for path in paths:
        relative = path.relative_to(source).as_posix()
        if relative == 'tests/unit/job_runner/test_runner_release_diagnostic.cpp':
            continue  # Original CMake owns this source in its separate Runner executable.
        match = re.fullmatch(r'tests/(unit|integration)/([^/]+)/test_([A-Za-z0-9_]+)\.cpp', relative)
        require(match is not None, 'unclassified test source: ' + relative)
        require(path.name not in basenames, 'duplicate test basename: ' + path.name)
        basenames.add(path.name)
        name = '.'.join(match.groups())
        require(name not in sources, 'duplicate CTest name: ' + name)
        sources[name] = relative
    require(bool(sources), 'empty source roster')
    return sources


def attachment_sources(text):
    """Read this checkout's unchanged aggregate support/private/helper additions."""
    blocks = re.findall(r'add_executable\(lubancode_tests\s+(.*?)\n\)', text, re.S)
    require(len(blocks) == 1, 'aggregate executable definition is ambiguous')
    support = re.findall(r'^\s*(support/[A-Za-z0-9_]+\.cpp)\s*$', blocks[0], re.M)
    additions = re.findall(r'target_sources\(lubancode_tests PRIVATE "\$\{CMAKE_SOURCE_DIR\}/([^"\n]+)"\)', text)
    result = ['tests/' + path for path in support] + additions
    require(bool(support) and bool(additions), 'aggregate attachment roster is empty')
    require(len(result) == len(set(result)), 'duplicate aggregate attachment')
    require(all(re.fullmatch(r'(tests/support|src/sdk|examples/sdk-consumer)/[A-Za-z0-9_]+\.cpp', path)
                for path in result), 'unexpected aggregate attachment spelling')
    return sorted(result)


def make_manifest(source):
    source = source.resolve()
    workflow = source / '.github/workflows/ci.yml'
    selectors = selectors_from_workflow(workflow.read_text(encoding='utf-8'))
    roster = source_roster(source)
    selected = []
    for selector in selectors:
        selected.append(sorted(name for name in roster if re.search(selector['include'], name)
                               and not (selector['exclude'] and re.search(selector['exclude'], name))))
    require(all(selected), 'an ASan selector matched no source')
    union = sorted(set(selected[0]) | set(selected[1]))
    # The first registration is the specialized gate. The second is the actual
    # execution command; reject a future change that would only register a book.
    require(set(selected[0]) <= set(selected[1]), 'required ASan source is absent from execution selector')
    matches = {name: sorted(path for path in roster.values()
                           if fnmatch.fnmatchcase(path, '*' + Path(roster[name]).name)) for name in union}
    compiled = sorted({path for paths in matches.values() for path in paths})
    attachments = attachment_sources((source / 'tests/CMakeLists.txt').read_text(encoding='utf-8'))
    tracked_inputs = ['.github/workflows/ci.yml', 'tests/CMakeLists.txt',
                      'cmake/LubanCoreTests.cmake', 'cmake/LubanCoreHostTests.cmake']
    return {'schema': 1, 'source_root': source.as_posix(), 'selectors': selectors,
            'selected': selected, 'roster': roster, 'wildcard_matches': matches,
            'compile_sources': compiled, 'attachments': attachments,
            'source_sha256': {path: digest(source / path) for path in sorted(set(roster.values()) | set(attachments))},
            'input_sha256': {path: digest(source / path) for path in tracked_inputs}}


def load_manifest(source, filename):
    value = json.loads(filename.read_text(encoding='utf-8'))
    require(value == make_manifest(source), 'ASan manifest differs from current source/workflow closure')
    return value


def read_graph(source, build):
    reply = build / '.cmake/api/v1/reply'
    indices = sorted(reply.glob('index-*.json'))
    require(bool(indices), 'missing actual File API reply')
    index = json.loads(indices[-1].read_text(encoding='utf-8'))
    references = index.get('reply', {}).get(CLIENT, {})
    require(all(name in references for name in ('codemodel-v2', 'cache-v2')), 'missing ASan File API queries')
    model = read_reply(reply, references['codemodel-v2'])
    cache = read_reply(reply, references['cache-v2'])
    require(model.get('kind') == 'codemodel' and model.get('version', {}).get('major') == 2,
            'unsupported actual codemodel')
    require(cache.get('kind') == 'cache' and cache.get('version', {}).get('major') == 2,
            'unsupported actual cache')
    require(Path(model['paths']['source']).resolve() == source and Path(model['paths']['build']).resolve() == build,
            'File API source/build root differs')
    configs = [config for config in model['configurations'] if config['name'] == 'Release']
    require(len(configs) == 1, 'expected one Release codemodel')
    targets = {}
    for reference in configs[0].get('targets', []):
        target = read_reply(reply, reference)
        require(target.get('id') == reference.get('id') and target.get('name') == reference.get('name'),
                'File API target reference differs')
        require(target['id'] not in targets, 'duplicate File API target')
        targets[target['id']] = target
    return targets, {entry['name']: entry['value'] for entry in cache['entries']}


def check_graph(manifest, targets, cache, source, build, filename):
    for name in ('LUBANCODE_ASAN_TEST_PROFILE', 'LUBANCODE_BUILD_CLI', 'LUBANCODE_BUILD_SDK'):
        require(str(cache.get(name, '')).upper() in {'ON', 'TRUE', 'YES', '1'}, 'ASan cache option differs: ' + name)
    if 'LUBANCORE_WITH_LUA' in cache:
        require(str(cache['LUBANCORE_WITH_LUA']).upper() in {'ON', 'TRUE', 'YES', '1'}, 'ASan requires Lua')
    require(Path(cache.get('LUBANCODE_ASAN_PROFILE_FILE', '')).resolve() == filename.resolve(),
            'configured manifest path differs')
    require(cache.get('CMAKE_BUILD_TYPE') == 'Release', 'ASan build type differs')
    for flag in ('CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS', 'CMAKE_EXE_LINKER_FLAGS'):
        require('-fsanitize=address' in shlex.split(cache.get(flag, '')), 'missing actual sanitizer flag: ' + flag)

    def one(name):
        matches = [target for target in targets.values() if target['name'] == name]
        require(len(matches) == 1, 'missing or duplicate actual target: ' + name)
        return matches[0]

    aggregate = one('lubancode_tests')
    require(one('lubancode_lua').get('type') == 'STATIC_LIBRARY', 'actual Lua target missing')
    require(aggregate.get('type') == 'EXECUTABLE', 'aggregate target type differs')
    require(all(one(name).get('type') == 'EXECUTABLE' for name in ('lubancore_sdk_tests', 'lubancore_host_tests')),
            'reference targets must remain defined')
    expected = sorted(manifest['compile_sources'] + manifest['attachments'])
    groups = aggregate.get('compileGroups', [])
    require(bool(groups), 'aggregate compile groups missing')
    owner_path = aggregate.get('paths', {}).get('build')
    require(isinstance(owner_path, str) and bool(owner_path), 'aggregate build owner path missing')
    owner = Path(owner_path)
    owner = (owner if owner.is_absolute() else build / owner).resolve()
    require(owner.is_relative_to(build), 'aggregate build owner escapes the build tree')
    pch_source = owner / 'CMakeFiles' / (aggregate['name'] + '.dir') / 'cmake_pch.hxx.cxx'
    actual, generated = [], []
    for source_index, item in enumerate(aggregate.get('sources', [])):
        if 'compileGroupIndex' not in item:
            continue
        require(type(item['compileGroupIndex']) is int and 0 <= item['compileGroupIndex'] < len(groups),
                'aggregate source references an invalid compile group')
        path = Path(item['path'])
        path = (path if path.is_absolute() else source / path).resolve()
        if path == pch_source:
            # CMake's actual PCH translation unit can omit isGenerated. Its
            # target-owned path and the linked compile group's header identify
            # this single exception; a same-named file elsewhere is no substitute.
            group = groups[item['compileGroupIndex']]
            headers = group.get('precompileHeaders', [])
            require(len(headers) == 1 and
                    Path(headers[0].get('header', '')).is_absolute() and
                    Path(headers[0].get('header', '')).resolve() == source / 'tests/support/pch.hpp' and
                    group.get('sourceIndexes', []).count(source_index) == 1,
                    'aggregate PCH lacks its actual header/compile-group binding')
            generated.append(str(path))
        else:
            require(not item.get('isGenerated'), 'unexpected generated aggregate compilation: ' + str(path))
            require(path.is_relative_to(source), 'foreign aggregate source: ' + str(path))
            actual.append(path.relative_to(source).as_posix())
    require(sorted(actual) == expected, 'actual aggregate compilation closure differs')
    require(len(generated) == 1, 'actual aggregate PCH compilation missing or duplicated')
    for group in groups:
        require(group.get('language') == 'CXX', 'aggregate language differs')
        fragments = ' '.join(item['fragment'] for item in group.get('compileCommandFragments', []))
        require('-fsanitize=address' in shlex.split(fragments), 'aggregate lacks actual address instrumentation')
        defines = [entry['define'] for entry in group.get('defines', [])]
        require('LUBANCORE_TEST_JOB_POST_SDK=1' in defines, 'Job SDK fixture definition missing')
        for macro, probe in (('LUBANCORE_TEST_SEARCH_PROBE', 'lubancore_sdk_search_probe'),
                             ('LUBANCORE_TEST_COMMAND_LIMITS_PROBE', 'lubancore_command_limits_probe')):
            artifacts = one(probe).get('artifacts', [])
            require(len(artifacts) == 1, 'probe executable artifact differs')
            probe_path = Path(artifacts[0]['path'])
            probe_path = (probe_path if probe_path.is_absolute() else build / probe_path).resolve()
            require(probe_path.is_relative_to(build) and probe_path.is_file(), 'probe executable not built')
            require(defines.count(macro + '="' + probe_path.as_posix() + '"') == 1,
                    'actual probe path definition differs: ' + macro)
    pch = [entry['header'] for group in groups for entry in group.get('precompileHeaders', [])]
    require(bool(pch) and all(Path(path).resolve() == source / 'tests/support/pch.hpp' for path in pch),
            'aggregate PCH differs')
    seen, pending = set(), [aggregate['id']]
    while pending:
        key = pending.pop()
        if key in seen:
            continue
        require(key in targets, 'unknown actual target dependency')
        seen.add(key)
        pending.extend(dependency['id'] for dependency in targets[key].get('dependencies', []))
    names = {targets[key]['name'] for key in seen}
    require(not names & {'lubancore_sdk_tests', 'lubancore_host_tests'}, 'aggregate still builds companion tests')
    dependencies = {'lubancode_app', 'lubancore_sdk', 'lubancore_sdk_search_probe',
                    'lubancore_command_limits_probe', 'hello_plugin_fixture', 'bad_version_plugin',
                    'memory_worker_fixture', 'fake_ripgrep', 'gateway_lock_racer', 'memory_lock_racer',
                    'workspace_manifest_racer', 'updater_lock_racer', 'updater_probe_stub', 'model_probe_e2e'}
    require(dependencies <= names, 'aggregate production/probe/fixture dependency missing')
    artifacts = aggregate.get('artifacts', [])
    require(len(artifacts) == 1, 'aggregate executable artifact differs')
    executable = Path(artifacts[0]['path'])
    executable = (executable if executable.is_absolute() else build / executable).resolve()
    require(executable.is_relative_to(build) and executable.name == 'lubancode_tests' and executable.is_file(),
            'aggregate executable was not built at its actual artifact path')
    return {'status': 'passed', 'executable': executable.as_posix(), 'compile_sources': actual,
            'generated_pch': generated, 'dependency_names': sorted(names)}


def check_registration(manifest, registration, index, executable):
    tests = registration.get('tests', [])
    names = [test.get('name') for test in tests]
    require(sorted(names) == manifest['selected'][index], 'actual selector registration roster differs')
    commands = {}
    for test in tests:
        name = test['name']
        expected = [executable, '--source-file=*' + Path(manifest['roster'][name]).name]
        require(test.get('command') == expected, 'actual aggregate full argv differs: ' + name)
        properties = {item['name']: item['value'] for item in test.get('properties', [])}
        require(not properties.get('DISABLED'), 'selected ASan source disabled: ' + name)
        require(properties.get('TIMEOUT') == (300 if name.startswith('integration.') else 180),
                'original per-source timeout differs: ' + name)
        commands[name] = expected
    return commands


def check_execution(manifest, registrations, executable, junit, last_test):
    commands = {}
    for index, registration in enumerate(registrations):
        commands.update(check_registration(manifest, registration, index, executable))
    expected = sorted(set(manifest['selected'][0]) | set(manifest['selected'][1]))
    cases = junit.findall('.//testcase')
    require(sorted(case.attrib.get('name', '') for case in cases) == expected,
            'actual ASan JUnit roster is missing, duplicated or foreign')
    parts = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$', last_test, flags=re.M)
    require(sorted(parts[1::2]) == expected, 'actual ASan LastTest roster differs')
    sections = dict(zip(parts[1::2], parts[2::2]))
    totals = {'sources': len(expected), 'cases': 0, 'assertions': 0}
    for case in cases:
        name = case.attrib['name']
        require(case.attrib.get('status') == 'run' and all(case.find(tag) is None for tag in ('failure', 'error', 'skipped')),
                'ASan source failed or skipped: ' + name)
        section = sections[name]
        check_native_command(section, commands[name])
        require(len(re.findall(r'^Test Passed\.\s*$', section, re.M)) == 1, 'native test did not pass: ' + name)
        for label, total_key in (('test cases', 'cases'), ('assertions', 'assertions')):
            counts = re.findall(r'\[doctest\] ' + label + r':\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
            require(len(counts) == 1, 'native summary missing or duplicated: ' + name + ' ' + label)
            total, passed, failed = map(int, counts[0])
            require(total > 0 and passed == total and failed == 0, 'native summary empty or failing: ' + name)
            totals[total_key] += total
    return {'status': 'passed', **totals}


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('prepare', 'graph', 'register', 'execution'))
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--build', type=Path, required=True)
    args = parser.parse_args()
    source, build = args.source.resolve(), args.build.resolve()
    folder = build / 'asan-profile'
    manifest_path = folder / 'manifest.json'
    if args.mode == 'prepare':
        manifest = make_manifest(source)
        write_json(manifest_path, manifest)
        prepare(build)
        print(f"ASan profile: {len(manifest['compile_sources'])} source bodies; "
              f"{len(manifest['selected'][0])}/{len(manifest['selected'][1])} selected registrations")
        return
    manifest = load_manifest(source, manifest_path)
    if args.mode == 'graph':
        targets, cache = read_graph(source, build)
        report = check_graph(manifest, targets, cache, source, build, manifest_path)
        write_json(folder / 'graph.json', report)
    elif args.mode == 'register':
        # Remote CI only: preserves the actual second command's -R/-E selection
        # and captures its registration before the original execution command.
        selector = manifest['selectors'][1]
        command = ['ctest', '--test-dir', str(build), '-C', 'Release', '-R', selector['include']]
        if selector['exclude']:
            command += ['-E', selector['exclude']]
        command += ['--show-only=json-v1']
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        (folder / 'execution-registration.json').write_bytes(result.stdout)
        (folder / 'execution-registration.stderr').write_bytes(result.stderr)
        require(result.returncode == 0, 'actual ASan execution registration failed')
        executable = json.loads((folder / 'graph.json').read_text(encoding='utf-8'))['executable']
        check_registration(manifest, json.loads(result.stdout), 1, executable)
    else:
        report_path = folder / 'execution.json'
        try:
            executable = json.loads((folder / 'graph.json').read_text(encoding='utf-8'))['executable']
            registrations = [json.loads(path.read_text(encoding='utf-8')) for path in
                             (build / 'asan-host-registration.json', folder / 'execution-registration.json')]
            report = check_execution(manifest, registrations, executable,
                                     ET.parse(build / 'asan-results.xml').getroot(),
                                     (build / 'Testing/Temporary/LastTest.log').read_text(encoding='utf-8'))
        except Exception as error:
            write_json(report_path, {'status': 'failed', 'error': str(error)})
            raise
        write_json(report_path, report)
    print('ASan profile ' + args.mode + ': passed')


if __name__ == '__main__':
    main()
