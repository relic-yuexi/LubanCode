"""Check actual model-input source executions; never execute native programs."""
from __future__ import annotations

import re

try:
    from . import sdk_command_jobs as commands
except ImportError:
    try:
        import sdk_command_jobs as commands
    except ModuleNotFoundError:
        from scripts.ci import sdk_command_jobs as commands

SOURCES = {
    'lubancore_model_input': (6, '[sdk-model-input-path] ', (
        'exact-generate', 'refused-and-cancelled', 'legacy-and-four-wire',
        'rebuildable-forwarding', 'summary-prepared-adopted', 'summary-fail-closed')),
    'model_input_wrappers': (1, '[model-input-wrappers-path] ', ('spinner-three-state',)),
}


def require(ok, message):
    if not ok:
        raise RuntimeError('Model input: ' + message)


def check_registration(command, stem, exe='lubancore_sdk_tests'):
    require(stem in SOURCES, 'unknown source')
    require(exe in ('lubancore_sdk_tests', 'lubancode_tests'), 'foreign executable')
    require(stem != 'model_input_wrappers' or exe == 'lubancode_tests', 'CLI wrapper is not an SDK-only source')
    require(isinstance(command, list) and len(command) == 2 and all(isinstance(arg, str) for arg in command)
            and commands.absolute(command[0])
            and command[0].replace('\\', '/').split('/')[-1] in (exe, exe + '.exe')
            and command[1] == '--source-file=*test_' + stem + '.cpp',
            'requires absolute original single-source argv')


def check_native(section, command, stem):
    require(isinstance(command, list) and command and isinstance(command[0], str), 'malformed native argv')
    exe = command[0].replace('\\', '/').split('/')[-1].removesuffix('.exe')
    check_registration(command, stem, exe)
    commands.actual_command(section, command)
    count, prefix, paths = SOURCES[stem]
    cases = re.findall(r'\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    assertions = re.findall(r'\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    require(cases == [(str(count), str(count), '0')], 'whole source CASE roster did not pass')
    require(len(assertions) == 1 and int(assertions[0][0]) > 0 and assertions[0][0] == assertions[0][1]
            and assertions[0][2] == '0', 'assertions empty, duplicated or failed')
    observed = commands.check_paths(section, prefix, paths)
    for other, (_, other_prefix, _) in SOURCES.items():
        if other != stem:
            require(not any(line.startswith(other_prefix) for line in section.splitlines()), 'borrowed another source witness')
    return {'source': stem, 'command': command, 'nativeCases': count,
            'nativeAssertions': int(assertions[0][0]), 'paths': observed,
            'scope': 'actual original argv, completion, whole CASE/assertion roster and once paths; C++ assertions own request/material authority'}
