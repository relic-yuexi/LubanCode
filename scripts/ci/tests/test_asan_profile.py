"""Pure ASan profile data tests; never configure, compile or run native code."""
from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from scripts.ci import check_asan_profile as gate


WORKFLOW = '''jobs:
  linux-asan:
    steps:
      - run: |
          ctest --test-dir build -C Release -R '^unit\\.runtime\\.thread$' --show-only=json-v1 > required.json
          ctest --test-dir build -C Release -R '^unit\\.runtime\\.' -E 'excluded' --parallel 4 --output-junit results.xml
'''
CMAKE = '''add_executable(lubancode_tests
  support/main.cpp
  support/fake_http_server.cpp
  ${LUBANCODE_TEST_COMPILE_SOURCES}
)
target_sources(lubancode_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/sdk/results.cpp")
target_sources(lubancode_tests PRIVATE "${CMAKE_SOURCE_DIR}/examples/sdk-consumer/dynamic.cpp")
'''


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name).resolve() / 'source'
        self.build = self.source.parent / 'build'
        contents = {
            '.github/workflows/ci.yml': WORKFLOW, 'tests/CMakeLists.txt': CMAKE,
            'cmake/LubanCoreTests.cmake': '# sdk', 'cmake/LubanCoreHostTests.cmake': '# host',
            'tests/unit/runtime/test_thread.cpp': 'selected',
            'tests/unit/runtime/test_other.cpp': 'second selector only',
            'tests/unit/runtime/test_excluded.cpp': 'excluded',
            'tests/unit/unselected/test_outer_test_thread.cpp': 'suffix wildcard matches original filter',
            'tests/unit/unselected/test_unrelated.cpp': 'outside compile closure',
            'tests/support/main.cpp': 'support main', 'tests/support/fake_http_server.cpp': 'support server',
            'src/sdk/results.cpp': 'private reference', 'examples/sdk-consumer/dynamic.cpp': 'public helper',
        }
        for name, text in contents.items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
        self.manifest = gate.make_manifest(self.source)
        self.filename = self.build / 'asan-profile/manifest.json'
        gate.write_json(self.filename, self.manifest)
        self.executable = (self.build / 'tests/lubancode_tests').as_posix()

    def registrations(self):
        return [{'tests': [{'name': name,
                            'command': [self.executable, '--source-file=*' + Path(self.manifest['roster'][name]).name],
                            'properties': [{'name': 'TIMEOUT', 'value': 180}]} for name in names]}
                for names in self.manifest['selected']]

    def evidence(self):
        junit = ET.Element('testsuite')
        sections = []
        for index, test in enumerate(self.registrations()[1]['tests'], 1):
            ET.SubElement(junit, 'testcase', name=test['name'], status='run')
            sections.append(f'{index}/2 Testing: {test["name"]}\nCommand: '
                            + ' '.join('"' + part + '"' for part in test['command'])
                            + '\n[doctest] test cases: 2 | 2 passed | 0 failed | 8 skipped\n'
                              '[doctest] assertions: 9 | 9 passed | 0 failed |\nTest Passed.\n')
        return junit, ''.join(sections)

    def graph(self):
        targets = {}
        names = ('lubancode_tests', 'lubancore_sdk_tests', 'lubancore_host_tests', 'lubancode_lua',
                 'lubancode_app', 'lubancore_sdk', 'lubancore_sdk_search_probe', 'lubancore_command_limits_probe',
                 'hello_plugin_fixture', 'bad_version_plugin', 'memory_worker_fixture', 'fake_ripgrep',
                 'gateway_lock_racer', 'memory_lock_racer', 'workspace_manifest_racer', 'updater_lock_racer',
                 'updater_probe_stub', 'model_probe_e2e')
        for name in names:
            targets[name] = {'id': name, 'name': name, 'type': 'EXECUTABLE', 'dependencies': [],
                             'artifacts': [{'path': 'tests/' + name}]}
            path = self.build / 'tests' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fake fixture data, not an executable')
        targets['lubancode_lua']['type'] = 'STATIC_LIBRARY'
        target = targets['lubancode_tests']
        target['paths'] = {'build': 'tests', 'source': 'tests'}
        target['dependencies'] = [{'id': name} for name in names[3:]]
        target['sources'] = [{'path': name, 'compileGroupIndex': 0}
                             for name in self.manifest['compile_sources'] + self.manifest['attachments']]
        target['sources'].append({'path': (self.build / 'tests/CMakeFiles/lubancode_tests.dir/cmake_pch.hxx.cxx').as_posix(),
                                  'compileGroupIndex': 0, 'isGenerated': True})
        definitions = ['LUBANCORE_TEST_JOB_POST_SDK=1']
        for macro, name in (('LUBANCORE_TEST_SEARCH_PROBE', 'lubancore_sdk_search_probe'),
                            ('LUBANCORE_TEST_COMMAND_LIMITS_PROBE', 'lubancore_command_limits_probe')):
            definitions.append(macro + '="' + (self.build / 'tests' / name).as_posix() + '"')
        target['compileGroups'] = [{'language': 'CXX', 'compileCommandFragments': [{'fragment': '-O3 -fsanitize=address'}],
                                    'defines': [{'define': value} for value in definitions],
                                    'sourceIndexes': list(range(len(target['sources']))),
                                    'precompileHeaders': [{'header': (self.source / 'tests/support/pch.hpp').as_posix()}]}]
        cache = {'LUBANCODE_ASAN_TEST_PROFILE': 'ON', 'LUBANCODE_BUILD_CLI': 'ON', 'LUBANCODE_BUILD_SDK': 'ON',
                 'LUBANCORE_WITH_LUA': 'ON', 'LUBANCODE_ASAN_PROFILE_FILE': str(self.filename),
                 'CMAKE_BUILD_TYPE': 'Release', 'CMAKE_CXX_FLAGS': '-fsanitize=address -fno-omit-frame-pointer',
                 'CMAKE_C_FLAGS': '-fsanitize=address -fno-omit-frame-pointer',
                 'CMAKE_EXE_LINKER_FLAGS': '-fsanitize=address'}
        return targets, cache

    def check_graph(self, targets, cache):
        return gate.check_graph(self.manifest, targets, cache, self.source, self.build, self.filename)

    def test_selected_union_retains_second_selector_and_suffix_matches(self):
        self.assertEqual(self.manifest['selected'][0], ['unit.runtime.thread'])
        self.assertEqual(self.manifest['selected'][1], ['unit.runtime.other', 'unit.runtime.thread'])
        self.assertEqual(self.manifest['compile_sources'], ['tests/unit/runtime/test_other.cpp',
            'tests/unit/runtime/test_thread.cpp', 'tests/unit/unselected/test_outer_test_thread.cpp'])
        self.assertIn('examples/sdk-consumer/dynamic.cpp', self.manifest['attachments'])
        self.assertEqual(gate.load_manifest(self.source, self.filename), self.manifest)

    def test_required_only_or_stale_or_foreign_manifest_rejected(self):
        for mutation in ('required-only', 'foreign', 'duplicate', 'stale'):
            changed = deepcopy(self.manifest)
            if mutation == 'required-only': changed['compile_sources'] = ['tests/unit/runtime/test_thread.cpp']
            if mutation == 'foreign': changed['compile_sources'].append('../outside.cpp')
            if mutation == 'duplicate': changed['compile_sources'].append(changed['compile_sources'][0])
            if mutation == 'stale': changed['source_sha256']['src/sdk/results.cpp'] = '0' * 64
            gate.write_json(self.filename, changed)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                gate.load_manifest(self.source, self.filename)

    def test_actual_source_and_workflow_drift_rejected(self):
        for name in ('tests/unit/unselected/test_unrelated.cpp', '.github/workflows/ci.yml'):
            path = self.source / name
            original = path.read_text(encoding='utf-8')
            path.write_text(original + '\n# drift', encoding='utf-8')
            with self.subTest(name=name), self.assertRaises(ValueError): gate.load_manifest(self.source, self.filename)
            path.write_text(original, encoding='utf-8')

    def test_missing_and_duplicate_actual_selectors_rejected(self):
        with self.assertRaises(ValueError): gate.selectors_from_workflow('jobs:\n')
        with self.assertRaises(ValueError): gate.selectors_from_workflow(WORKFLOW + WORKFLOW)
        bad = WORKFLOW.replace("-E 'excluded'", "-E 'thread'")
        (self.source / '.github/workflows/ci.yml').write_text(bad, encoding='utf-8')
        with self.assertRaises(ValueError): gate.make_manifest(self.source)

    def test_duplicate_basename_rejected(self):
        path = self.source / 'tests/unit/unselected/test_thread.cpp'
        path.write_text('duplicate', encoding='utf-8')
        with self.assertRaises(ValueError): gate.make_manifest(self.source)

    def test_actual_graph_preserves_dynamic_attachments_and_probe_paths(self):
        targets, cache = self.graph()
        self.assertEqual(self.check_graph(targets, cache)['status'], 'passed')
        del cache['LUBANCORE_WITH_LUA']  # Older checkout has mandatory Lua, no optional profile yet.
        self.assertEqual(self.check_graph(targets, cache)['status'], 'passed')

    def test_actual_pch_without_generated_flag_is_bound_to_its_target_and_group(self):
        # df72's first remote File API reports this relative path and no
        # isGenerated field. Preserve that shape with the build below source.
        self.build = self.source / 'build'
        self.filename = self.build / 'asan-profile/manifest.json'
        for flag in ('missing', True):
            targets, cache = self.graph()
            target = targets['lubancode_tests']
            # Match the actual leading PCH source and its distinct group, not
            # just the absence of a flag on the old synthetic last source.
            source = {'backtrace': 0, 'compileGroupIndex': 0,
                      'path': 'build/tests/CMakeFiles/lubancode_tests.dir/cmake_pch.hxx.cxx', 'sourceGroupIndex': 0}
            if flag is True: source['isGenerated'] = True
            target['sources'] = [source] + target['sources'][:-1]
            target['compileGroups'] = [deepcopy(target['compileGroups'][0]) for _ in range(3)]
            for group in target['compileGroups']: group['sourceIndexes'] = []
            for index, item in enumerate(target['sources']):
                item['compileGroupIndex'] = min(index, 2)
                target['compileGroups'][min(index, 2)]['sourceIndexes'].append(index)
            self.assertEqual(self.check_graph(targets, cache)['status'], 'passed')

    def test_pch_same_name_wrong_owner_missing_binding_and_duplicates_are_rejected(self):
        for mutation in ('other-owner', 'outside-build', 'owner-escape', 'wrong-name',
                         'missing-header', 'foreign-header', 'missing-group-binding', 'duplicate', 'other-generated'):
            targets, cache = self.graph()
            target = targets['lubancode_tests']
            source = target['sources'][-1]
            del source['isGenerated']
            if mutation == 'other-owner': source['path'] = (self.build / 'other/CMakeFiles/lubancode_tests.dir/cmake_pch.hxx.cxx').as_posix()
            if mutation == 'outside-build': source['path'] = (self.source / 'tests/CMakeFiles/lubancode_tests.dir/cmake_pch.hxx.cxx').as_posix()
            if mutation == 'owner-escape': target['paths']['build'] = '../foreign'
            if mutation == 'wrong-name': source['path'] += '.other'
            if mutation == 'missing-header': target['compileGroups'][0]['precompileHeaders'] = []
            if mutation == 'foreign-header': target['compileGroups'][0]['precompileHeaders'][0]['header'] = str(self.source / 'foreign.hpp')
            if mutation == 'missing-group-binding': target['compileGroups'][0]['sourceIndexes'].remove(len(target['sources']) - 1)
            if mutation == 'duplicate': target['sources'].append(deepcopy(source))
            if mutation == 'other-generated': target['sources'][0]['isGenerated'] = True
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): self.check_graph(targets, cache)

    def test_graph_missing_extra_duplicate_and_foreign_source_rejected(self):
        for mutation in ('missing', 'extra', 'duplicate', 'foreign', 'missing-helper', 'required-only', 'invalid-group'):
            targets, cache = self.graph()
            sources = targets['lubancode_tests']['sources']
            if mutation == 'missing': sources.pop(0)
            if mutation == 'extra': sources.append({'path': 'tests/unit/unselected/test_unrelated.cpp', 'compileGroupIndex': 0})
            if mutation == 'duplicate': sources.append(deepcopy(sources[0]))
            if mutation == 'foreign': sources[0]['path'] = (self.source.parent / 'foreign.cpp').as_posix()
            if mutation == 'missing-helper': sources[:] = [item for item in sources if item['path'] != 'examples/sdk-consumer/dynamic.cpp']
            if mutation == 'required-only': sources[:] = [item for item in sources if item['path'] != 'tests/unit/runtime/test_other.cpp']
            if mutation == 'invalid-group': sources[0]['compileGroupIndex'] = 99
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): self.check_graph(targets, cache)

    def test_file_api_requires_actual_root_configuration_and_unique_target_refs(self):
        targets, cache = self.graph()
        reply = self.build / '.cmake/api/v1/reply'
        model = {'kind': 'codemodel', 'version': {'major': 2},
                 'paths': {'source': str(self.source), 'build': str(self.build)},
                 'configurations': [{'name': 'Release', 'targets': []}]}
        for key, target in targets.items():
            filename = key + '.json'
            model['configurations'][0]['targets'].append({'id': key, 'name': target['name'], 'jsonFile': filename})
            gate.write_json(reply / filename, target)
        gate.write_json(reply / 'cache.json', {'kind': 'cache', 'version': {'major': 2},
                        'entries': [{'name': key, 'value': value} for key, value in cache.items()]})
        gate.write_json(reply / 'index-1.json', {'reply': {gate.CLIENT: {
                        'codemodel-v2': {'jsonFile': 'codemodel.json'}, 'cache-v2': {'jsonFile': 'cache.json'}}}})
        gate.write_json(reply / 'codemodel.json', model)
        actual_targets, actual_cache = gate.read_graph(self.source, self.build)
        self.assertEqual(actual_targets, targets)
        self.assertEqual(actual_cache, cache)
        for mutation in ('root', 'configuration', 'duplicate-target', 'foreign-reference'):
            changed = deepcopy(model)
            if mutation == 'root': changed['paths']['source'] += '-other'
            if mutation == 'configuration': changed['configurations'][0]['name'] = 'Debug'
            if mutation == 'duplicate-target': changed['configurations'][0]['targets'].append(deepcopy(changed['configurations'][0]['targets'][0]))
            if mutation == 'foreign-reference': changed['configurations'][0]['targets'][0]['jsonFile'] = '../foreign.json'
            gate.write_json(reply / 'codemodel.json', changed)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): gate.read_graph(self.source, self.build)

    def test_graph_companion_dependency_definition_pch_and_instrumentation_rejected(self):
        for mutation in ('companion-edge', 'missing-target', 'missing-probe', 'wrong-probe', 'job-define', 'pch', 'asan', 'lua'):
            targets, cache = self.graph()
            target = targets['lubancode_tests']
            group = target['compileGroups'][0]
            if mutation == 'companion-edge': target['dependencies'].append({'id': 'lubancore_sdk_tests'})
            if mutation == 'missing-target': del targets['lubancore_host_tests']
            if mutation == 'missing-probe': (self.build / 'tests/lubancore_sdk_search_probe').unlink()
            if mutation == 'wrong-probe': group['defines'][1]['define'] += 'foreign'
            if mutation == 'job-define': group['defines'].pop(0)
            if mutation == 'pch': group['precompileHeaders'] = []
            if mutation == 'asan': group['compileCommandFragments'] = [{'fragment': '-O3'}]
            if mutation == 'lua': cache['LUBANCORE_WITH_LUA'] = 'OFF'
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): self.check_graph(targets, cache)

    def test_registration_requires_complete_exact_argv_and_roster(self):
        for mutation in ('missing', 'duplicate', 'foreign-exe', 'relative-exe', 'extra-arg', 'source-suffix', 'disabled', 'timeout'):
            registration = self.registrations()[1]
            test = registration['tests'][0]
            if mutation == 'missing': registration['tests'].pop()
            if mutation == 'duplicate': registration['tests'].append(deepcopy(test))
            if mutation == 'foreign-exe': test['command'][0] += '-other'
            if mutation == 'relative-exe': test['command'][0] = 'lubancode_tests'
            if mutation == 'extra-arg': test['command'].append('--test-case=fewer')
            if mutation == 'source-suffix': test['command'][1] = '--source-file=*other.cpp'
            if mutation == 'disabled': test['properties'].append({'name': 'DISABLED', 'value': True})
            if mutation == 'timeout': test['properties'][0]['value'] = 600
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                gate.check_registration(self.manifest, registration, 1, self.executable)

    def test_every_selected_source_must_execute_nonempty_native_cases_and_assertions(self):
        junit, native = self.evidence()
        self.assertEqual(gate.check_execution(self.manifest, self.registrations(), self.executable, junit, native),
                         {'status': 'passed', 'sources': 2, 'cases': 4, 'assertions': 18})
        for mutation in ('missing', 'duplicate', 'failed', 'skipped', 'zero-cases', 'zero-assertions', 'foreign-argv', 'missing-last', 'duplicated-last'):
            junit, native = self.evidence()
            if mutation == 'missing': junit.remove(junit[0])
            if mutation == 'duplicate': junit.append(deepcopy(junit[0]))
            if mutation == 'failed': ET.SubElement(junit[0], 'failure')
            if mutation == 'skipped': ET.SubElement(junit[0], 'skipped')
            if mutation == 'zero-cases': native = native.replace('2 | 2 passed', '0 | 0 passed', 1)
            if mutation == 'zero-assertions': native = native.replace('9 | 9 passed', '0 | 0 passed', 1)
            if mutation == 'foreign-argv': native = native.replace(self.executable, self.executable + '-other', 1)
            if mutation == 'missing-last': native = native[native.index('2/2 Testing:'):]
            if mutation == 'duplicated-last': native += native
            with self.subTest(mutation=mutation), self.assertRaises((ValueError, RuntimeError)):
                gate.check_execution(self.manifest, self.registrations(), self.executable, junit, native)


if __name__ == '__main__':
    unittest.main()
