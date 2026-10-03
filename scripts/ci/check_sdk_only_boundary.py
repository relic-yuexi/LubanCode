#!/usr/bin/env python3
"""Inspect a completed SDK-only build without running CMake or native code.

Before configuring, use --prepare to request CMake File API codemodel/cache v2.
After the default ALL build, run the check with --expect-testing on or off.
This verifies the configured build graph and project include boundaries. It is
not a call-graph analyzer or a substitute for relocated consumer/runtime tests.
File API contract: https://cmake.org/cmake/help/latest/manual/cmake-file-api.7.html
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys


CLIENT = "client-lubancore-boundary"
HOST_TARGETS = {
    "lubancode", "lubancode_core", "lubancode_updater", "miniz", "lubancode_app", "lubancode_tests", "lubancore_host_tests",
    "lubancode_official_skills", "lubancode_official_docs", "lubancode_assistant_web",
}
HOST_PREFIXES = ("src/cli/", "src/app/", "src/app_server/", "src/frontend/", "src/tui/", "src/updater/")
SHARED_SDK_TEST_SOURCES = {
    "tests/unit/trajectory/test_session_recovery_view.cpp",
    "tests/unit/platform/test_atomic_write.cpp",
    "tests/unit/runtime/test_session_resources.cpp",
    "tests/unit/runtime/test_session_execution.cpp",
    "tests/unit/runtime/test_execution_owner.cpp",
    "tests/unit/runtime/test_subagent_terminal_receipt.cpp",
    "tests/unit/runtime/test_child_foreground_integration.cpp",
    "tests/unit/runtime/test_child_parent_observation.cpp",
    "tests/unit/runtime/test_child_history_adoption.cpp",
    "tests/unit/runtime/test_scoped_turn_bindings.cpp",
}
SEARCH_PROBE_TARGET = "lubancore_sdk_search_probe"
SEARCH_PROBE_SOURCE = "tests/support/sdk_search_probe.cpp"
# Real private implementations compiled into the SDK reference-test executable,
# rather than exposed as additional DLL ABI. No other SDK implementation gets
# this testing-only exception.
PRIVATE_SDK_TEST_IMPLEMENTATIONS = {"src/sdk/results.cpp", "src/sdk/approval.cpp", "src/sdk/memory.cpp"}
TERMINAL_PATH = re.compile(r"^src/platform/(?:console|clipboard|hidden_input|terminal_batch)(?:[_.]|$)")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', re.MULTILINE)
# Keep strings intact while removing comments; URL/regex literals are not comments.
CPP_TOKENS = re.compile(
    r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    r'|//[^\n]*|/\*.*?\*/', re.DOTALL)
HOST_IO = re.compile(
    r'\bstd\s*::\s*(?:cin|cout|cerr|clog|wcin|wcout|wcerr|wclog)\b'
    r'|\b(?:printf|wprintf|puts|putchar|getchar|std\s*::\s*print(?:ln)?)\s*\('
    r'|\b(?:fprintf|fwprintf)\s*\(\s*(?:stdout|stderr)\b')
GLOBAL_SETTERS = re.compile(
    r'\b(?:chdir|fchdir|SetCurrentDirectory[AW]?|SetLanguage|LoadLanguagePacksFromDir|'
    r'setenv|unsetenv|_putenv|_putenv_s|SetEnvironmentVariable[AW]?|signal|sigaction)\s*\('
    r'|\b(?:std\s*::\s*)?locale\s*::\s*global\s*\(')
# Public SDK headers intentionally expose only C++ standard-library facilities
# and other installed lubancore headers. This is a language-header allowlist,
# not a snapshot of the current SDK includes or implementation source list.
STANDARD_HEADERS = set("""
algorithm any array atomic barrier bit bitset charconv chrono codecvt compare
complex concepts condition_variable coroutine deque exception execution expected
filesystem flat_map flat_set format forward_list fstream functional future
generator initializer_list iomanip ios iosfwd iostream istream iterator latch
limits list locale map mdspan memory memory_resource mutex new numbers numeric
optional ostream print queue random ranges ratio regex scoped_allocator
semaphore set shared_mutex source_location span spanstream sstream stack stacktrace
stdexcept stop_token streambuf string string_view strstream syncstream system_error
thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility
valarray variant vector version cassert cctype cerrno cfenv cfloat cinttypes
ciso646 climits clocale cmath csetjmp csignal cstdalign cstdarg cstdbool cstddef
cstdint cstdio cstdlib cstring ctgmath ctime cuchar cwchar cwctype assert.h ctype.h
errno.h fenv.h float.h inttypes.h iso646.h limits.h locale.h math.h setjmp.h signal.h
stdalign.h stdarg.h stdbool.h stddef.h stdint.h stdio.h stdlib.h string.h tgmath.h
time.h uchar.h wchar.h wctype.h stdatomic.h
""".split())


def without_comments(text: str) -> str:
    def replace(match: re.Match[str]) -> str:
        value = match.group()
        return re.sub(r"[^\n]", " ", value) if value.startswith(("//", "/*")) else value
    return CPP_TOKENS.sub(replace, text)


def code_only(text: str) -> str:
    return CPP_TOKENS.sub(lambda match: re.sub(r"[^\n]", " ", match.group()), text)


def includes_in(text: str):
    # A raw string may contain an entire C++ fixture, including apparent
    # preprocessor lines. It is data, not a dependency of this translation unit.
    text = CPP_TOKENS.sub(lambda match: re.sub(r"[^\n]", " ", match.group())
                         if match.group().startswith('R"') else match.group(), without_comments(text))
    return INCLUDE.finditer(text)


def relative(path: Path, root: Path) -> str | None:
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        return None


def host_path(path: str) -> bool:
    return path == "src/main.cpp" or path.startswith(HOST_PREFIXES) or bool(TERMINAL_PATH.match(path))


def prepare(build: Path) -> None:
    query = build / ".cmake/api/v1/query" / CLIENT
    query.mkdir(parents=True, exist_ok=True)
    for name in ("codemodel-v2", "cache-v2"):
        (query / name).touch()


def read_reply(reply: Path, reference: dict) -> dict:
    if "error" in reference:
        raise ValueError("File API query failed: " + str(reference["error"]))
    filename = reference.get("jsonFile")
    if not isinstance(filename, str) or Path(filename).name != filename:
        raise ValueError("invalid File API reply reference")
    return json.loads((reply / filename).read_text(encoding="utf-8"))


def inspect(source: Path, build: Path, config: str, expect_testing: bool) -> dict:
    source, build = source.resolve(), build.resolve()
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"))
    if not indices:
        raise ValueError("no File API reply: prepare the query before configuring")
    index_path = indices[-1]  # CMake specifies lexicographic ordering, not mtime.
    index = json.loads(index_path.read_text(encoding="utf-8"))
    references = index.get("reply", {}).get(CLIENT, {})
    if not all(name in references for name in ("codemodel-v2", "cache-v2")):
        raise ValueError("CMake did not answer this boundary check's queries")
    model = read_reply(reply, references["codemodel-v2"])
    cache = read_reply(reply, references["cache-v2"])
    if model.get("kind") != "codemodel" or model.get("version", {}).get("major") != 2:
        raise ValueError("unsupported codemodel reply")
    if cache.get("kind") != "cache" or cache.get("version", {}).get("major") != 2:
        raise ValueError("unsupported cache reply")
    if (Path(model["paths"]["source"]).resolve() != source or
            Path(model["paths"]["build"]).resolve() != build):
        raise ValueError("File API reply belongs to a different source/build tree")
    configurations = [item for item in model["configurations"] if item["name"] == config]
    if len(configurations) != 1:
        raise ValueError(f"expected exactly one {config!r} configuration")
    entries = {item["name"]: item["value"] for item in cache["entries"]}
    violations: list[str] = []
    flags = {}
    for name, expected in (("LUBANCODE_BUILD_CLI", False), ("LUBANCODE_BUILD_SDK", True),
                           ("BUILD_TESTING", expect_testing)):
        value = str(entries.get(name, "<missing>"))
        flags[name] = value
        accepted = {"ON", "TRUE", "YES", "1"} if expected else {"OFF", "FALSE", "NO", "0"}
        if value.upper() not in accepted:
            violations.append(f"cache {name} must be {'ON' if expected else 'OFF'}, got {value}")

    targets = {}
    source_contexts: list[tuple[Path, tuple[Path, ...]]] = []

    def check_project_path(name: str, owner: str) -> None:
        if host_path(name):
            violations.append(f"target {owner} includes host source {name}")
        if name == "src/sdk/memory.cpp" and owner not in {"lubancore_sdk", "lubancore_sdk_tests"}:
            violations.append(f"unregistered private SDK reference owner: {owner} includes {name}")
        if owner == "lubancore_sdk_tests" and name.startswith("src/sdk/") and name.endswith(".cpp"):
            if not expect_testing:
                violations.append(f"testing is OFF but private SDK reference is compiled: {name}")
            elif name not in PRIVATE_SDK_TEST_IMPLEMENTATIONS:
                violations.append(f"unregistered private SDK reference implementation: {name}")
        if name.startswith("tests/"):
            if not expect_testing:
                violations.append(f"testing is OFF but target {owner} includes {name}")
            elif not ((owner == SEARCH_PROBE_TARGET and name == SEARCH_PROBE_SOURCE) or
                      (owner == "lubancore_sdk_tests" and name != SEARCH_PROBE_SOURCE and (
                          name.startswith(("tests/integration/sdk/", "tests/unit/sdk/", "tests/support/")) or
                          name in SHARED_SDK_TEST_SOURCES))):
                violations.append(f"non-SDK test compilation: target {owner} includes {name}")

    for reference in configurations[0].get("targets", []):
        target = read_reply(reply, reference)
        if target.get("id") != reference.get("id") or target.get("name") != reference.get("name"):
            raise ValueError("target reference does not match its reply")
        if target["id"] in targets:
            raise ValueError("duplicate target id in codemodel")
        groups = target.get("compileGroups", [])
        include_groups = [tuple(Path(item["path"]).resolve() for item in group.get("includes", []))
                          for group in groups]
        for group, include_dirs in zip(groups, include_groups):
            for entry in group.get("precompileHeaders", []):
                header = Path(entry["header"])
                if not header.is_absolute():
                    header = source / header
                header = header.resolve()
                name = relative(header, source)
                if name and name.startswith(("src/", "include/", "tests/")):
                    check_project_path(name, target["name"])
                    source_contexts.append((header, include_dirs))
        source_facts = []
        for entry in target.get("sources", []):
            # File API source paths are relative to the top-level source tree,
            # including GENERATED paths when the build directory is inside it.
            path = Path(entry["path"])
            path = (path if path.is_absolute() else source / path).resolve()
            name = relative(path, source)
            source_facts.append({"path": str(path), "projectPath": name,
                                 "compiled": "compileGroupIndex" in entry,
                                 "generated": entry.get("isGenerated", False)})
            if name and name.startswith(("src/", "include/", "tests/")):
                check_project_path(name, target["name"])
                group_index = entry.get("compileGroupIndex")
                include_dirs = include_groups[group_index] if group_index is not None else ()
                source_contexts.append((path, include_dirs))
        if target["name"] in HOST_TARGETS:
            violations.append(f"host/resource target is defined: {target['name']}")
        targets[target["id"]] = {
            "name": target["name"], "type": target["type"], "sources": source_facts,
            "dependencies": [entry["id"] for entry in target.get("dependencies", [])],
            "artifacts": [entry["path"] for entry in target.get("artifacts", [])],
            "includeDirectories": [[str(path) for path in paths] for paths in include_groups],
        }
        if target["name"] == SEARCH_PROBE_TARGET:
            if not expect_testing or target["type"] != "EXECUTABLE":
                violations.append("search probe requires testing ON and an executable target")
            compiled = {entry["projectPath"] for entry in source_facts if entry["compiled"]}
            if compiled != {SEARCH_PROBE_SOURCE}:
                violations.append("search probe must compile only its isolated fixture")
    for target in targets.values():
        if target["name"] == SEARCH_PROBE_TARGET:
            for dependency in target["dependencies"]:
                # Visual Studio can add CMake's regeneration utility. It is not
                # a linked SDK/runtime dependency and contains no probe code.
                linked = targets.get(dependency, {})
                if linked.get("name") != "ZERO_CHECK" or linked.get("type") != "UTILITY":
                    violations.append("search probe must not depend on a project library or host target")
    sdk = [target_id for target_id, target in targets.items() if target["name"] == "lubancore_sdk"]
    if len(sdk) != 1 or targets[sdk[0]]["type"] != "SHARED_LIBRARY":
        violations.append("expected exactly one shared lubancore_sdk target")
    sdk_closure: set[str] = set()
    pending = list(sdk)
    while pending:
        target_id = pending.pop()
        if target_id in sdk_closure:
            continue
        if target_id not in targets:
            raise ValueError(f"unknown build dependency {target_id}")
        sdk_closure.add(target_id)
        pending.extend(targets[target_id]["dependencies"])
    if any(targets[target_id]["name"] == SEARCH_PROBE_TARGET for target_id in sdk_closure):
        violations.append("SDK library depends on the private search probe")
    sdk_sources = sorted({entry["projectPath"] for target_id in sdk_closure
                          for entry in targets[target_id]["sources"]
                          if entry["projectPath"] and entry["projectPath"].startswith("src/")})
    if not sdk_sources or not any(name.startswith("src/sdk/") for name in sdk_sources):
        violations.append("SDK build closure contains no SDK implementation sources")
    if sdk:
        # Visual Studio advertises the configured PDB path even for Release
        # without /DEBUG. Symbols are optional; the DLL and import library are
        # still required, and the installed consumer must actually load/link.
        artifacts = [path for path in targets[sdk[0]]["artifacts"] if Path(path).suffix.lower() != ".pdb"]
        if not artifacts:
            violations.append("SDK target has no link/load build artifact")
        for entry in artifacts:
            artifact = Path(entry)
            artifact = artifact if artifact.is_absolute() else build / artifact
            if not artifact.is_file() or artifact.stat().st_size == 0:
                violations.append(f"SDK artifact is missing or empty: {artifact}")

    public_root = source / "include/lubancore"
    public_headers = sorted(path for path in public_root.rglob("*") if path.suffix in (".h", ".hpp"))
    if not public_headers:
        violations.append("no public SDK headers found")
    include_edges = []
    scanned: set[Path] = set()
    scanned_contexts: set[tuple[Path, tuple[Path, ...]]] = set()
    pending_files = source_contexts + [(path, ()) for path in public_headers]
    while pending_files:
        path, include_dirs = pending_files.pop()
        context = (path, include_dirs)
        if context in scanned_contexts or path.suffix not in (".h", ".hpp", ".c", ".cc", ".cpp", ".cxx", ".m", ".mm"):
            continue
        if not path.is_file():
            violations.append(f"project source/header missing: {path}")
            continue
        scanned.add(path)
        scanned_contexts.add(context)
        name = relative(path, source)
        if not name:
            continue
        text = path.read_text(encoding="utf-8-sig")
        public = relative(path, public_root) is not None
        for match in includes_in(text):
            include = match.group(1)
            resolved = next((candidate.resolve() for candidate in
                             (path.parent / include, *(directory / include for directory in include_dirs),
                              source / "src" / include, source / "include" / include,
                              source / include) if candidate.is_file()), None)
            destination = relative(resolved, source) if resolved else None
            if public and include not in STANDARD_HEADERS and not (
                    resolved and relative(resolved, public_root) is not None):
                violations.append(f"public header {name} exposes non-public include {include}")
            if destination and destination.startswith(("src/", "include/", "tests/")):
                include_edges.append({"from": name, "to": destination})
                if host_path(destination):
                    violations.append(f"reverse host include: {name} -> {destination}")
                else:
                    pending_files.append((resolved, include_dirs))
        # This narrow syntactic guard protects the SDK/assembly entry points.
        # Runtime state preservation and indirect diagnostics need real tests;
        # global cwd or signal operations inside child process code are not
        # equivalent to changing the embedding host's state.
        if name.startswith(("src/sdk/", "src/runtime/assembly/")):
            code = code_only(text)
            for rule, pattern in (("direct host stdio", HOST_IO), ("process-global setter", GLOBAL_SETTERS)):
                for match in pattern.finditer(code):
                    line = code[:match.start()].count("\n") + 1
                    violations.append(f"{rule}: {name}:{line}: {match.group().strip()}")
    return {
        "schemaVersion": 1, "githubSha": os.environ.get("GITHUB_SHA"),
        "sourceDir": str(source), "buildDir": str(build), "configuration": config,
        "expectTesting": expect_testing, "cacheFlags": flags,
        "fileApiIndex": str(index_path), "cmake": index.get("cmake", {}),
        "targets": targets, "sdkBuildClosure": sorted(sdk_closure),
        "sdkProjectSources": sdk_sources,
        "scannedProjectFiles": sorted(relative(path, source) for path in scanned),
        "publicHeaders": [relative(path, source) for path in public_headers],
        "projectIncludeEdges": include_edges,
        "status": "failed" if violations else "passed", "violations": sorted(set(violations)),
        "scope": "Configured build graph and source boundaries after ALL build; runtime and installation checks are separate.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--source-dir", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--config", default="Release")
    parser.add_argument("--expect-testing", choices=("on", "off"))
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.prepare:
        prepare(args.build_dir)
        return 0
    if args.expect_testing is None or args.report is None:
        parser.error("checking requires --expect-testing and --report")
    try:
        report = inspect(args.source_dir, args.build_dir, args.config, args.expect_testing == "on")
    except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
        report = {"schemaVersion": 1, "githubSha": os.environ.get("GITHUB_SHA"),
                  "status": "failed", "violations": [str(error)]}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for violation in report["violations"]:
        print(violation, file=sys.stderr)
    print(f"SDK-only boundary: {report['status']} ({args.report})")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
