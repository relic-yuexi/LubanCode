"""Verify the two real workspace read suites; this helper runs no native code."""
import argparse
import json
import os
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

SOURCES = {
    "unit.workspace.workspace_manifest": "test_workspace_manifest.cpp",
    "integration.workspace.identity_cross_platform": "test_identity_cross_platform.cpp",
}


def validate(registration, junit, native, platform):
    tests = registration["tests"]
    if len(tests) != 2 or {t["name"] for t in tests} != SOURCES.keys():
        raise ValueError("exactly two workspace read sources must be registered")
    for test in tests:
        if any(p["name"] == "DISABLED" and p["value"] for p in test.get("properties", [])):
            raise ValueError("workspace read source is disabled")
        if not any(arg.startswith("--source-file=") and arg.endswith(SOURCES[test["name"]]) for arg in test["command"]):
            raise ValueError("workspace source-file filter is missing")
    cases = junit.findall(".//testcase")
    if len(cases) != 2 or {c.attrib["name"] for c in cases} != SOURCES.keys():
        raise ValueError("both workspace sources must appear once in JUnit")
    if any(c.attrib.get("status") != "run" or any(c.find(tag) is not None for tag in ("error", "failure", "skipped")) for c in cases):
        raise ValueError("workspace source failed or was skipped")
    parts = re.split(r"^test \d+\s*$", native, flags=re.M)
    proof = {}
    for name in SOURCES:
        chunks = [p for p in parts if re.search(r"Start\s+\d+: " + re.escape(name) + r"\s*$", p, flags=re.M)]
        if len(chunks) != 1:
            raise ValueError("one original verbose native block is required per source")
        stats = re.findall(r"\[doctest\] test cases:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", chunks[0])
        assertions = re.findall(r"\[doctest\] assertions:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", chunks[0])
        if len(stats) != 1 or len(assertions) != 1:
            raise ValueError("native counts and assertions must be retained")
        count, passed, failed = map(int, stats[0])
        assertion_count, assertions_passed, assertions_failed = map(int, assertions[0])
        expected = (13 if platform == "windows-msvc" else 12) if name.startswith("unit.") else (5 if platform == "windows-msvc" else 4)
        if count != expected or count != passed or failed or assertion_count <= 0 or assertion_count != assertions_passed or assertions_failed:
            raise ValueError("workspace native cases/assertions were empty, omitted or failed")
        proof[name] = {"cases": count, "assertions": assertion_count, "sourceFile": SOURCES[name]}
    return proof


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--platform", choices=("windows-msvc", "macos-clang", "linux-manylinux"), required=True)
    args = parser.parse_args()
    root = args.evidence
    result = validate(json.loads((root / "registration.json").read_text(encoding="utf-8")),
                      ET.parse(root / "results.xml").getroot(),
                      (root / "native.log").read_text(encoding="utf-8"), args.platform)
    actual_head = subprocess.run(["git", "rev-parse", "HEAD"], check=True, text=True, capture_output=True).stdout.strip()
    if actual_head != os.environ["GITHUB_SHA"]:
        raise ValueError("actual checkout differs from GITHUB_SHA")
    summary = {"githubSha": actual_head, "platform": args.platform, "ctests": 2, "sources": result}
    (root / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
