"""Command Jobs: strict real-command receipts and relocated private probe.

All functions read/copy data only. This module never configures, builds, starts
the probe or runs a test. Native execution belongs to the existing CI callers.
"""
from __future__ import annotations

import hashlib
import json
import ntpath
import os
from pathlib import Path
import posixpath
import re
import shlex
import shutil

PUBLIC_PATHS = ("admission", "completion", "queue", "blocked-parent", "failed-parent",
                "isolation", "close", "approvals", "parent-cancel", "step-limit")
GUARD_PATHS = ("approval-retirement", "bridge-provenance", "native-faults",
               "cancel-confirm", "writer-cancel", "actual-public-source", "passive-hold")
SOURCES = {"lubancore_command_jobs": (10, "[sdk-command-jobs-path] ", PUBLIC_PATHS),
           "lubancore_command_job_guards": (7, "[sdk-command-job-guards-path] ", GUARD_PATHS)}
IMPLEMENTATIONS = ("src/sdk/adapters.cpp", "src/sdk/command_jobs.cpp", "src/sdk/command_jobs_opening.cpp")
HELPER = "examples/sdk-consumer/command_jobs.cpp"
HEADER = "include/lubancore/jobs.hpp"
PROBE_TARGET = "lubancore_command_limits_probe"
PROBE_SOURCE = "tests/support/command_limits_probe.cpp"
PROBE_PROJECT = "scripts/ci/fixtures/command-jobs-probe/CMakeLists.txt"
FAULT_KINDS = {1: "tool.job.registered", 3: "sdk.job.operation.bound",
               6: "tool.result.persisted", 14: "hook.completed"}
FAULT_KEYS = {"sync", "row_kind", "row_seq", "journal_status", "native_stage", "platform",
              "native_succeeded", "native_return", "injected_unconfirmed", "first_line_count",
              "retained_line_count", "command_completed", "running_after_close"}


def require(ok, message):
    if not ok:
        raise RuntimeError("Command Jobs: " + message)


def absolute(value):
    return isinstance(value, str) and bool(value) and not any(ch in value for ch in "\0\r\n") and (
        value.startswith("/") or (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0])))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate field: " + key)
        result[key] = value
    return result


def actual_command(section, command):
    require(isinstance(section, str), "native section must be text")
    lines = re.findall(r"^Command: ([^\r\n]*)\r?$", section, re.M)
    try:
        actual = shlex.split(lines[0].replace("\\", "/")) if len(lines) == 1 else None
    except ValueError as error:
        raise RuntimeError("Command Jobs: malformed actual argv") from error
    require(actual == [arg.replace("\\", "/") for arg in command], "actual argv differs from registration")
    require(section.splitlines().count("Test Passed.") == 1, "native completion absent or duplicated")
    require(not re.search(r"^Test (?:Failed|Timeout|Not Run|Skipped)\b|\bSKIP(?:PED)?\s*:|skipped test case",
                          section, re.M | re.I), "native source failed or skipped")


def check_registration(command, stem, executable="lubancore_sdk_tests"):
    require(stem in SOURCES, "unknown native source")
    require(isinstance(command, list) and len(command) == 2 and absolute(command[0]) and
            command[0].replace("\\", "/").split("/")[-1] in (executable, executable + ".exe") and
            command[1] == "--source-file=*test_" + stem + ".cpp", "requires its absolute single-source argv")


def check_paths(section, prefix, expected):
    paths = [line.removeprefix(prefix) for line in section.splitlines() if line.startswith(prefix)]
    require(sorted(paths) == sorted(expected), "missing, duplicate or foreign actual paths")
    return paths


def check_native(section, command, stem, platform_name):
    require(platform_name in ("nt", "posix"), "unknown native platform")
    require(isinstance(command, list) and command and isinstance(command[0], str), "malformed native argv")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    require(executable in ("lubancore_sdk_tests", "lubancode_tests"), "foreign native executable")
    check_registration(command, stem, executable)
    actual_command(section, command)
    count, prefix, paths = SOURCES[stem]
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    require(cases == [(str(count), str(count), "0")], "native CASE roster did not pass completely")
    require(len(assertions) == 1 and int(assertions[0][0]) > 0 and
            assertions[0][0] == assertions[0][1] and assertions[0][2] == "0", "native assertions empty or failed")
    observed = check_paths(section, prefix, paths)
    facts = []
    raw = [line.removeprefix("[sdk-command-job-native-fault] ") for line in section.splitlines()
           if line.startswith("[sdk-command-job-native-fault] ")]
    if stem == "lubancore_command_jobs":
        require(not raw, "public host emitted private fault receipts")
    else:
        try:
            facts = [json.loads(line, object_pairs_hook=unique_object,
                               parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value))) for line in raw]
        except (ValueError, TypeError) as error:
            raise RuntimeError("Command Jobs: invalid typed native fault JSON") from error
        require(len(facts) == 4, "four actual native fault boundaries are required")
        seen = set()
        for fact in facts:
            require(isinstance(fact, dict) and set(fact) == FAULT_KEYS, "native fault field contract differs")
            sync = fact["sync"]
            require(type(sync) is int and sync in FAULT_KINDS and sync not in seen, "fault boundary is foreign or duplicated")
            seen.add(sync)
            require(fact["row_kind"] == FAULT_KINDS[sync] and fact["journal_status"] == "Unconfirmed" and
                    fact["native_stage"] == "FileSync", "fault did not observe its real row/native stage")
            require(fact["platform"] == ("windows" if platform_name == "nt" else "posix"), "native platform was substituted")
            require(fact["native_succeeded"] is True and fact["injected_unconfirmed"] is True,
                    "native success and injected uncertainty must both be explicit true")
            value = fact["native_return"]
            require(type(value) is int and (value != 0 if platform_name == "nt" else value == 0),
                    "actual native return does not match this platform")
            for key in ("row_seq", "first_line_count", "retained_line_count"):
                require(type(fact[key]) is int and fact[key] > 0, "native ordinal must be a positive integer")
            require(fact["first_line_count"] == fact["retained_line_count"], "first uncertainty was replaced")
            require(type(fact["command_completed"]) is bool and fact["command_completed"] == (sync == 14),
                    "pre-dispatch fault ran a command or Post fault lacks a real completion")
            require(type(fact["running_after_close"]) is int and fact["running_after_close"] == 0,
                    "actual workers did not retire")
    return {"source": stem, "nativeCases": count, "nativeAssertions": int(assertions[0][0]),
            "command": command, "paths": observed, "nativeFaults": facts,
            "holdEvidence": "passed passive-hold source assertions; no separate typed owner receipt"
                            if stem.endswith("_guards") else None}


def check_consumer(section, command, context):
    require(isinstance(command, list) and len(command) == 4 and all(isinstance(arg, str) for arg in command) and
            absolute(command[0]) and absolute(command[2]) and absolute(command[3]) and
            command[0].replace("\\", "/").split("/")[-1] in ("lubancore_consumer", "lubancore_consumer.exe") and
            command[1] == "command-jobs", "installed caller must name consumer/state/real probe explicitly")
    actual_command(section, command)
    require(command[3].replace("\\", "/") == context["copy"]["path"].replace("\\", "/"),
            "consumer borrowed a different command probe")
    paths = check_paths(section, "[sdk-command-jobs-path] ", PUBLIC_PATHS)
    require(section.splitlines().count("[sdk-command-jobs-consumer] complete") == 1 and
            section.splitlines().count("installed SDK consumer command-jobs passed") == 1,
            "installed consumer did not finish all real paths")
    return {"command": command, "paths": paths,
            "probeEvidence": "explicit sealed relocated probe; native helper assertions verify entry/cwd/retirement"}


def ownership_violations(targets, testing, with_cli=False):
    """Accept actual compiled-source lists from either existing File API reader."""
    owners = {}
    for target in targets.values():
        compiled = target.get("luaSources")
        if compiled is None:
            compiled = [item["projectPath"] for item in target.get("sources", []) if item.get("compiled") and item.get("projectPath")]
        for source in (*IMPLEMENTATIONS, HELPER):
            owners.setdefault(source, []).extend([(target["name"], target["type"])] * compiled.count(source))
    references = [("lubancore_sdk_tests", "EXECUTABLE")] if testing else []
    if testing and with_cli:
        references.append(("lubancode_tests", "EXECUTABLE"))
    errors = []
    for source, actual in owners.items():
        expected = references + ([] if source == HELPER else [("lubancore_sdk", "SHARED_LIBRARY")])
        if sorted(actual) != sorted(expected):
            errors.append("Command Jobs compiled owner closure differs for " + source)
    return errors


def seal_sources(repo):
    try:
        from .check_sdk_only_boundary import CPP_TOKENS, STANDARD_HEADERS, includes_in, without_comments
    except ImportError:
        from check_sdk_only_boundary import CPP_TOKENS, STANDARD_HEADERS, includes_in, without_comments
    result = {}
    for relative, expected_public in ((HELPER, {"lubancore/core.hpp", "lubancore/jobs.hpp"}),
                                      (HEADER, {"lubancore/api.hpp"})):
        path = Path(repo) / relative
        require(path.is_file() and not path.is_symlink(), "missing/linked public source " + relative)
        text = path.read_text(encoding="utf-8")
        includes = list(includes_in(text))
        directives = CPP_TOKENS.sub(lambda match: re.sub(r"[^\n]", " ", match.group())
                                    if match.group().startswith('R"') else match.group(), without_comments(text))
        require(len(re.findall(r"^\s*#\s*include\b", directives, re.M)) == len(includes), "dynamic include in " + relative)
        names = [match.group(1) for match in includes]
        require(set(name for name in names if name.startswith("lubancore/")) == expected_public and
                all(name in STANDARD_HEADERS or name in expected_public for name in names) and
                all(re.match(r'#\s*include\s*<', match.group().lstrip()) for match in includes),
                "private/nonstandard public include in " + relative)
        result[relative] = {"sha256": digest(path), "includes": names}
    return result


def prepare_probe_fixture(repo, scratch, evidence):
    """Copy a standalone fixture and declare remote commands; never run them."""
    try:
        from .check_sdk_only_boundary import CLIENT
    except ImportError:
        from check_sdk_only_boundary import CLIENT
    repo, scratch, evidence = Path(repo).resolve(), Path(scratch).resolve(), Path(evidence).resolve()
    require(not scratch.is_relative_to(repo), "private probe must leave the product source tree")
    source, build = scratch / "command-jobs-probe-source", scratch / "command-jobs-probe-build"
    require(not source.exists() and not build.exists() and not evidence.exists(),
            "private probe fixture destination already exists")
    source.mkdir(parents=True)
    copies = {}
    for original, relative in ((PROBE_PROJECT, "CMakeLists.txt"), (PROBE_SOURCE, PROBE_SOURCE)):
        path = repo / original
        require(path.is_file() and not path.is_symlink(), "private probe fixture source missing or linked")
        destination = source / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, destination)
        saved = evidence / relative
        saved.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(destination, saved)
        require(digest(path) == digest(destination) == digest(saved), "private probe fixture source copy differs")
        copies[original] = {"projectPath": relative, "bytes": destination.stat().st_size, "sha256": digest(destination)}
    query = build / ".cmake/api/v1/query" / CLIENT
    query.mkdir(parents=True)
    (query / "codemodel-v2").write_text("", encoding="utf-8")
    return {"schemaVersion": 1, "originalSource": str(repo), "source": str(source), "build": str(build),
            "sourceCopies": copies,
            "configure": ["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
                          "-DCMAKE_SUPPRESS_REGENERATION=ON"],
            "buildCommand": ["cmake", "--build", str(build), "--config", "Release",
                             "--target", PROBE_TARGET, "--parallel", "4"]}


def preserve_probe_file_api(reply, index_path, index, model_ref, model, target_ref, target, output):
    """Keep original JSON bytes, including every codemodel target/directory.

    These files allow independent source ownership checks after the runner is
    gone. The producer's live CMake objects remain the authority; no JSON graph
    is manufactured from the acceptance receipt.
    """
    reply, output = Path(reply), Path(output)
    require(not output.exists(), "probe raw File API destination already exists")
    names = {index_path.name, model_ref["jsonFile"], target_ref["jsonFile"]}
    for configuration in model["configurations"]:
        for entry in (*configuration.get("targets", []), *configuration.get("directories", [])):
            if "jsonFile" in entry:
                names.add(entry["jsonFile"])
    require(len(names) <= 4096 and all(isinstance(name, str) and Path(name).name == name and
            name.endswith(".json") and not any(c in name for c in "\0/\\") for name in names),
            "probe File API graph references invalid or excessive files")
    # Read bounded originals before creating the evidence directory. Retain
    # formatting, unknown fields and native artifact paths byte for byte.
    originals = {}
    total = 0
    for name in sorted(names):
        path = reply / name
        require(path.is_file() and not path.is_symlink() and path.stat().st_size <= 16 * 1024 * 1024,
                "probe File API original missing, linked or oversized")
        originals[name] = path.read_bytes()
        total += len(originals[name])
        require(total <= 256 * 1024 * 1024, "probe File API graph exceeds evidence byte cap")
    for name, expected in ((index_path.name, index), (model_ref["jsonFile"], model),
                           (target_ref["jsonFile"], target)):
        require(json.loads(originals[name], object_pairs_hook=unique_object) == expected,
                "probe File API changed after its actual validation")
    output.mkdir(parents=True)
    files = []
    for name, data in originals.items():
        path = output / name
        path.write_bytes(data)
        require(path.read_bytes() == data, "probe raw File API evidence copy differs")
        files.append({"name": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    return {"schemaVersion": 1, "directory": str(output), "index": index_path.name,
            "codemodel": model_ref["jsonFile"], "target": target_ref["jsonFile"], "files": files}


def check_probe_file_api(directory, context):
    """Recheck uploaded originals without needing the producer's filesystem."""
    try:
        from .check_sdk_only_boundary import CLIENT
    except ImportError:
        from check_sdk_only_boundary import CLIENT
    directory = Path(directory)
    raw = context["fileApiRaw"]
    require(raw.get("schemaVersion") == 1 and isinstance(raw.get("files"), list) and
            0 < len(raw["files"]) <= 4096, "probe raw File API receipt absent or malformed")
    objects = {}
    total = 0
    for item in raw["files"]:
        name = item["name"]
        require(isinstance(name, str) and Path(name).name == name and name.endswith(".json") and
                not any(c in name for c in "\0/\\") and name not in objects,
                "probe raw File API duplicate or unsafe member")
        path = directory / name
        require(path.is_file() and not path.is_symlink() and path.stat().st_size == item["bytes"] and
                path.stat().st_size <= 16 * 1024 * 1024 and digest(path) == item["sha256"],
                "probe raw File API missing, changed or oversized member")
        data = path.read_bytes()
        total += len(data)
        require(total <= 256 * 1024 * 1024, "probe raw File API graph exceeds evidence byte cap")
        objects[name] = json.loads(data, object_pairs_hook=unique_object)
    require(set(objects) == {path.name for path in directory.iterdir()}, "probe raw File API unindexed member")
    require(all(raw[key] in objects for key in ("index", "codemodel", "target")), "probe raw File API root missing")
    index, model, target = (objects[raw[key]] for key in ("index", "codemodel", "target"))
    def normalized(value):
        value = value.replace("\\", "/")
        return ntpath.normcase(ntpath.normpath(value)) if ntpath.splitdrive(value)[0] else posixpath.normpath(value)
    require(index["reply"][CLIENT]["codemodel-v2"]["jsonFile"] == raw["codemodel"] and
            model.get("kind") == "codemodel" and model.get("version", {}).get("major") == 2 and
            normalized(model["paths"]["build"]) == normalized(context["producerBuild"]) and
            normalized(model["paths"]["source"]) == normalized(context["producerSource"]),
            "probe raw index/codemodel identity differs")
    configs = [entry for entry in model["configurations"] if entry["name"] == context["configuration"]]
    require(len(configs) == 1, "probe raw configuration missing or duplicated")
    refs = [entry for entry in configs[0]["targets"] if entry["name"] == PROBE_TARGET]
    require(len(refs) == 1 and refs[0]["jsonFile"] == raw["target"] and refs[0]["id"] == target["id"] ==
            context["fileApiTarget"]["id"] and target["name"] == PROBE_TARGET and
            target["type"] == "EXECUTABLE" and not target.get("dependencies"), "probe raw target identity differs")
    require(digest(directory / raw["target"]) == context["fileApiTarget"]["sha256"], "probe raw target fingerprint differs")
    sources = [entry["path"] for entry in target["sources"] if "compileGroupIndex" in entry]
    source_root = model["paths"]["source"].replace("\\", "/")
    actual_sources = [normalized(path if absolute(path) else source_root + "/" + path) for path in sources]
    require(actual_sources == [normalized(source_root + "/" + PROBE_SOURCE)], "probe raw compiled source differs")
    artifacts = target.get("artifacts", [])
    require(len(artifacts) == 1, "probe raw executable artifact missing or ambiguous")
    artifact = artifacts[0]["path"]
    require(normalized(artifact if absolute(artifact) else model["paths"]["build"] + "/" + artifact) ==
            normalized(context["original"]["path"]), "probe raw native artifact differs")
    referenced = {raw["index"], raw["codemodel"]}
    for configuration in model["configurations"]:
        for entry in (*configuration.get("targets", []), *configuration.get("directories", [])):
            if "jsonFile" in entry:
                require(entry["jsonFile"] in objects, "probe raw graph reference missing")
                referenced.add(entry["jsonFile"])
        for entry in configuration.get("targets", []):
            require(objects[entry["jsonFile"]].get("id") == entry["id"] and
                    objects[entry["jsonFile"]].get("name") == entry["name"], "probe raw owner target differs")
    require(set(objects) == referenced, "probe raw File API graph incomplete or foreign")
    return {"status": "passed", "rawFiles": len(objects), "rawBytes": total,
            "index": raw["index"], "codemodel": raw["codemodel"], "target": raw["target"]}


def copy_probe(repo, build, scratch, config="Release", evidence=None):
    """Resolve the actual compiled private executable through the original File API."""
    try:
        from .check_sdk_only_boundary import CLIENT, read_reply
    except ImportError:
        from check_sdk_only_boundary import CLIENT, read_reply
    repo, build, scratch = Path(repo).resolve(), Path(build).resolve(), Path(scratch).resolve()
    require(not scratch.is_relative_to(repo) and not scratch.is_relative_to(build), "probe relocation must leave the producer trees")
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"))
    require(bool(indices), "actual File API index missing")
    index = json.loads(indices[-1].read_text(encoding="utf-8"))
    model_ref = index["reply"][CLIENT]["codemodel-v2"]
    model = read_reply(reply, model_ref)
    require(model.get("kind") == "codemodel" and model.get("version", {}).get("major") == 2 and
            Path(model["paths"]["source"]).resolve() == repo and Path(model["paths"]["build"]).resolve() == build,
            "probe File API source/build differs")
    configs = [entry for entry in model["configurations"] if entry["name"] == config]
    require(len(configs) == 1, "probe configuration missing or duplicated")
    refs = [entry for entry in configs[0]["targets"] if entry["name"] == PROBE_TARGET]
    require(len(refs) == 1, "actual probe target missing or duplicated")
    target = read_reply(reply, refs[0])
    require(target["id"] == refs[0]["id"] and target["name"] == PROBE_TARGET and
            target["type"] == "EXECUTABLE" and not target.get("dependencies"), "probe must remain a standalone private executable")
    sources = []
    for entry in target["sources"]:
        if "compileGroupIndex" not in entry:
            continue
        path = Path(entry["path"])
        sources.append((path if path.is_absolute() else repo / path).resolve())
    require(sources == [(repo / PROBE_SOURCE).resolve()], "probe compiled a different source body")
    artifacts = target.get("artifacts", [])
    require(len(artifacts) == 1, "probe executable artifact missing or ambiguous")
    original = Path(artifacts[0]["path"])
    original = original if original.is_absolute() else build / original
    require(not original.is_symlink(), "probe artifact is linked")
    original = original.resolve()
    require(original.is_relative_to(build) and original.is_file() and not original.is_symlink() and
            original.stat().st_size > 0 and original.name in (PROBE_TARGET, PROBE_TARGET + ".exe"),
            "actual producer probe artifact unavailable")
    destination = scratch / "command-jobs-fixture" / original.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    require(not destination.exists(), "probe relocation destination already exists")
    shutil.copy2(original, destination)
    require(digest(destination) == digest(original) and
            (os.name == "nt" or os.access(destination, os.X_OK)), "copied probe bytes/permissions differ")
    context = {"schemaVersion": 1, "githubSha": os.environ.get("GITHUB_SHA"), "configuration": config,
            "producerBuild": str(build), "producerSource": str(repo),
            "source": {"path": PROBE_SOURCE, "sha256": digest(repo / PROBE_SOURCE)},
            "fileApiTarget": {"path": str(reply / refs[0]["jsonFile"]), "sha256": digest(reply / refs[0]["jsonFile"]),
                              "id": target["id"], "name": target["name"]},
            "original": {"path": str(original), "sha256": digest(original), "bytes": original.stat().st_size},
            "copy": {"path": str(destination), "sha256": digest(destination), "bytes": destination.stat().st_size}}
    if evidence is not None:
        context["fileApiRaw"] = preserve_probe_file_api(reply, indices[-1], index, model_ref, model,
                                                      refs[0], target, evidence)
        context["fileApiRawAcceptance"] = check_probe_file_api(evidence, context)
    return context


def check_copies(source, prefix, seal, probe, evidence=None):
    copies = {}
    for relative, actual in ((HELPER, Path(source) / "command_jobs.cpp"), (HEADER, Path(prefix) / HEADER)):
        require(actual.is_file() and not actual.is_symlink() and digest(actual) == seal[relative]["sha256"],
                "relocated helper/header drifted: " + relative)
        if evidence is not None:
            destination = Path(evidence) / relative
            require(not destination.exists(), "relocated source evidence destination already exists")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(actual, destination)
            require(digest(destination) == seal[relative]["sha256"], "relocated source evidence differs")
            copies[relative] = {"actualPath": str(actual), "evidencePath": relative,
                                "bytes": destination.stat().st_size, "sha256": digest(destination)}
    actual = Path(probe["copy"]["path"])
    require(actual.is_file() and not actual.is_symlink() and actual.stat().st_size == probe["copy"]["bytes"] and
            digest(actual) == probe["copy"]["sha256"] == probe["original"]["sha256"], "relocated probe changed")
    return copies
