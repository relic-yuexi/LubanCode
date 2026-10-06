"""Pure evidence contract for real Owned Job deadline execution and retirement."""
import json
import ntpath
import re
import shlex

PATHS = ('registered-queue', 'authorization', 'runtime-budget', 'late-worker',
         'startup-close', 'strict-recovery')
FACT_KEYS = {'path', 'quiescent', 'global_running', 'started_intent',
             'command_not_invoked', 'command_calls', 'effective_timeout_ms'}
QUEUE_OBSERVATION_KEYS = {'unlimited_budget_ms', 'unlimited_queued', 'unlimited_started',
                          'unlimited_final_state', 'registration_budget_ms', 'immediate_state',
                          'elapsed_before_register_ms', 'global_running'}


def check_owned_job_deadline_registration(command, executable='lubancore_sdk_tests'):
    absolute = lambda value: isinstance(value, str) and bool(value) and not any(ch in value for ch in ('\0', '\n', '\r')) and (
        value.startswith('/') or (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0])))
    if (not isinstance(command, list) or len(command) != 2 or not absolute(command[0]) or
            command[0].replace('\\', '/').split('/')[-1] not in (executable, executable + '.exe') or
            command[1] != '--source-file=*test_lubancore_owned_job_deadline.cpp'):
        raise RuntimeError('Owned Job deadline requires its actual absolute single-source command')


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('duplicate fact field: ' + key)
        value[key] = item
    return value


def check_queue_observation(section):
    prefix = '[owned-job-deadline-queue-observation] '
    raw = [line.removeprefix(prefix) for line in section.splitlines() if line.startswith(prefix)]
    if len(raw) != 1:
        raise RuntimeError('Owned Job deadline requires exactly one actual queue observation')
    try:
        value = json.loads(raw[0], object_pairs_hook=unique_object,
                           parse_constant=lambda token: (_ for _ in ()).throw(ValueError(token)))
    except (ValueError, TypeError) as error:
        raise RuntimeError('Owned Job deadline queue observation is not strict JSON') from error
    if not isinstance(value, dict) or set(value) != QUEUE_OBSERVATION_KEYS:
        raise RuntimeError('Owned Job deadline queue observation shape differs')
    for key, expected in (('unlimited_budget_ms', 0), ('registration_budget_ms', 2000), ('global_running', 1)):
        if type(value[key]) is not int or value[key] != expected:
            raise RuntimeError('Owned Job deadline changed its actual queue budget or live blocker')
    if (value['unlimited_queued'] is not True or value['unlimited_started'] is not False or
            value['unlimited_final_state'] != 'cancelled'):
        raise RuntimeError('Owned Job deadline did not observe and retire a real unstarted queue ticket')
    elapsed = value['elapsed_before_register_ms']
    state = value['immediate_state']
    if type(elapsed) is not int or elapsed < 0 or state not in ('queued', 'cancelled'):
        raise RuntimeError('Owned Job deadline immediate state or elapsed observation is invalid')
    if state == 'cancelled' and elapsed < 1999:
        raise RuntimeError('Owned Job deadline cancellation preceded its earliest possible registration expiry')
    return value


def check_owned_job_deadline_native(section, command):
    if not isinstance(command, list) or not command or not isinstance(command[0], str):
        raise RuntimeError('Owned Job deadline actual command is malformed')
    executable = command[0].replace('\\', '/').split('/')[-1].removesuffix('.exe')
    if executable not in ('lubancore_sdk_tests', 'lubancode_tests'):
        raise RuntimeError('Owned Job deadline used a foreign executable')
    check_owned_job_deadline_registration(command, executable)
    commands = re.findall(r'^Command: ([^\r\n]*)\r?$', section, re.M)
    try:
        actual = shlex.split(commands[0].replace('\\', '/')) if len(commands) == 1 else None
    except ValueError as error:
        raise RuntimeError('Owned Job deadline actual argv is malformed') from error
    if actual != [value.replace('\\', '/') for value in command]:
        raise RuntimeError('Owned Job deadline actual argv differs from its registration')
    summaries = re.findall(r'\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    assertions = re.findall(r'\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', section)
    if summaries != [('6', '6', '0')]:
        raise RuntimeError('Owned Job deadline did not pass all six native cases')
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            assertions[0][0] != assertions[0][1] or int(assertions[0][2]) != 0 or
            section.splitlines().count('Test Passed.') != 1):
        raise RuntimeError('Owned Job deadline native assertions or completion did not pass')
    paths = [line.removeprefix('[owned-job-deadline-path] ') for line in section.splitlines()
             if line.startswith('[owned-job-deadline-path] ')]
    if sorted(paths) != sorted(PATHS):
        raise RuntimeError('Owned Job deadline has absent, duplicate or foreign paths')
    check_queue_observation(section)
    raw = [line.removeprefix('[owned-job-deadline-fact] ') for line in section.splitlines()
           if line.startswith('[owned-job-deadline-fact] ')]
    try:
        facts = [json.loads(line, object_pairs_hook=unique_object) for line in raw]
    except (ValueError, TypeError) as error:
        raise RuntimeError('Owned Job deadline fact is not strict JSON') from error
    if len(facts) != len(PATHS):
        raise RuntimeError('Owned Job deadline needs one actual terminal fact per path')
    seen = set()
    for fact in facts:
        if not isinstance(fact, dict) or set(fact) != FACT_KEYS:
            raise RuntimeError('Owned Job deadline fact shape differs from its versioned contract')
        path = fact['path']
        if not isinstance(path, str) or path not in PATHS or path in seen:
            raise RuntimeError('Owned Job deadline fact path is absent, duplicate or foreign')
        seen.add(path)
        if fact['quiescent'] is not True or type(fact['global_running']) is not int or fact['global_running'] != 0:
            raise RuntimeError('Owned Job deadline did not actually retire all running workers')
        if type(fact['started_intent']) is not bool or type(fact['command_not_invoked']) is not bool:
            raise RuntimeError('Owned Job deadline execution observations must be typed booleans')
        calls, timeout = fact['command_calls'], fact['effective_timeout_ms']
        if type(calls) is not int or calls < 0:
            raise RuntimeError('Owned Job deadline command count must be an actual nonnegative integer')
        if calls == 0:
            if timeout is not None:
                raise RuntimeError('An uninvoked command cannot invent an effective runtime budget')
        elif type(timeout) is not int or timeout <= 0 or not fact['started_intent'] or fact['command_not_invoked']:
            raise RuntimeError('A real command requires a positive budget and consistent execution facts')
        if path in ('registered-queue', 'authorization') and (fact['started_intent'] or calls != 0):
            raise RuntimeError('Queue/authorization deadline must expire before Started and command entry')
        if path in ('late-worker', 'strict-recovery') and (
                not fact['started_intent'] or not fact['command_not_invoked'] or calls != 0):
            raise RuntimeError('Late published worker must retain intent and prove no command invocation')
        if path in ('runtime-budget', 'startup-close') and calls <= 0:
            raise RuntimeError('Runtime/startup retirement path did not reach the real command')
    return facts
