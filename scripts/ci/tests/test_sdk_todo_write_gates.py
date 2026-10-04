"""Reject incomplete or substituted native TodoWrite evidence; no native runs."""
import importlib.util
import hashlib
import tempfile
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'check_sdk_focused.py'
SPEC = importlib.util.spec_from_file_location('sdk_todo_write_gate', SCRIPT)
focused = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(focused)


class TodoWriteEvidenceTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_lubancore_todo_write.cpp']

    def body(self, command=None):
        command = self.command if command is None else command
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[sdk-todo-write-path] ' + path for path in focused.TODO_WRITE_PATHS)))

    def test_actual_absolute_commands_and_line_endings(self):
        for exe in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancore_sdk_tests.exe',
                    '/real build/lubancode_tests', 'C:/real build/lubancode_tests.exe'):
            command = [exe, self.command[1]]
            executable = exe.split('/')[-1].removesuffix('.exe')
            focused.check_todo_write_registration(command, executable)
            for ending in ('\n', '\r\n'):
                focused.check_todo_write_native(self.body(command).replace('\n', ending), command)

    def test_relative_foreign_or_filtered_registration_is_rejected(self):
        bad = (None, {}, [], tuple(self.command), self.command[:1],
            [7, self.command[1]], [self.command[0], None],
            ['lubancore_sdk_tests', self.command[1]], ['../lubancore_sdk_tests', self.command[1]],
            ['\\real build\\lubancore_sdk_tests', self.command[1]],
            ['C:real build\\lubancore_sdk_tests', self.command[1]],
            ['/real build/another_tests', self.command[1]],
            [self.command[0], '--source-file=*test_lubancore_todo_write_spi_extra.cpp'],
            [self.command[0], '--source-file=*test_lubancore_session.cpp'],
            self.command + ['--test-case=one'])
        for command in bad:
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_todo_write_registration(command)
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_todo_write_native(self.body(), command)

    def test_actual_command_matches_the_registered_path_and_source(self):
        for command in (['/other build/lubancore_sdk_tests', self.command[1]],
                        [self.command[0], '--source-file=*test_lubancore_session.cpp'],
                        self.command + ['--test-case=one']):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_todo_write_native(self.body(command), self.command)

    def test_each_source_path_finishes_once(self):
        for path in focused.TODO_WRITE_PATHS:
            marker = '[sdk-todo-write-path] ' + path
            for body in (self.body().replace(marker, ''), self.body() + '\n' + marker,
                         self.body().replace(marker, 'different-provider: ' + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_todo_write_native(body, self.command)

    def test_exact_successful_case_roster(self):
        body = self.body()
        summary = '[doctest] test cases: 6 | 6 passed | 0 failed'
        for replacement in ('', '[doctest] test cases: 0 | 0 passed | 0 failed',
            '[doctest] test cases: 5 | 5 passed | 0 failed',
            '[doctest] test cases: 7 | 7 passed | 0 failed',
            '[doctest] test cases: 6 | 5 passed | 1 failed', summary + '\n' + summary):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_todo_write_native(body.replace(summary, replacement), self.command)

    def test_nonzero_complete_assertions_and_completion(self):
        body = self.body()
        summary = '[doctest] assertions: 100 | 100 passed | 0 failed'
        for replacement in ('', '[doctest] assertions: 0 | 0 passed | 0 failed',
            '[doctest] assertions: 100 | 99 passed | 1 failed', summary + '\n' + summary):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_todo_write_native(body.replace(summary, replacement), self.command)
        for replacement in ('', 'Test Failed.', 'Test Passed.\nTest Passed.'):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_todo_write_native(body.replace('Test Passed.', replacement), self.command)


class RelocatedTodoWriteEvidenceTests(unittest.TestCase):
    command = ['/relocated consumer/build/lubancore_consumer', 'todo-write', '/relocated consumer/build/state-todo-write']

    def body(self, command=None):
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in (command or self.command)),
                          '[sdk-todo-write-consumer] complete', 'Test Passed.',
                          *('[sdk-todo-write-path] ' + path for path in focused.TODO_WRITE_PATHS)))

    def test_actual_relocated_command_and_line_endings(self):
        for executable in ('/relocated consumer/build/lubancore_consumer', 'C:/relocated consumer/build/lubancore_consumer.exe'):
            command = [executable, *self.command[1:]]
            for ending in ('\n', '\r\n'):
                focused.check_todo_write_consumer(self.body(command).replace('\n', ending), command)

    def test_foreign_binary_arguments_and_registration_are_rejected(self):
        for changed in (['/foreign/build/lubancore_consumer', *self.command[1:]],
                        [self.command[0], 'smoke', self.command[2]], self.command + ['extra'],
                        self.command[:2]):
            with self.subTest(command=changed), self.assertRaises(RuntimeError):
                focused.check_todo_write_consumer(self.body(changed), self.command)
        for invalid in (None, [], ['lubancore_consumer', *self.command[1:]],
                        ['C:build/lubancore_consumer.exe', *self.command[1:]],
                        ['/real/other_consumer', *self.command[1:]],
                        [self.command[0], self.command[1], 'relative-state'],
                        [self.command[0], self.command[1], 'C:relative-state'],
                        [self.command[0], self.command[1], None]):
            with self.subTest(command=invalid), self.assertRaises(RuntimeError):
                focused.check_todo_write_consumer(self.body(), invalid)

    def test_missing_duplicate_and_decorated_success_are_rejected(self):
        for marker in ('[sdk-todo-write-consumer] complete', 'Test Passed.',
                       *('[sdk-todo-write-path] ' + path for path in focused.TODO_WRITE_PATHS)):
            for changed in ('', marker + '\n' + marker, 'borrowed: ' + marker):
                with self.subTest(marker=marker, changed=changed), self.assertRaises(RuntimeError):
                    focused.check_todo_write_consumer(self.body().replace(marker, changed), self.command)




def load_gate(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TodoSourceSealTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="todo-public-source-")
        self.addCleanup(scratch.cleanup)
        self.repo = Path(scratch.name)
        self.path = self.repo / "examples/sdk-consumer/todo_write.cpp"
        self.path.parent.mkdir(parents=True)
        self.installed = load_gate("todo_installed_gate", SCRIPT.parent / "check_installed_sdk.py")

    def write(self, text):
        self.path.write_text(text, encoding="utf-8")

    def test_actual_public_helper_is_owned_and_sealed(self):
        repo = SCRIPT.parents[2]
        seal = self.installed.check_todo_consumer_source(repo)
        actual = repo / seal["path"]
        self.assertEqual(seal["sha256"], hashlib.sha256(actual.read_bytes()).hexdigest())
        self.assertEqual(seal["includes"].count("lubancore/core.hpp"), 1)

    def test_only_public_and_standard_includes_are_accepted(self):
        self.write('#include <lubancore/core.hpp>\n#include <filesystem>\n#include <string>\n'
                   '// #include "src/sdk/core.hpp"\nconst char* x=R"(\n#include "private.hpp"\n)";\n')
        self.installed.check_todo_consumer_source(self.repo)
        for header in ('"lubancore/core.hpp"', '<sdk/core.hpp>', '<nlohmann/json.hpp>',
                       '<src/tools/todo_tool.hpp>', 'PRIVATE_HEADER'):
            with self.subTest(header=header), self.assertRaises(RuntimeError):
                self.write('#include <lubancore/core.hpp>\n#include ' + header + '\n')
                self.installed.check_todo_consumer_source(self.repo)

    def test_missing_and_duplicate_sdk_header_or_source_are_rejected(self):
        for text in ('#include <string>\n', '#include <lubancore/core.hpp>\n' * 2):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                self.write(text)
                self.installed.check_todo_consumer_source(self.repo)
        self.path.unlink()
        with self.assertRaises(RuntimeError):
            self.installed.check_todo_consumer_source(self.repo)

    def test_relocated_copy_retains_the_sealed_public_source_bytes(self):
        self.write('#include <lubancore/core.hpp>\n#include <filesystem>\n')
        seal = self.installed.check_todo_consumer_source(self.repo)
        self.installed.check_todo_consumer_copy(self.path.parent, seal)
        self.write(self.path.read_text(encoding="utf-8") + '#include "sdk/core.hpp"\n')
        with self.assertRaises(RuntimeError):
            self.installed.check_todo_consumer_copy(self.path.parent, seal)
        self.path.unlink()
        with self.assertRaises(RuntimeError):
            self.installed.check_todo_consumer_copy(self.path.parent, seal)


class TodoActualGraphOwnershipTests(unittest.TestCase):
    def setUp(self):
        fixtures = load_gate("todo_boundary_fixtures", SCRIPT.parent / "tests/test_sdk_only_boundary.py")
        self.fixture = fixtures.BoundaryTests("test_clean_graph_and_recursive_headers_are_recorded")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.flags["BUILD_TESTING"] = "ON"
        self.fixture.source_file("examples/sdk-consumer/todo_write.cpp", '#include <lubancore/core.hpp>\n')
        self.fixture.targets.append({
            "id": "todo", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
            "sources": [{"path": "examples/sdk-consumer/todo_write.cpp", "compileGroupIndex": 0}],
            "compileGroups": [{}]})

    def test_actual_file_api_records_selected_helper_and_recursive_public_header(self):
        report = self.fixture.check(testing=True)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertIn("examples/sdk-consumer/todo_write.cpp", report["scannedProjectFiles"])
        self.assertIn({"from": "examples/sdk-consumer/todo_write.cpp", "to": "include/lubancore/core.hpp"},
                      report["projectIncludeEdges"])

    def test_missing_and_duplicate_actual_owners_are_rejected(self):
        target = self.fixture.targets[-1]
        original = target["sources"][0]
        for sources in ([], [original, original]):
            with self.subTest(sources=sources):
                target["sources"] = sources
                self.fixture.assert_rejected(self.fixture.check(testing=True), "exactly the selected")

    def test_foreign_owner_and_testing_off_cannot_compile_helper(self):
        self.fixture.targets[-1]["name"] = "foreign_reference"
        self.fixture.assert_rejected(self.fixture.check(testing=True), "not a selected testing-only source")
        self.fixture.targets[-1]["name"] = "lubancore_sdk_tests"
        self.fixture.targets[-1]["type"] = "STATIC_LIBRARY"
        self.fixture.assert_rejected(self.fixture.check(testing=True), "exactly the selected")
        self.fixture.targets[-1]["type"] = "EXECUTABLE"
        self.fixture.flags["BUILD_TESTING"] = "OFF"
        self.fixture.assert_rejected(self.fixture.check(), "not a selected testing-only source")

    def test_nearby_source_cannot_supply_required_helper_or_smuggle_host_header(self):
        self.fixture.source_file("examples/sdk-consumer/todo_write_extra.cpp", "int example;\n")
        self.fixture.targets[-1]["sources"][0]["path"] = "examples/sdk-consumer/todo_write_extra.cpp"
        self.fixture.assert_rejected(self.fixture.check(testing=True), "exactly the selected")
        self.fixture.targets[-1]["sources"][0]["path"] = "examples/sdk-consumer/todo_write.cpp"
        self.fixture.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.fixture.source_file("examples/sdk-consumer/todo_write.cpp", '#include "app/turn_runner.hpp"\n')
        self.fixture.assert_rejected(self.fixture.check(testing=True), "reverse host include")


class TodoProfilesAndClassificationTests(unittest.TestCase):
    def test_on_off_rosters_both_require_todo_actual_routes(self):
        installed = load_gate("todo_profile_installed", SCRIPT.parent / "check_installed_sdk.py")
        profile = load_gate("todo_lua_profile", SCRIPT.parent / "sdk_lua_profile.py")
        for enabled, native_count, consumer_count in ((True, 53, 31), (False, 51, 28)):
            self.assertEqual(len(profile.focused_roster(focused.REQUIRED, enabled)), native_count)
            self.assertEqual(len(profile.consumer_roster(installed.REQUIRED_TESTS, enabled)), consumer_count)
            self.assertIn("sdk.focused.lubancore_todo_write", profile.focused_roster(focused.REQUIRED, enabled))
            self.assertIn("sdk.consumer.todo_write", profile.consumer_roster(installed.REQUIRED_TESTS, enabled))

    def test_both_actual_ci_branches_classify_helper_native_and_pure_gate(self):
        import fnmatch
        classifier = load_gate("todo_ci_classifier", SCRIPT.parent / "tests/test_job_current_integration_paths.py")
        text = classifier.WORKFLOW.read_text(encoding="utf-8")
        for patterns, body in classifier.sdk_classification_branches(text):
            for path in ("examples/sdk-consumer/todo_write.cpp", "tests/integration/sdk/test_lubancore_todo_write.cpp",
                         "src/runtime/assembly/builtin_tools.cpp", "scripts/ci/tests/test_sdk_todo_write_gates.py"):
                with self.subTest(path=path):
                    self.assertTrue(any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns))
            self.assertIn("sdk_tests=$sdk_present", body)
            self.assertIn("cross_platform=true", body)
            self.assertEqual(patterns.count("scripts/ci/tests/test_sdk_todo_write_gates.py"), 1)
            self.assertFalse(any(fnmatch.fnmatchcase("scripts/ci/tests/test_sdk_todo_write_gates_extra.py", pattern)
                                 for pattern in patterns))
        self.assertEqual(text.count('"$PY" -m unittest discover -s scripts/ci/tests -p test_sdk_todo_write_gates.py'), 1)

if __name__ == '__main__':
    unittest.main()
