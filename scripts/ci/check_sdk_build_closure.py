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
    from .check_sdk_only_boundary import (CLIENT, SDK_HOST_ONLY_SOURCE_FILES,
                                         SDK_HOST_ONLY_SOURCE_PREFIXES, prepare, read_reply, relative)
except ImportError:
    from check_sdk_only_boundary import (CLIENT, SDK_HOST_ONLY_SOURCE_FILES,
                                        SDK_HOST_ONLY_SOURCE_PREFIXES, prepare, read_reply, relative)


FORBIDDEN_TARGETS = {"lubancode_core", "lubancode_updater", "miniz"}


def inspect_graph(targets: dict) -> dict:
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
                      for name in sources if name in SDK_HOST_ONLY_SOURCE_FILES
                      or name.startswith(SDK_HOST_ONLY_SOURCE_PREFIXES))
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
    return {"sdkBuildClosure": sorted(closure), "sdkProjectSources": sources,
            "status": "failed" if violations else "passed", "violations": violations}


def inspect(source: Path, build: Path, config: str) -> dict:
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
        names = []
        for item in target.get("sources", []):
            path = Path(item["path"])
            name = relative(path if path.is_absolute() else source / path, source)
            if name and name.startswith(("src/", "include/")):
                names.append(name)
        targets[target["id"]] = {
            "name": target["name"], "type": target["type"], "projectSources": names,
            "dependencies": [item["id"] for item in target.get("dependencies", [])],
        }
    result = inspect_graph(targets)
    return {"schemaVersion": 1, "githubSha": os.environ.get("GITHUB_SHA", ""),
            "sourceDir": str(source), "buildDir": str(build), "configuration": config,
            "fileApiIndex": str(indices[-1]), "targets": targets, **result}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--source-dir", type=Path, default=Path.cwd())
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    if args.prepare:
        prepare(args.build_dir)
        return 0
    try:
        report = inspect(args.source_dir, args.build_dir, args.config)
    except (KeyError, OSError, TypeError, ValueError) as error:
        report = {"status": "failed", "violations": [str(error)]}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": report["status"], "violations": report["violations"]}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
