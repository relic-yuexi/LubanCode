"""Read actual single-source native receipts for private Managed opening."""
import ntpath
import re
import shlex

SOURCES = {
    'managed_session_ownership': ('actual', 'local', 'recovery', 'lock', 'drift', 'unknown'),
    'managed_session_reservation': ('actual', 'reserve', 'unknown', 'not-committed', 'finish',
                                    'local', 'refusal', 'locked-refusal', 'ancestor', 'delete-residue'),
}


def check_managed_opening_registration(command, stem, executable='lubancore_sdk_tests'):
    if stem not in SOURCES:
        raise RuntimeError('Unknown Managed opening native source')
    absolute = lambda value: isinstance(value, str) and bool(value) and not any(
        ch in value for ch in ('\0', '\n', '\r')) and (
        value.startswith('/') or (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0])))
    if (not isinstance(command, list) or len(command) != 2 or not absolute(command[0]) or
            command[0].replace('\\', '/').split('/')[-1] not in (executable, executable + '.exe') or
            command[1] != '--source-file=*test_' + stem + '.cpp'):
        raise RuntimeError('Managed opening requires its actual absolute single-source command')


def check_managed_opening_native(section, command, stem):
    if not isinstance(command, list) or not command or not isinstance(command[0], str):
        raise RuntimeError('Managed opening native command is malformed')
    executable = command[0].replace('\\', '/').split('/')[-1].removesuffix('.exe')
    if executable not in ('lubancore_sdk_tests', 'lubancode_tests'):
        raise RuntimeError('Managed opening used a foreign executable')
    check_managed_opening_registration(command, stem, executable)
    raw = re.findall(r'^Command: ([^\r\n]*)\r?$', section, re.M)
    try:
        actual = shlex.split(raw[0].replace('\\', '/')) if len(raw) == 1 else None
    except ValueError as error:
        raise RuntimeError('Managed opening actual argv is malformed') from error
    if actual != [value.replace('\\', '/') for value in command]:
        raise RuntimeError('Managed opening actual argv differs from its registration')
    count = str(len(SOURCES[stem]))
    cases = re.findall(r'\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    assertions = re.findall(r'\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    if cases != [(count, count, '0')]:
        raise RuntimeError('Managed opening did not pass every registered native case')
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            assertions[0][0] != assertions[0][1] or assertions[0][2] != '0' or
            section.splitlines().count('Test Passed.') != 1):
        raise RuntimeError('Managed opening native assertions or completion did not pass')
    prefix = '[' + stem.replace('_', '-') + '-path] '
    paths = [line.removeprefix(prefix) for line in section.splitlines() if line.startswith(prefix)]
    if sorted(paths) != sorted(SOURCES[stem]):
        raise RuntimeError('Managed opening has missing, duplicate or foreign actual paths')
    return paths
