#!/usr/bin/env python3
"""Run the CLI Runner Release source and retain actual native evidence."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import xml.etree.ElementTree as ET


REQUIRED = "unit.job_runner.runner_release_diagnostic"
SOURCE = "tests/unit/job_runner/test_runner_release_diagnostic.cpp"
SOURCE_FILTER = "--source-file=*test_runner_release_diagnostic.cpp"
NATIVE_CASES = 1
READY_MARKER = "[runner-release-path] " + ("windows-normal-release" if os.name == "nt" else "posix-owned-gate-rejection")


def validate_registration(tests):
    if len(tests) != 1 or tests[0].get("name") != REQUIRED:
        raise RuntimeError("Runner Release source is missing, duplicated or unexpected")
    test = tests[0]
    command = test.get("command", [])
    if (len(command) != 2 or not isinstance(command[0], str)
            or command[0].replace("\\", "/").rsplit("/", 1)[-1] not in
            ("luban_runner_release_tests", "luban_runner_release_tests.exe") or command[1] != SOURCE_FILTER):
        raise RuntimeError("Runner Release command does not select exactly the original CLI source")
    properties = test.get("properties", [])
    props = {prop["name"]: prop["value"] for prop in properties}
    if len(props) != len(properties):
        raise RuntimeError("Runner Release source has duplicated properties")
    try:
        timeout = float(props.get("TIMEOUT", 0))
    except (TypeError, ValueError) as error:
        raise RuntimeError("Runner Release source has an invalid timeout") from error
    if (props.get("DISABLED") or not 0 < timeout <= 300
            or not {"unit", "job_runner", "unit.job_runner"} <= set(props.get("LABELS", []))):
        raise RuntimeError("Runner Release source is disabled, mislabeled or unbounded")


def validate_results(results, native_log):
    suite = ET.fromstring(results)
    for key in ("failures", "errors", "disabled", "skipped"):
        if int(suite.attrib.get(key, "0")):
            raise RuntimeError("Runner Release JUnit reports a failure or skip")
    cases = suite.findall(".//testcase")
    if len(cases) != 1 or cases[0].attrib.get("name") != REQUIRED:
        raise RuntimeError("JUnit does not cover exactly the Runner Release source")
    case = cases[0]
    if case.attrib.get("status") != "run" or any(case.find(kind) is not None
                                                for kind in ("failure", "error", "skipped")):
        raise RuntimeError("Runner Release source failed or skipped")
    sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$', native_log, flags=re.M)
    if len(sections) != 3 or sections[1] != REQUIRED:
        raise RuntimeError("Native log does not identify exactly the Runner Release source")
    section = sections[2]
    commands = re.findall(r"^Command: ([^\r\n]+)\r?$", section, flags=re.M)
    if len(commands) != 1:
        raise RuntimeError("Native Runner Release command is missing or duplicated")
    command = shlex.split(commands[0].replace("\\", "/"))
    if (len(command) != 2 or command[0].rsplit("/", 1)[-1] not in ("luban_runner_release_tests", "luban_runner_release_tests.exe")
            or command[1] != SOURCE_FILTER):
        raise RuntimeError("Native Runner Release command runs a different source or binary")
    if section.count("Test Passed.") != 1 or "Test Failed." in section:
        raise RuntimeError("Native Runner Release source did not pass")
    counts = re.findall(r"\[doctest\] test cases:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if counts != [(str(NATIVE_CASES), str(NATIVE_CASES), "0")]:
        raise RuntimeError("Runner Release native roster differs from one passing case")
    assertions = re.findall(r"\[doctest\] assertions:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(assertions) != 1:
        raise RuntimeError("Runner Release native assertion summary is missing or duplicated")
    total, passed, failed = map(int, assertions[0])
    if total == 0 or total != passed or failed:
        raise RuntimeError("Runner Release native assertions are empty or failed")
    markers = re.findall(r"^" + re.escape(READY_MARKER) + r"\r?$", section, flags=re.M)
    if len(markers) != 1:
        raise RuntimeError("Runner Release native log does not prove one actual platform path")
    return {"nativeCases": NATIVE_CASES, "nativePassed": NATIVE_CASES,
            "assertionsPassed": passed, "readyPaths": 1}


def run_gate(build, configuration):
    build = Path(build).resolve()
    evidence = build / "test-evidence" / "runner-release"
    evidence.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[2]
    context = {
        "githubSha": os.environ.get("GITHUB_SHA"), "buildDir": str(build),
        "configuration": configuration, "requiredTests": [REQUIRED],
        "sourceFile": SOURCE, "sourceFilter": SOURCE_FILTER, "nativeCases": NATIVE_CASES,
        "requiredReadyMarker": READY_MARKER,
        "sourceSha256": hashlib.sha256((repo / SOURCE).read_bytes()).hexdigest(),
        "processSourceSha256": hashlib.sha256((repo / ("src/job_runner/process_win.cpp" if os.name == "nt"
                                                        else "src/job_runner/process_posix.cpp")).read_bytes()).hexdigest(),
    }
    (evidence / "context.json").write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    command = ["ctest", "--test-dir", str(build), "-C", configuration, "-R",
               r"^unit\.job_runner\.runner_release_diagnostic$"]
    listed = subprocess.run(command + ["--show-only=json-v1"], check=True, text=True,
                            encoding="utf-8", capture_output=True)
    (evidence / "tests.json").write_text(listed.stdout, encoding="utf-8")
    validate_registration(json.loads(listed.stdout)["tests"])
    results = evidence / "results.xml"
    try:
        subprocess.run(command + ["--output-on-failure", "--no-tests=error", "--output-junit", str(results)], check=True)
    finally:
        for filename in ("LastTest.log", "LastTestsFailed.log"):
            source = build / "Testing" / "Temporary" / filename
            if source.exists():
                shutil.copyfile(source, evidence / filename)
    counts = validate_results(results.read_text(encoding="utf-8"), (evidence / "LastTest.log").read_text(encoding="utf-8"))
    (evidence / "summary.json").write_text(json.dumps(counts, indent=2) + "\n", encoding="utf-8")
    print("Runner Release: original CLI source passed " + json.dumps(counts, sort_keys=True))
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    run_gate(args.build_dir, args.config)


if __name__ == "__main__":
    main()
