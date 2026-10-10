"""Counterexamples for original NamedResults logs and compiled ownership; data only."""
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from scripts.ci import sdk_named_results as gate
from scripts.ci import check_sdk_focused as focused
from scripts.ci import check_installed_sdk as installed
from scripts.ci.sdk_lua_profile import focused_roster, consumer_roster

REPO = Path(__file__).resolve().parents[3]


def summary():
    return dict(session_id="actual-scene", tool_call_id="actual-tool", summary_event_id="actual-summary",
                source_revision=3, model_calls=2, raw_results=1, formal_results=1, local_named_mirrors=0,
                summary_candidates=1, selected=True, subsequent_request_verified=True)


def command(stem="lubancore_named_results", executable="lubancore_sdk_tests", windows=False):
    root = "C:/actual/" if windows else "/actual/"
    return [root + executable + (".exe" if windows else ""), "--source-file=*test_" + stem + ".cpp"]


def section(stem="lubancore_named_results", argv=None, value=None):
    argv = argv or command(stem)
    count, prefix, paths = gate.SOURCES[stem]
    lines = ["Command: " + " ".join('"' + arg + '"' for arg in argv)]
    lines.extend(prefix + path for path in paths)
    if stem == "lubancore_named_results":
        lines.append("[sdk-named-results-summary] " + json.dumps(summary() if value is None else value))
    lines.extend([f"[doctest] test cases: {count} | {count} passed | 0 failed | 0 skipped",
                  "[doctest] assertions: 300 | 300 passed | 0 failed", "Test Passed."])
    return "\n".join(lines) + "\n"


class NativeEvidenceTests(unittest.TestCase):
    def test_both_real_executable_names_and_platform_paths(self):
        for windows in (False, True):
            for executable in ("lubancore_sdk_tests", "lubancode_tests"):
                for stem in gate.SOURCES:
                    argv = command(stem, executable, windows)
                    report = gate.check_native(section(stem, argv), argv, stem)
                    self.assertEqual(report["nativeCases"], gate.SOURCES[stem][0])
                    self.assertEqual(report["summary"] is not None, stem == "lubancore_named_results")

    def test_exact_source_and_original_command_are_required(self):
        original = command()
        for argv in (None, [], [None, original[1]], [original[0], None], [original[0]],
                     [*original, "--test-case=one"], ["lubancore_sdk_tests", original[1]],
                     [original[0], "--source-file=*named*"],
                     [original[0], "--source-file=*test_lubancore_named_result_guards.cpp"]):
            with self.subTest(argv=argv), self.assertRaises(RuntimeError):
                gate.check_registration(argv, "lubancore_named_results")
        for text in (section().replace(original[0], "/foreign/lubancore_sdk_tests"),
                     section() + "Command: other\n", section().replace("Test Passed.", "Test Failed."),
                     section() + "SKIPPED: prerequisite missing\n", section() + "Test Passed.\n"):
            with self.assertRaises(RuntimeError):
                gate.check_native(text, original, "lubancore_named_results")

    def test_case_assertion_and_path_roster_cannot_be_reduced_or_borrowed(self):
        for stem, (count, prefix, paths) in gate.SOURCES.items():
            text = section(stem)
            for changed in (text.replace(f"{count} passed", f"{count - 1} passed"),
                            text.replace("300 | 300 passed", "0 | 0 passed"),
                            text.replace(prefix + paths[0] + "\n", ""),
                            text + prefix + paths[0] + "\n", text + prefix + "foreign\n",
                            text + "[doctest] assertions: 300 | 300 passed | 0 failed\n"):
                with self.subTest(stem=stem), self.assertRaises(RuntimeError):
                    gate.check_native(changed, command(stem), stem)
        private = "lubancore_named_result_guards"
        with self.assertRaises(RuntimeError):
            gate.check_native(section(private) + "[sdk-named-results-summary] " + json.dumps(summary()),
                              command(private), private)

    def test_summary_must_reference_adopted_external_materials_and_subsequent_request(self):
        mutations = [("session_id", ""), ("tool_call_id", "x" * 201), ("summary_event_id", "bad\nidentity"),
                     ("source_revision", True), ("source_revision", 0), ("model_calls", 0),
                     ("summary_candidates", "1"), ("raw_results", 0), ("formal_results", 2),
                     ("local_named_mirrors", 1), ("raw_results", True), ("selected", False),
                     ("subsequent_request_verified", False), ("selected", 1)]
        for key, value in mutations:
            changed = summary(); changed[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(RuntimeError):
                gate.check_native(section(value=changed), command(), "lubancore_named_results")
        raw = section()
        for changed in (raw.replace('"model_calls": 2', '"model_calls": NaN'),
                        raw.replace('"selected": true', '"selected": true, "selected": true'),
                        raw + "[sdk-named-results-summary] " + json.dumps(summary()) + "\n",
                        "\n".join(line for line in raw.splitlines() if not line.startswith("[sdk-named-results-summary]"))):
            with self.assertRaises(RuntimeError):
                gate.check_native(changed, command(), "lubancore_named_results")

    def test_installed_helper_uses_same_real_probe_and_all_ten_paths(self):
        argv = ["/second/build/lubancore_consumer", "named-results", "/second/build/state-named-results", "/second/probe"]
        probe = {"copy": {"path": argv[3]}}
        text = "\n".join(["Command: " + " ".join('"' + arg + '"' for arg in argv),
                           *("[sdk-named-results-path] " + path for path in gate.PUBLIC_PATHS),
                           "[sdk-named-results-consumer] complete", "installed SDK consumer named-results passed",
                           "Test Passed."])
        self.assertEqual(len(gate.check_consumer(text, argv, probe)["paths"]), 10)
        for changed in (text.replace("[sdk-named-results-path] close-read", "missing"),
                        text + "[sdk-named-results-path] jobs\n",
                        text + "[sdk-named-results-summary] " + json.dumps(summary()),
                        text.replace("[sdk-named-results-consumer] complete", "missing")):
            with self.assertRaises(RuntimeError):
                gate.check_consumer(changed, argv, probe)
        for changed in ([*argv, "extra"], argv[:-1], [*argv[:3], "/producer/probe"]):
            with self.assertRaises(RuntimeError):
                gate.check_consumer(text, changed, probe)


class OwnershipTests(unittest.TestCase):
    def graph(self, testing=True, cli=True, file_api=False):
        graph = {"engine": {"name": "lubancode_engine", "type": "STATIC_LIBRARY", "luaSources": list(gate.ENGINE_IMPLEMENTATIONS)},
                 "sdk": {"name": "lubancore_sdk", "type": "SHARED_LIBRARY", "luaSources": [gate.ADAPTER]}}
        if testing:
            graph["reference"] = {"name": "lubancore_sdk_tests", "type": "EXECUTABLE", "luaSources": [gate.ADAPTER, gate.HELPER]}
            if cli:
                graph["aggregate"] = {"name": "lubancode_tests", "type": "EXECUTABLE", "luaSources": [gate.ADAPTER, gate.HELPER]}
        if file_api:
            for target in graph.values():
                target["sources"] = [{"projectPath": path, "compiled": True} for path in target.pop("luaSources")]
        return graph

    def test_on_off_combined_and_sdk_only_own_actual_compiled_occurrences(self):
        for testing in (True, False):
            for cli in (True, False):
                for file_api in (True, False):
                    self.assertEqual(gate.ownership_violations(self.graph(testing, cli, file_api), testing, cli), [])
        original = self.graph()
        for source in (gate.ADAPTER, gate.HELPER, *gate.ENGINE_IMPLEMENTATIONS):
            for mutation in ("missing", "duplicate", "rogue", "declaration-only"):
                graph = deepcopy(original)
                owner = next(target for target in graph.values() if source in target["luaSources"])
                if mutation == "missing": owner["luaSources"].remove(source)
                elif mutation == "duplicate": owner["luaSources"].append(source)
                elif mutation == "rogue": graph["rogue"] = {"name": "unselected", "type": "STATIC_LIBRARY", "luaSources": [source]}
                else:
                    owner["sources"] = [{"projectPath": path, "compiled": path != source} for path in owner.pop("luaSources")]
                with self.subTest(source=source, mutation=mutation):
                    self.assertTrue(gate.ownership_violations(graph, True, True))

    def test_actual_sdk_stl_helper_and_installed_header_bytes_are_sealed(self):
        records = gate.seal_sources(REPO)
        self.assertEqual(set(records), {gate.HEADER, gate.HELPER, gate.ADAPTER, *gate.ENGINE_IMPLEMENTATIONS})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); consumer = root / "consumer"; consumer.mkdir(); prefix = root / "prefix"
            header = prefix / gate.HEADER; header.parent.mkdir(parents=True)
            helper = consumer / "named_results.cpp"
            helper.write_bytes((REPO / gate.HELPER).read_bytes()); header.write_bytes((REPO / gate.HEADER).read_bytes())
            copied = gate.check_copies(consumer, prefix, records, REPO, root / "evidence")
            for source, record in copied.items():
                self.assertEqual(record["sha256"], records[source]["sha256"])
                path = root / "evidence" / ("consumer" if source == gate.HELPER else "installed") / Path(source).name
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), record["sha256"])
            for path in (helper, header):
                original = path.read_bytes(); path.write_bytes(original + b"\ncorrupted copy\n")
                with self.assertRaises(RuntimeError): gate.check_copies(consumer, prefix, records, REPO)
                path.write_bytes(original)
            changed = deepcopy(records); changed[gate.HEADER]["sha256"] = "0" * 64
            with self.assertRaises(RuntimeError): gate.check_copies(consumer, prefix, changed, REPO)

    def test_current_source_rosters_preserve_every_old_member(self):
        self.assertEqual((len(focused.REQUIRED), len(focused_roster(focused.REQUIRED, False))), (73, 71))
        self.assertIn('sdk.focused.lubancore_model_sampling', focused_roster(focused.REQUIRED, False))
        self.assertEqual((len(installed.REQUIRED_TESTS), len(consumer_roster(installed.REQUIRED_TESTS, False))), (37, 34))
        self.assertIn(gate.HEADER, installed.REQUIRED_PUBLIC_HEADERS)
        for stem in gate.SOURCES: self.assertIn("sdk.focused." + stem, focused.REQUIRED)
        for script in ("check_sdk_focused.py", "check_installed_sdk.py", "check_sdk_only_boundary.py", "check_sdk_build_closure.py"):
            self.assertIn("sdk_named_results", (REPO / "scripts/ci" / script).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
