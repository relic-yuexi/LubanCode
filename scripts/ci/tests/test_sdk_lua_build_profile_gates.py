"""Actual gate data and File API fixtures; no CMake/compiler/native execution."""
from copy import deepcopy
from pathlib import Path
import json
import os
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts.ci import check_installed_sdk as installed
from scripts.ci import check_sdk_build_closure as closure
from scripts.ci import check_sdk_focused as focused
from scripts.ci import check_sdk_lua_cross_profile as cross
from scripts.ci import sdk_lua_profile as profile
from scripts.ci.tests import test_sdk_only_boundary as fixtures


def command_line(command):
    return 'Command: ' + ' '.join('"' + value + '"' for value in command)


class ProfileGraphTests(unittest.TestCase):
    def graph(self, enabled):
        targets = {
            'sdk': {'name': 'lubancore_sdk', 'type': 'SHARED_LIBRARY', 'dependencies': ['runtime'],
                    'projectSources': ['src/sdk/core.cpp', 'src/sdk/job_operations.cpp',
                                       profile.LUA_LOAD if enabled else profile.LUA_UNAVAILABLE]},
            'engine': {'name': 'lubancode_engine', 'type': 'STATIC_LIBRARY', 'dependencies': [],
                       'projectSources': ['src/channel/types.cpp', 'src/channel/channel_config.cpp',
                                          *sorted(closure.SDK_NEUTRAL_PACKAGE_SOURCES)]},
            'runtime': {'name': 'lubancode_runtime', 'type': 'STATIC_LIBRARY', 'dependencies': ['engine'],
                        'projectSources': ['src/runtime/session_runtime.cpp']},
        }
        if enabled:
            targets['lua'] = {'name': 'lubancode_lua', 'type': 'STATIC_LIBRARY', 'dependencies': [],
                              'projectSources': ['/real/_deps/lua-src/lapi.c']}
            targets['engine']['projectSources'].append(profile.LUA_ENGINE_SOURCE)
            targets['runtime']['projectSources'].extend(sorted(profile.LUA_RUNTIME_SOURCES))
            targets['engine']['dependencies'].append('lua')
            targets['runtime']['dependencies'].append('lua')
        return targets

    def test_explicit_boolean_and_minimal_configuration(self):
        for value in profile.TRUE | profile.FALSE:
            enabled = value in profile.TRUE
            self.assertEqual(profile.read_lua_profile({'LUBANCORE_WITH_LUA': value,
                'LUBANCODE_BUILD_CLI': 'OFF', 'LUBANCODE_BUILD_SDK': 'ON'}, 'on' if enabled else 'off'), enabled)
        invalid = ({}, {'LUBANCORE_WITH_LUA': 'AUTO'}, {'LUBANCORE_WITH_LUA': 'OFF'},
                   {'LUBANCORE_WITH_LUA': 'OFF', 'LUBANCODE_BUILD_CLI': 'ON', 'LUBANCODE_BUILD_SDK': 'ON'},
                   {'LUBANCORE_WITH_LUA': 'OFF', 'LUBANCODE_BUILD_CLI': 'OFF', 'LUBANCODE_BUILD_SDK': 'OFF'})
        for entries in invalid:
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                profile.read_lua_profile(entries, 'off')
        with self.assertRaises(ValueError): profile.read_lua_profile({'LUBANCORE_WITH_LUA': 'ON'}, 'off')

    def test_real_closure_graph_accepts_both_distinct_profiles(self):
        for enabled in (True, False):
            report = closure.inspect_graph(self.graph(enabled), enabled)
            self.assertEqual(report['status'], 'passed', report['violations'])

    def test_selected_loader_must_have_exact_shared_owner(self):
        for enabled in (True, False):
            graph = self.graph(enabled)
            source = profile.LUA_LOAD if enabled else profile.LUA_UNAVAILABLE
            for variant in ('missing', 'duplicate', 'static', 'wrong-owner', 'both'):
                targets = deepcopy(graph)
                if variant == 'missing': targets['sdk']['projectSources'].remove(source)
                if variant == 'duplicate': targets['sdk']['projectSources'].append(source)
                if variant == 'static': targets['sdk']['type'] = 'STATIC_LIBRARY'
                if variant == 'wrong-owner':
                    targets['sdk']['projectSources'].remove(source)
                    targets['runtime']['projectSources'].append(source)
                if variant == 'both':
                    targets['sdk']['projectSources'].append(profile.LUA_UNAVAILABLE if enabled else profile.LUA_LOAD)
                with self.subTest(enabled=enabled, variant=variant):
                    if variant == 'static':
                        with self.assertRaises(ValueError): closure.inspect_graph(targets, enabled)
                    else:
                        self.assertEqual(closure.inspect_graph(targets, enabled)['status'], 'failed')

    def test_on_retains_each_of_six_original_owners(self):
        for source in profile.LUA_SOURCES:
            owner = 'engine' if source == profile.LUA_ENGINE_SOURCE else 'runtime'
            for variant in ('missing', 'duplicate', 'wrong-owner'):
                targets = self.graph(True)
                if variant == 'missing': targets[owner]['projectSources'].remove(source)
                if variant == 'duplicate': targets[owner]['projectSources'].append(source)
                if variant == 'wrong-owner':
                    targets[owner]['projectSources'].remove(source)
                    targets['sdk']['projectSources'].append(source)
                with self.subTest(source=source, variant=variant):
                    self.assertEqual(closure.inspect_graph(targets, True)['status'], 'failed')

    def test_off_rejects_disconnected_or_renamed_native_lua_material(self):
        for source in (*profile.LUA_SOURCES, '/hidden/_deps/lua-src/lapi.c', r'C:\hidden\lua-build\generated.c'):
            targets = self.graph(False)
            targets['hidden'] = {'name': 'innocent', 'type': 'STATIC_LIBRARY', 'dependencies': [], 'projectSources': [source]}
            with self.subTest(source=source):
                self.assertEqual(closure.inspect_graph(targets, False)['status'], 'failed')
        for name in ('lubancode_lua', 'LubanCode_Lua', 'lua', 'lua_static', 'lua_shared'):
            targets = self.graph(False)
            targets['hidden'] = {'name': name, 'type': 'STATIC_LIBRARY', 'dependencies': [], 'projectSources': []}
            with self.subTest(name=name):
                self.assertEqual(closure.inspect_graph(targets, False)['status'], 'failed')

    def test_exact_rosters_keep_old_on_cases_and_define_off_separately(self):
        self.assertEqual(len(focused.REQUIRED), 70)
        self.assertEqual(len(installed.REQUIRED_TESTS), 37)
        self.assertEqual(len(profile.focused_roster(focused.REQUIRED, False)), 68)
        self.assertEqual(len(profile.consumer_roster(installed.REQUIRED_TESTS, False)), 34)
        self.assertEqual(profile.focused_roster(focused.REQUIRED, True), focused.REQUIRED)
        self.assertEqual(profile.consumer_roster(installed.REQUIRED_TESTS, True), installed.REQUIRED_TESTS)
        self.assertEqual(focused.REQUIRED - profile.focused_roster(focused.REQUIRED, False),
                         {'sdk.focused.lubancore_lua', 'sdk.focused.lua_protected'})
        self.assertEqual(installed.REQUIRED_TESTS - profile.consumer_roster(installed.REQUIRED_TESTS, False),
                         {'sdk.consumer.lua', 'sdk.consumer.lua_seed', 'sdk.consumer.lua_resume'})
        self.assertIn('sdk.focused.lubancore_lua_build_profile', profile.focused_roster(focused.REQUIRED, False))
        self.assertIn('sdk.consumer.lua_build_profile', profile.consumer_roster(installed.REQUIRED_TESTS, False))


class ActualFileApiTests(unittest.TestCase):
    def fixture(self, enabled):
        value = fixtures.BoundaryTests(); value.setUp(); self.addCleanup(value.doCleanups)
        value.add_package_parsers()
        value.flags['LUBANCORE_WITH_LUA'] = 'ON' if enabled else 'OFF'
        value.source_file('src/sdk/job_operations.cpp', '#include <string>\n')
        value.source_file('src/channel/types.cpp', '#include <string>\n')
        value.source_file('src/channel/channel_config.cpp', '#include <string>\n')
        value.targets[1]['sources'].extend({'path': name, 'compileGroupIndex': 0}
            for name in ('src/channel/types.cpp', 'src/channel/channel_config.cpp'))
        loader = profile.LUA_LOAD if enabled else profile.LUA_UNAVAILABLE
        value.source_file(loader, '#include <string>\n')
        value.targets[0]['sources'].extend({'path': name, 'compileGroupIndex': 0}
            for name in ('src/sdk/job_operations.cpp', loader))
        runtime = {'id': 'runtime', 'name': 'lubancode_runtime', 'type': 'STATIC_LIBRARY',
                   'sources': [], 'compileGroups': [{}], 'dependencies': [{'id': 'engine'}]}
        value.targets.append(runtime)
        value.targets[0]['dependencies'] = [{'id': 'runtime'}]
        if enabled:
            for name in profile.LUA_SOURCES:
                value.source_file(name, '#include <string>\n')
                owner = value.targets[1] if name == profile.LUA_ENGINE_SOURCE else runtime
                owner['sources'].append({'path': name, 'compileGroupIndex': 0})
            value.targets.append({'id': 'lua', 'name': 'lubancode_lua', 'type': 'STATIC_LIBRARY',
                'sources': [{'path': str(value.build / '_deps/lua-src/lapi.c'), 'compileGroupIndex': 0}],
                'compileGroups': [{}]})
            runtime['dependencies'].append({'id': 'lua'})
        value.write_model(); return value

    def test_actual_json_replies_accept_on_and_off_without_native(self):
        for enabled in (True, False):
            value = self.fixture(enabled); name = 'on' if enabled else 'off'
            for inspect in (lambda: fixtures.boundary.inspect(value.source, value.build, 'Release', False, name),
                            lambda: closure.inspect(value.source, value.build, 'Release', name)):
                report = inspect(); self.assertEqual(report['status'], 'passed', report['violations'])

    def test_actual_reply_rejects_off_hidden_external_c_source(self):
        value = self.fixture(False)
        value.targets.append({'id': 'hidden', 'name': 'renamed_dependency', 'type': 'STATIC_LIBRARY',
            'sources': [{'path': str(value.build / '_deps/lua-src/lapi.c'), 'compileGroupIndex': 0}], 'compileGroups': [{}]})
        value.write_model()
        for report in (fixtures.boundary.inspect(value.source, value.build, 'Release', False, 'off'),
                       closure.inspect(value.source, value.build, 'Release', 'off')):
            self.assertEqual(report['status'], 'failed')
            self.assertTrue(any('native Lua dependency material' in text for text in report['violations']))

    def test_actual_off_scan_rejects_transitive_native_lua_include(self):
        value = self.fixture(False)
        value.source_file('src/sdk/lua_unavailable.cpp', '#include "neutral/bridge.hpp"\n')
        value.source_file('src/neutral/bridge.hpp', '#include <lua.h>\n')
        report = fixtures.boundary.inspect(value.source, value.build, 'Release', False, 'off')
        self.assertEqual(report['status'], 'failed')
        self.assertTrue(any('Lua OFF includes native Lua' in text for text in report['violations']), report['violations'])

    def test_actual_missing_or_mismatched_profile_is_rejected(self):
        value = self.fixture(False)
        with self.assertRaises(ValueError): closure.inspect(value.source, value.build, 'Release', 'on')
        del value.flags['LUBANCORE_WITH_LUA']; value.write_model()
        with self.assertRaises(ValueError): closure.inspect(value.source, value.build, 'Release', 'off')
        with self.assertRaises(ValueError): fixtures.boundary.inspect(value.source, value.build, 'Release', False, 'off')


class NativeEvidenceTests(unittest.TestCase):
    command = ['/actual build/lubancore_sdk_tests', '--source-file=*test_lubancore_lua_build_profile.cpp']

    def body(self, command=None, enabled=True):
        return '\n'.join((command_line(self.command if command is None else command),
            '[doctest] test cases: 6 | 6 passed | 0 failed', '[doctest] assertions: 50 | 50 passed | 0 failed',
            *('[sdk-lua-build-path] ' + path for path in focused.LUA_BUILD_PATHS),
            *('[sdk-lua-build-case-profile] ' + path + (' on' if enabled else ' off') for path in focused.LUA_BUILD_PATHS),
            'Test Passed.'))

    def test_posix_windows_both_native_owners_and_profiles(self):
        for executable in ('/actual build/lubancore_sdk_tests', 'C:/actual build/lubancore_sdk_tests.exe',
                           '/actual build/lubancode_tests', 'C:/actual build/lubancode_tests.exe'):
            command = [executable, self.command[1]]
            for enabled in (True, False):
                for ending in ('\n', '\r\n'):
                    focused.check_lua_build_native(self.body(command, enabled).replace('\n', ending), command, enabled)

    def test_registration_rejects_relative_filtered_and_foreign_commands(self):
        for command in (None, {}, [], tuple(self.command), self.command[:1], self.command + ['--test-case=one'],
            ['lubancore_sdk_tests', self.command[1]], [r'\actual\lubancore_sdk_tests.exe', self.command[1]],
            ['C:actual/lubancore_sdk_tests.exe', self.command[1]], ['/actual/foreign', self.command[1]],
            [self.command[0], '--source-file=*test_lubancore_lua.cpp'], [7, self.command[1]]):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_lua_build_native(self.body(), command)

    def test_actual_log_must_match_all_registered_arguments(self):
        for command in (['/other/lubancore_sdk_tests', self.command[1]], self.command + ['--test-case=one'],
                        [self.command[0], '--source-file=*test_lubancore_lua.cpp']):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_lua_build_native(self.body(command), self.command)

    def test_six_paths_and_actual_profile_finish_once(self):
        body = self.body()
        for path in focused.LUA_BUILD_PATHS:
            for marker in ('[sdk-lua-build-path] ' + path, '[sdk-lua-build-case-profile] ' + path + ' on'):
                for altered in (body.replace(marker, ''), body + '\n' + marker, body.replace(marker, 'foreign: ' + marker)):
                    with self.subTest(marker=marker), self.assertRaises(RuntimeError):
                        focused.check_lua_build_native(altered, self.command)
        for altered in (self.body(enabled=False), body + '\n[sdk-lua-build-case-profile] selection off'):
            with self.assertRaises(RuntimeError): focused.check_lua_build_native(altered, self.command)

    def test_exact_nonzero_case_assertion_rosters_and_completion(self):
        body = self.body()
        for old, replacements in (
            ('[doctest] test cases: 6 | 6 passed | 0 failed', ('', '[doctest] test cases: 0 | 0 passed | 0 failed',
              '[doctest] test cases: 5 | 5 passed | 0 failed', '[doctest] test cases: 7 | 7 passed | 0 failed',
              '[doctest] test cases: 6 | 5 passed | 1 failed')),
            ('[doctest] assertions: 50 | 50 passed | 0 failed', ('', '[doctest] assertions: 0 | 0 passed | 0 failed',
              '[doctest] assertions: 50 | 49 passed | 1 failed')),
            ('Test Passed.', ('', 'Test Failed.'))):
            for replacement in (*replacements, old + '\n' + old):
                with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                    focused.check_lua_build_native(body.replace(old, replacement), self.command)


class ConsumerEvidenceTests(unittest.TestCase):
    command = ['/relocated build/lubancore_consumer', 'lua-build-profile', '/relocated state/lua']

    def body(self, command=None, enabled=True):
        return '\n'.join((command_line(self.command if command is None else command),
            '[sdk-lua-build-profile] ' + ('on' if enabled else 'off'), '[sdk-lua-build-consumer] complete', 'Test Passed.'))

    def test_posix_and_windows_absolute_actual_three_argv(self):
        for command in (self.command, ['C:/moved/lubancore_consumer.exe', self.command[1], 'C:/moved state/lua']):
            for enabled in (True, False):
                for ending in ('\n', '\r\n'):
                    focused.check_lua_build_consumer(self.body(command, enabled).replace('\n', ending), command, enabled)

    def test_malformed_relative_foreign_or_filtered_consumer_is_rejected(self):
        for command in (None, {}, [], tuple(self.command), self.command[:2], self.command + ['extra'],
            ['lubancore_consumer', *self.command[1:]], [self.command[0], 'lua', self.command[2]],
            [self.command[0], self.command[1], 'state'], [self.command[0], self.command[1], 'C:state'],
            [self.command[0], self.command[1], '/state\0tail'], [None, *self.command[1:]]):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_lua_build_consumer(self.body(), command)

    def test_actual_full_command_and_profile_must_match(self):
        for command in (['/other/lubancore_consumer', *self.command[1:]],
                        [*self.command[:2], '/other state'], self.command + ['extra']):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_lua_build_consumer(self.body(command), self.command)
        for altered in (self.body(enabled=False), self.body() + '\n[sdk-lua-build-profile] off'):
            with self.assertRaises(RuntimeError): focused.check_lua_build_consumer(altered, self.command)

    def test_owned_completion_markers_must_finish_once(self):
        for marker in ('[sdk-lua-build-profile] on', '[sdk-lua-build-consumer] complete', 'Test Passed.'):
            for altered in (self.body().replace(marker, ''), self.body() + '\n' + marker,
                            self.body().replace(marker, 'substituted: ' + marker)):
                with self.subTest(marker=marker), self.assertRaises(RuntimeError):
                    focused.check_lua_build_consumer(altered, self.command)


class CrossImageEvidenceTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix='lua-cross-data-'); self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name).resolve()
        self.prefix, self.build = self.root / 'installed', self.root / 'consumer build'
        self.prefix.mkdir(); self.build.mkdir()
        self.executable = self.build / 'lubancore_consumer.exe'; self.executable.write_bytes(b'data only; never run')
        self.sha = 'a' * 40
        self.context = {'status': 'passed', 'lua_profile': 'on', 'github_sha': self.sha,
            'installed_prefix': str(self.prefix), 'consumer_build': str(self.build), 'consumer_executable': str(self.executable)}

    def test_same_head_completed_relocated_image(self):
        self.assertEqual(cross.validate_image(self.context, 'on', self.sha, self.root), (self.prefix, self.executable))

    def test_failed_wrong_profile_or_head_context_is_rejected(self):
        for key, value in (('status', 'running'), ('status', 'failed'), ('lua_profile', 'off'), ('github_sha', 'b' * 40)):
            context = {**self.context, key: value}
            with self.subTest(key=key, value=value), self.assertRaises(RuntimeError):
                cross.validate_image(context, 'on', self.sha, self.root)
        for sha in ('', 'g' * 40, 'a' * 39, None):
            with self.subTest(sha=sha), self.assertRaises(RuntimeError):
                cross.validate_image({**self.context, 'github_sha': sha}, 'on', sha, self.root)

    def test_image_paths_must_be_real_absolute_and_owned_by_scratch(self):
        for key in ('installed_prefix', 'consumer_build', 'consumer_executable'):
            for value in ('relative', '', '/outside/missing', str(self.root), str(self.prefix / 'missing'), 'bad\0path'):
                with self.subTest(key=key, value=value), self.assertRaises(RuntimeError):
                    cross.validate_image({**self.context, key: value}, 'on', self.sha, self.root)
        fake = self.prefix / 'lubancore_consumer.exe'; fake.write_bytes(b'not from actual consumer build')
        with self.assertRaises(RuntimeError):
            cross.validate_image({**self.context, 'consumer_executable': str(fake)}, 'on', self.sha, self.root)

    def test_actual_cross_phase_requires_one_exact_profile_completion(self):
        marker = '[sdk-lua-build-cross] lua-build-resume-off off complete'
        cross.check_phase_output(marker + '\r\n', 'lua-build-resume-off', 'off')
        for output in ('', marker + '\n' + marker, marker.replace(' off ', ' on '),
                       'substituted: ' + marker, marker + '\n[sdk-lua-build-cross] foreign on complete'):
            with self.subTest(output=output), self.assertRaises(RuntimeError):
                cross.check_phase_output(output, 'lua-build-resume-off', 'off')


class CrossControllerTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix='lua-cross-controller-'); self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name).resolve(); self.sha = 'a' * 40
        self.source = self.root / 'producer source'; self.source.mkdir()
        self.paths = {}; self.contexts = {}
        for name in ('on', 'off'):
            home = self.root / name; prefix = home / 'installed'; build = home / 'consumer build'
            build.mkdir(parents=True); (prefix / 'include/lubancore').mkdir(parents=True)
            (prefix / 'include/lubancore/core.hpp').write_bytes(b'unchanged public API')
            executable = build / 'lubancore_consumer.exe'; executable.write_bytes(('fake ' + name).encode())
            context = {'status': 'passed', 'lua_profile': name, 'github_sha': self.sha,
                'producer_source': str(self.source), 'producer_build': str(home / 'producer build'),
                'installed_prefix': str(prefix), 'consumer_build': str(build), 'consumer_executable': str(executable)}
            self.contexts[name] = context; self.paths[name] = home / 'context.json'
            self.paths[name].write_text(json.dumps(context), encoding='utf-8')
        self.calls = []
        self.evidence = self.root / 'evidence'
        self.environment = {'RUNNER_TEMP': str(self.root), 'GITHUB_SHA': self.sha,
            'PATH': os.pathsep.join((str(self.source), str(self.root / 'on/installed/bin'), str(self.root / 'off/installed/bin'))),
            'LD_LIBRARY_PATH': '/untrusted', 'DYLD_LIBRARY_PATH': '/untrusted', 'DYLD_FALLBACK_LIBRARY_PATH': '/untrusted'}

    def process_fixture(self, command, **kwargs):
        # This is an in-process data fixture, never a native executable invocation.
        self.calls.append((command, kwargs))
        image = 'on' if command[0] == self.contexts['on']['consumer_executable'] else 'off'
        mode, state = command[1], Path(command[2])
        if mode == 'lua-build-seed-off':
            plan = state / 'disabled/data/session/sdk-lua-plan.json'; plan.parent.mkdir(parents=True)
            plan.write_bytes(b'actual disabled frozen-plan fixture')
        if mode == 'lua-build-seed-on':
            journal = state / 'enabled/data/session/session.jsonl'; journal.parent.mkdir(parents=True)
            journal.write_bytes(b'original enabled source fixture')
        return subprocess.CompletedProcess(command, 0, '[sdk-lua-build-cross] ' + mode + ' ' + image + ' complete\n', '')

    def run_controller(self, process=None):
        with patch.dict(os.environ, self.environment), patch.object(cross.subprocess, 'run', side_effect=process or self.process_fixture):
            return cross.run_cross_images(self.paths['on'], self.paths['off'], self.evidence)

    def test_actual_controller_uses_two_images_five_processes_and_owned_bytes(self):
        report = self.run_controller()
        self.assertEqual(report['status'], 'passed')
        expected = [('lua-build-seed-off', 'on'), ('lua-build-resume-off', 'off'), ('lua-build-resume-off', 'on'),
                    ('lua-build-seed-on', 'on'), ('lua-build-reject-on', 'off')]
        self.assertEqual([(phase['command'][1], phase['profile']) for phase in report['phases']], expected)
        for (command, kwargs), (_, image) in zip(self.calls, expected):
            self.assertEqual(len(command), 3); self.assertTrue(all(Path(command[index]).is_absolute() for index in (0, 2)))
            self.assertEqual(command[0], self.contexts[image]['consumer_executable'])
            self.assertEqual(kwargs['timeout'], 120)
            self.assertNotIn(str(self.source), kwargs['env']['PATH'].split(os.pathsep))
            self.assertNotIn('DYLD_LIBRARY_PATH', kwargs['env'])
            self.assertNotIn('DYLD_FALLBACK_LIBRARY_PATH', kwargs['env'])
            if cross.sys.platform == 'win32':
                self.assertEqual(kwargs['env']['PATH'].split(os.pathsep)[0], str(Path(self.contexts[image]['installed_prefix']) / 'bin'))
            else:
                self.assertNotIn('/untrusted', kwargs['env'].get('LD_LIBRARY_PATH', ''))
        self.assertEqual(report['enabledOriginalFingerprints'], report['enabledRefusedFingerprints'])
        self.assertTrue(report['enabledOriginalFingerprints'])
        self.assertEqual(json.loads((self.evidence / 'cross-profile.json').read_text())['status'], 'passed')

    def test_cross_failure_keeps_failure_report_and_original_returncode(self):
        def fail(command, **kwargs):
            result = self.process_fixture(command, **kwargs)
            if command[1] == 'lua-build-resume-off' and 'off' in Path(command[0]).parts:
                return subprocess.CompletedProcess(command, 23, result.stdout, 'real failed phase')
            return result
        with self.assertRaises(RuntimeError): self.run_controller(fail)
        report = json.loads((self.evidence / 'cross-profile.json').read_text())
        self.assertEqual(report['status'], 'failed')
        self.assertEqual(report['phases'][-1]['returncode'], 23)
        self.assertEqual((self.evidence / 'lua-build-resume-off-off.stderr').read_text(), 'real failed phase')

    def test_cross_acceptance_rejects_enabled_source_mutation(self):
        def mutate(command, **kwargs):
            result = self.process_fixture(command, **kwargs)
            if command[1] == 'lua-build-reject-on':
                (Path(command[2]) / 'enabled/data/session/session.jsonl').write_bytes(b'changed original source')
            return result
        with self.assertRaisesRegex(RuntimeError, 'modified enabled original state'): self.run_controller(mutate)
        self.assertEqual(json.loads((self.evidence / 'cross-profile.json').read_text())['status'], 'failed')

    def test_cross_images_keep_public_header_bytes(self):
        (Path(self.contexts['off']['installed_prefix']) / 'include/lubancore/core.hpp').write_bytes(b'changed ABI')
        with self.assertRaisesRegex(RuntimeError, 'public header bytes'): self.run_controller()
        self.assertEqual(self.calls, [])


if __name__ == '__main__':
    unittest.main()
