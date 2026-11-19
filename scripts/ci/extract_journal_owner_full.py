#!/usr/bin/env python3
"""Retain and check the same full run's JournalOwner six-source group; never run native code."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import xml.etree.ElementTree as ET

try:
    from . import sdk_journal_owner as journal
except ImportError:
    import sdk_journal_owner as journal

ORIGINALS = {'unit.trajectory_v3.v3_journal_owner': 'v3_journal_owner',
             'integration.sdk.lubancore_journal_owner': 'lubancore_journal_owner',
             'integration.sdk.lubancore_journal_owner_guards': 'lubancore_journal_owner_guards'}
REQUIRED = {'sdk.focused.' + stem: (stem, 'lubancore_sdk_tests') for stem in journal.SOURCES}
REQUIRED.update({name: (stem, 'lubancode_tests') for name, stem in ORIGINALS.items()})


def source_timeout(name):
    # The original unit source retains its existing unit budget. Focused and
    # integration registrations keep their actual 300-second CMake allowance.
    return 180 if name == 'unit.trajectory_v3.v3_journal_owner' else 300


def check_registration(raw, build):
    value = json.loads(raw, object_pairs_hook=journal.commands.unique_object)
    tests = value.get("tests") if isinstance(value, dict) else None
    journal.require(isinstance(tests, list) and len(tests) == 6 and
                 all(isinstance(test, dict) and test.get("name") in REQUIRED for test in tests) and
                 {test["name"] for test in tests} == set(REQUIRED), "full registration six-source group differs")
    commands = {}
    for test in tests:
        name = test["name"]
        stem, executable = REQUIRED[name]
        command = test.get("command")
        journal.check_registration(command, stem, executable)
        journal.require(Path(command[0]).resolve().is_relative_to(Path(build).resolve()), "full executable escaped its build")
        properties = test.get("properties")
        journal.require(isinstance(properties, list) and all(isinstance(item, dict) and
                     isinstance(item.get("name"), str) and "value" in item for item in properties),
                     "full test properties malformed")
        props = {item["name"]: item["value"] for item in properties}
        journal.require(len(props) == len(properties) and not props.get("DISABLED") and
                     type(props.get("TIMEOUT")) in (int, float) and props["TIMEOUT"] == source_timeout(name),
                     "full source disabled or actual timeout changed")
        commands[name] = command
    return commands


def extract(build, platform_name):
    build = Path(build).resolve()
    journal.require(platform_name in ("nt", "posix"), "unknown full-run platform")
    output = build / "test-evidence" / "journal-owner-full"
    journal.require(output.resolve().is_relative_to(build), "derived full evidence escaped the build")
    output.mkdir(parents=True, exist_ok=True)
    for name in ("context.json", "registration.json", "results.xml", "LastTest.log"):
        (output / name).unlink(missing_ok=True)  # only our prior derived copies
    inputs = {"registration.json": build / "journal-owner-full-registration.json",
              "results.xml": build / "result-store-full-results.xml",
              "LastTest.log": build / "Testing/Temporary/LastTest.log"}
    report = {"githubSha": os.environ.get("GITHUB_SHA"), "platform": platform_name,
              "requiredTests": sorted(REQUIRED), "status": "collecting", "inputs": {},
              "scope": "JournalOwner original/focused six-source group from this full run; overall full CI is independently gated"}
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
        journal.require(set(raw) == set(inputs), "same full-run original input missing")
        commands = check_registration(raw["registration.json"], build)
        root = ET.fromstring(raw["results.xml"])
        cases = [case for case in root.findall(".//testcase") if case.attrib.get("name") in REQUIRED]
        journal.require(len(cases) == 6 and {case.attrib["name"] for case in cases} == set(REQUIRED),
                     "full JUnit six-source group missing/duplicated")
        for case in cases:
            journal.require(case.attrib.get("status") == "run" and not any(case.find(kind) is not None
                         for kind in ("failure", "error", "skipped")), "full JUnit source failed or skipped")
        sections = re.split(r"^\d+/\d+ Testing: ([^\r\n]+)\r?$", raw["LastTest.log"].decode("utf-8"), flags=re.M)
        details = {}
        for name, (stem, _) in REQUIRED.items():
            matched = [sections[index + 1] for index in range(1, len(sections), 2) if sections[index] == name]
            journal.require(len(matched) == 1, "full native section missing or duplicated: " + name)
            details[name] = journal.check_native(matched[0], commands[name], stem)
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
