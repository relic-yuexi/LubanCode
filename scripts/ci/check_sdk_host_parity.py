#!/usr/bin/env python3
"""Run the registered host parity tests in CI and retain complete evidence."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET


REQUIRED = {
    "sdk.host.app_server_session_parity",
    "sdk.host.session_assembly",
    "sdk.host.plugin_assembly",
    "sdk.host.mcp_host_policy",
    "sdk.host.tool_runtime",
    "sdk.host.tool_runtime_deferral",
    "sdk.host.turn_runner_scoped_bindings",
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    evidence = build / "test-evidence" / "sdk-host-parity"
    evidence.mkdir(parents=True, exist_ok=True)
    command = ["ctest", "--test-dir", str(build), "-C", args.config, "-L", "^sdk-host-parity$"]
    listed = subprocess.run(command + ["--show-only=json-v1"], check=True, text=True,
                            encoding="utf-8", capture_output=True)
    (evidence / "tests.json").write_text(listed.stdout, encoding="utf-8")
    tests = json.loads(listed.stdout)["tests"]
    if len(tests) != len(REQUIRED) or {t["name"] for t in tests} != REQUIRED:
        raise RuntimeError("Host parity tests are missing, duplicated or unexpected")
    for test in tests:
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        if props.get("DISABLED") or not 0 < float(props.get("TIMEOUT", 0)) <= 300:
            raise RuntimeError("Host parity test is disabled or has no bounded timeout: " + test["name"])
    (evidence / "context.json").write_text(json.dumps({
        "githubSha": os.environ.get("GITHUB_SHA"), "buildDir": str(build),
        "configuration": args.config, "requiredTests": sorted(REQUIRED),
    }, indent=2), encoding="utf-8")
    results = evidence / "results.xml"
    try:
        subprocess.run(command + ["--output-on-failure", "--no-tests=error",
                                  "--output-junit", str(results)], check=True)
    finally:
        native_log = build / "Testing" / "Temporary" / "LastTest.log"
        if native_log.exists():
            shutil.copyfile(native_log, evidence / "LastTest.log")
    cases = ET.parse(results).getroot().findall(".//testcase")
    if len(cases) != len(REQUIRED) or {c.attrib["name"] for c in cases} != REQUIRED:
        raise RuntimeError("JUnit does not cover every host parity test")
    if any(c.attrib.get("status") != "run" or any(c.find(k) is not None for k in
               ("failure", "error", "skipped")) for c in cases):
        raise RuntimeError("Host parity contains a skipped or failed test")
    native_sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$',
        (evidence / "LastTest.log").read_text(encoding="utf-8"), flags=re.M)
    for name in REQUIRED:
        sections = [native_sections[index + 1] for index in range(1, len(native_sections), 2)
                    if native_sections[index] == name]
        if len(sections) != 1:
            raise RuntimeError("Native log does not identify one host source: " + name)
        counts = re.findall(r"\[doctest\] test cases:\s+(\d+)", sections[0])
        if len(counts) != 1 or int(counts[0]) == 0:
            raise RuntimeError("Host source filter ran no native test cases: " + name)
        if name == "sdk.host.tool_runtime" and int(counts[0]) != 14:
            raise RuntimeError("Host tool runtime roster differs from 14 cases")
    print(f"Host parity: all {len(REQUIRED)} registered test files executed successfully")


if __name__ == "__main__":
    main()
