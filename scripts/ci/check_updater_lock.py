#!/usr/bin/env python3
"""Run the original CLI updater lock source and retain full native evidence."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET


REQUIRED = "integration.updater.updater_lock"
SOURCE = "tests/integration/updater/test_updater_lock.cpp"
SOURCE_FILTER = "--source-file=*test_updater_lock.cpp"
NATIVE_CASES = 8
READY_MARKER = "[updater-lock-ready] alive-holder-after-acquire"


def validate_registration(tests):
    if len(tests) != 1 or tests[0].get("name") != REQUIRED:
        raise RuntimeError("Updater lock source is missing, duplicated or unexpected")
    test = tests[0]
    command = test.get("command", [])
    if (len(command) != 2 or not isinstance(command[0], str)
            or command[0].replace("\\", "/").rsplit("/", 1)[-1] not in
            ("lubancode_tests", "lubancode_tests.exe") or command[1] != SOURCE_FILTER):
        raise RuntimeError("Updater lock command does not select exactly the original CLI source")
    properties = test.get("properties", [])
    props = {prop["name"]: prop["value"] for prop in properties}
    if len(props) != len(properties):
        raise RuntimeError("Updater lock source has duplicated properties")
    try:
        timeout = float(props.get("TIMEOUT", 0))
    except (TypeError, ValueError) as error:
        raise RuntimeError("Updater lock source has an invalid timeout") from error
    if (props.get("DISABLED") or not 0 < timeout <= 300
            or not {"integration", "updater", "integration.updater"} <= set(props.get("LABELS", []))):
        raise RuntimeError("Updater lock source is disabled, mislabeled or unbounded")


def validate_results(results, native_log):
    suite = ET.fromstring(results)
    for key in ("failures", "errors", "disabled", "skipped"):
        if int(suite.attrib.get(key, "0")):
            raise RuntimeError("Updater lock JUnit reports a failure or skip")
    cases = suite.findall(".//testcase")
    if len(cases) != 1 or cases[0].attrib.get("name") != REQUIRED:
        raise RuntimeError("JUnit does not cover exactly the updater lock source")
    case = cases[0]
    if case.attrib.get("status") != "run" or any(case.find(kind) is not None
                                                for kind in ("failure", "error", "skipped")):
        raise RuntimeError("Updater lock source failed or skipped")
    sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$', native_log, flags=re.M)
    if len(sections) != 3 or sections[1] != REQUIRED:
        raise RuntimeError("Native log does not identify exactly the updater lock source")
    section = sections[2]
    if section.count("Test Passed.") != 1 or "Test Failed." in section:
        raise RuntimeError("Native updater lock source did not pass")
    counts = re.findall(r"\[doctest\] test cases:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if counts != [(str(NATIVE_CASES), str(NATIVE_CASES), "0")]:
        raise RuntimeError("Updater lock native roster differs from eight passing cases")
    assertions = re.findall(r"\[doctest\] assertions:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(assertions) != 1:
        raise RuntimeError("Updater lock native assertion summary is missing or duplicated")
    total, passed, failed = map(int, assertions[0])
    if total == 0 or total != passed or failed:
        raise RuntimeError("Updater lock native assertions are empty or failed")
    markers = re.findall(r"^" + re.escape(READY_MARKER) + r"\r?$", section, flags=re.M)
    if len(markers) != 1:
        raise RuntimeError("Updater lock native log does not prove one acquire-ready path")
    return {"nativeCases": NATIVE_CASES, "nativePassed": NATIVE_CASES,
            "assertionsPassed": passed, "readyPaths": 1}


def run_gate(build, configuration):
    build = Path(build).resolve()
    evidence = build / "test-evidence" / "updater-lock"
    evidence.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[2]
    context = {
        "githubSha": os.environ.get("GITHUB_SHA"), "buildDir": str(build),
        "configuration": configuration, "requiredTests": [REQUIRED],
        "sourceFile": SOURCE, "sourceFilter": SOURCE_FILTER, "nativeCases": NATIVE_CASES,
        "requiredReadyMarker": READY_MARKER,
        "sourceSha256": hashlib.sha256((repo / SOURCE).read_bytes()).hexdigest(),
        "lockSourceSha256": hashlib.sha256((repo / "src/updater/lock.cpp").read_bytes()).hexdigest(),
        "racerSourceSha256": hashlib.sha256((repo / "tests/support/updater_lock_racer.cpp").read_bytes()).hexdigest(),
    }
    (evidence / "context.json").write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    command = ["ctest", "--test-dir", str(build), "-C", configuration, "-R",
               r"^integration\.updater\.updater_lock$"]
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
    print("Updater lock: original CLI source passed " + json.dumps(counts, sort_keys=True))
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    run_gate(args.build_dir, args.config)


if __name__ == "__main__":
    main()
