"""Actual native receipt gate for File result publication, with no execution."""
import ntpath
import re
import shlex

PATHS = ('actual', 'race', 'exclusive-temp', 'file-failure', 'directory-unknown', 'store', 'partial', 'listing')
SOURCE = '--source-file=*test_v3_result_immutable_publication.cpp'


def check_result_immutable_registration(command, executable='lubancore_sdk_tests'):
    absolute = lambda value: isinstance(value, str) and bool(value) and not any(
        ch in value for ch in ('\0', '\n', '\r')) and (
        value.startswith('/') or (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0])))
    if (not isinstance(command, list) or len(command) != 2 or not absolute(command[0]) or
            command[0].replace('\\', '/').split('/')[-1] not in (executable, executable + '.exe') or
            command[1] != SOURCE):
        raise RuntimeError('Immutable result publication requires its actual absolute single-source command')


def check_result_immutable_native(section, command):
    if not isinstance(command, list) or not command or not isinstance(command[0], str):
        raise RuntimeError('Immutable result native command is malformed')
    executable = command[0].replace('\\', '/').split('/')[-1].removesuffix('.exe')
    if executable not in ('lubancore_sdk_tests', 'lubancode_tests'):
        raise RuntimeError('Immutable result native executable is foreign')
    check_result_immutable_registration(command, executable)
    raw = re.findall(r'^Command: ([^\r\n]*)\r?$', section, re.M)
    try:
        actual = shlex.split(raw[0].replace('\\', '/')) if len(raw) == 1 else None
    except ValueError as error:
        raise RuntimeError('Immutable result actual argv is malformed') from error
    if actual != [value.replace('\\', '/') for value in command]:
        raise RuntimeError('Immutable result actual argv differs from registration')
    cases = re.findall(r'\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    assertions = re.findall(r'\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    if cases != [('8', '8', '0')]:
        raise RuntimeError('Immutable result publication did not pass all eight native cases')
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or assertions[0][0] != assertions[0][1] or
            assertions[0][2] != '0' or section.splitlines().count('Test Passed.') != 1):
        raise RuntimeError('Immutable result native assertions or completion did not pass')
    prefix = '[result-immutable-publication-path] '
    paths = [line.removeprefix(prefix) for line in section.splitlines() if line.startswith(prefix)]
    if sorted(paths) != sorted(PATHS):
        raise RuntimeError('Immutable result publication paths are missing, duplicated or foreign')
    return paths
