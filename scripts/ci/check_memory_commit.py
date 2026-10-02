#!/usr/bin/env python3
"""Run project commit and existing CLI memory tests, retaining original evidence."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET


REQUIRED = {"unit.memory.memory_project_commit", "unit.memory.project_memory"}
GATE_CASES = 12


def validate_registration(tests):
    if len(tests) != len(REQUIRED) or {item["name"] for item in tests} != REQUIRED:
        raise RuntimeError("Memory commit sources are missing, duplicated or unexpected")
    for item in tests:
        props = {prop["name"]: prop["value"] for prop in item.get("properties", [])}
        if props.get("DISABLED") or not 0 < float(props.get("TIMEOUT", 0)) <= 300:
            raise RuntimeError("Memory commit source is disabled or unbounded: " + item["name"])


def validate_results(results, native_log):
    cases = ET.fromstring(results).findall(".//testcase")
    if len(cases) != len(REQUIRED) or {item.attrib.get("name") for item in cases} != REQUIRED:
        raise RuntimeError("JUnit does not cover every memory commit source")
    for item in cases:
        if item.attrib.get("status") != "run" or any(item.find(kind) is not None for kind in
                                                     ("failure", "error", "skipped")):
            raise RuntimeError("Memory commit source failed or skipped: " + item.attrib.get("name", ""))
    sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$', native_log, flags=re.M)
    counts = {}
    for name in REQUIRED:
        found = [sections[index + 1] for index in range(1, len(sections), 2) if sections[index] == name]
        if len(found) != 1:
            raise RuntimeError("Native log does not identify one memory commit source: " + name)
        matches = re.findall(r"\[doctest\] test cases:\s+(\d+)", found[0])
        if len(matches) != 1 or int(matches[0]) == 0:
            raise RuntimeError("Memory source filter executed no native cases: " + name)
        counts[name] = int(matches[0])
    if counts["unit.memory.memory_project_commit"] != GATE_CASES:
        raise RuntimeError("Project commit native case count differs from the fixed roster")
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    evidence = build / "test-evidence" / "memory-commit"
    evidence.mkdir(parents=True, exist_ok=True)
    command = ["ctest", "--test-dir", str(build), "-C", args.config, "-R",
               r"^unit\.memory\.(memory_project_commit|project_memory)$"]
    listed = subprocess.run(command + ["--show-only=json-v1"], check=True, text=True,
                            encoding="utf-8", capture_output=True)
    (evidence / "tests.json").write_text(listed.stdout, encoding="utf-8")
    validate_registration(json.loads(listed.stdout)["tests"])
    (evidence / "context.json").write_text(json.dumps({
        "githubSha": os.environ.get("GITHUB_SHA"), "buildDir": str(build),
        "configuration": args.config, "requiredTests": sorted(REQUIRED), "gateNativeCases": GATE_CASES,
    }, indent=2), encoding="utf-8")
    results = evidence / "results.xml"
    try:
        subprocess.run(command + ["--output-on-failure", "--no-tests=error", "--output-junit", str(results)], check=True)
    finally:
        for filename in ("LastTest.log", "LastTestsFailed.log"):
            source = build / "Testing" / "Temporary" / filename
            if source.exists():
                shutil.copyfile(source, evidence / filename)
    counts = validate_results(results.read_text(encoding="utf-8"), (evidence / "LastTest.log").read_text(encoding="utf-8"))
    (evidence / "summary.json").write_text(json.dumps({"nativeCases": counts}, indent=2), encoding="utf-8")
    print("Memory commit: both sources passed with native case counts " + json.dumps(counts, sort_keys=True))


if __name__ == "__main__":
    main()
