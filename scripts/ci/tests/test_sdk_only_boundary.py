"""Negative architecture-gate fixtures; no CMake/compiler/native process runs."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "check_sdk_only_boundary.py"
SPEC = importlib.util.spec_from_file_location("sdk_boundary", SCRIPT)
boundary = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boundary)


class BoundaryTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="sdk-boundary-data-")
        self.addCleanup(self.scratch.cleanup)
        self.source = Path(self.scratch.name) / "source"
        self.build = Path(self.scratch.name) / "build"
        self.reply = self.build / ".cmake/api/v1/reply"
        self.reply.mkdir(parents=True)
        self.source_file("include/lubancore/core.hpp", "#include <string>\n")
        self.source_file("src/sdk/core.cpp", '#include "neutral/bridge.hpp"\n')
        self.source_file("src/neutral/bridge.hpp", "#pragma once\n")
        self.source_file("src/neutral/engine.cpp", '#include "neutral/bridge.hpp"\n')
        (self.build / "liblubancore.fake-artifact").write_bytes(b"fixture-only; never executed")
        self.targets = [
            {"id": "sdk", "name": "lubancore_sdk", "type": "SHARED_LIBRARY",
             "sources": [{"path": "src/sdk/core.cpp", "compileGroupIndex": 0}],
             "compileGroups": [{}],
             "dependencies": [{"id": "engine"}],
             "artifacts": [{"path": "liblubancore.fake-artifact"}]},
            {"id": "engine", "name": "neutral_engine", "type": "STATIC_LIBRARY",
             "sources": [{"path": "src/neutral/engine.cpp", "compileGroupIndex": 0}],
             "compileGroups": [{}]},
        ]
        self.flags = {"LUBANCODE_BUILD_CLI": "OFF", "LUBANCODE_BUILD_SDK": "ON", "BUILD_TESTING": "OFF"}

    def source_file(self, name, contents):
        path = self.source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")

    def json_file(self, name, data):
        (self.reply / name).write_text(json.dumps(data), encoding="utf-8")

    def write_model(self):
        references = []
        for index, target in enumerate(self.targets):
            name = f"target-{index}.json"
            self.json_file(name, target)
            references.append({"id": target["id"], "name": target["name"], "jsonFile": name})
        self.json_file("model.json", {
            "kind": "codemodel", "version": {"major": 2, "minor": 2},
            "paths": {"source": str(self.source), "build": str(self.build)},
            "configurations": [{"name": "Release", "targets": references}],
        })
        self.json_file("cache.json", {"kind": "cache", "version": {"major": 2},
                                      "entries": [{"name": key, "value": value} for key, value in self.flags.items()]})
        self.json_file("index-0001.json", {"reply": {boundary.CLIENT: {
            "codemodel-v2": {"jsonFile": "model.json"}, "cache-v2": {"jsonFile": "cache.json"},
        }}})

    def check(self, testing=False):
        self.write_model()
        return boundary.inspect(self.source, self.build, "Release", testing)

    def assert_rejected(self, report, reason):
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any(reason in violation for violation in report["violations"]), report["violations"])

    def test_clean_graph_and_recursive_headers_are_recorded(self):
        report = self.check()
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertEqual(set(report["sdkBuildClosure"]), {"sdk", "engine"})
        self.assertIn("src/neutral/bridge.hpp", report["scannedProjectFiles"])

    def test_testing_off_rejects_project_test_sources_even_without_test_target_name(self):
        self.source_file("tests/integration/sdk/test_probe.cpp", "int probe;\n")
        self.targets.append({"id": "probe", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": "tests/integration/sdk/test_probe.cpp", "compileGroupIndex": 0}],
                             "compileGroups": [{}]})
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")

    def test_testing_on_does_not_enable_the_cli_test_graph(self):
        self.flags["BUILD_TESTING"] = "ON"
        self.source_file("tests/unit/cli/test_prompt.cpp", "int cli_test;\n")
        self.targets.append({"id": "tests", "name": "lubancode_tests", "type": "EXECUTABLE",
                             "sources": [{"path": "tests/unit/cli/test_prompt.cpp"}]})
        self.assert_rejected(self.check(testing=True), "host/resource target")
        self.targets[-1]["name"] = "lubancore_sdk_tests"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_real_private_memory_cas_reference_is_only_testing_on(self):
        reference = "src/sdk/memory.cpp"
        self.source_file(reference, '#include "sdk/memory.hpp"\n')
        self.source_file("src/sdk/memory.hpp", '#include "trajectory/cas_store.hpp"\n')
        self.source_file("src/trajectory/cas_store.hpp", "#pragma once\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": reference, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        report = self.check(testing=True)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertIn("src/trajectory/cas_store.hpp", report["scannedProjectFiles"])
        self.targets[-1]["name"] = "arbitrary_host"
        self.assert_rejected(self.check(testing=True), "unregistered private SDK reference")

    def test_real_private_approval_reference_is_only_testing_on(self):
        reference = "src/sdk/approval.cpp"
        self.source_file(reference, '#include "sdk/approval.hpp"\n')
        self.source_file("src/sdk/approval.hpp", '#include "runtime/scoped_approval.hpp"\n')
        self.source_file("src/runtime/scoped_approval.hpp", "#pragma once\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": reference, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        report = self.check(testing=True)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertIn("src/runtime/scoped_approval.hpp", report["scannedProjectFiles"])
        self.targets[-1]["sources"][0]["path"] = "src/sdk/core.cpp"
        self.assert_rejected(self.check(testing=True), "unregistered private SDK reference")

    def test_private_approval_reference_cannot_hide_a_host_include(self):
        self.flags["BUILD_TESTING"] = "ON"
        reference = "src/sdk/approval.cpp"
        self.source_file(reference, '#include "sdk/approval.hpp"\n')
        self.source_file("src/sdk/approval.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": reference, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def add_search_probe(self):
        self.source_file(boundary.SEARCH_PROBE_SOURCE, "int main() { return 0; }\n")
        probe = {"id": "probe", "name": boundary.SEARCH_PROBE_TARGET, "type": "EXECUTABLE",
                 "sources": [{"path": boundary.SEARCH_PROBE_SOURCE, "compileGroupIndex": 0}],
                 "compileGroups": [{}]}
        self.targets.append(probe)
        return probe

    def test_search_probe_is_only_a_testing_on_isolated_executable(self):
        probe = self.add_search_probe()
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        probe["type"] = "STATIC_LIBRARY"
        self.assert_rejected(self.check(testing=True), "executable target")

    def test_search_probe_cannot_broaden_its_source_allowance(self):
        self.flags["BUILD_TESTING"] = "ON"
        probe = self.add_search_probe()
        other = "tests/support/unrelated_probe.cpp"
        self.source_file(other, "int other;\n")
        probe["sources"].append({"path": other, "compileGroupIndex": 0})
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_search_probe_cannot_link_runtime_or_enter_sdk_closure(self):
        self.flags["BUILD_TESTING"] = "ON"
        probe = self.add_search_probe()
        probe["dependencies"] = [{"id": "engine"}]
        self.assert_rejected(self.check(testing=True), "must not depend")
        probe.pop("dependencies")
        self.targets[0]["dependencies"].append({"id": "probe"})
        self.assert_rejected(self.check(testing=True), "SDK library depends")

    def test_search_probe_allows_only_the_cmake_regeneration_utility(self):
        self.flags["BUILD_TESTING"] = "ON"
        probe = self.add_search_probe()
        self.targets.append({"id": "zero", "name": "ZERO_CHECK", "type": "UTILITY"})
        probe["dependencies"] = [{"id": "zero"}]
        self.assertEqual(self.check(testing=True)["status"], "passed")
        self.targets[-1]["name"] = "other_utility"
        self.assert_rejected(self.check(testing=True), "must not depend")

    def test_unbuilt_host_target_cannot_hide_behind_exclude_from_all(self):
        target = {"id": "host", "type": "UTILITY"}
        self.targets.append(target)
        for name in ("lubancode_official_docs", "lubancore_host_tests"):
            with self.subTest(target=name):
                target["name"] = name
                self.assert_rejected(self.check(), "host/resource target")

    def test_shared_resource_owner_test_is_a_narrow_sdk_allowance(self):
        shared = "tests/unit/runtime/test_session_resources.cpp"
        self.source_file(shared, "int resource_owner_test;\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": shared, "compileGroupIndex": 0}],
                             "compileGroups": [{}]})
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        nearby = "tests/unit/runtime/test_headless_parallel_read.cpp"
        self.source_file(nearby, "int other_runtime_test;\n")
        self.targets[-1]["sources"][0]["path"] = nearby
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        self.targets[-1]["sources"][0]["path"] = shared
        self.targets[-1]["name"] = "runtime_tests"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_shared_resource_owner_test_cannot_pull_in_host_headers(self):
        self.flags["BUILD_TESTING"] = "ON"
        shared = "tests/unit/runtime/test_session_resources.cpp"
        self.source_file(shared, '#include "runtime/assembly/session_resources.hpp"\n')
        self.source_file("src/runtime/assembly/session_resources.hpp",
                         '#include "app_server/session_assembly.hpp"\n')
        self.source_file("src/app_server/session_assembly.hpp", "#pragma once\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": shared, "compileGroupIndex": 0}],
                             "compileGroups": [{}]})
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_result_store_original_fixture_is_an_exact_testing_only_allowance(self):
        shared = "tests/unit/trajectory_v3/test_v3_result_store.cpp"
        self.source_file(shared, '#include "trajectory/v3/result_store.hpp"\n')
        self.source_file("src/trajectory/v3/result_store.hpp", "#pragma once\n")
        target = {"id": "result-store", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        report = self.check(testing=True)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertIn("src/trajectory/v3/result_store.hpp", report["scannedProjectFiles"])
        target["name"] = "lubancode_engine"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        nearby = "tests/unit/trajectory_v3/test_v3_reader.cpp"
        self.source_file(nearby, "int unrelated_trajectory_test;\n")
        target["sources"][0]["path"] = nearby
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_result_store_original_fixture_cannot_import_a_recursive_host_header(self):
        self.flags["BUILD_TESTING"] = "ON"
        shared = "tests/unit/trajectory_v3/test_v3_result_store.cpp"
        self.source_file(shared, '#include "trajectory/v3/result_store.hpp"\n')
        self.source_file("src/trajectory/v3/result_store.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.targets.append({"id": "result-store", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_child_terminal_allowance_remains_testing_only_and_neutral(self):
        shared = "tests/unit/runtime/test_subagent_terminal_receipt.cpp"
        self.source_file(shared, '#include "runtime/subagent_terminal.hpp"\n')
        self.source_file("src/runtime/subagent_terminal.hpp", "#pragma once\n")
        self.targets.append({"id": "receipt", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": shared, "compileGroupIndex": 0}],
                             "compileGroups": [{}]})
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        self.source_file("src/runtime/subagent_terminal.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_child_integration_allowance_cannot_enter_library_or_borrow_host_headers(self):
        shared = "tests/unit/runtime/test_child_foreground_integration.cpp"
        self.source_file(shared, '#include "tools/agent_tool.hpp"\n')
        self.source_file("src/tools/agent_tool.hpp", "#pragma once\n")
        target = {"id": "integration", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["name"] = "lubancode_runtime"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        self.source_file("src/tools/agent_tool.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_child_observation_allowance_remains_testing_only_and_cannot_import_host_state(self):
        shared = "tests/unit/runtime/test_child_parent_observation.cpp"
        self.source_file(shared, '#include "runtime/trajectory_turn_bridge.hpp"\n')
        self.source_file("src/runtime/trajectory_turn_bridge.hpp", "#pragma once\n")
        target = {"id": "observation", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["name"] = "lubancode_runtime"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        self.source_file("src/runtime/trajectory_turn_bridge.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_child_adoption_allowance_follows_its_shared_fixture_and_remains_testing_only(self):
        shared = "tests/unit/runtime/test_child_history_adoption.cpp"
        self.source_file(shared, '#include "child_observation_fixture.hpp"\n')
        self.source_file("tests/support/child_observation_fixture.hpp", '#include "runtime/trajectory_turn_bridge.hpp"\n')
        self.source_file("src/runtime/trajectory_turn_bridge.hpp", "#pragma once\n")
        target = {"id": "adoption", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}],
                  "compileGroups": [{"includes": [{"path": str(self.source / "tests/support")}]}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["name"] = "lubancode_runtime"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        self.source_file("tests/support/child_observation_fixture.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_execution_owner_allowance_is_testing_only_and_keeps_the_host_boundary(self):
        shared = "tests/unit/runtime/test_execution_owner.cpp"
        self.source_file(shared, '#include "runtime/execution_owner.hpp"\n')
        self.source_file("src/runtime/execution_owner.hpp", "#pragma once\n")
        target = {"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["name"] = "execution_tests"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        self.source_file("src/runtime/execution_owner.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_turn_bindings_allowance_excludes_its_cli_host_test(self):
        shared = "tests/unit/runtime/test_scoped_turn_bindings.cpp"
        host = "tests/unit/app/test_turn_runner_scoped_bindings.cpp"
        self.source_file(shared, "int turn_bindings_test;\n")
        self.source_file(host, "int cli_host_test;\n")
        target = {"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["sources"][0]["path"] = host
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["sources"][0]["path"] = shared
        target["name"] = "turn_tests"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_turn_bindings_shared_fixture_cannot_smuggle_cli_headers(self):
        self.flags["BUILD_TESTING"] = "ON"
        shared = "tests/unit/runtime/test_scoped_turn_bindings.cpp"
        self.source_file(shared, '#include "scoped_turn_fixture.hpp"\n')
        self.source_file("tests/support/scoped_turn_fixture.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": shared, "compileGroupIndex": 0}],
                             "compileGroups": [{"includes": [
                                 {"path": str(self.source / "tests/support")},
                             ]}]})
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_renamed_host_target_is_caught_by_its_source(self):
        self.source_file("src/cli/renamed.cpp", "int hidden_host;\n")
        self.targets.append({"id": "hidden", "name": "innocent_name", "type": "STATIC_LIBRARY",
                             "sources": [{"path": "src/cli/renamed.cpp"}]})
        self.assert_rejected(self.check(), "includes host source")

    def test_terminal_platform_source_is_not_a_core_exception(self):
        self.source_file("src/platform/console_posix.cpp", "int terminal;\n")
        self.targets[1]["sources"].append({"path": "src/platform/console_posix.cpp", "compileGroupIndex": 0})
        self.assert_rejected(self.check(), "includes host source")

    def test_transitive_private_header_cannot_include_host(self):
        self.source_file("src/neutral/bridge.hpp", '#include "another.hpp"\n')
        self.source_file("src/neutral/another.hpp", '#include "app/hidden.hpp"\n')
        self.source_file("src/app/hidden.hpp", "#pragma once\n")
        self.assert_rejected(self.check(), "reverse host include")

    def test_public_header_cannot_expose_internal_or_third_party_headers(self):
        for include in ("neutral/bridge.hpp", "nlohmann/json.hpp", "windows.h"):
            with self.subTest(include=include):
                self.source_file("include/lubancore/core.hpp", f'#include <{include}>\n')
                self.assert_rejected(self.check(), "non-public include")

    def test_target_include_directory_and_pch_cannot_hide_host_header(self):
        self.source_file("tests/support/helper.hpp", '#include "app/hidden.hpp"\n')
        self.source_file("src/app/hidden.hpp", "#pragma once\n")
        self.source_file("src/sdk/core.cpp", '#include "helper.hpp"\n')
        self.targets[0]["compileGroups"] = [{"includes": [{"path": str(self.source / "tests/support")}]}]
        self.assert_rejected(self.check(), "reverse host include")
        self.source_file("src/sdk/core.cpp", "int no_regular_includes;\n")
        self.targets[0]["compileGroups"] = [{"precompileHeaders": [
            {"header": str(self.source / "tests/support/helper.hpp")},
        ]}]
        self.assert_rejected(self.check(), "reverse host include")

    def test_comments_and_raw_cpp_literals_are_not_dependencies_or_stdio(self):
        self.source_file("src/sdk/core.cpp", '''// #include "app/no.hpp"
/* std::cout << "not executable"; */
const char* example = R"fixture(
#include "app/no.hpp"
std::cerr << "not executable";
)fixture";
#include "neutral/bridge.hpp"
''')
        self.assertEqual(self.check()["status"], "passed")

    def test_direct_entrypoint_io_and_global_setters_are_rejected(self):
        for code, reason in (("std::cerr << value;", "direct host stdio"),
                             ("std::println(value);", "direct host stdio"),
                             ("SetLanguage(value);", "process-global setter")):
            with self.subTest(code=code):
                self.source_file("src/sdk/core.cpp", code)
                self.assert_rejected(self.check(), reason)

    def test_wrong_cache_and_empty_sdk_artifact_do_not_pass(self):
        self.flags["LUBANCODE_BUILD_CLI"] = "ON"
        self.assert_rejected(self.check(), "cache LUBANCODE_BUILD_CLI")
        self.flags["LUBANCODE_BUILD_CLI"] = "OFF"
        (self.build / "liblubancore.fake-artifact").write_bytes(b"")
        self.assert_rejected(self.check(), "SDK artifact is missing or empty")

    def test_optional_release_symbols_do_not_replace_required_binary(self):
        self.targets[0]["artifacts"].append({"path": "lubancore.pdb"})
        self.assertEqual(self.check()["status"], "passed")
        (self.build / "liblubancore.fake-artifact").unlink()
        self.assert_rejected(self.check(), "SDK artifact is missing or empty")
        self.targets[0]["artifacts"] = [{"path": "lubancore.pdb"}]
        self.assert_rejected(self.check(), "no link/load build artifact")

    def test_missing_queries_or_wrong_build_reply_fail_closed(self):
        with self.assertRaisesRegex(ValueError, "no File API reply"):
            boundary.inspect(self.source, self.build, "Release", False)
        self.write_model()
        model = json.loads((self.reply / "model.json").read_text())
        model["paths"]["build"] = str(self.build / "other")
        self.json_file("model.json", model)
        with self.assertRaisesRegex(ValueError, "different source/build"):
            boundary.inspect(self.source, self.build, "Release", False)


if __name__ == "__main__":
    unittest.main()
