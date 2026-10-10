#!/usr/bin/env python3
"""Retain and check the same full run's Command Jobs quartet; never run native code."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import xml.etree.ElementTree as ET

try:
    from . import sdk_command_jobs as jobs
except ImportError:
    import sdk_command_jobs as jobs

REQUIRED = {prefix + stem: (stem, executable)
            for prefix, executable in (("sdk.focused.", "lubancore_sdk_tests"),
                                       ("integration.sdk.", "lubancode_tests"))
            for stem in jobs.SOURCES}


def check_registration(raw, build):
    value = json.loads(raw, object_pairs_hook=jobs.unique_object)
    tests = value.get("tests") if isinstance(value, dict) else None
    jobs.require(isinstance(tests, list) and len(tests) == 4 and
                 all(isinstance(test, dict) and test.get("name") in REQUIRED for test in tests) and
                 {test["name"] for test in tests} == set(REQUIRED), "full registration quartet differs")
    commands = {}
    for test in tests:
        name = test["name"]
        stem, executable = REQUIRED[name]
        command = test.get("command")
        jobs.check_registration(command, stem, executable)
        jobs.require(Path(command[0]).resolve().is_relative_to(Path(build).resolve()), "full executable escaped its build")
        properties = test.get("properties")
        jobs.require(isinstance(properties, list) and all(isinstance(item, dict) and
                     isinstance(item.get("name"), str) and "value" in item for item in properties),
                     "full test properties malformed")
        props = {item["name"]: item["value"] for item in properties}
        jobs.require(len(props) == len(properties) and not props.get("DISABLED") and
                     type(props.get("TIMEOUT")) in (int, float) and props["TIMEOUT"] == 300,
                     "full source disabled or actual timeout changed")
        commands[name] = command
    return commands


def extract(build, platform_name):
    build = Path(build).resolve()
    jobs.require(platform_name in ("nt", "posix"), "unknown full-run platform")
    output = build / "test-evidence" / "command-jobs-full"
    output.mkdir(parents=True, exist_ok=True)
    jobs.require(output.resolve().is_relative_to(build), "derived full evidence escaped the build")
    for name in ("context.json", "registration.json", "results.xml", "LastTest.log"):
        (output / name).unlink(missing_ok=True)  # only our prior derived copies
    inputs = {"registration.json": build / "command-jobs-full-registration.json",
              "results.xml": build / "result-store-full-results.xml",
              "LastTest.log": build / "Testing/Temporary/LastTest.log"}
    report = {"githubSha": os.environ.get("GITHUB_SHA"), "platform": platform_name,
              "requiredTests": sorted(REQUIRED), "status": "collecting", "inputs": {}}
    raw = {}

    def save():
        (output / "context.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    save()
    try:
        for name, path in inputs.items():
            try:
                raw[name] = path.read_bytes()
                report["inputs"][name] = {"path": str(path), "bytes": len(raw[name]),
                                          "sha256": hashlib.sha256(raw[name]).hexdigest()}
                (output / name).write_bytes(raw[name])  # preserve failure/abort bytes before parsing
            except OSError as error:
                report["inputs"][name] = {"path": str(path), "error": str(error)}
        save()
        jobs.require(set(raw) == set(inputs), "same full-run original input missing")
        commands = check_registration(raw["registration.json"], build)
        root = ET.fromstring(raw["results.xml"])
        cases = [case for case in root.findall(".//testcase") if case.attrib.get("name") in REQUIRED]
        jobs.require(len(cases) == 4 and {case.attrib["name"] for case in cases} == set(REQUIRED),
                     "full JUnit quartet missing/duplicated")
        for case in cases:
            jobs.require(case.attrib.get("status") == "run" and not any(case.find(kind) is not None
                         for kind in ("failure", "error", "skipped")), "full JUnit source failed or skipped")
        sections = re.split(r"^\d+/\d+ Testing: ([^\r\n]+)\r?$", raw["LastTest.log"].decode("utf-8"), flags=re.M)
        details = {}
        for name, (stem, _) in REQUIRED.items():
            matched = [sections[index + 1] for index in range(1, len(sections), 2) if sections[index] == name]
            jobs.require(len(matched) == 1, "full native section missing or duplicated: " + name)
            details[name] = jobs.check_native(matched[0], commands[name], stem, platform_name)
        report.update(status="passed", details=details)
        save()
        return report
    except Exception as error:
        report.update(status="failed", error=str(error))
        save()
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("nt", "posix"), required=True)
    args = parser.parse_args()
    print(json.dumps(extract(args.build_dir, args.platform), ensure_ascii=True))


if __name__ == "__main__":
    main()
