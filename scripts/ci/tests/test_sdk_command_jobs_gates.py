"""Pure JSON/log/source counterexamples. No native executable or CMake runs."""
import ast
from copy import deepcopy
import fnmatch
import json
import os
from pathlib import Path
import re
import tempfile
import unittest

from scripts.ci import sdk_command_jobs as gate
from scripts.ci import extract_command_jobs_full as full
from scripts.ci import check_sdk_focused as focused
from scripts.ci import check_installed_sdk as installed
from scripts.ci import check_asan_profile as asan
from scripts.ci.sdk_lua_profile import focused_roster, consumer_roster

REPO = Path(__file__).resolve().parents[3]


def command(stem="lubancore_command_jobs", executable="lubancore_sdk_tests", root="/actual"):
    return [root + "/" + executable, "--source-file=*test_" + stem + ".cpp"]


def facts(platform="posix"):
    return [dict(sync=sync, row_kind=kind, row_seq=40 + sync, journal_status="Unconfirmed",
                 native_stage="FileSync", platform="windows" if platform == "nt" else "posix",
                 native_succeeded=True, native_return=1 if platform == "nt" else 0,
                 injected_unconfirmed=True, first_line_count=39 + sync, retained_line_count=39 + sync,
                 command_completed=sync == 14, running_after_close=0)
            for sync, kind in gate.FAULT_KINDS.items()]


def section(stem="lubancore_command_jobs", argv=None, platform="posix", values=None):
    argv = argv or command(stem)
    count, prefix, paths = gate.SOURCES[stem]
    lines = ["Command: " + " ".join('"' + arg + '"' for arg in argv)]
    lines += [prefix + path for path in paths]
    if stem.endswith("_guards"):
        lines += ["[sdk-command-job-native-fault] " + json.dumps(value)
                  for value in (facts(platform) if values is None else values)]
    lines += [f"[doctest] test cases: {count} | {count} passed | 0 failed | 0 skipped",
              "[doctest] assertions: 200 | 200 passed | 0 failed", "Test Passed."]
    return "\n".join(lines) + "\n"


class ReceiptTests(unittest.TestCase):
    def test_actual_platforms_and_both_targets(self):
        for platform in ("nt", "posix"):
            for executable in ("lubancore_sdk_tests", "lubancode_tests"):
                for stem in gate.SOURCES:
                    argv = command(stem, executable)
                    result = gate.check_native(section(stem, argv, platform), argv, stem, platform)
                    self.assertEqual(result["nativeCases"], gate.SOURCES[stem][0])

    def test_filters_relative_paths_aliases_and_extra_args_rejected(self):
        original = command()
        for argv in ([original[0]], [*original, "--test-case=one"],
                     ["lubancore_sdk_tests", original[1]], ["/fake/other", original[1]],
                     [original[0], "--source-file=*jobs*"], [original[0], "--source-file=*test_lubancore_command_job_guards.cpp"]):
            with self.subTest(argv=argv), self.assertRaises(RuntimeError):
                gate.check_registration(argv, "lubancore_command_jobs")

    def test_native_path_and_receipt_omissions(self):
        argv = command()
        original = section()
        for text in (original.replace(argv[0], "/foreign/lubancore_sdk_tests"),
                     original.replace("10 passed", "9 passed"),
                     original.replace("200 | 200 passed", "0 | 0 passed"),
                     original.replace("[sdk-command-jobs-path] close\n", ""),
                     original + "[sdk-command-jobs-path] close\n",
                     original + "[sdk-command-jobs-path] foreign\n",
                     original.replace("Test Passed.", "Test Failed."),
                     original + "SKIPPED: fixture unavailable\n",
                     original + "Command: other\n"):
            with self.subTest(text=text[-80:]), self.assertRaises(RuntimeError):
                gate.check_native(text, argv, "lubancore_command_jobs", "posix")

    def test_native_unknown_facts_are_typed_scoped_and_not_weakened(self):
        stem = "lubancore_command_job_guards"; argv = command(stem)
        mutations = [
            ("sync", True), ("sync", 2), ("row_kind", "tool.execution.started"),
            ("row_seq", 0), ("row_seq", True), ("first_line_count", "40"),
            ("retained_line_count", 999), ("native_succeeded", 1),
            ("injected_unconfirmed", False), ("native_return", True),
            ("native_return", 1), ("platform", "windows"), ("command_completed", True),
            ("running_after_close", 1), ("running_after_close", False),
            ("journal_status", "Committed"), ("native_stage", "Flush")]
        for key, value in mutations:
            changed = facts(); changed[0][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(RuntimeError):
                gate.check_native(section(stem, argv, values=changed), argv, stem, "posix")
        for changed in (facts()[:-1], facts() + [facts()[0]], [facts()[0]] * 4):
            with self.assertRaises(RuntimeError):
                gate.check_native(section(stem, argv, values=changed), argv, stem, "posix")
        bad = section(stem, argv).replace('"sync": 1,', '"sync": 1, "sync": 1,', 1)
        with self.assertRaises(RuntimeError):
            gate.check_native(bad, argv, stem, "posix")
        bad = facts("nt"); bad[0]["native_return"] = 0
        with self.assertRaises(RuntimeError):
            gate.check_native(section(stem, argv, "nt", bad), argv, stem, "nt")

    def test_hold_path_cannot_be_replaced_by_public_success(self):
        stem = "lubancore_command_job_guards"; argv = command(stem)
        text = section(stem, argv).replace("[sdk-command-job-guards-path] passive-hold",
                                          "[sdk-command-job-guards-path] actual-public-source")
        with self.assertRaises(RuntimeError):
            gate.check_native(text, argv, stem, "posix")

    def test_consumer_uses_exact_relocated_probe_and_all_paths(self):
        argv = ["/scratch/build/lubancore_consumer", "command-jobs", "/scratch/build/state", "/scratch/fixture/probe"]
        context = {"copy": {"path": argv[3]}}
        text = "\n".join(["Command: " + " ".join('"' + arg + '"' for arg in argv),
                          *("[sdk-command-jobs-path] " + path for path in gate.PUBLIC_PATHS),
                          "[sdk-command-jobs-consumer] complete", "installed SDK consumer command-jobs passed",
                          "Test Passed."])
        self.assertEqual(len(gate.check_consumer(text, argv, context)["paths"]), 10)
        for bad in ([*argv, "extra"], argv[:-1], [*argv[:3], "/producer/probe"]):
            with self.assertRaises(RuntimeError):
                gate.check_consumer(text, bad, context)
        with self.assertRaises(RuntimeError):
            gate.check_consumer(text.replace("[sdk-command-jobs-path] approvals", "missing"), argv, context)


class ClosureTests(unittest.TestCase):
    def graph(self, testing=True, cli=True):
        graph = {"sdk": {"name": "lubancore_sdk", "type": "SHARED_LIBRARY", "luaSources": list(gate.IMPLEMENTATIONS)}}
        if testing:
            for name in ("lubancore_sdk_tests", "lubancode_tests") if cli else ("lubancore_sdk_tests",):
                graph[name] = {"name": name, "type": "EXECUTABLE", "luaSources": [*gate.IMPLEMENTATIONS, gate.HELPER]}
        return graph

    def test_testing_modes_and_wrong_owners(self):
        for testing in (True, False):
            for cli in (True, False):
                self.assertEqual(gate.ownership_violations(self.graph(testing, cli), testing, cli), [])
        for source in (*gate.IMPLEMENTATIONS, gate.HELPER):
            graph = self.graph(); graph["sdk"]["luaSources"].append(source)
            self.assertTrue(gate.ownership_violations(graph, True, True))
            graph = self.graph(); graph["lubancode_tests"]["luaSources"].remove(source)
            self.assertTrue(gate.ownership_violations(graph, True, True))
        graph = self.graph(); graph["rogue"] = {"name": "lubancode_runtime", "type": "STATIC_LIBRARY", "luaSources": [gate.IMPLEMENTATIONS[1]]}
        self.assertTrue(gate.ownership_violations(graph, True, True))

    def test_public_source_seal_and_corrupt_copy(self):
        seal = gate.seal_sources(REPO)
        self.assertEqual(set(seal), {gate.HELPER, gate.HEADER})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / "source"; prefix = root / "prefix"; source.mkdir()
            header = prefix / gate.HEADER; header.parent.mkdir(parents=True)
            (source / "command_jobs.cpp").write_bytes((REPO / gate.HELPER).read_bytes())
            header.write_bytes((REPO / gate.HEADER).read_bytes())
            probe = root / "probe"; probe.write_bytes(b"pure fixture, never executable")
            context = {"copy": {"path": str(probe), "bytes": probe.stat().st_size, "sha256": gate.digest(probe)},
                       "original": {"sha256": gate.digest(probe)}}
            evidence = root / "evidence"
            copied = gate.check_copies(source, prefix, seal, context, evidence=evidence)
            self.assertEqual(set(copied), {gate.HELPER, gate.HEADER})
            for relative in copied:
                self.assertEqual((evidence / relative).read_bytes(), (REPO / relative).read_bytes())
                self.assertEqual(copied[relative]["sha256"], seal[relative]["sha256"])
            probe.write_bytes(b"other")
            with self.assertRaises(RuntimeError):
                gate.check_copies(source, prefix, seal, context)

    def test_real_current_rosters_and_all_old_members(self):
        self.assertEqual((len(focused.REQUIRED), len(focused_roster(focused.REQUIRED, False))), (70, 68))
        self.assertEqual((len(installed.REQUIRED_TESTS), len(consumer_roster(installed.REQUIRED_TESTS, False))), (37, 34))
        for stem in gate.SOURCES:
            self.assertIn("sdk.focused." + stem, focused.REQUIRED)
        manifest = asan.make_manifest(REPO)
        for stem in gate.SOURCES:
            self.assertIn("integration.sdk." + stem, manifest["selected"][0])
            self.assertIn("integration.sdk." + stem, manifest["selected"][1])
            self.assertIn("tests/integration/sdk/test_" + stem + ".cpp", manifest["compile_sources"])
        for path in (*gate.IMPLEMENTATIONS, gate.HELPER):
            self.assertIn(path, manifest["attachments"])
        # The real registration selector and its inline exact-set check must agree.
        workflow = (REPO / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        block = re.search(r"^  linux-asan:\n(.*?)(?=^  [A-Za-z][\w-]*:|\Z)", workflow, re.M | re.S).group(1)
        literal = [value for value in re.findall(r"required = (\{[^}]+\})", block)
                   if isinstance(ast.parse(value, mode="eval").body, ast.Set)]
        self.assertEqual(len(literal), 1)
        self.assertEqual(ast.literal_eval(literal[0]), set(manifest["selected"][0]))
        for path in ("scripts/ci/sdk_command_jobs.py", "scripts/ci/extract_command_jobs_full.py",
                     "scripts/ci/tests/test_sdk_command_jobs_gates.py"):
            self.assertEqual(sum(path in line for line in workflow.splitlines() if line.strip().endswith(")")), 2)

    def test_actual_classifiers_cover_owned_dispatch_dependencies(self):
        workflow = (REPO / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        patterns = re.findall(r"^\s*([^\n]*scripts/ci/sdk_command_jobs\.py[^\n]*)\)\s*$", workflow, re.M)
        self.assertEqual(len(patterns), 2)
        dependencies = ("src/agent/async_tool_seam.hpp", "src/agent/loop.cpp",
            "src/runtime/async_tool_runtime.cpp", "src/runtime/async_tool_runtime.hpp",
            "src/runtime/trajectory_turn_bridge.cpp", "src/runtime/trajectory_turn_bridge.hpp",
            *gate.IMPLEMENTATIONS, "src/sdk/command_jobs.hpp", "src/sdk/command_jobs_opening.hpp",
            gate.HEADER, gate.PROBE_PROJECT,
            "src/tools/tool_job_coordinator.cpp", "src/tools/tool_job_coordinator.hpp")
        for pattern in patterns:
            for source in dependencies:
                self.assertTrue(any(fnmatch.fnmatchcase(source, part) for part in pattern.strip().split('|')), source)
            self.assertFalse(any(fnmatch.fnmatchcase("docs/readme.md", part) for part in pattern.strip().split('|')))

    def test_private_fixture_is_fresh_and_has_no_product_graph_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            receipt = gate.prepare_probe_fixture(REPO, root / "scratch", root / "evidence")
            self.assertEqual(set(receipt["sourceCopies"]), {gate.PROBE_PROJECT, gate.PROBE_SOURCE})
            source, build = Path(receipt["source"]), Path(receipt["build"])
            self.assertFalse(source.is_relative_to(REPO))
            self.assertFalse(build.is_relative_to(REPO))
            self.assertEqual(receipt["configure"], ["cmake", "-S", str(source), "-B", str(build),
                "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SUPPRESS_REGENERATION=ON"])
            self.assertEqual(receipt["buildCommand"], ["cmake", "--build", str(build), "--config", "Release",
                "--target", gate.PROBE_TARGET, "--parallel", "4"])
            self.assertTrue((build / ".cmake/api/v1/query/client-lubancore-boundary/codemodel-v2").is_file())
            for original, copy in receipt["sourceCopies"].items():
                self.assertEqual((source / copy["projectPath"]).read_bytes(), (REPO / original).read_bytes())
                self.assertEqual((root / "evidence" / copy["projectPath"]).read_bytes(), (REPO / original).read_bytes())
            with self.assertRaises(RuntimeError):
                gate.prepare_probe_fixture(REPO, root / "scratch", root / "second-evidence")

    def test_probe_artifact_selection_accepts_only_native_runtime_and_exact_pdb(self):
        runtime = "Release/" + gate.PROBE_TARGET + ".exe"
        pdb = "Release/" + gate.PROBE_TARGET + ".pdb"
        target = {"artifacts": [{"path": runtime}, {"path": pdb}]}
        # Actual MSVC shape, including a different order, on every test host.
        for paths in ([runtime, pdb], [pdb, runtime], [runtime]):
            self.assertEqual(gate.select_probe_artifact({"artifacts": [{"path": path} for path in paths]}, "windows"),
                             {"runtime": runtime, "auxiliary": [pdb] if pdb in paths else []})
        posix = "tests/" + gate.PROBE_TARGET
        self.assertEqual(gate.select_probe_artifact({"artifacts": [{"path": posix}]}, "posix"),
                         {"runtime": posix, "auxiliary": []})
        bad_shapes = ([pdb], [], [runtime, runtime], [runtime, runtime.upper()],
                      [runtime, "Debug/" + gate.PROBE_TARGET + ".exe"],
                      [runtime, "Debug/" + gate.PROBE_TARGET + ".pdb"],
                      [runtime, "Release/other.pdb"], [runtime, pdb, pdb],
                      [runtime, "Release/unknown.lib"], [runtime, "Release/../Release/" + gate.PROBE_TARGET + ".exe"],
                      [runtime, "\0"], [posix, pdb])
        for paths in bad_shapes:
            with self.subTest(paths=paths), self.assertRaises(RuntimeError):
                gate.select_probe_artifact({"artifacts": [{"path": path} for path in paths]}, "windows")
        for platform in ("posix", "foreign", None):
            with self.subTest(platform=platform), self.assertRaises(RuntimeError):
                gate.select_probe_artifact(target, platform)
        with self.assertRaises(RuntimeError):
            gate.select_probe_artifact({"artifacts": [{"path": posix}, {"path": posix + ".pdb"}]}, "posix")

    def test_probe_copy_comes_from_real_target_artifact_shape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve(); repo = root / "source"; build = root / "build"; scratch = root / "scratch"
            (repo / gate.PROBE_SOURCE).parent.mkdir(parents=True)
            (repo / gate.PROBE_SOURCE).write_text("// pure source fixture, never compiled")
            binary = build / "tests" / (gate.PROBE_TARGET + (".exe" if os.name == "nt" else ""))
            binary.parent.mkdir(parents=True); binary.write_bytes(b"pure binary fixture, never executed"); binary.chmod(0o755)
            reply = build / ".cmake/api/v1/reply"; reply.mkdir(parents=True)
            reference = {"id": "probe-id", "name": gate.PROBE_TARGET, "jsonFile": "target.json"}
            target = {**reference, "type": "EXECUTABLE", "dependencies": [],
                      "sources": [{"path": gate.PROBE_SOURCE, "compileGroupIndex": 0}],
                      "artifacts": [{"path": "tests/" + binary.name}]}
            if os.name == "nt":
                target["artifacts"].append({"path": "tests/" + gate.PROBE_TARGET + ".pdb"})
            other = {"id": "other-id", "name": "other-owner", "jsonFile": "other.json"}
            model = {"kind": "codemodel", "version": {"major": 2}, "paths": {"source": str(repo), "build": str(build)},
                     "configurations": [{"name": "Release", "targets": [reference, other],
                                         "directories": [{"jsonFile": "directory.json"}]}]}
            (reply / "index-1.json").write_text(json.dumps({"reply": {"client-lubancore-boundary": {"codemodel-v2": {"jsonFile": "model.json"}}}}))
            (reply / "model.json").write_text(json.dumps(model))
            (reply / "target.json").write_text(json.dumps(target))
            (reply / "other.json").write_text(json.dumps({**other, "type": "UTILITY", "sources": []}))
            (reply / "directory.json").write_bytes(b'{ "kind": "directory", "unknown": "retain me" }\n')
            raw = root / "raw-evidence"
            copied = gate.copy_probe(repo, build, scratch, evidence=raw)
            self.assertEqual(copied["copy"]["sha256"], copied["original"]["sha256"])
            self.assertTrue(Path(copied["copy"]["path"]).is_relative_to(scratch.resolve()))
            self.assertEqual(copied["fileApiRawAcceptance"]["rawFiles"], 5)
            for item in copied["fileApiRaw"]["files"]:
                self.assertEqual((raw / item["name"]).read_bytes(), (reply / item["name"]).read_bytes())
            # Verify uploaded originals in an unrelated directory, with the
            # producer paths retained only as logical evidence identities.
            import shutil
            relocated = root / "relocated-raw"
            shutil.copytree(raw, relocated)
            self.assertEqual(gate.check_probe_file_api(relocated, copied)["status"], "passed")
            for key, value in (("nativePlatform", "posix" if os.name == "nt" else "windows"),
                               ("artifactSelection", {"runtime": "invented", "auxiliary": []})):
                changed = deepcopy(copied); changed[key] = value
                with self.subTest(context=key), self.assertRaises(RuntimeError):
                    gate.check_probe_file_api(relocated, changed)
            for mutation in ("missing", "tampered", "unindexed", "duplicate-receipt"):
                changed = deepcopy(copied)
                directory = root / mutation
                shutil.copytree(raw, directory)
                if mutation == "missing": (directory / "other.json").unlink()
                elif mutation == "tampered": (directory / "target.json").write_bytes(b'{}')
                elif mutation == "unindexed": (directory / "foreign.json").write_bytes(b'{}')
                else: changed["fileApiRaw"]["files"].append(changed["fileApiRaw"]["files"][0])
                with self.subTest(mutation=mutation), self.assertRaises(RuntimeError):
                    gate.check_probe_file_api(directory, changed)
            # A matching receipt hash cannot make a foreign compiled source,
            # native artifact or owner identity acceptable.
            for key, value in (("sources", [{"path": "tests/support/other.cpp", "compileGroupIndex": 0}]),
                               ("artifacts", [{"path": "tests/other-probe"}]), ("id", "foreign-id")):
                changed = deepcopy(copied)
                directory = root / ("rehashed-" + key)
                shutil.copytree(raw, directory)
                bad_target = deepcopy(target); bad_target[key] = value
                path = directory / "target.json"
                path.write_text(json.dumps(bad_target))
                sha = gate.digest(path)
                changed["fileApiTarget"]["sha256"] = sha
                member = next(item for item in changed["fileApiRaw"]["files"] if item["name"] == "target.json")
                member.update(bytes=path.stat().st_size, sha256=sha)
                with self.subTest(rehashed=key), self.assertRaises(RuntimeError):
                    gate.check_probe_file_api(directory, changed)
            for key, value in (("dependencies", [{"id": "sdk"}]), ("type", "SHARED_LIBRARY"),
                               ("sources", [{"path": "tests/support/other.cpp", "compileGroupIndex": 0}]),
                               ("artifacts", [{"path": "tests/" + gate.PROBE_TARGET}] * 2)):
                bad = deepcopy(target); bad[key] = value
                (reply / "target.json").write_text(json.dumps(bad))
                with self.subTest(key=key), self.assertRaises(RuntimeError):
                    gate.copy_probe(repo, build, scratch / key)
            # A failed eligibility check must leave the actual original graph;
            # preserving it never converts the rejection into acceptance.
            (reply / "target.json").write_text(json.dumps(target))
            binary.unlink()
            observed = gate.preserve_probe_reply(build, root / "observed-missing-artifact")
            self.assertEqual(observed["acceptance"], "not_evaluated")
            self.assertEqual(observed["status"], "copied")
            with self.assertRaisesRegex(RuntimeError, "artifact unavailable"):
                gate.copy_probe(repo, build, scratch / "missing-artifact", evidence=root / "rejected-raw")
            self.assertFalse((root / "rejected-raw").exists())
            for item in observed["files"]:
                saved = root / "observed-missing-artifact" / item["name"]
                self.assertEqual(saved.read_bytes(), (reply / item["name"]).read_bytes())
                self.assertEqual(gate.digest(saved), item["sha256"])
            # Malformed originals remain bytes, not a manufactured graph.
            (reply / "target.json").write_bytes(b'{ broken original')
            malformed = gate.preserve_probe_reply(build, root / "observed-malformed")
            self.assertEqual(malformed["status"], "copied")
            self.assertEqual((root / "observed-malformed/target.json").read_bytes(), b'{ broken original')
            with self.assertRaises(RuntimeError):
                gate.preserve_probe_reply(build, root / "observed-malformed")
            absent = gate.preserve_probe_reply(root / "unconfigured", root / "observed-empty")
            self.assertEqual(absent["status"], "no_reply_json")
            self.assertEqual(absent["files"], [])


class FullRunTests(unittest.TestCase):
    def fixture(self, build):
        tests, native, cases = [], [], []
        for index, (name, (stem, executable)) in enumerate(full.REQUIRED.items(), 1):
            argv = command(stem, executable, str(build).replace("\\", "/"))
            tests.append({"name": name, "command": argv, "properties": [{"name": "TIMEOUT", "value": 300}]})
            native.append(f"{index}/4 Testing: {name}\n" + section(stem, argv))
            cases.append(f'<testcase name="{name}" status="run"/>')
        return json.dumps({"tests": tests}), "".join(native), "<testsuite>" + "".join(cases) + "</testsuite>"

    def test_actual_full_quartet_and_failure_originals_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory); (build / "Testing/Temporary").mkdir(parents=True)
            registration, native, junit = self.fixture(build)
            (build / "command-jobs-full-registration.json").write_text(registration)
            (build / "Testing/Temporary/LastTest.log").write_text(native)
            (build / "result-store-full-results.xml").write_text(junit)
            self.assertEqual(full.extract(build, "posix")["status"], "passed")
            failed = junit.replace('status="run"', 'status="notrun"', 1)
            (build / "result-store-full-results.xml").write_text(failed)
            with self.assertRaises(RuntimeError):
                full.extract(build, "posix")
            out = build / "test-evidence/command-jobs-full"
            self.assertEqual((out / "results.xml").read_text(), failed)
            self.assertEqual(json.loads((out / "context.json").read_text())["status"], "failed")

    def test_full_registration_no_swapped_source_or_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            raw, _, _ = self.fixture(Path(directory))
            original = json.loads(raw)
            for change in ("duplicate", "disabled", "timeout", "foreign"):
                value = deepcopy(original)
                if change == "duplicate": value["tests"][1] = deepcopy(value["tests"][0])
                if change == "disabled": value["tests"][0]["properties"].append({"name": "DISABLED", "value": True})
                if change == "timeout": value["tests"][0]["properties"][0]["value"] = 301
                if change == "foreign": value["tests"][0]["command"][1] = "--source-file=*test_other.cpp"
                with self.subTest(change=change), self.assertRaises(RuntimeError):
                    full.check_registration(json.dumps(value), directory)


if __name__ == "__main__":
    unittest.main()
