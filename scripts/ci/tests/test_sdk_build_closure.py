"""Build-graph data fixtures; no CMake, compiler or native executable runs."""
from __future__ import annotations

from copy import deepcopy
import unittest

from scripts.ci import check_sdk_build_closure as closure
from scripts.ci.tests import test_sdk_only_boundary as file_api_fixtures


def complete_channel_hosts(targets):
    targets["engine"]["name"] = "lubancode_engine"
    targets["engine"]["projectSources"].extend(
        ["src/channel/types.cpp", "src/channel/channel_config.cpp", *sorted(closure.SDK_NEUTRAL_PACKAGE_SOURCES)])
    targets["runtime"] = {"name": "lubancode_runtime", "type": "STATIC_LIBRARY",
                          "projectSources": ["src/runtime/session_runtime.cpp"], "dependencies": ["engine"]}
    targets["channel_host"] = {"name": "lubancode_channel_host", "type": "STATIC_LIBRARY",
                               "projectSources": sorted(closure.CHANNEL_HOST_SOURCES),
                               "dependencies": ["engine", "tls"]}
    targets["channel_runtime"] = {"name": "lubancode_channel_runtime", "type": "STATIC_LIBRARY",
                                  "projectSources": sorted(closure.CHANNEL_RUNTIME_SOURCES),
                                  "dependencies": ["runtime", "channel_host"]}
    for key, name in (("tls", "mbedtls"), ("x509", "mbedx509"), ("crypto", "mbedcrypto")):
        targets[key] = {"name": name, "type": "STATIC_LIBRARY", "projectSources": [], "dependencies": []}
    targets["sdk"]["dependencies"] = ["runtime"]
    return targets


class OptionalHostClosureTests(unittest.TestCase):
    def setUp(self):
        self.targets = {
            "sdk": {"name": "lubancore_sdk", "type": "SHARED_LIBRARY",
                    "projectSources": ["src/sdk/core.cpp", "src/sdk/job_operations.cpp"], "dependencies": ["engine"]},
            "engine": {"name": "engine", "type": "STATIC_LIBRARY",
                       "projectSources": ["src/neutral/core.cpp"], "dependencies": []},
            "host": {"name": "lubancode_updater", "type": "STATIC_LIBRARY",
                     "projectSources": ["src/updater/engine.cpp", "src/config/update_checker.cpp"],
                     "dependencies": ["engine", "zip"]},
            "zip": {"name": "miniz", "type": "STATIC_LIBRARY",
                    "projectSources": [], "dependencies": []},
        }

    def test_combined_host_may_depend_on_engine_without_reverse_sdk_dependency(self):
        report = closure.inspect_graph(self.targets)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(set(report["sdkBuildClosure"]), {"sdk", "engine"})

    def test_job_binding_is_sdk_owned_with_only_real_reference_executable_copies(self):
        source = "src/sdk/job_operations.cpp"
        for name in ("lubancore_sdk_tests", "lubancode_tests"):
            self.targets[name] = {"name": name, "type": "EXECUTABLE",
                                  "projectSources": [source], "dependencies": ["sdk"]}
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "passed")
        for name in ("engine", "disconnected_archive", "another_test"):
            with self.subTest(name=name):
                targets = deepcopy(self.targets)
                targets[name] = {"name": name, "type": "STATIC_LIBRARY",
                                 "projectSources": [source], "dependencies": []}
                report = closure.inspect_graph(targets)
                self.assertEqual(report["status"], "failed")
                self.assertTrue(any("unregistered reference owner" in v for v in report["violations"]))
        self.targets["sdk"]["projectSources"].remove(source)
        self.assertTrue(any("must belong exactly once to the shared SDK" in v
                            for v in closure.inspect_graph(self.targets)["violations"]))

    def test_job_binding_reference_name_cannot_hide_a_library(self):
        source = "src/sdk/job_operations.cpp"
        self.targets["reference"] = {"name": "lubancore_sdk_tests", "type": "STATIC_LIBRARY",
                                     "projectSources": [source], "dependencies": []}
        self.assertTrue(any("unregistered reference owner" in v
                            for v in closure.inspect_graph(self.targets)["violations"]))

    def test_job_binding_cannot_be_missing_or_duplicated_within_any_owner(self):
        source = "src/sdk/job_operations.cpp"
        missing = deepcopy(self.targets)
        missing["sdk"]["projectSources"].remove(source)
        report = closure.inspect_graph(missing)
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any("must belong exactly once" in v for v in report["violations"]))
        for name in ("sdk", "lubancore_sdk_tests", "lubancode_tests"):
            with self.subTest(owner=name):
                targets = deepcopy(self.targets)
                if name == "sdk":
                    targets[name]["projectSources"].append(source)
                else:
                    targets[name] = {"name": name, "type": "EXECUTABLE",
                                     "projectSources": [source, source], "dependencies": ["sdk"]}
                report = closure.inspect_graph(targets)
                self.assertEqual(report["status"], "failed")
                self.assertTrue(any("duplicate source occurrences" in v for v in report["violations"]))

    def test_direct_and_transitive_host_dependency_are_rejected(self):
        for owner in ("sdk", "engine"):
            with self.subTest(owner=owner):
                targets = deepcopy(self.targets)
                targets[owner]["dependencies"].append("host")
                report = closure.inspect_graph(targets)
                self.assertEqual(report["status"], "failed")
                self.assertTrue(any("lubancode_updater" in v for v in report["violations"]))
                self.assertTrue(any("miniz" in v for v in report["violations"]))

    def test_unrenamed_dependency_without_updater_sources_still_fails(self):
        self.targets["engine"]["dependencies"].append("zip")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "failed")

    def test_updater_sources_hidden_in_a_neutral_target_still_fail(self):
        self.targets["engine"]["projectSources"].append("src/updater/paths.cpp")
        report = closure.inspect_graph(self.targets)
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any("src/updater/paths.cpp" in v for v in report["violations"]))

    def test_residual_release_query_sources_cannot_hide_in_a_neutral_target(self):
        for path in ("src/config/update_checker.cpp", "src/config/update_checker.hpp"):
            with self.subTest(path=path):
                targets = deepcopy(self.targets)
                targets["engine"]["projectSources"].append(path)
                report = closure.inspect_graph(targets)
                self.assertEqual(report["status"], "failed")
                self.assertTrue(any("host-only source: " + path in v for v in report["violations"]))

    def test_package_may_remain_in_cli_without_entering_the_sdk_closure(self):
        complete_channel_hosts(self.targets)
        self.targets["cli"] = {"name": "lubancode_core", "type": "STATIC_LIBRARY",
                               "projectSources": ["src/package/inventory.cpp"],
                               "dependencies": ["host", "channel_runtime"]}
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "passed")
        self.targets["sdk"]["dependencies"].append("cli")
        report = closure.inspect_graph(self.targets)
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any("lubancode_core" in v for v in report["violations"]))
        self.assertTrue(any("src/package/inventory.cpp" in v for v in report["violations"]))
        self.targets["sdk"]["dependencies"].remove("cli")
        self.targets["engine"]["projectSources"].append("src/package/semver.cpp")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "failed")

    def test_neutral_package_parsers_have_single_static_engine_ownership(self):
        complete_channel_hosts(self.targets)
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "passed")
        for source in sorted(closure.SDK_NEUTRAL_PACKAGE_SOURCES):
            for variant in ("missing", "runtime", "duplicate", "host", "renamed", "shared"):
                with self.subTest(source=source, variant=variant):
                    targets = deepcopy(self.targets)
                    if variant in ("missing", "runtime", "host"):
                        targets["engine"]["projectSources"].remove(source)
                    if variant in ("runtime", "host", "duplicate"):
                        targets[{"runtime": "runtime", "host": "host", "duplicate": "engine"}[variant]]["projectSources"].append(source)
                    elif variant == "renamed":
                        targets["engine"]["name"] = "innocent_parser"
                    elif variant == "shared":
                        targets["engine"]["type"] = "SHARED_LIBRARY"
                    report = closure.inspect_graph(targets)
                    self.assertEqual(report["status"], "failed")
                    self.assertTrue(any("Neutral Package parser must belong once" in reason for reason in report["violations"]))

    def test_cli_release_query_has_one_exact_static_owner(self):
        for variant in ("missing", "engine", "duplicate", "renamed", "shared"):
            with self.subTest(variant=variant):
                targets = deepcopy(self.targets)
                if variant == "missing":
                    targets["host"]["projectSources"].remove("src/config/update_checker.cpp")
                elif variant == "engine":
                    targets["host"]["projectSources"].remove("src/config/update_checker.cpp")
                    targets["engine"]["projectSources"].append("src/config/update_checker.cpp")
                elif variant == "duplicate":
                    targets["host"]["projectSources"].append("src/config/update_checker.cpp")
                elif variant == "renamed":
                    targets["host"]["name"] = "innocent_name"
                else:
                    targets["host"]["type"] = "SHARED_LIBRARY"
                report = closure.inspect_graph(targets)
                self.assertEqual(report["status"], "failed")
                self.assertTrue(any("Release query implementation" in v for v in report["violations"]))

    def test_unknown_transitive_dependency_fails_closed(self):
        self.targets["engine"]["dependencies"].append("missing")
        with self.assertRaisesRegex(ValueError, "unknown build dependency"):
            closure.inspect_graph(self.targets)

    def test_sdk_identity_and_implementation_are_required(self):
        self.targets["sdk"]["projectSources"] = []
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "failed")
        self.targets["sdk"]["type"] = "STATIC_LIBRARY"
        with self.assertRaisesRegex(ValueError, "exactly one shared"):
            closure.inspect_graph(self.targets)

    def test_repeated_dependencies_do_not_pull_an_unrelated_host_into_closure(self):
        self.targets["sdk"]["dependencies"] *= 2
        self.targets["engine"]["dependencies"].append("sdk")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "passed")

    def test_loop_lease_provider_must_share_the_engine_static_target(self):
        targets = deepcopy(self.targets)
        targets["engine"]["name"] = "lubancode_engine"
        targets["engine"]["projectSources"].extend(
            ["src/agent/loop.cpp", "src/runtime/scoped_approval.cpp",
             "src/channel/types.cpp", "src/channel/channel_config.cpp", *sorted(closure.SDK_NEUTRAL_PACKAGE_SOURCES)])
        targets["runtime"] = {"name": "lubancode_runtime", "type": "STATIC_LIBRARY",
                              "projectSources": ["src/runtime/session_runtime.cpp"],
                              "dependencies": ["engine"]}
        targets["sdk"]["dependencies"] = ["runtime"]
        self.assertEqual(closure.inspect_graph(targets)["status"], "passed")
        # The old graph has the same complete source union, but the provider is
        # in the upstream archive and the Host static link cannot resolve it.
        targets["engine"]["projectSources"].remove("src/runtime/scoped_approval.cpp")
        targets["runtime"]["projectSources"].append("src/runtime/scoped_approval.cpp")
        report = closure.inspect_graph(targets)
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any("lease implementation" in v for v in report["violations"]))

    def test_loop_lease_missing_or_duplicate_provider_is_rejected(self):
        self.targets["engine"]["name"] = "lubancode_engine"
        self.targets["engine"]["projectSources"].extend(
            ["src/channel/types.cpp", "src/channel/channel_config.cpp", *sorted(closure.SDK_NEUTRAL_PACKAGE_SOURCES)])
        self.targets["engine"]["projectSources"].append("src/agent/loop.cpp")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "failed")
        self.targets["engine"]["projectSources"].append("src/runtime/scoped_approval.cpp")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "passed")
        self.targets["engine"]["projectSources"].append("src/runtime/scoped_approval.cpp")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "failed")


class ChannelGatewayHostClosureTests(unittest.TestCase):
    def setUp(self):
        OptionalHostClosureTests.setUp(self)
        self.targets = complete_channel_hosts(self.targets)
        self.targets["cli"] = {"name": "lubancode_core", "type": "STATIC_LIBRARY",
                               "projectSources": ["src/cli/channel_status_command.cpp"],
                               "dependencies": ["host", "channel_runtime"]}

    def test_channel_combined_graph_keeps_neutral_config_and_host_owners(self):
        report = closure.inspect_graph(self.targets)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertEqual(set(report["sdkBuildClosure"]), {"sdk", "runtime", "engine"})
        self.assertTrue({"src/channel/types.cpp", "src/channel/channel_config.cpp"} <= set(report["sdkProjectSources"]))
        self.assertFalse(set(report["sdkProjectSources"]) & (closure.CHANNEL_HOST_SOURCES | closure.CHANNEL_RUNTIME_SOURCES))

    def test_every_host_source_requires_one_exact_static_owner(self):
        for owner, sources in (("channel_host", closure.CHANNEL_HOST_SOURCES),
                               ("channel_runtime", closure.CHANNEL_RUNTIME_SOURCES)):
            for source in sources:
                for variant in ("missing", "engine", "duplicate", "foreign_duplicate"):
                    with self.subTest(source=source, variant=variant):
                        targets = deepcopy(self.targets)
                        if variant in ("missing", "engine"):
                            targets[owner]["projectSources"].remove(source)
                        if variant == "engine":
                            targets["engine"]["projectSources"].append(source)
                        elif variant == "duplicate":
                            targets[owner]["projectSources"].append(source)
                        elif variant == "foreign_duplicate":
                            targets["host"]["projectSources"].append(source)
                        self.assertEqual(closure.inspect_graph(targets)["status"], "failed")

    def test_host_identity_roster_and_type_cannot_hide_behind_disconnected_cli(self):
        for key in ("channel_host", "channel_runtime"):
            for variant in ("missing", "renamed", "shared", "extra", "duplicate_target"):
                with self.subTest(owner=key, variant=variant):
                    targets = deepcopy(self.targets)
                    if variant == "missing":
                        del targets[key]
                        for target in targets.values():
                            target["dependencies"] = [dep for dep in target["dependencies"] if dep != key]
                    elif variant == "renamed":
                        targets[key]["name"] = "innocent_host"
                    elif variant == "shared":
                        targets[key]["type"] = "SHARED_LIBRARY"
                    elif variant == "extra":
                        targets[key]["projectSources"].append("src/neutral/unexpected.cpp")
                    else:
                        targets["duplicate"] = deepcopy(targets[key])
                    self.assertEqual(closure.inspect_graph(targets)["status"], "failed")
        targets = deepcopy(self.targets)
        del targets["channel_host"], targets["channel_runtime"]
        targets["cli"]["dependencies"] = ["host"]
        self.assertEqual(closure.inspect_graph(targets)["status"], "failed")

    def test_neutral_channel_sources_must_remain_once_in_engine_even_sdk_only(self):
        for source in ("src/channel/types.cpp", "src/channel/channel_config.cpp"):
            for variant in ("missing", "runtime", "duplicate", "host"):
                with self.subTest(source=source, variant=variant):
                    targets = deepcopy(self.targets)
                    targets["engine"]["projectSources"].remove(source)
                    if variant != "missing":
                        target = {"runtime": "runtime", "duplicate": "engine", "host": "channel_host"}[variant]
                        targets[target]["projectSources"].extend([source] * (2 if variant == "duplicate" else 1))
                    self.assertEqual(closure.inspect_graph(targets)["status"], "failed")
        targets = {key: deepcopy(self.targets[key]) for key in ("sdk", "engine", "runtime")}
        self.assertEqual(closure.inspect_graph(targets)["status"], "passed")
        targets["engine"]["projectSources"].remove("src/channel/types.cpp")
        self.assertEqual(closure.inspect_graph(targets)["status"], "failed")

    def test_sdk_cannot_reach_hosts_or_any_tls_target_directly_or_transitively(self):
        for dependency in ("channel_host", "channel_runtime", "tls", "x509", "crypto"):
            for owner in ("sdk", "engine", "runtime"):
                with self.subTest(dependency=dependency, owner=owner):
                    targets = deepcopy(self.targets)
                    targets[owner]["dependencies"].append(dependency)
                    self.assertEqual(closure.inspect_graph(targets)["status"], "failed")

    def test_future_host_source_cannot_hide_in_renamed_neutral_target(self):
        for source in ("src/channel/new_transport.cpp", "src/gateway/new_service.cpp",
                       "src/runtime/headless_executor.cpp"):
            targets = deepcopy(self.targets)
            targets["engine"]["projectSources"].append(source)
            report = closure.inspect_graph(targets)
            self.assertEqual(report["status"], "failed")
            self.assertTrue(any("host-only source: " + source in v for v in report["violations"]))

    def test_host_dependency_direction_and_cli_presence_are_required(self):
        for key, dependencies in (("channel_host", []), ("channel_host", ["engine"]),
                                  ("channel_runtime", ["runtime"]), ("channel_runtime", ["channel_host"]),
                                  ("cli", ["host"])):
            with self.subTest(key=key, dependencies=dependencies):
                targets = deepcopy(self.targets)
                targets[key]["dependencies"] = dependencies
                self.assertEqual(closure.inspect_graph(targets)["status"], "failed")
        for key, dependency in (("channel_host", "runtime"), ("channel_host", "channel_runtime"),
                                ("channel_host", "cli"), ("channel_runtime", "cli"),
                                ("channel_runtime", "host"), ("channel_runtime", "channel_runtime")):
            with self.subTest(key=key, dependency=dependency):
                targets = deepcopy(self.targets)
                targets[key]["dependencies"].append(dependency)
                self.assertEqual(closure.inspect_graph(targets)["status"], "failed")
        for source in ("src/package/component.cpp", "src/updater/engine.cpp", "src/config/update_checker.cpp"):
            with self.subTest(hidden_service=source):
                targets = deepcopy(self.targets)
                targets["hidden_service"] = {"name": "innocent_static", "type": "STATIC_LIBRARY",
                                             "projectSources": [source], "dependencies": []}
                targets["channel_host"]["dependencies"].append("hidden_service")
                self.assertEqual(closure.inspect_graph(targets)["status"], "failed")


class FileApiClosureTests(unittest.TestCase):
    def setUp(self):
        self.fixture = file_api_fixtures.BoundaryTests()
        self.fixture.setUp()
        self.fixture.source_file("src/sdk/job_operations.cpp", "int private_binding;\n")
        self.fixture.targets[0]["sources"].append({"path": "src/sdk/job_operations.cpp", "compileGroupIndex": 0})
        self.addCleanup(self.fixture.doCleanups)

    def test_actual_file_api_keeps_missing_and_duplicate_job_occurrences(self):
        original = deepcopy(self.fixture.targets)
        for variant in ("missing", "duplicate"):
            with self.subTest(variant=variant):
                self.fixture.targets = deepcopy(original)
                sources = self.fixture.targets[0]["sources"]
                if variant == "missing":
                    sources[:] = [item for item in sources if item["path"] != "src/sdk/job_operations.cpp"]
                else:
                    sources.append({"path": str(self.fixture.source / "src/sdk/job_operations.cpp"), "compileGroupIndex": 0})
                self.fixture.write_model()
                report = closure.inspect(self.fixture.source, self.fixture.build, "Release")
                self.assertEqual(report["status"], "failed")
                self.assertTrue(any("Job binding" in v for v in report["violations"]))

    def test_report_reads_actual_target_references_and_source_identity(self):
        self.fixture.write_model()
        report = closure.inspect(self.fixture.source, self.fixture.build, "Release")
        self.assertEqual(report["status"], "passed")
        self.assertEqual(set(report["sdkBuildClosure"]), {"sdk", "engine"})
        self.assertIn("src/neutral/engine.cpp", report["sdkProjectSources"])
        other_source = self.fixture.source / "foreign"
        with self.assertRaisesRegex(ValueError, "another source/build tree"):
            closure.inspect(other_source, self.fixture.build, "Release")

    def test_target_reply_cannot_substitute_another_identity(self):
        self.fixture.write_model()
        reply = dict(self.fixture.targets[0])
        reply["id"] = "foreign"
        self.fixture.json_file("target-0.json", reply)
        with self.assertRaisesRegex(ValueError, "mismatched target identity"):
            closure.inspect(self.fixture.source, self.fixture.build, "Release")

    def test_sdk_only_cannot_define_disconnected_updater_or_miniz_targets(self):
        for name in ("lubancode_updater", "miniz"):
            with self.subTest(name=name):
                original = deepcopy(self.fixture.targets)
                self.fixture.targets.append({"id": "host", "name": name,
                                             "type": "STATIC_LIBRARY", "sources": []})
                report = self.fixture.check()
                self.assertEqual(report["status"], "failed")
                self.assertTrue(any("host/resource target" in v for v in report["violations"]))
                self.fixture.targets = original


if __name__ == "__main__":
    unittest.main()
