"""Build-graph data fixtures; no CMake, compiler or native executable runs."""
from __future__ import annotations

from copy import deepcopy
import unittest

from scripts.ci import check_sdk_build_closure as closure
from scripts.ci.tests import test_sdk_only_boundary as file_api_fixtures


class OptionalHostClosureTests(unittest.TestCase):
    def setUp(self):
        self.targets = {
            "sdk": {"name": "lubancore_sdk", "type": "SHARED_LIBRARY",
                    "projectSources": ["src/sdk/core.cpp"], "dependencies": ["engine"]},
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
        self.targets["cli"] = {"name": "lubancode_core", "type": "STATIC_LIBRARY",
                               "projectSources": ["src/package/manifest.cpp"], "dependencies": ["host"]}
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "passed")
        self.targets["sdk"]["dependencies"].append("cli")
        report = closure.inspect_graph(self.targets)
        self.assertEqual(report["status"], "failed")
        self.assertTrue(any("lubancode_core" in v for v in report["violations"]))
        self.assertTrue(any("src/package/manifest.cpp" in v for v in report["violations"]))
        self.targets["sdk"]["dependencies"].remove("cli")
        self.targets["engine"]["projectSources"].append("src/package/semver.cpp")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "failed")

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
            ["src/agent/loop.cpp", "src/runtime/scoped_approval.cpp"])
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
        self.targets["engine"]["projectSources"].append("src/agent/loop.cpp")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "failed")
        self.targets["engine"]["projectSources"].append("src/runtime/scoped_approval.cpp")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "passed")
        self.targets["engine"]["projectSources"].append("src/runtime/scoped_approval.cpp")
        self.assertEqual(closure.inspect_graph(self.targets)["status"], "failed")


class FileApiClosureTests(unittest.TestCase):
    def setUp(self):
        self.fixture = file_api_fixtures.BoundaryTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

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
