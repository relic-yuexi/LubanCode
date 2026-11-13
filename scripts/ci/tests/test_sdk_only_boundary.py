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

    def test_managed_and_memory_reference_sources_require_exact_testing_owner(self):
        sources = (
            "tests/unit/trajectory/test_managed_session_ownership.cpp",
            "tests/unit/trajectory/test_managed_session_reservation.cpp",
            "tests/unit/memory/test_memory_project_commit_handoff.cpp",
            "tests/unit/trajectory_v3/test_v3_result_immutable_publication.cpp",
        )
        for source in sources:
            self.source_file(source, "int reference_fixture;\n")
        target = {"id": "managed_memory_reference", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": source, "compileGroupIndex": 0} for source in sources],
                  "compileGroups": [{}]}
        self.targets.append(target)
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["name"] = "unrelated_reference"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        self.flags["BUILD_TESTING"] = "OFF"
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        extra = "tests/unit/trajectory/test_managed_session_ownership_extra.cpp"
        self.source_file(extra, "int unrelated_reference_fixture;\n")
        target["sources"].append({"path": extra, "compileGroupIndex": 0})
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_owned_file_paths_requires_its_exact_testing_reference_owner(self):
        from scripts.ci import sdk_owned_file_paths as owned
        self.source_file(owned.SOURCE, "int synthetic_source_data;\n")
        self.flags["BUILD_TESTING"] = "ON"
        self.assert_rejected(self.check(testing=True), "Owned file paths source")
        target = {"id": "owned_paths", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": owned.SOURCE, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assertEqual(self.check(testing=True)["status"], "passed")
        for field, value in (("name", "foreign_tests"), ("type", "STATIC_LIBRARY")):
            old = target[field]; target[field] = value
            self.assert_rejected(self.check(testing=True), "Owned file paths source")
            target[field] = old
        target["sources"].append(dict(target["sources"][0]))
        self.assert_rejected(self.check(testing=True), "Owned file paths source")
        target["sources"].pop()
        near = owned.SOURCE.replace(".cpp", "_other.cpp"); self.source_file(near, "int near_name;\n")
        target["sources"][0]["path"] = near
        self.assert_rejected(self.check(testing=True), "Owned file paths source")
        target["sources"][0]["path"] = owned.SOURCE; self.flags["BUILD_TESTING"] = "OFF"
        self.assert_rejected(self.check(), "Owned file paths source")
        self.targets.pop(); self.assertEqual(self.check()["status"], "passed")

    def deferred_action_reference(self, private=False):
        reference = "src/sdk/action_dispatch.cpp"
        self.source_file(reference, '#include "runtime/middleware_deferred_effects.hpp"\n')
        self.source_file("src/runtime/middleware_deferred_effects.hpp", '#include "hooks/middleware.hpp"\n')
        self.source_file("src/hooks/middleware.hpp", "#pragma once\n")
        target = {"id": "deferred_reference", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": reference, "compileGroupIndex": 0}], "compileGroups": [{}]}
        if private:
            self.flags["BUILD_TESTING"] = "ON"
            self.targets.append(target)
        else:
            self.targets[0]["sources"].extend(target["sources"])

    def test_deferred_action_header_is_recorded_in_production_and_private_reference(self):
        for private in (False, True):
            with self.subTest(private=private):
                self.deferred_action_reference(private)
                report = self.check(testing=private)
                self.assertEqual(report["status"], "passed", report["violations"])
                self.assertIn("src/runtime/middleware_deferred_effects.hpp", report["scannedProjectFiles"])
                self.assertIn({"from": "src/sdk/action_dispatch.cpp",
                               "to": "src/runtime/middleware_deferred_effects.hpp"}, report["projectIncludeEdges"])

    def test_deferred_action_header_cannot_smuggle_transitive_host_dependencies(self):
        self.deferred_action_reference(private=True)
        self.source_file("src/runtime/middleware_deferred_effects.hpp", '#include "neutral/deferred_bridge.hpp"\n')
        for host in ("app/turn_runner.hpp", "channel/manager.hpp", "updater/updater.hpp"):
            with self.subTest(host=host):
                self.source_file("src/neutral/deferred_bridge.hpp", f'#include "{host}"\n')
                self.source_file("src/" + host, "#pragma once\n")
                self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_public_header_cannot_expose_internal_deferred_action_header(self):
        self.deferred_action_reference()
        self.source_file("include/lubancore/core.hpp", '#include "runtime/middleware_deferred_effects.hpp"\n')
        self.assert_rejected(self.check(), "exposes non-public include")

    def test_deferred_action_move_preserves_host_stdio_and_global_state_guards(self):
        self.deferred_action_reference()
        for code, reason in (("std::cerr << 1;", "direct host stdio"),
                             ("printf(\"fixture\");", "direct host stdio"),
                             ("chdir(\"fixture\");", "process-global setter"),
                             ("SetEnvironmentVariableW(nullptr, nullptr);", "process-global setter")):
            with self.subTest(code=code):
                self.source_file("src/runtime/middleware_deferred_effects.hpp", code + "\n")
                self.assert_rejected(self.check(), reason)

    def test_deferred_action_state_guard_ignores_comments_and_literals(self):
        self.deferred_action_reference()
        self.source_file("src/runtime/middleware_deferred_effects.hpp",
                         '// std::cout << 1; setenv("fixture", "fixture", 1);\n'
                         'const char* diagnostic = "std::cerr chdir SetEnvironmentVariableW";\n'
                         'const char* fixture = R"(printf("fixture"); chdir("fixture"))";\n')
        report = self.check()
        self.assertEqual(report["status"], "passed", report["violations"])

    def job_post_reference(self, private=False):
        reference = "tests/unit/hooks/test_middleware_job_post_contract.cpp" if private else "src/sdk/extensions.cpp"
        self.source_file(reference, '#include "hooks/middleware_action_contract.hpp"\n')
        self.source_file("src/hooks/middleware_action_contract.hpp", '#include "hooks/middleware.hpp"\n')
        self.source_file("src/hooks/middleware.hpp", "#pragma once\n")
        target = {"id": "job_post_reference", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": reference, "compileGroupIndex": 0}], "compileGroups": [{}]}
        if private:
            self.flags["BUILD_TESTING"] = "ON"
            self.targets.append(target)
        else:
            self.targets[0]["sources"].extend(target["sources"])

    def test_job_post_contract_header_is_scanned_without_public_exposure(self):
        for private in (False, True):
            with self.subTest(private=private):
                self.job_post_reference(private)
                report = self.check(testing=private)
                self.assertEqual(report["status"], "passed", report["violations"])
                self.assertIn("src/hooks/middleware_action_contract.hpp", report["scannedProjectFiles"])
        self.source_file("include/lubancore/core.hpp", '#include "hooks/middleware_action_contract.hpp"\n')
        self.assert_rejected(self.check(testing=True), "exposes non-public include")

    def test_job_post_contract_header_rejects_transitive_host_dependencies(self):
        self.job_post_reference(private=True)
        self.source_file("src/hooks/middleware_action_contract.hpp", '#include "neutral/job_post_bridge.hpp"\n')
        for host in ("app/turn_runner.hpp", "channel/manager.hpp", "updater/updater.hpp"):
            with self.subTest(host=host):
                self.source_file("src/neutral/job_post_bridge.hpp", f'#include "{host}"\n')
                self.source_file("src/" + host, "#pragma once\n")
                self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_job_post_contract_preserves_stdio_and_global_state_guards(self):
        self.job_post_reference()
        for code, reason in (("std::cerr << 1;", "direct host stdio"),
                             ("printf(\"fixture\");", "direct host stdio"),
                             ("chdir(\"fixture\");", "process-global setter"),
                             ("SetEnvironmentVariableW(nullptr, nullptr);", "process-global setter")):
            with self.subTest(code=code):
                self.source_file("src/hooks/middleware_action_contract.hpp", code + "\n")
                self.assert_rejected(self.check(), reason)

    def test_job_post_native_source_allowance_rejects_cli_and_neighbors(self):
        self.job_post_reference(private=True)
        target = self.targets[-1]
        self.flags["BUILD_TESTING"] = "OFF"
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        nearby = "tests/unit/hooks/test_middleware_job_post_contract_extra.cpp"
        self.source_file(nearby, "int fixture_only;\n")
        target["sources"][0]["path"] = nearby
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["sources"][0]["path"] = "tests/unit/hooks/test_middleware_job_post_contract.cpp"
        target["name"] = "lubancode_tests"
        self.assert_rejected(self.check(testing=True), "host/resource target")

    def test_testing_on_does_not_enable_the_cli_test_graph(self):
        self.flags["BUILD_TESTING"] = "ON"
        self.source_file("tests/unit/cli/test_prompt.cpp", "int cli_test;\n")
        self.targets.append({"id": "tests", "name": "lubancode_tests", "type": "EXECUTABLE",
                             "sources": [{"path": "tests/unit/cli/test_prompt.cpp"}]})
        self.assert_rejected(self.check(testing=True), "host/resource target")
        self.targets[-1]["name"] = "lubancore_sdk_tests"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_exact_job_sources_are_only_compiled_when_testing_is_on(self):
        for name in ("tests/unit/tools/test_tool_job_coordinator.cpp", "tests/unit/tools/test_tool_job_start_transaction.cpp", "tests/unit/tools/test_tool_job_hold_recovery.cpp"):
            self.source_file(name, "int fixture_only;\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
            "sources": [{"path": name, "compileGroupIndex": 0} for name in
                ("tests/unit/tools/test_tool_job_coordinator.cpp", "tests/unit/tools/test_tool_job_start_transaction.cpp", "tests/unit/tools/test_tool_job_hold_recovery.cpp")],
            "compileGroups": [{}]})
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        report = self.check(testing=True)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.targets[-1]["sources"][0]["path"] = "tests/unit/tools/test_tool_job_unrelated.cpp"
        self.source_file("tests/unit/tools/test_tool_job_unrelated.cpp", "int unrelated;\n")
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

    def test_real_private_action_adapter_has_exact_testing_on_allowance(self):
        reference = "src/sdk/action_dispatch.cpp"
        self.source_file(reference, '#include "sdk/action_dispatch.hpp"\n')
        self.source_file("src/sdk/action_dispatch.hpp", '#include "hooks/middleware.hpp"\n')
        self.source_file("src/hooks/middleware.hpp", "#pragma once\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": reference, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        report = self.check(testing=True)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertIn("src/hooks/middleware.hpp", report["scannedProjectFiles"])
        self.source_file("src/sdk/action_opening.cpp", "int opening;\n")
        self.targets[-1]["sources"][0]["path"] = "src/sdk/action_opening.cpp"
        self.assert_rejected(self.check(testing=True), "unregistered private SDK reference")

    def test_private_action_adapter_cannot_hide_a_reverse_host_include(self):
        self.flags["BUILD_TESTING"] = "ON"
        reference = "src/sdk/action_dispatch.cpp"
        self.source_file(reference, '#include "sdk/action_dispatch.hpp"\n')
        self.source_file("src/sdk/action_dispatch.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": reference, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def operation_turn_reference(self):
        self.source_file("src/sdk/operation_ledger.cpp", '#include "sdk/operation_ledger.hpp"\n')
        self.source_file("src/sdk/operation_ledger.hpp", '#include "runtime/session_service.hpp"\n')
        self.source_file("src/runtime/session_service.hpp", "#pragma once\n")
        self.targets.append({"id": "binding", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
            "sources": [{"path": "src/sdk/operation_ledger.cpp", "compileGroupIndex": 0}], "compileGroups": [{}]})

    def job_operations_reference(self):
        self.source_file("src/sdk/job_operations.cpp", '#include "sdk/job_operations.hpp"\n')
        self.source_file("src/sdk/job_operations.hpp", '#include "runtime/session_service.hpp"\n')
        self.source_file("src/runtime/session_service.hpp", "#pragma once\n")
        self.targets.append({"id": "binding", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
            "sources": [{"path": "src/sdk/job_operations.cpp", "compileGroupIndex": 0}], "compileGroups": [{}]})

    def test_private_operation_turn_reference_has_exact_owner_and_testing_gate(self):
        self.operation_turn_reference()
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        report = self.check(testing=True)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertIn("src/runtime/session_service.hpp", report["scannedProjectFiles"])
        self.targets[-1]["name"] = "arbitrary_host"
        self.assert_rejected(self.check(testing=True), "unregistered private SDK reference owner")
        self.targets[-1]["name"] = "lubancore_sdk_tests"
        self.source_file("src/sdk/operation_ledger_extra.cpp", "int fixture;\n")
        self.targets[-1]["sources"][0]["path"] = "src/sdk/operation_ledger_extra.cpp"
        self.assert_rejected(self.check(testing=True), "unregistered private SDK reference implementation")

    def test_operation_turn_reference_cannot_hide_reverse_host_includes(self):
        self.operation_turn_reference()
        self.flags["BUILD_TESTING"] = "ON"
        self.source_file("src/runtime/session_service.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_private_job_operations_reference_has_exact_owner_and_testing_gate(self):
        self.job_operations_reference()
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        report = self.check(testing=True)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertIn("src/runtime/session_service.hpp", report["scannedProjectFiles"])
        for owner in ("arbitrary_host", "lubancode_engine", "lubancode_runtime"):
            self.targets[-1]["name"] = owner
            self.assert_rejected(self.check(testing=True), "unregistered private SDK reference owner")
        self.targets[-1]["name"] = "lubancore_sdk_tests"
        self.source_file("src/sdk/job_operations_extra.cpp", "int fixture;\n")
        self.targets[-1]["sources"][0]["path"] = "src/sdk/job_operations_extra.cpp"
        self.assert_rejected(self.check(testing=True), "unregistered private SDK reference implementation")

    def test_job_operations_reference_cannot_hide_reverse_host_includes(self):
        self.job_operations_reference()
        self.flags["BUILD_TESTING"] = "ON"
        self.source_file("src/runtime/session_service.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def add_search_probe(self):
        self.source_file(boundary.SEARCH_PROBE_SOURCE, "int main() { return 0; }\n")
        probe = {"id": "probe", "name": boundary.SEARCH_PROBE_TARGET, "type": "EXECUTABLE",
                 "sources": [{"path": boundary.SEARCH_PROBE_SOURCE, "compileGroupIndex": 0}],
                 "compileGroups": [{}]}
        self.targets.append(probe)
        return probe

    def add_command_probe(self):
        self.source_file(boundary.COMMAND_LIMITS_PROBE_SOURCE, "#include <iostream>\nint main() { return 0; }\n")
        probe = {"id": "command-probe", "name": boundary.COMMAND_LIMITS_PROBE_TARGET,
                 "type": "EXECUTABLE", "compileGroups": [{}],
                 "sources": [{"path": boundary.COMMAND_LIMITS_PROBE_SOURCE, "compileGroupIndex": 0}]}
        self.targets.append(probe)
        return probe

    def test_command_probe_testing_off_and_wrong_target_type_reject(self):
        probe = self.add_command_probe()
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        probe["type"] = "STATIC_LIBRARY"
        self.assert_rejected(self.check(testing=True), "executable target")

    def test_command_probe_wrong_owner_and_adjacent_support_source_reject(self):
        self.flags["BUILD_TESTING"] = "ON"
        probe = self.add_command_probe()
        probe["name"] = "unrelated_probe"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        probe["name"] = boundary.COMMAND_LIMITS_PROBE_TARGET
        other = "tests/support/command_limits_probe_adjacent.cpp"
        self.source_file(other, "int adjacent;\n")
        probe["sources"].append({"path": other, "compileGroupIndex": 0})
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_command_probe_source_cannot_compile_directly_in_sdk_test_target(self):
        self.flags["BUILD_TESTING"] = "ON"
        probe = self.add_command_probe()
        probe["name"] = "lubancore_sdk_tests"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_command_probe_must_not_link_a_project_library_or_enter_sdk_closure(self):
        self.flags["BUILD_TESTING"] = "ON"
        probe = self.add_command_probe()
        probe["dependencies"] = [{"id": "engine"}]
        self.assert_rejected(self.check(testing=True), "must not depend")
        probe.pop("dependencies")
        self.targets[0]["dependencies"].append({"id": "command-probe"})
        self.assert_rejected(self.check(testing=True), "SDK library depends")

    def test_command_probe_allows_only_cmake_regeneration_utility(self):
        self.flags["BUILD_TESTING"] = "ON"
        probe = self.add_command_probe()
        self.targets.append({"id": "zero", "name": "ZERO_CHECK", "type": "UTILITY"})
        probe["dependencies"] = [{"id": "zero"}]
        self.assertEqual(self.check(testing=True)["status"], "passed")
        self.targets[-1]["name"] = "arbitrary_utility"
        self.assert_rejected(self.check(testing=True), "must not depend")

    def test_command_probe_cannot_hide_project_or_platform_header_dependencies(self):
        self.flags["BUILD_TESTING"] = "ON"
        self.add_command_probe()
        for include in ("neutral/bridge.hpp", "windows.h"):
            self.source_file(boundary.COMMAND_LIMITS_PROBE_SOURCE, '#include "' + include + '"\nint main() {}\n')
            self.assert_rejected(self.check(testing=True), "only standard-library headers")

    def test_command_source_is_only_one_testing_on_sdk_owned_shared_fixture(self):
        shared = "tests/unit/tools/test_run_command_execution_limits.cpp"
        self.source_file(shared, "int command_test;\n")
        target = {"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["name"] = "neutral_engine"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        nearby = "tests/unit/tools/test_run_command_process.cpp"
        self.source_file(nearby, "int adjacent_test;\n")
        target["sources"][0]["path"] = nearby
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_command_source_still_rejects_reverse_host_headers(self):
        self.flags["BUILD_TESTING"] = "ON"
        shared = "tests/unit/tools/test_run_command_execution_limits.cpp"
        self.source_file(shared, '#include "tools/run_command.hpp"\n')
        self.source_file("src/tools/run_command.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_journal_receipt_source_is_exact_testing_on_and_sdk_test_owned(self):
        shared = "tests/unit/trajectory/test_journal_native_receipts.cpp"
        self.source_file(shared, "int journal_receipt_test;\n")
        target = {"id": "journal_receipts", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        for owner in ("receipt_tests", "neutral_engine", "lubancore_sdk"):
            with self.subTest(owner=owner):
                target["name"] = owner
                self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        nearby = "tests/unit/trajectory/test_journal_native_receipts_extra.cpp"
        self.source_file(nearby, "int adjacent_journal_test;\n")
        target["sources"][0]["path"] = nearby
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_journal_receipt_source_keeps_recursive_reverse_host_rejection(self):
        self.flags["BUILD_TESTING"] = "ON"
        shared = "tests/unit/trajectory/test_journal_native_receipts.cpp"
        self.source_file(shared, '#include "trajectory/journal.hpp"\n')
        self.source_file("src/trajectory/journal.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.targets.append({"id": "journal_receipts", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_v3_journal_witness_source_is_exact_testing_on_and_sdk_test_owned(self):
        shared = "tests/unit/trajectory_v3/test_v3_journal_receipts.cpp"
        self.source_file(shared, "int journal_receipt_test;\n")
        target = {"id": "journal_receipts", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        for owner in ("receipt_tests", "neutral_engine", "lubancore_sdk"):
            with self.subTest(owner=owner):
                target["name"] = owner
                self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        nearby = "tests/unit/trajectory_v3/test_v3_journal_receipts_extra.cpp"
        self.source_file(nearby, "int adjacent_journal_test;\n")
        target["sources"][0]["path"] = nearby
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")

    def test_v3_journal_witness_source_keeps_recursive_reverse_host_rejection(self):
        self.flags["BUILD_TESTING"] = "ON"
        shared = "tests/unit/trajectory_v3/test_v3_journal_receipts.cpp"
        self.source_file(shared, '#include "trajectory/v3/writer.hpp"\n')
        self.source_file("src/trajectory/v3/writer.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.targets.append({"id": "journal_receipts", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(testing=True), "reverse host include")

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

    def test_lua_protection_allowance_is_one_neutral_test_source(self):
        shared = "tests/unit/tools/test_lua_protected.cpp"
        self.source_file(shared, '#include "tools/lua_tool.hpp"\n')
        self.source_file("src/tools/lua_tool.hpp", "#pragma once\n")
        target = {"id": "lua-protection", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["name"] = "lubancode_runtime"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        nearby = "tests/unit/tools/test_lua_tool.cpp"
        self.source_file(nearby, '#include "tools/lua_tool.hpp"\n')
        target["sources"].append({"path": nearby, "compileGroupIndex": 0})
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["sources"].pop()
        self.source_file("src/tools/lua_tool.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
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

    def test_middleware_receipt_allowance_is_exact_testing_only_and_keeps_the_host_boundary(self):
        shared = "tests/unit/runtime/test_middleware_native_receipts.cpp"
        self.source_file(shared, '#include "runtime/middleware_v3_sink.hpp"\n')
        self.source_file("src/runtime/middleware_v3_sink.hpp", "#pragma once\n")
        target = {"id": "receipts", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["name"] = "receipt_tests"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        nearby = "tests/unit/runtime/test_middleware_session_binding.cpp"
        self.source_file(nearby, "int unregistered_test;\n")
        target["sources"][0]["path"] = nearby
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["sources"][0]["path"] = shared
        self.source_file("src/runtime/middleware_v3_sink.hpp", '#include "app/turn_runner.hpp"\n')
        self.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.assert_rejected(self.check(testing=True), "reverse host include")

    def test_middleware_cause_allowance_is_exact_testing_only_and_keeps_the_host_boundary(self):
        shared = "tests/unit/hooks/test_middleware_dispatch_cause.cpp"
        self.source_file(shared, '#include "hooks/middleware.hpp"\n')
        self.source_file("src/hooks/middleware.hpp", "#pragma once\n")
        target = {"id": "receipts", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                  "sources": [{"path": shared, "compileGroupIndex": 0}], "compileGroups": [{}]}
        self.targets.append(target)
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(testing=True)["status"], "passed")
        target["name"] = "receipt_tests"
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["name"] = "lubancore_sdk_tests"
        nearby = "tests/unit/hooks/test_middleware_runtime_wire.cpp"
        self.source_file(nearby, "int unregistered_test;\n")
        target["sources"][0]["path"] = nearby
        self.assert_rejected(self.check(testing=True), "non-SDK test compilation")
        target["sources"][0]["path"] = shared
        self.source_file("src/hooks/middleware.hpp", '#include "app/turn_runner.hpp"\n')
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

    def test_release_query_and_package_sources_cannot_hide_in_sdk_only_targets(self):
        for path in ("src/config/update_checker.cpp", "src/config/update_checker.hpp",
                     "src/package/catalog.cpp", "src/package/catalog.hpp"):
            with self.subTest(path=path):
                self.source_file(path, "int host_material;\n")
                self.targets.append({"id": "hidden", "name": "neutral_name", "type": "STATIC_LIBRARY",
                                     "sources": [{"path": path, "compileGroupIndex": 0}], "compileGroups": [{}]})
                self.assert_rejected(self.check(), "includes host source " + path)
                self.targets.pop()

    def test_private_header_cannot_reimport_release_query_or_package(self):
        for path in ("src/config/update_checker.hpp", "src/package/catalog.hpp"):
            with self.subTest(path=path):
                self.source_file(path, "#pragma once\n")
                self.source_file("src/neutral/bridge.hpp", '#include "' + path.removeprefix("src/") + '"\n')
                self.assert_rejected(self.check(), "reverse host include")

    def add_package_parsers(self):
        self.targets[1]["name"] = "lubancode_engine"
        for name in sorted(boundary.SDK_NEUTRAL_PACKAGE_FILES):
            contents = ('#include "package/' + Path(name).stem + '.hpp"\n') if name.endswith(".cpp") else "#pragma once\n"
            self.source_file(name, contents)
        self.source_file("src/package/manifest.hpp", '#include "package/semver.hpp"\n')
        self.source_file("src/sdk/core.cpp", '#include "package/manifest.hpp"\n')
        self.targets[1]["sources"].extend({"path": name, "compileGroupIndex": 0}
                                         for name in sorted(boundary.SDK_NEUTRAL_PACKAGE_SOURCES))

    def test_package_exact_neutral_implementations_and_recursive_headers(self):
        self.add_package_parsers()
        for testing in (False, True):
            self.flags["BUILD_TESTING"] = "ON" if testing else "OFF"
            report = self.check(testing)
            self.assertEqual(report["status"], "passed", report["violations"])
            self.assertTrue(boundary.SDK_NEUTRAL_PACKAGE_FILES <= set(report["scannedProjectFiles"]))

    def test_package_parsers_require_their_exact_static_single_owner(self):
        self.add_package_parsers()
        originals = list(self.targets[1]["sources"])
        for source in sorted(boundary.SDK_NEUTRAL_PACKAGE_SOURCES):
            for variant in ("missing", "duplicate", "sdk", "shared", "renamed"):
                with self.subTest(source=source, variant=variant):
                    self.targets[1]["sources"] = list(originals)
                    self.targets[1]["name"], self.targets[1]["type"] = "lubancode_engine", "STATIC_LIBRARY"
                    if variant in ("missing", "sdk"):
                        self.targets[1]["sources"] = [entry for entry in originals if entry["path"] != source]
                    if variant == "sdk":
                        self.targets[0]["sources"].append({"path": source, "compileGroupIndex": 0})
                    elif variant == "duplicate":
                        self.targets[1]["sources"].append({"path": source, "compileGroupIndex": 0})
                    elif variant == "shared":
                        self.targets[1]["type"] = "SHARED_LIBRARY"
                    elif variant == "renamed":
                        self.targets[1]["name"] = "innocent_parser"
                    self.assert_rejected(self.check(), "Neutral Package parser must belong once")
                    if variant == "sdk":
                        self.targets[0]["sources"].pop()

    def test_package_exceptions_do_not_cover_nearby_sources_or_recursive_host_headers(self):
        self.add_package_parsers()
        for name in ("src/package/manifest_extra.cpp", "src/package/semver_extra.cpp", "src/package/component.cpp",
                     "src/package/inventory_extra.cpp", "src/package/inventory_snapshot_extra.cpp"):
            with self.subTest(name=name):
                self.source_file(name, "int only_fixture;\n")
                self.targets[1]["sources"].append({"path": name, "compileGroupIndex": 0})
                self.assert_rejected(self.check(), "includes host source " + name)
                self.targets[1]["sources"].pop()
        self.source_file("src/package/component.hpp", "#pragma once\n")
        for header in sorted(boundary.SDK_NEUTRAL_PACKAGE_FILES - boundary.SDK_NEUTRAL_PACKAGE_SOURCES):
            with self.subTest(header=header):
                original = (self.source / header).read_text(encoding="utf-8")
                self.source_file(header, '#include "package/component.hpp"\n')
                self.assert_rejected(self.check(), "reverse host include")
                self.source_file(header, original)

    def test_original_package_test_exception_is_exact_and_testing_only(self):
        source = "tests/unit/packages/test_package_manifest.cpp"
        self.source_file(source, "int fixture;\n")
        self.targets.append({"id": "tests", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                             "sources": [{"path": source, "compileGroupIndex": 0}], "compileGroups": [{}]})
        self.assert_rejected(self.check(), "testing is OFF")
        self.flags["BUILD_TESTING"] = "ON"
        self.assertEqual(self.check(True)["status"], "passed")
        nearby = "tests/unit/packages/test_package_component.cpp"
        self.source_file(nearby, "int fixture;\n")
        self.targets[-1]["sources"][0]["path"] = nearby
        self.assert_rejected(self.check(True), "non-SDK test compilation")

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

    def test_neutral_channel_config_and_types_keep_their_recursive_headers(self):
        self.source_file("src/channel/types.hpp", "#include <string>\n")
        self.source_file("src/channel/types.cpp", '#include "channel/types.hpp"\n')
        self.source_file("src/channel/channel_config.hpp", '#include "channel/types.hpp"\n')
        self.source_file("src/channel/channel_config.cpp", '#include "channel/channel_config.hpp"\n')
        self.source_file("src/config/config.hpp", '#include "channel/channel_config.hpp"\n')
        self.source_file("src/neutral/bridge.hpp", '#include "config/config.hpp"\n')
        self.targets[1]["sources"].extend(
            [{"path": name, "compileGroupIndex": 0} for name in
             ("src/channel/types.cpp", "src/channel/channel_config.cpp")])
        report = self.check()
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertTrue(boundary.SDK_NEUTRAL_CHANNEL_FILES <= set(report["scannedProjectFiles"]))

    def test_disconnected_channel_hosts_or_tls_targets_do_not_belong_in_sdk_only(self):
        for name in sorted(boundary.CHANNEL_HOST_TARGETS | boundary.MBEDTLS_TARGETS):
            with self.subTest(name=name):
                self.targets.append({"id": "disconnected", "name": name,
                                     "type": "STATIC_LIBRARY", "sources": []})
                self.assert_rejected(self.check(), "host/resource target")
                self.targets.pop()

    def test_all_frozen_host_sources_are_rejected_even_under_neutral_target_name(self):
        for source in sorted(boundary.CHANNEL_HOST_SOURCES | boundary.CHANNEL_RUNTIME_SOURCES):
            with self.subTest(source=source):
                self.source_file(source, "int host_fixture_only;\n")
                self.targets[1]["sources"].append({"path": source, "compileGroupIndex": 0})
                try:
                    self.assert_rejected(self.check(), "target neutral_engine includes host source " + source)
                finally:
                    self.targets[1]["sources"].pop()

    def test_new_host_sources_and_neutral_lookalikes_are_not_allowlisted(self):
        for source in ("src/channel/new_transport.cpp", "src/gateway/new_host.cpp",
                       "src/channel/types_extra.cpp", "src/channel/channel_config_extra.cpp"):
            with self.subTest(source=source):
                self.source_file(source, "int host_fixture_only;\n")
                self.targets[1]["sources"].append({"path": source, "compileGroupIndex": 0})
                try:
                    self.assert_rejected(self.check(), "target neutral_engine includes host source " + source)
                finally:
                    self.targets[1]["sources"].pop()

    def test_neutral_config_exception_cannot_forward_to_transport_or_gateway_header(self):
        self.source_file("src/config/config.hpp", '#include "channel/channel_config.hpp"\n')
        self.source_file("src/neutral/bridge.hpp", '#include "config/config.hpp"\n')
        for header in ("channel/channel_router.hpp", "channel/transport/tls.hpp", "gateway/profile.hpp"):
            with self.subTest(header=header):
                self.source_file("src/" + header, "#pragma once\n")
                self.source_file("src/channel/channel_config.hpp", '#include "' + header + '"\n')
                self.assert_rejected(self.check(), "reverse host include")

    def test_runtime_host_headers_cannot_leak_through_sdk_or_precompiled_header(self):
        for source in sorted(boundary.CHANNEL_RUNTIME_SOURCES):
            header = source.removesuffix(".cpp") + ".hpp"
            self.source_file(header, "#pragma once\n")
            include = header.removeprefix("src/")
            for kind in ("direct", "pch"):
                with self.subTest(header=header, kind=kind):
                    self.source_file("src/sdk/core.cpp", '#include "' + include + '"\n' if kind == "direct" else "int fixture;\n")
                    self.targets[0]["compileGroups"] = [{}] if kind == "direct" else [
                        {"precompileHeaders": [{"header": str(self.source / header)}]}]
                    reason = "reverse host include" if kind == "direct" else "target lubancore_sdk includes host source " + header
                    self.assert_rejected(self.check(), reason)


if __name__ == "__main__":
    unittest.main()
