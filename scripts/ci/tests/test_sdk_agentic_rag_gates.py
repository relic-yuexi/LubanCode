"""Reject incomplete or substituted native AgenticRag evidence; no native runs."""
import importlib.util
import hashlib
import tempfile
from pathlib import Path
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'check_sdk_focused.py'
SPEC = importlib.util.spec_from_file_location('sdk_agentic_rag_gate', SCRIPT)
focused = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(focused)


class AgenticRagEvidenceTests(unittest.TestCase):
    command = ['/real build/lubancore_sdk_tests', '--source-file=*test_lubancore_agentic_rag.cpp']

    def body(self, command=None):
        command = self.command if command is None else command
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in command),
            '[doctest] test cases: 6 | 6 passed | 0 failed',
            '[doctest] assertions: 100 | 100 passed | 0 failed', 'Test Passed.',
            *('[sdk-agentic-rag-path] ' + path for path in focused.AGENTIC_RAG_PATHS)))

    def test_actual_absolute_commands_and_line_endings(self):
        for exe in ('/real build/lubancore_sdk_tests', 'C:/real build/lubancore_sdk_tests.exe',
                    '/real build/lubancode_tests', 'C:/real build/lubancode_tests.exe'):
            command = [exe, self.command[1]]
            executable = exe.split('/')[-1].removesuffix('.exe')
            focused.check_agentic_rag_registration(command, executable)
            for ending in ('\n', '\r\n'):
                focused.check_agentic_rag_native(self.body(command).replace('\n', ending), command)

    def test_relative_foreign_or_filtered_registration_is_rejected(self):
        bad = (None, {}, [], tuple(self.command), self.command[:1],
            [7, self.command[1]], [self.command[0], None],
            ['lubancore_sdk_tests', self.command[1]], ['../lubancore_sdk_tests', self.command[1]],
            ['\\real build\\lubancore_sdk_tests', self.command[1]],
            ['C:real build\\lubancore_sdk_tests', self.command[1]],
            ['/real build/another_tests', self.command[1]],
            [self.command[0], '--source-file=*test_lubancore_agentic_rag_spi_extra.cpp'],
            [self.command[0], '--source-file=*test_lubancore_session.cpp'],
            self.command + ['--test-case=one'])
        for command in bad:
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_agentic_rag_registration(command)
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_agentic_rag_native(self.body(), command)

    def test_actual_command_matches_the_registered_path_and_source(self):
        for command in (['/other build/lubancore_sdk_tests', self.command[1]],
                        [self.command[0], '--source-file=*test_lubancore_session.cpp'],
                        self.command + ['--test-case=one']):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_agentic_rag_native(self.body(command), self.command)

    def test_each_source_path_finishes_once(self):
        for path in focused.AGENTIC_RAG_PATHS:
            marker = '[sdk-agentic-rag-path] ' + path
            for body in (self.body().replace(marker, ''), self.body() + '\n' + marker,
                         self.body().replace(marker, 'different-provider: ' + marker)):
                with self.subTest(path=path), self.assertRaises(RuntimeError):
                    focused.check_agentic_rag_native(body, self.command)

    def test_exact_successful_case_roster(self):
        body = self.body()
        summary = '[doctest] test cases: 6 | 6 passed | 0 failed'
        for replacement in ('', '[doctest] test cases: 0 | 0 passed | 0 failed',
            '[doctest] test cases: 5 | 5 passed | 0 failed',
            '[doctest] test cases: 7 | 7 passed | 0 failed',
            '[doctest] test cases: 6 | 5 passed | 1 failed', summary + '\n' + summary):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_agentic_rag_native(body.replace(summary, replacement), self.command)

    def test_nonzero_complete_assertions_and_completion(self):
        body = self.body()
        summary = '[doctest] assertions: 100 | 100 passed | 0 failed'
        for replacement in ('', '[doctest] assertions: 0 | 0 passed | 0 failed',
            '[doctest] assertions: 100 | 99 passed | 1 failed', summary + '\n' + summary):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_agentic_rag_native(body.replace(summary, replacement), self.command)
        for replacement in ('', 'Test Failed.', 'Test Passed.\nTest Passed.'):
            with self.subTest(replacement=replacement), self.assertRaises(RuntimeError):
                focused.check_agentic_rag_native(body.replace('Test Passed.', replacement), self.command)


class RelocatedAgenticRagEvidenceTests(unittest.TestCase):
    command = ['/relocated consumer/build/lubancore_consumer', 'agentic-rag', '/relocated consumer/build/state-agentic-rag']

    def body(self, command=None):
        return '\n'.join(('Command: ' + ' '.join('"' + value + '"' for value in (command or self.command)),
                          '[sdk-agentic-rag-consumer] complete', 'Test Passed.',
                          *('[sdk-agentic-rag-path] ' + path for path in focused.AGENTIC_RAG_PATHS)))

    def test_actual_relocated_command_and_line_endings(self):
        for executable in ('/relocated consumer/build/lubancore_consumer', 'C:/relocated consumer/build/lubancore_consumer.exe'):
            command = [executable, *self.command[1:]]
            for ending in ('\n', '\r\n'):
                focused.check_agentic_rag_consumer(self.body(command).replace('\n', ending), command)

    def test_foreign_binary_arguments_and_registration_are_rejected(self):
        for changed in (['/foreign/build/lubancore_consumer', *self.command[1:]],
                        [self.command[0], 'smoke', self.command[2]], self.command + ['extra'],
                        self.command[:2]):
            with self.subTest(command=changed), self.assertRaises(RuntimeError):
                focused.check_agentic_rag_consumer(self.body(changed), self.command)
        for invalid in (None, [], ['lubancore_consumer', *self.command[1:]],
                        ['C:build/lubancore_consumer.exe', *self.command[1:]],
                        ['/real/other_consumer', *self.command[1:]],
                        [self.command[0], self.command[1], 'relative-state'],
                        [self.command[0], self.command[1], 'C:relative-state'],
                        [self.command[0], self.command[1], None]):
            with self.subTest(command=invalid), self.assertRaises(RuntimeError):
                focused.check_agentic_rag_consumer(self.body(), invalid)

    def test_missing_duplicate_and_decorated_success_are_rejected(self):
        for marker in ('[sdk-agentic-rag-consumer] complete', 'Test Passed.',
                       *('[sdk-agentic-rag-path] ' + path for path in focused.AGENTIC_RAG_PATHS)):
            for changed in ('', marker + '\n' + marker, 'borrowed: ' + marker):
                with self.subTest(marker=marker, changed=changed), self.assertRaises(RuntimeError):
                    focused.check_agentic_rag_consumer(self.body().replace(marker, changed), self.command)




def load_gate(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


import ast
import copy
import json
import fnmatch
import contextlib
import io
from types import SimpleNamespace
from unittest.mock import patch
from scripts.ci import sdk_rag_demo as rag
from scripts.ci import check_installed_sdk as installed
from scripts.ci import sdk_lua_profile as profile


class RagSourceSealTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="rag-public-source-")
        self.addCleanup(scratch.cleanup)
        self.repo = Path(scratch.name) / "repo"
        self.source = Path(scratch.name) / "copied"
        for name, relative in rag.FILES.items():
            path = self.repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include <lubancore/core.hpp>\n#include <filesystem>\n'
                            if name == "agentic_rag.cpp" else "int main();\n", encoding="utf-8")

    def test_actual_public_example_has_all_four_owned_sources(self):
        repo = SCRIPT.parents[2]
        seal = rag.seal_sources(repo)
        self.assertEqual(set(seal["files"]), set(rag.FILES))
        for name, relative in rag.FILES.items():
            self.assertEqual(seal["files"][name]["sha256"], hashlib.sha256((repo / relative).read_bytes()).hexdigest())
        self.assertEqual(seal["files"]["agentic_rag.cpp"]["includes"].count("lubancore/core.hpp"), 1)

    def test_only_installed_public_and_standard_includes_are_allowed(self):
        path = self.repo / rag.FILES["agentic_rag.cpp"]
        good = '#include <lubancore/core.hpp>\n#include <filesystem>\n'
        path.write_text(good + '// #include "src/sdk/core.hpp"\nconst char*x=R"(\n#include "private.hpp"\n)";\n', encoding="utf-8")
        rag.seal_sources(self.repo)
        for extra in ('#include PRIVATE_HEADER\n', '#include "lubancore/core.hpp"\n',
                      '#include <nlohmann/json.hpp>\n', '#include <src/llm/backend.hpp>\n'):
            with self.subTest(extra=extra), self.assertRaises(RuntimeError):
                path.write_text(good + extra, encoding="utf-8")
                rag.seal_sources(self.repo)
        for text in ('#include <string>\n', good + '#include <lubancore/core.hpp>\n'):
            with self.subTest(text=text), self.assertRaises(RuntimeError):
                path.write_text(text, encoding="utf-8")
                rag.seal_sources(self.repo)

    def test_all_four_copied_files_and_two_include_surfaces_remain_sealed(self):
        seal = rag.seal_sources(self.repo)
        rag.copy_demo(self.repo, self.source, seal)
        for name in rag.FILES:
            path = self.source / name
            original = path.read_bytes()
            for wrong in (b"changed", None):
                with self.subTest(name=name, wrong=wrong), self.assertRaises(RuntimeError):
                    if wrong is None:
                        path.unlink()
                    else:
                        path.write_bytes(wrong)
                    rag.check_copy(self.source, seal)
                path.write_bytes(original)
        (self.source / "private.cpp").write_text("int x;", encoding="utf-8")
        with self.assertRaises(RuntimeError):
            rag.check_copy(self.source, seal)

    def test_consumer_copy_still_requires_the_actual_public_helper_bytes(self):
        seal = rag.seal_sources(self.repo)
        path = self.repo / rag.FILES["agentic_rag.cpp"]
        rag.check_copy(path.parent, seal, consumer=True)
        path.write_text(path.read_text(encoding="utf-8") + "// changed\n", encoding="utf-8")
        with self.assertRaises(RuntimeError):
            rag.check_copy(path.parent, seal, consumer=True)
        for bad in (None, {}, {"files": {}}):
            with self.subTest(bad=bad), self.assertRaises(RuntimeError):
                rag.check_copy(path.parent, bad, consumer=True)


class _RagFileApiFixture(unittest.TestCase):
    """Synthetic File API files exercise the reader; fake artifacts never run."""
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="rag-file-api-")
        self.addCleanup(temporary.cleanup)
        self.parent = Path(temporary.name).resolve()
        self.scratch = self.parent / "run"
        self.producer = self.parent / "producer"
        self.producer.mkdir()
        self.source = self.scratch / "source"
        self.build = self.scratch / "build"
        self.prefix = self.scratch / "installed"
        self.package = self.prefix / "lib/cmake/LubanCore"
        self.package.mkdir(parents=True)
        (self.prefix / "include").mkdir()
        (self.package / "LubanCoreConfig.cmake").write_text("# fake package, never configured\n", encoding="utf-8")
        self.library = self.prefix / "lib/liblubancore.so.0"
        self.library.write_bytes(b"fake library; never linked")
        self.seal = rag.seal_sources(SCRIPT.parents[2])
        rag.copy_demo(SCRIPT.parents[2], self.source, self.seal)
        self.reply = self.build / ".cmake/api/v1/reply"
        self.reply.mkdir(parents=True)
        (self.build / "CMakeCache.txt").write_text("# fake cache; never configured\n", encoding="utf-8")
        self.executable = self.build / "lubancore_rag"
        self.executable.write_bytes(b"fake executable; never executed")
        self.target = {"id": "rag::@one", "name": "lubancore_rag", "type": "EXECUTABLE",
                       "sources": [{"path": name, "compileGroupIndex": 0} for name in sorted(rag.COMPILED)],
                       "compileGroups": [{"language": "CXX", "includes": [{"path": str(self.prefix / "include")}]}],
                       "link": {"commandFragments": [{"role": "libraries", "fragment": '"' + str(self.library) + '"'}]},
                       "artifacts": [{"path": self.executable.name}]}
        self.targets = [self.target]
        self.cache = {"kind": "cache", "version": {"major": 2}, "entries": [
            {"name": "LubanCore_DIR", "value": str(self.package)},
            {"name": "CMAKE_HOME_DIRECTORY", "value": str(self.source)}]}
        self.model = {"kind": "codemodel", "version": {"major": 2},
                      "paths": {"source": str(self.source), "build": str(self.build)},
                      "configurations": [{"name": "Release", "targets": []}]}
        self.index = {"reply": {rag.CLIENT: {"codemodel-v2": {"jsonFile": "model.json"},
                                          "cache-v2": {"jsonFile": "cache.json"}}}}

    def write_model(self):
        references = []
        for number, target in enumerate(self.targets):
            name = "target-" + str(number) + ".json"
            (self.reply / name).write_text(json.dumps(target), encoding="utf-8")
            references.append({"id": target["id"], "name": target["name"], "jsonFile": name})
        self.model["configurations"][0]["targets"] = references
        for name, value in (("model.json", self.model), ("cache.json", self.cache), ("index-001.json", self.index)):
            (self.reply / name).write_text(json.dumps(value), encoding="utf-8")

    def check(self):
        self.write_model()
        return rag.inspect_demo(self.scratch, self.source, self.build, self.prefix, self.producer, self.seal)

    def reject(self):
        with self.assertRaises((RuntimeError, ValueError, OSError)):
            self.check()


class RagActualFileApiTests(_RagFileApiFixture):
    def test_actual_graph_copied_sources_link_and_binary_are_recorded(self):
        report = self.check()
        self.assertEqual(report["status"], "built")
        self.assertEqual(set(report["compiled_sources"]), rag.COMPILED)
        self.assertEqual(report["core_libraries"], [str(self.library)])
        self.assertEqual(report["executable"], str(self.executable))
        self.assertEqual(set(report["copied_files"]), set(rag.FILES))
        rag.check_demo_context(report, self.scratch, self.prefix)
        evidence = self.parent / "evidence"
        evidence.mkdir()
        rag.preserve_demo(evidence, self.build, self.source, report)
        self.assertTrue((evidence / "agentic-rag-demo/file-api/index-001.json").is_file())
        self.assertEqual(set(path.name for path in (evidence / "agentic-rag-demo/source").iterdir()), set(rag.FILES))

    def test_each_cpp_has_exactly_one_executable_owner(self):
        original = copy.deepcopy(self.target)
        for sources in ([], original["sources"][:1], original["sources"] * 2,
                        [*original["sources"], {"path": "foreign.cpp", "compileGroupIndex": 0}]):
            with self.subTest(sources=sources):
                self.target["sources"] = sources
                self.reject()
        self.target.update(copy.deepcopy(original))
        for kind in ("STATIC_LIBRARY", "SHARED_LIBRARY", "UTILITY"):
            with self.subTest(kind=kind):
                self.target["type"] = kind
                self.reject()
        self.target.update(copy.deepcopy(original))
        foreign = copy.deepcopy(original)
        foreign.update(id="foreign", name="foreign", type="STATIC_LIBRARY")
        self.targets.append(foreign)
        self.reject()

    def test_language_group_and_source_path_cannot_substitute_another_copy(self):
        original = copy.deepcopy(self.target)
        for group in (None, -1, 1, True):
            with self.subTest(group=group):
                self.target["sources"][0]["compileGroupIndex"] = group
                self.reject()
        self.target.update(copy.deepcopy(original))
        for language in ("C", None):
            with self.subTest(language=language):
                self.target["compileGroups"][0]["language"] = language
                self.reject()
        self.target.update(copy.deepcopy(original))
        self.target["sources"][0]["path"] = str(self.producer / "main.cpp")
        self.reject()

    def test_real_package_and_library_must_belong_to_this_relocated_prefix(self):
        package = self.cache["entries"][0]
        original = package["value"]
        for value in ("LubanCore", str(self.producer), str(self.scratch / "missing")):
            with self.subTest(value=value):
                package["value"] = value
                self.reject()
        package["value"] = original
        libraries = self.target["link"]["commandFragments"]
        for fragment in ("-llubancore", '"' + str(self.producer / "liblubancore.so") + '"',
                         '"' + str(self.library) + '" "' + str(self.library) + '"', ""):
            with self.subTest(fragment=fragment):
                libraries[0]["fragment"] = fragment
                self.reject()
        libraries[0]["fragment"] = '"' + str(self.library) + '"'
        self.library.unlink()
        self.reject()

    def test_quoted_actual_link_fragments_accept_msvc_and_macos_library_names(self):
        for filename in ("lubancore.lib", "liblubancore.0.dylib", "liblubancore.so.0.1"):
            path = self.prefix / "lib/library path" / filename
            path.parent.mkdir(exist_ok=True)
            path.write_bytes(b"fake library; never linked")
            self.target["link"]["commandFragments"][0]["fragment"] = '"' + str(path) + '"'
            with self.subTest(filename=filename):
                self.assertEqual(self.check()["core_libraries"], [str(path)])

    def test_installed_public_include_is_required_and_producer_include_is_rejected(self):
        group = self.target["compileGroups"][0]
        for includes in ([], [{"path": str(self.producer / "include")}],
                         [{"path": str(self.prefix / "include")}, {"path": str(self.producer / "src")} ]):
            with self.subTest(includes=includes):
                group["includes"] = includes
                self.reject()

    def test_reply_and_cache_must_describe_this_actual_independent_configuration(self):
        original = copy.deepcopy(self.model)
        for key in ("source", "build"):
            self.model["paths"][key] = str(self.producer)
            self.reject()
            self.model = copy.deepcopy(original)
        self.model["configurations"][0]["name"] = "Debug"
        self.reject()
        self.model = copy.deepcopy(original)
        self.cache["entries"].append(copy.deepcopy(self.cache["entries"][0]))
        self.reject()
        self.cache["entries"].pop()
        self.cache["entries"][1]["value"] = str(self.producer)
        self.reject()
        self.cache["entries"][1]["value"] = str(self.source)
        self.index["reply"][rag.CLIENT]["cache-v2"]["jsonFile"] = "../cache.json"
        self.reject()

    def test_missing_empty_duplicate_or_foreign_binary_cannot_satisfy_the_demo(self):
        for artifacts in ([], self.target["artifacts"] * 2, [{"path": str(self.producer / "lubancore_rag")}],
                          [{"path": "other_program"}]):
            with self.subTest(artifacts=artifacts):
                self.target["artifacts"] = artifacts
                self.reject()
        self.target["artifacts"] = [{"path": self.executable.name}]
        self.executable.write_bytes(b"")
        self.reject()
        self.executable.unlink()
        self.reject()

    def test_no_actual_reply_cannot_be_replaced_by_a_context(self):
        with self.assertRaises(RuntimeError):
            rag.inspect_demo(self.scratch, self.source, self.build, self.prefix, self.producer, self.seal)
        for context in (None, {}, {"status": "running"}, {"status": "passed"}):
            with self.subTest(context=context), self.assertRaises(RuntimeError):
                rag.check_demo_context(context, self.scratch, self.prefix)
        self.check()
        with self.assertRaises(RuntimeError):
            rag.inspect_demo(self.scratch, self.source, self.build, self.prefix, self.producer, self.seal,
                             producer_build=self.scratch)

    def test_sealed_copy_or_artifact_cannot_change_after_the_build(self):
        context = self.check()
        self.executable.write_bytes(b"different artifact")
        with self.assertRaises(RuntimeError):
            rag.check_demo_context(context, self.scratch, self.prefix)
        self.source.joinpath("README.md").write_text("changed instructions", encoding="utf-8")
        self.reject()

    def test_package_prefix_and_compiled_source_changes_are_visible_after_the_build(self):
        before = self.check()
        self.library.write_bytes(b"different same-prefix library")
        self.assertNotEqual(self.check(), before)
        with self.assertRaises(RuntimeError):
            rag.check_demo_context(before, self.scratch, self.prefix)
        before = self.check()
        (self.package / "LubanCoreConfig.cmake").write_text("# changed same-prefix config\n", encoding="utf-8")
        self.assertNotEqual(self.check(), before)
        with self.assertRaises(RuntimeError):
            rag.check_demo_context(before, self.scratch, self.prefix)
        other_package = self.prefix / "lib/alternate/LubanCore"
        other_package.mkdir(parents=True)
        (other_package / "LubanCoreConfig.cmake").write_text("# alternate fake config\n", encoding="utf-8")
        self.cache["entries"][0]["value"] = str(other_package)
        self.assertNotEqual(self.check(), before)
        self.source.joinpath("agentic_rag.cpp").write_text("// different source after build\n", encoding="utf-8")
        self.reject()

    def test_failed_configure_still_preserves_sources_and_the_failed_context(self):
        evidence = self.parent / "failed-evidence"
        evidence.mkdir()
        context = {"status": "failed", "configure_argv": ["cmake", "-S", str(self.source)], "returncode": 17}
        rag.preserve_demo(evidence, self.build, self.source, context)
        saved = json.loads((evidence / "agentic-rag-demo/context.json").read_text(encoding="utf-8"))
        self.assertEqual(saved, context)
        self.assertEqual(set(path.name for path in (evidence / "agentic-rag-demo/source").iterdir()), set(rag.FILES))
        with self.assertRaises(RuntimeError):
            rag.check_demo_context(saved, self.scratch, self.prefix)


class RagIndependentExecutedEvidenceTests(_RagFileApiFixture):
    def setup_evidence(self):
        context = self.check()
        self.consumer_build = self.scratch / "consumer-build"
        self.consumer_build.mkdir(exist_ok=True)
        binary = self.consumer_build / "lubancore_consumer"
        binary.write_bytes(b"fixture binary; never executed")
        consumer = [str(binary), "agentic-rag", str(self.consumer_build / "state-rag")]
        demo = [context["executable"], "--fixture", str(self.consumer_build / "state-demo")]
        listing = {"tests": [{"name": "sdk.consumer.agentic_rag", "command": consumer},
                             {"name": "sdk.consumer.agentic_rag_demo", "command": demo}]}
        sections = ["", listing["tests"][0]["name"], self.body(consumer), listing["tests"][1]["name"], self.body(demo)]
        return context, listing, sections

    @staticmethod
    def body(command):
        return "\n".join(("Command: " + " ".join('"' + value + '"' for value in command),
                          "[sdk-agentic-rag-consumer] complete", "Test Passed.",
                          *("[sdk-agentic-rag-path] " + path for path in focused.AGENTIC_RAG_PATHS)))

    def check_evidence(self, context, listing, sections):
        installed.check_rag_consumer_evidence(listing, sections, context, self.scratch, self.prefix, self.consumer_build)

    def test_both_complete_real_commands_pair_with_this_built_context(self):
        context, listing, sections = self.setup_evidence()
        self.check_evidence(context, listing, sections)
        self.check_evidence(context, listing, [entry.replace("\n", "\r\n") for entry in sections])

    def test_installed_acceptance_refuses_missing_or_foreign_demo_context(self):
        context, listing, sections = self.setup_evidence()
        for value in (None, {}, {**context, "executable": str(self.producer / "lubancore_rag")},
                      {**context, "installed_prefix": str(self.producer)},
                      {**context, "file_api_index": str(self.consumer_build / "index.json")}):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                self.check_evidence(value, listing, sections)

    def test_missing_duplicate_or_disabled_registration_or_log_is_rejected(self):
        context, listing, sections = self.setup_evidence()
        for number in (0, 1):
            bad = copy.deepcopy(listing)
            bad["tests"].pop(number)
            with self.assertRaises(RuntimeError):
                self.check_evidence(context, bad, sections)
            bad = copy.deepcopy(listing)
            bad["tests"].append(copy.deepcopy(bad["tests"][number]))
            with self.assertRaises(RuntimeError):
                self.check_evidence(context, bad, sections)
            bad = copy.deepcopy(listing)
            bad["tests"][number]["properties"] = [{"name": "DISABLED", "value": True}]
            with self.assertRaises(RuntimeError):
                self.check_evidence(context, bad, sections)
            with self.assertRaises(RuntimeError):
                self.check_evidence(context, listing, sections + sections[number * 2 + 1:number * 2 + 3])

    def test_demo_full_argv_cannot_borrow_producer_path_mode_or_state(self):
        context, listing, sections = self.setup_evidence()
        original = listing["tests"][1]["command"]
        for command in ([str(self.producer / "lubancore_rag"), *original[1:]],
                        [original[0], "--live", original[2]], original + ["extra"], original[:2],
                        [original[0], original[1], "relative-state"],
                        [original[0], original[1], str(self.producer / "state")],
                        [original[0], original[1], str(self.source / "state")],
                        [original[0], original[1], str(self.prefix / "state")]):
            with self.subTest(command=command), self.assertRaises(RuntimeError):
                focused.check_agentic_rag_demo(self.body(command), command, context, self.scratch, self.prefix)
        with self.assertRaises(RuntimeError):
            focused.check_agentic_rag_demo(self.body([original[0], "--live", original[2]]), original,
                                          context, self.scratch, self.prefix)

    def test_each_demo_path_and_completion_must_finish_once(self):
        context, listing, sections = self.setup_evidence()
        command = listing["tests"][1]["command"]
        for marker in ("[sdk-agentic-rag-consumer] complete", "Test Passed.",
                       *("[sdk-agentic-rag-path] " + name for name in focused.AGENTIC_RAG_PATHS)):
            for replacement in ("", marker + "\n" + marker, "borrowed: " + marker):
                with self.subTest(marker=marker, replacement=replacement), self.assertRaises(RuntimeError):
                    focused.check_agentic_rag_demo(self.body(command).replace(marker, replacement), command,
                                                  context, self.scratch, self.prefix)


class RagActualGraphOwnershipTests(unittest.TestCase):
    def setUp(self):
        fixture = load_gate("rag_boundary_fixture", SCRIPT.parent / "tests/test_sdk_only_boundary.py")
        self.fixture = fixture.BoundaryTests("test_clean_graph_and_recursive_headers_are_recorded")
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.flags["BUILD_TESTING"] = "ON"
        self.fixture.source_file("examples/sdk-consumer/agentic_rag.cpp", '#include <lubancore/core.hpp>\n')
        self.target = {"id": "rag", "name": "lubancore_sdk_tests", "type": "EXECUTABLE",
                       "sources": [{"path": "examples/sdk-consumer/agentic_rag.cpp", "compileGroupIndex": 0}],
                       "compileGroups": [{}]}
        self.fixture.targets.append(self.target)

    def test_helper_and_its_recursive_public_header_are_actually_scanned(self):
        report = self.fixture.check(testing=True)
        self.assertEqual(report["status"], "passed", report["violations"])
        self.assertIn("examples/sdk-consumer/agentic_rag.cpp", report["scannedProjectFiles"])
        self.assertIn({"from": "examples/sdk-consumer/agentic_rag.cpp", "to": "include/lubancore/core.hpp"},
                      report["projectIncludeEdges"])

    def test_missing_duplicate_wrong_type_or_owner_and_testing_off_are_rejected(self):
        original = copy.deepcopy(self.target)
        for change in ({"sources": []}, {"sources": original["sources"] * 2}, {"name": "foreign"},
                       {"type": "STATIC_LIBRARY"}):
            self.target.update(copy.deepcopy(original))
            self.target.update(change)
            report = self.fixture.check(testing=True)
            self.assertEqual(report["status"], "failed", report)
        self.target.update(original)
        self.fixture.flags["BUILD_TESTING"] = "OFF"
        self.fixture.assert_rejected(self.fixture.check(), "not a selected testing-only source")

    def test_nearby_helper_cannot_supply_the_exact_owner_or_hide_host_headers(self):
        self.fixture.source_file("examples/sdk-consumer/agentic_rag_extra.cpp", "int fixture;\n")
        self.target["sources"][0]["path"] = "examples/sdk-consumer/agentic_rag_extra.cpp"
        self.fixture.assert_rejected(self.fixture.check(testing=True), "exactly the selected")
        self.target["sources"][0]["path"] = "examples/sdk-consumer/agentic_rag.cpp"
        self.fixture.source_file("src/app/turn_runner.hpp", "#pragma once\n")
        self.fixture.source_file("examples/sdk-consumer/agentic_rag.cpp", '#include "app/turn_runner.hpp"\n')
        self.fixture.assert_rejected(self.fixture.check(testing=True), "reverse host include")


class RagProfilesAndRemoteRoutesTests(unittest.TestCase):
    def test_exact_on_off_rosters_keep_all_old_members(self):
        for enabled, count, consumer in ((True, 68, 37), (False, 66, 34)):
            native = profile.focused_roster(focused.REQUIRED, enabled)
            installed_names = profile.consumer_roster(installed.REQUIRED_TESTS, enabled)
            self.assertEqual(len(native), count)
            self.assertEqual(len(installed_names), consumer)
            self.assertIn("sdk.focused.lubancore_agentic_rag", native)
            self.assertTrue({"sdk.consumer.agentic_rag", "sdk.consumer.agentic_rag_demo"} <= installed_names)
            self.assertIn("sdk.focused.lubancore_todo_write", native)

    def test_both_real_classifiers_route_four_demo_files_and_exact_python_gate(self):
        classifier = load_gate("rag_ci_paths", SCRIPT.parent / "tests/test_job_current_integration_paths.py")
        text = classifier.WORKFLOW.read_text(encoding="utf-8")
        for patterns, body in classifier.sdk_classification_branches(text):
            for path in (*rag.FILES.values(), "tests/integration/sdk/test_lubancore_agentic_rag.cpp",
                         "scripts/ci/sdk_rag_demo.py", "scripts/ci/tests/test_sdk_agentic_rag_gates.py"):
                with self.subTest(path=path):
                    self.assertTrue(any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns))
            self.assertIn("sdk_tests=$sdk_present", body)
            self.assertIn("cross_platform=true", body)
            for exact in ("scripts/ci/sdk_rag_demo.py", "scripts/ci/tests/test_sdk_agentic_rag_gates.py"):
                self.assertEqual(patterns.count(exact), 1)
                self.assertFalse(any(fnmatch.fnmatchcase(exact.removesuffix(".py") + "_extra.py", pattern) for pattern in patterns))
        self.assertEqual(text.count('"$PY" -m unittest discover -s scripts/ci/tests -p test_sdk_agentic_rag_gates.py'), 1)

    def test_real_driver_builds_a_separate_project_before_consumer_and_reads_it_twice(self):
        # AST checks complement the exercised File API reader. They do not claim
        # a native build succeeded; those commands execute only on CI runners.
        source = (SCRIPT.parent / "check_installed_sdk.py").read_text(encoding="utf-8")
        tree = ast.parse(source)
        calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call) and
                 isinstance(node.func, ast.Attribute) and isinstance(node.func.value, ast.Name) and
                 node.func.value.id == "rag"]
        names = [node.func.attr for node in calls]
        self.assertEqual(names.count("prepare"), 1)
        self.assertEqual(names.count("copy_demo"), 1)
        self.assertEqual(names.count("inspect_demo"), 2)
        self.assertEqual(names.count("preserve_demo"), 3)
        self.assertIn('run_demo_command(demo_configure, env,', source)
        self.assertIn('run_demo_command(demo_all_build, env,', source)
        self.assertLess(source.index('run_demo_command(demo_all_build, env,'), source.index('str(consumer_source), "-B", str(consumer_build)'))
        self.assertIn('check_rag_consumer_evidence(listing, sections, demo_context', source)
        self.assertIn('ET.parse(evidence / "consumer-results.xml")', source)
        self.assertIn('after_demo != demo_context', source)


class RagRemoteCommandReceiptsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="rag-command-receipt-")
        self.addCleanup(temporary.cleanup)
        self.receipt = Path(temporary.name) / "receipt.json"
        self.argv = ["cmake", "--build", "explicit-independent-build", "--config", "Release", "--parallel", "4"]

    def test_actual_failure_code_argv_and_output_are_kept_without_running_cmake(self):
        with contextlib.redirect_stdout(io.StringIO()), patch.object(installed.subprocess, "run", return_value=SimpleNamespace(returncode=17, stdout=b"original failure\n")) as invoked:
            with self.assertRaises(RuntimeError):
                installed.run_demo_command(self.argv, {"PATH": "fixture-only"}, self.receipt)
        invoked.assert_called_once()
        self.assertEqual(invoked.call_args.args[0], self.argv)
        report = json.loads(self.receipt.read_text(encoding="utf-8"))
        self.assertEqual(report["argv"], self.argv)
        self.assertEqual(report["returncode"], 17)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(self.receipt.with_suffix(".log").read_bytes(), b"original failure\n")

    def test_non_utf8_build_output_retains_original_bytes_and_exit_code(self):
        output = b"MSVC failure: \xff\x80\r\n"
        with contextlib.redirect_stdout(io.StringIO()), patch.object(installed.subprocess, "run", return_value=SimpleNamespace(returncode=23, stdout=output)):
            with self.assertRaises(RuntimeError):
                installed.run_demo_command(self.argv, {}, self.receipt)
        report = json.loads(self.receipt.read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["returncode"], 23)
        self.assertEqual(report["output_encoding"], "raw_bytes")
        self.assertEqual(self.receipt.with_suffix(".log").read_bytes(), output)

    def test_launch_failure_remains_unknown_instead_of_success(self):
        with contextlib.redirect_stdout(io.StringIO()), patch.object(installed.subprocess, "run", side_effect=OSError("fixture launch failed")):
            with self.assertRaises(OSError):
                installed.run_demo_command(self.argv, {}, self.receipt)
        report = json.loads(self.receipt.read_text(encoding="utf-8"))
        self.assertEqual(report["status"], "failed")
        self.assertIsNone(report["returncode"])
        self.assertEqual(report["error"], "fixture launch failed")


if __name__ == "__main__":
    unittest.main()


