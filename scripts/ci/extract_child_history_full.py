#!/usr/bin/env python3
"""Retain the actual full-test history sections; never run native tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import xml.etree.ElementTree as ET

REQUIRED = {
    "sdk.focused.child_history_adoption": ("lubancore_sdk_tests", 300),
    "unit.runtime.child_history_adoption": ("lubancode_tests", 180),
}
FILTER = "--source-file=*test_child_history_adoption.cpp"
CASE_IDS = ("complete", "post-hook", "historical-chain", "observation-gap",
            "source-gap", "artifact-gap", "adoption-gap", "scope-reuse")


def check_native(body):
    counts = re.findall(r'\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', body)
    assertions = re.findall(r'\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', body)
    if (counts != [("8", "8", "0")] or len(assertions) != 1
            or int(assertions[0][0]) <= 0 or assertions[0][0] != assertions[0][1]
            or int(assertions[0][2]) or "SKIP:" in body or "skipped test case" in body.lower()):
        raise RuntimeError("Full child-history source did not execute eight nonempty successful cases")
    lines = body.splitlines()
    detail = {}
    for case_id in CASE_IDS:
        if lines.count("[child-adoption-path] " + case_id) != 1:
            raise RuntimeError("Full child-history success path is missing or duplicated: " + case_id)
        prefix = "[child-history-stage] case=" + case_id + " rig="
        stages = [line[len(prefix):] for line in lines if line.startswith(prefix)]
        if not 0 < len(stages) <= 256 or stages.count("0 stage=case.enter") != 1 or stages.count("0 stage=case.leave") != 1:
            raise RuntimeError("Full child-history case diagnostics are missing, duplicated or unbounded: " + case_id)
        rig_ids = set()
        for value in stages:
            match = re.fullmatch(r'(\d+) stage=(case\.(?:enter|leave)|owners\.(?:before|after)|real-run\.(?:enter|leave)|check-history\.(?:enter|leave)|close\.(?:enter|leave))', value)
            if not match or len(prefix + value) >= 128:
                raise RuntimeError("Full child-history stage has a nonfixed shape")
            rig_id, point = int(match[1]), match[2]
            if (rig_id == 0) != point.startswith("case."):
                raise RuntimeError("Full child-history stage has the wrong owner ordinal")
            if rig_id:
                rig_ids.add(rig_id)
        if not rig_ids or stages[0] != "0 stage=case.enter" or stages[-1] != "0 stage=case.leave":
            raise RuntimeError("Full child-history case did not surround its owners")
        for rig_id in rig_ids:
            before, after = f"{rig_id} stage=owners.before", f"{rig_id} stage=owners.after"
            if stages.count(before) != 1 or stages.count(after) != 1 or stages.index(before) >= stages.index(after):
                raise RuntimeError("Full child-history owner retirement is missing or reversed")
            for point in ("real-run", "check-history", "close"):
                enter, leave = f"{rig_id} stage={point}.enter", f"{rig_id} stage={point}.leave"
                if ((point != "close" and stages.count(enter) == 0)
                        or stages.count(enter) != stages.count(leave)):
                    raise RuntimeError("Full child-history owner has an incomplete stage pair")
                if any(index >= stages.index(before) for index, value in enumerate(stages) if value in (enter, leave)):
                    raise RuntimeError("Full child-history work appears after owner retirement")
        detail[case_id] = {"rigOrdinals": sorted(rig_ids), "stageLines": len(stages)}
    return {"nativeCases": 8, "nativeAssertions": int(assertions[0][0]), "stages": detail}


def extract(build, platform_name):
    build = Path(build).resolve()
    output = build / "test-evidence" / "child-history-full"
    output.mkdir(parents=True, exist_ok=True)
    context = {"githubSha": os.environ.get("GITHUB_SHA"), "platform": platform_name,
               "requiredTests": sorted(REQUIRED), "inputs": {}, "status": "collecting"}
    repo = Path(__file__).resolve().parents[2]
    context["sourceSha256"] = hashlib.sha256((repo / "tests/unit/runtime/test_child_history_adoption.cpp").read_bytes()).hexdigest()
    sources = {"registration": build / "child-history-full-registration.json",
               # Reuse the same full Test JUnit output as the ResultStore gate.
               "junit": build / "result-store-full-results.xml",
               "native": build / "Testing/Temporary/LastTest.log"}
    raw = {}
    for key, path in sources.items():
        try:
            raw[key] = path.read_bytes()
            context["inputs"][key] = {"state": "value", "bytes": len(raw[key]), "sha256": hashlib.sha256(raw[key]).hexdigest()}
        except OSError as error:
            context["inputs"][key] = {"state": "missing" if isinstance(error, FileNotFoundError) else "io_error", "error": str(error)}

    def save():
        (output / "context.json").write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")

    save()
    try:
        if "registration" in raw:
            (output / "registration.json").write_bytes(raw["registration"])
        sections, cases = [], []
        if "native" in raw:
            names = {name.encode("ascii"): name for name in REQUIRED}
            matches = list(re.finditer(rb'^\d+/\d+ Testing: ([^\r\n]+)\r?$', raw["native"], re.M))
            for index, match in enumerate(matches):
                if match[1] in names:
                    end = matches[index + 1].start() if index + 1 < len(matches) else len(raw["native"])
                    sections.append((names[match[1]], raw["native"][match.start():end]))
            (output / "LastTest.log").write_bytes(b"".join(body for _, body in sections))
        if "junit" in raw:
            cases = [case for case in ET.fromstring(raw["junit"]).findall(".//testcase") if case.attrib.get("name") in REQUIRED]
            subset = ET.Element("testsuite", tests=str(len(cases)))
            subset.extend(cases)
            ET.ElementTree(subset).write(output / "results.xml", encoding="utf-8", xml_declaration=True)
        context["nativeSections"] = [name for name, _ in sections]
        context["junitTests"] = [case.attrib.get("name") for case in cases]
        save()  # Keep timeout/abort/failure bytes before validating success.
        if set(raw) != set(sources):
            raise RuntimeError("Full child-history input is missing or unreadable")
        tests = json.loads(raw["registration"])["tests"]
        if (len(tests) != 2 or {test["name"] for test in tests} != set(REQUIRED)
                or len(sections) != 2 or {name for name, _ in sections} != set(REQUIRED)
                or len(cases) != 2 or {case.attrib.get("name") for case in cases} != set(REQUIRED)):
            raise RuntimeError("Full child-history source pair is missing or duplicated")
        commands = {}
        for test in tests:
            binary, timeout = REQUIRED[test["name"]]
            command = test.get("command", [])
            if (len(command) != 2 or not isinstance(command[0], str)
                    or command[0].replace("\\", "/").rsplit("/", 1)[-1] not in (binary, binary + ".exe")
                    or command[1] != FILTER):
                raise RuntimeError("Full child-history registration has a wrong binary or source filter")
            props = {prop["name"]: prop["value"] for prop in test.get("properties", [])}
            if len(props) != len(test.get("properties", [])) or props.get("DISABLED") or props.get("TIMEOUT") != timeout:
                raise RuntimeError("Full child-history registration changed its timeout or disabled the source")
            commands[test["name"]] = [value.replace("\\", "/") for value in command]
        details = {}
        for name, original_body in sections:
            body = original_body.decode("utf-8")
            command_lines = re.findall(r'^Command: ([^\r\n]+)\r?$', body, re.M)
            if len(command_lines) != 1 or shlex.split(command_lines[0].replace("\\", "/")) != commands[name]:
                raise RuntimeError("Full child-history actual command differs from registration")
            case = next(case for case in cases if case.attrib["name"] == name)
            if case.attrib.get("status") != "run" or any(case.find(key) is not None for key in ("failure", "error", "skipped")):
                raise RuntimeError("Full child-history JUnit reports failure or skip")
            if body.splitlines().count("Test Passed.") != 1 or "Test Failed." in body:
                raise RuntimeError("Full child-history native source did not pass")
            details[name] = {**check_native(body), "command": commands[name]}
        context["status"], context["details"] = "passed", details
        save()
        return context
    except Exception as error:
        context["status"], context["error"] = "failed", str(error)
        save()
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("nt", "posix"), required=True)
    args = parser.parse_args()
    report = extract(args.build_dir, args.platform)
    print(json.dumps({"status": report["status"], "details": report["details"]}, ensure_ascii=True))


if __name__ == "__main__":
    main()
