"""Negative Runner-only data gates; never configure, compile or execute native code."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "check_installed_process_host.py"
SPEC = importlib.util.spec_from_file_location("runner_process_gate", SCRIPT)
gate = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = gate
SPEC.loader.exec_module(gate)


class RunnerOnlyBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="runner-only-data-")
        self.addCleanup(self.scratch.cleanup)
        self.repo = Path(self.scratch.name).resolve() / "source"
        self.repo.mkdir()
        self.build = Path(self.scratch.name).resolve() / "build"
        self.reply = self.build / ".cmake/api/v1/reply"
        self.reply.mkdir(parents=True)
        self.flags = {"LUBANCODE_BUILD_CLI": "OFF", "LUBANCODE_BUILD_SDK": "OFF",
                      "LUBANCODE_BUILD_WORKER_HOST": "OFF", "LUBANCODE_BUILD_EXPERIMENT_RUNNER": "ON",
                      "BUILD_TESTING": "ON"}
        self.targets = [
            {"name": "luban-runner", "type": "EXECUTABLE", "artifacts": [{"path": "luban-runner"}],
             "sources": [{"path": "src/job_runner/main.cpp", "compileGroupIndex": 0}]},
            {"name": "luban_job_runner_client", "type": "STATIC_LIBRARY", "artifacts": [{"path": "libclient.a"}],
             "sources": [{"path": "src/platform/atomic_write.cpp", "compileGroupIndex": 0},
                         {"path": "src/job_runner/client.cpp", "compileGroupIndex": 0}]},
        ]
        self.evidence = self.build / "boundary.json"

    def arrange(self):
        (self.build / "CMakeCache.txt").write_text(
            "\n".join(f"{key}:BOOL={value}" for key, value in self.flags.items()), encoding="utf-8")
        references = []
        for index, target in enumerate(self.targets):
            filename = f"target-{index}.json"
            (self.reply / filename).write_text(json.dumps(target), encoding="utf-8")
            references.append({"jsonFile": filename})
        (self.reply / "model.json").write_text(json.dumps(
            {"configurations": [{"name": "Release", "targets": references}]}), encoding="utf-8")
        (self.reply / "index-1.json").write_text(json.dumps(
            {"reply": {"codemodel-v2": {"jsonFile": "model.json"}}}), encoding="utf-8")

    def check(self, testing=True):
        self.arrange()
        gate.check_runner_only_graph(self.build, self.repo, testing, self.evidence)

    def test_only_runner_sources_are_accepted_in_both_testing_modes(self):
        for testing in (False, True):
            with self.subTest(testing=testing):
                self.flags["BUILD_TESTING"] = "ON" if testing else "OFF"
                self.check(testing)
                self.assertEqual(json.loads(self.evidence.read_text(encoding="utf-8"))["status"], "passed")

    def test_each_wrong_build_flag_is_rejected(self):
        for key in self.flags:
            previous = self.flags[key]
            self.flags[key] = "OFF" if previous == "ON" else "ON"
            with self.subTest(flag=key), self.assertRaisesRegex(RuntimeError, "wrong.*flags"):
                self.check()
            self.flags[key] = previous

    def test_core_or_native_test_target_is_rejected(self):
        self.targets.append({"name": "lubancode_engine", "type": "STATIC_LIBRARY"})
        with self.assertRaisesRegex(RuntimeError, "unexpected compiled target"):
            self.check()

    def test_core_sdk_native_test_and_external_sources_are_rejected(self):
        for source in ("src/sdk/core.cpp", "src/trajectory/journal.cpp", "tests/runner/native.cpp", "../other.cpp"):
            self.targets[0]["sources"] = [{"path": source, "compileGroupIndex": 0}]
            with self.subTest(source=source), self.assertRaisesRegex(RuntimeError, "source"):
                self.check()

    def test_missing_artifact_is_rejected(self):
        self.targets[0].pop("artifacts")
        with self.assertRaisesRegex(RuntimeError, "no artifact"):
            self.check()

    def test_missing_file_api_is_rejected(self):
        self.arrange()
        (self.reply / "index-1.json").unlink()
        with self.assertRaisesRegex(RuntimeError, "File API"):
            gate.check_runner_only_graph(self.build, self.repo, True, self.evidence)

    def test_wrong_target_type_is_rejected(self):
        self.targets[0]["type"] = "STATIC_LIBRARY"
        with self.assertRaisesRegex(RuntimeError, "target type"):
            self.check()

    def test_ordinary_install_rejects_any_non_runner_file(self):
        prefix = self.build / "ordinary"
        executable = prefix / "bin" / ("luban-runner.exe" if sys.platform == "win32" else "luban-runner")
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"fixture-only; never executed")
        gate.check_runner_only_install(prefix)
        (prefix / "core-file").write_bytes(b"unrelated")
        with self.assertRaisesRegex(RuntimeError, "installs Core"):
            gate.check_runner_only_install(prefix)


if __name__ == "__main__":
    unittest.main()
