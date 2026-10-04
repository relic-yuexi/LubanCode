#!/usr/bin/env python3
"""Check the actual SDK build closure, including combined host builds.

Reads CMake File API data only; --prepare requests it before CI configures.
This is a build-graph check, not a binary size or runtime call-graph claim.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

try:
    from .check_sdk_only_boundary import (CLIENT, CHANNEL_HOST_SOURCES, CHANNEL_RUNTIME_SOURCES,
                                         CHANNEL_HOST_TARGETS, MBEDTLS_TARGETS, SDK_NEUTRAL_PACKAGE_SOURCES,
                                         package_ownership_violations,
                                         prepare, read_reply, relative, sdk_host_only_source)
except ImportError:
    from check_sdk_only_boundary import (CLIENT, CHANNEL_HOST_SOURCES, CHANNEL_RUNTIME_SOURCES,
                                        CHANNEL_HOST_TARGETS, MBEDTLS_TARGETS, SDK_NEUTRAL_PACKAGE_SOURCES,
                                        package_ownership_violations,
                                        prepare, read_reply, relative, sdk_host_only_source)


try:
    from .sdk_lua_profile import lua_graph_violations, read_lua_profile
except ImportError:
    try:
        from sdk_lua_profile import lua_graph_violations, read_lua_profile
    except ModuleNotFoundError:
        from scripts.ci.sdk_lua_profile import lua_graph_violations, read_lua_profile


FORBIDDEN_TARGETS = {"lubancode_core", "lubancode_updater", "miniz",
                     *CHANNEL_HOST_TARGETS, *MBEDTLS_TARGETS}


def channel_ownership_violations(targets: dict) -> list[str]:
    expected = {"lubancode_channel_host": CHANNEL_HOST_SOURCES,
                "lubancode_channel_runtime": CHANNEL_RUNTIME_SOURCES}
    all_sources = CHANNEL_HOST_SOURCES | CHANNEL_RUNTIME_SOURCES
    relevant = any(target["name"] in (*CHANNEL_HOST_TARGETS, "lubancode_core")
                   or any(source in all_sources for source in target["projectSources"])
                   for target in targets.values())
    engine_names = [key for key, target in targets.items() if target["name"] == "lubancode_engine"]
    if not relevant and not engine_names:
        return []

    violations, owners = [], {}
    for name, sources in (expected.items() if relevant else ()):
        matching = [key for key, target in targets.items() if target["name"] == name]
        if len(matching) != 1 or targets[matching[0]]["type"] != "STATIC_LIBRARY":
            violations.append("Channel/Gateway host target must exist once as a static library: " + name)
            continue
        owners[name] = matching[0]
        if sorted(targets[matching[0]]["projectSources"]) != sorted(sources):
            violations.append("Channel/Gateway host source roster differs: " + name)
        for source in sources:
            actual = [key for key, target in targets.items() for item in target["projectSources"]
                      if item == source]
            if actual != matching:
                violations.append("Channel/Gateway source must have one exact host owner: " + source)

    def one_named(name):
        keys = [key for key, target in targets.items() if target["name"] == name]
        return keys[0] if len(keys) == 1 else None

    engine, runtime = one_named("lubancode_engine"), one_named("lubancode_runtime")
    for source in ("src/channel/types.cpp", "src/channel/channel_config.cpp"):
        actual = [key for key, target in targets.items() for item in target["projectSources"]
                  if item == source]
        if engine is None or actual != [engine]:
            violations.append("Neutral channel config/type source must belong once to engine: " + source)
    required = {"lubancode_channel_host": (engine, one_named("mbedtls")),
                "lubancode_channel_runtime": (runtime, owners.get("lubancode_channel_host"))}
    for name, dependencies in (required.items() if relevant else ()):
        key = owners.get(name)
        if key is not None and any(dep is None or dep not in targets[key]["dependencies"]
                                   for dep in dependencies):
            violations.append("Channel/Gateway host dependency direction differs: " + name)
    for target in targets.values():
        if target["name"] == "lubancode_core" and owners.get("lubancode_channel_runtime") not in target["dependencies"]:
            violations.append("CLI core must explicitly depend on Channel/Gateway runtime host")

    for name, key in owners.items():
        seen, pending = set(), list(targets[key]["dependencies"])
        while pending:
            dependency = pending.pop()
            if dependency in seen:
                continue
            if dependency not in targets:
                raise ValueError("unknown build dependency: " + dependency)
            seen.add(dependency)
            target = targets[dependency]
            forbidden = {"lubancore_sdk", "lubancode_core", "lubancode_updater", "miniz"}
            if name == "lubancode_channel_host":
                forbidden |= {"lubancode_runtime", "lubancode_channel_runtime"}
            host_service_sources = any((source.startswith("src/package/") and source not in SDK_NEUTRAL_PACKAGE_SOURCES)
                                       or source.startswith("src/updater/")
                                       or source == "src/config/update_checker.cpp"
                                       for source in target["projectSources"])
            if target["name"] in forbidden or dependency == key or host_service_sources:
                violations.append("Channel/Gateway host has a reverse dependency: " + name + " -> " + target["name"])
            pending.extend(target["dependencies"])
    return violations


def inspect_graph(targets: dict, with_lua: bool | None = None) -> dict:
    sdk = [key for key, target in targets.items() if target["name"] == "lubancore_sdk"]
    if len(sdk) != 1 or targets[sdk[0]]["type"] != "SHARED_LIBRARY":
        raise ValueError("expected exactly one shared SDK target")
    closure, pending = set(), list(sdk)
    while pending:
        key = pending.pop()
        if key in closure:
            continue
        if key not in targets:
            raise ValueError("unknown build dependency: " + key)
        closure.add(key)
        pending.extend(targets[key]["dependencies"])
    sources = sorted({name for key in closure for name in targets[key]["projectSources"]})
    violations = ["SDK depends on host-only target: " + targets[key]["name"]
                  for key in sorted(closure) if targets[key]["name"] in FORBIDDEN_TARGETS]
    violations.extend("SDK depends on host-only source: " + name
                      for name in sources if sdk_host_only_source(name))
    violations.extend(channel_ownership_violations(targets))
    violations.extend(package_ownership_violations(targets))
    # A combined build keeps the CLI query implementation, but only its existing
    # updater may own it. SDK-only defines neither. Inspect all actual targets,
    # including disconnected hosts, so omission or duplicate compilation cannot
    # hide behind a clean SDK source union.
    updater_owners = [key for key, target in targets.items() if target["name"] == "lubancode_updater"]
    query_owners = [key for key, target in targets.items() for name in target["projectSources"]
                    if name == "src/config/update_checker.cpp"]
    if updater_owners or query_owners:
        if (len(updater_owners) != 1 or query_owners != updater_owners or
                targets[updater_owners[0]]["type"] != "STATIC_LIBRARY"):
            violations.append("Release query implementation must belong once to CLI-only lubancode_updater")
    if not any(name.startswith("src/sdk/") for name in sources):
        violations.append("SDK closure contains no SDK implementation sources")
    # Job binding is a private SDK implementation, not a new neutral runtime
    # owner. Only the two reference executables may compile their own copy.
    job_source = "src/sdk/job_operations.cpp"
    job_owners = {key: target["projectSources"].count(job_source) for key, target in targets.items()
                  if job_source in target["projectSources"]}
    if job_owners.get(sdk[0]) != 1:
        violations.append("Job binding implementation must belong exactly once to the shared SDK")
    for key, occurrences in job_owners.items():
        target = targets[key]
        if occurrences != 1:
            violations.append("Job binding has duplicate source occurrences: " + target["name"])
        if key != sdk[0] and not (target["name"] in {"lubancore_sdk_tests", "lubancode_tests"}
                                 and target["type"] == "EXECUTABLE"):
            violations.append("Job binding has an unregistered reference owner: " + target["name"])
    # Agent loop calls the neutral lease directly. A runtime-owned provider can
    # appear in the SDK's transitive graph and still fail a single-pass static
    # host link: runtime is scanned before its engine dependency. Require the
    # implementation in the consuming engine instead of adding a reverse edge.
    loop_owners = [key for key in closure
                   if "src/agent/loop.cpp" in targets[key]["projectSources"]]
    if loop_owners:
        lease_owners = [key for key in closure
                        for name in targets[key]["projectSources"]
                        if name == "src/runtime/scoped_approval.cpp"]
        if (len(loop_owners) != 1 or targets[loop_owners[0]]["name"] != "lubancode_engine" or
                targets[loop_owners[0]]["type"] != "STATIC_LIBRARY" or
                lease_owners != loop_owners):
            violations.append("Agent loop lease implementation must belong once to lubancode_engine")
    if with_lua is not None:
        violations.extend(lua_graph_violations(targets, with_lua))
    return {"sdkBuildClosure": sorted(closure), "sdkProjectSources": sources,
            "status": "failed" if violations else "passed", "violations": violations}


def inspect(source: Path, build: Path, config: str, lua_profile: str | None = None) -> dict:
    source, build = source.resolve(), build.resolve()
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"))
    if not indices:
        raise ValueError("no File API reply; prepare before configuring")
    index = json.loads(indices[-1].read_text(encoding="utf-8"))
    model = read_reply(reply, index.get("reply", {}).get(CLIENT, {})["codemodel-v2"])
    if model.get("kind") != "codemodel" or model.get("version", {}).get("major") != 2:
        raise ValueError("unsupported codemodel reply")
    if (Path(model["paths"]["source"]).resolve() != source or
            Path(model["paths"]["build"]).resolve() != build):
        raise ValueError("File API reply belongs to another source/build tree")
    configurations = [entry for entry in model["configurations"] if entry["name"] == config]
    if len(configurations) != 1:
        raise ValueError("expected exactly one requested configuration")
    targets = {}
    for entry in configurations[0]["targets"]:
        target = read_reply(reply, entry)
        if target["id"] in targets or target["id"] != entry["id"]:
            raise ValueError("duplicate or mismatched target identity")
        names, lua_sources = [], []
        for item in target.get("sources", []):
            path = Path(item["path"])
            name = relative(path if path.is_absolute() else source / path, source)
            if name and name.startswith(("src/", "include/")):
                names.append(name)
            if "compileGroupIndex" in item:
                lua_sources.append(name or str(path if path.is_absolute() else source / path))
        targets[target["id"]] = {
            "name": target["name"], "type": target["type"], "projectSources": names,
            "dependencies": [item["id"] for item in target.get("dependencies", [])],
            "luaSources": lua_sources,
        }
    cache = read_reply(reply, index.get("reply", {}).get(CLIENT, {})["cache-v2"])
    entries = {entry["name"]: entry["value"] for entry in cache["entries"]}
    with_lua = read_lua_profile(entries, lua_profile)
    result = inspect_graph(targets)
    if "LUBANCORE_WITH_LUA" in entries or lua_profile is not None:
        violations = lua_graph_violations({key: {**target, "projectSources": target["luaSources"]}
                                           for key, target in targets.items()}, with_lua)
        result["violations"].extend(violations)
        result["status"] = "failed" if result["violations"] else "passed"
    result["luaProfile"] = "on" if with_lua else "off"
    return {"schemaVersion": 1, "githubSha": os.environ.get("GITHUB_SHA", ""),
            "sourceDir": str(source), "buildDir": str(build), "configuration": config,
            "fileApiIndex": str(indices[-1]), "targets": targets, **result}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--source-dir", type=Path, default=Path.cwd())
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--lua-profile", choices=("on", "off"), default="on")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.prepare:
        prepare(args.build_dir)
        return 0
    try:
        report = inspect(args.source_dir, args.build_dir, args.config, args.lua_profile)
    except (KeyError, OSError, TypeError, ValueError) as error:
        report = {"status": "failed", "violations": [str(error)]}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "violations": report["violations"]}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
