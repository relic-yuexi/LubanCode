"""Actual native receipts for the project commit ownership handoff."""
import ntpath
import re
import shlex

PATHS = ('same-directory', 'aliases', 'waiting-cancel', 'independent', 'retirement', 'external-lock')
SOURCE = '--source-file=*test_memory_project_commit_handoff.cpp'


def check_memory_handoff_registration(command, executable='lubancore_sdk_tests'):
    absolute = lambda value: isinstance(value, str) and bool(value) and not any(
        ch in value for ch in ('\0', '\n', '\r')) and (
        value.startswith('/') or (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0])))
    if (not isinstance(command, list) or len(command) != 2 or not absolute(command[0]) or
            command[0].replace('\\', '/').split('/')[-1] not in (executable, executable + '.exe') or
            command[1] != SOURCE):
        raise RuntimeError('Memory handoff requires its actual absolute single-source command')


def check_memory_handoff_native(section, command):
    if not isinstance(command, list) or not command or not isinstance(command[0], str):
        raise RuntimeError('Memory handoff native command is malformed')
    executable = command[0].replace('\\', '/').split('/')[-1].removesuffix('.exe')
    if executable not in ('lubancore_sdk_tests', 'lubancode_tests'):
        raise RuntimeError('Memory handoff used a foreign executable')
    check_memory_handoff_registration(command, executable)
    raw = re.findall(r'^Command: ([^\r\n]*)\r?$', section, re.M)
    try:
        actual = shlex.split(raw[0].replace('\\', '/')) if len(raw) == 1 else None
    except ValueError as error:
        raise RuntimeError('Memory handoff actual argv is malformed') from error
    if actual != [value.replace('\\', '/') for value in command]:
        raise RuntimeError('Memory handoff actual argv differs from its registration')
    cases = re.findall(r'\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    assertions = re.findall(r'\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    if cases != [('6', '6', '0')]:
        raise RuntimeError('Memory handoff did not pass every registered native case')
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            assertions[0][0] != assertions[0][1] or assertions[0][2] != '0' or
            section.splitlines().count('Test Passed.') != 1):
        raise RuntimeError('Memory handoff native assertions or completion did not pass')
    prefix = '[memory-project-handoff-path] '
    paths = [line.removeprefix(prefix) for line in section.splitlines() if line.startswith(prefix)]
    if sorted(paths) != sorted(PATHS):
        raise RuntimeError('Memory handoff has missing, duplicate or foreign actual paths')
    return paths
