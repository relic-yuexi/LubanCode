#!/usr/bin/env python3
"""Run the original CLI Release query source and retain complete native evidence."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET


REQUIRED = "unit.config.update_checker"
SOURCE = "tests/unit/config/test_update_checker.cpp"
SOURCE_FILTER = "--source-file=*test_update_checker.cpp"
NATIVE_CASES = 10


def validate_registration(tests):
    if len(tests) != 1 or tests[0].get("name") != REQUIRED:
        raise RuntimeError("Release query source is missing, duplicated or unexpected")
    test = tests[0]
    command = test.get("command", [])
    if (len(command) != 2 or not isinstance(command[0], str)
            or command[0].replace("\\", "/").rsplit("/", 1)[-1] not in
            ("lubancode_tests", "lubancode_tests.exe") or command[1] != SOURCE_FILTER):
        raise RuntimeError("Release query command does not select exactly the original CLI source")
    properties = test.get("properties", [])
    props = {prop["name"]: prop["value"] for prop in properties}
    if len(props) != len(properties):
        raise RuntimeError("Release query source has duplicated properties")
    try:
        timeout = float(props.get("TIMEOUT", 0))
    except (TypeError, ValueError) as error:
        raise RuntimeError("Release query source has an invalid timeout") from error
    if (props.get("DISABLED") or not 0 < timeout <= 180
            or not {"unit", "config", "unit.config"} <= set(props.get("LABELS", []))):
        raise RuntimeError("Release query source is disabled, mislabeled or unbounded")


def validate_results(results, native_log):
    suite = ET.fromstring(results)
    for key in ("failures", "errors", "disabled", "skipped"):
        if int(suite.attrib.get(key, "0")):
            raise RuntimeError("Release query JUnit reports a failure or skip")
    cases = suite.findall(".//testcase")
    if len(cases) != 1 or cases[0].attrib.get("name") != REQUIRED:
        raise RuntimeError("JUnit does not cover exactly the Release query source")
    case = cases[0]
    if case.attrib.get("status") != "run" or any(case.find(kind) is not None
                                                for kind in ("failure", "error", "skipped")):
        raise RuntimeError("Release query source failed or skipped")
    sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$', native_log, flags=re.M)
    if len(sections) != 3 or sections[1] != REQUIRED:
        raise RuntimeError("Native log does not identify exactly the Release query source")
    section = sections[2]
    if section.count("Test Passed.") != 1 or "Test Failed." in section:
        raise RuntimeError("Native Release query source did not pass")
    counts = re.findall(r"\[doctest\] test cases:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if counts != [(str(NATIVE_CASES), str(NATIVE_CASES), "0")]:
        raise RuntimeError("Release query native roster differs from ten passing cases")
    assertions = re.findall(r"\[doctest\] assertions:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(assertions) != 1:
        raise RuntimeError("Release query native assertion summary is missing or duplicated")
    total, passed, failed = map(int, assertions[0])
    if total == 0 or total != passed or failed:
        raise RuntimeError("Release query native assertions are empty or failed")
    return {"nativeCases": NATIVE_CASES, "nativePassed": NATIVE_CASES, "assertionsPassed": passed}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    evidence = build / "test-evidence" / "release-query"
    evidence.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[2]
    context = {
        "githubSha": os.environ.get("GITHUB_SHA"), "buildDir": str(build),
        "configuration": args.config, "requiredTests": [REQUIRED],
        "sourceFile": SOURCE, "sourceFilter": SOURCE_FILTER, "nativeCases": NATIVE_CASES,
        "sourceSha256": hashlib.sha256((repo / SOURCE).read_bytes()).hexdigest(),
        "querySourceSha256": hashlib.sha256((repo / "src/config/update_checker.cpp").read_bytes()).hexdigest(),
    }
    (evidence / "context.json").write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    command = ["ctest", "--test-dir", str(build), "-C", args.config, "-R", r"^unit\.config\.update_checker$"]
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
    print("Release query: original CLI source passed " + json.dumps(counts, sort_keys=True))


if __name__ == "__main__":
    main()
