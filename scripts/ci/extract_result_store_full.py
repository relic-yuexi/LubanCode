#!/usr/bin/env python3
"""Retain the two actual full-test sections; never runs native tests."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import xml.etree.ElementTree as ET

from check_sdk_focused import check_result_store_native, check_result_store_owned_roots

REQUIRED = {
    "sdk.focused.v3_result_store": "lubancore_sdk_tests",
    "unit.trajectory_v3.v3_result_store": "lubancode_tests",
}
FILTER = "--source-file=*test_v3_result_store.cpp"


def extract(build, platform_name):
    build = Path(build).resolve()
    output = build / "test-evidence" / "result-store-full"
    output.mkdir(parents=True, exist_ok=True)
    context = {"githubSha": os.environ.get("GITHUB_SHA"), "platform": platform_name,
               "requiredTests": sorted(REQUIRED), "inputs": {}, "status": "collecting"}
    repo = Path(__file__).resolve().parents[2]
    context["sourceSha256"] = hashlib.sha256(
        (repo / "tests/unit/trajectory_v3/test_v3_result_store.cpp").read_bytes()).hexdigest()
    sources = {"registration": build / "result-store-full-registration.json",
               "junit": build / "result-store-full-results.xml",
               "native": build / "Testing/Temporary/LastTest.log"}
    raw = {}
    for key, path in sources.items():
        try:
            raw[key] = path.read_bytes()
            context["inputs"][key] = {"state": "value", "bytes": len(raw[key]),
                                      "sha256": hashlib.sha256(raw[key]).hexdigest()}
        except OSError as error:
            context["inputs"][key] = {"state": "missing" if isinstance(error, FileNotFoundError) else "io_error",
                                      "error": str(error)}
    def save_context():
        (output / "context.json").write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    save_context()
    try:
        # Scoped registration is an original CTest result, not reconstructed argv.
        if "registration" in raw:
            (output / "registration.json").write_bytes(raw["registration"])
        sections = []
        if "native" in raw:
            text = raw["native"]
            names = {name.encode("ascii"): name for name in REQUIRED}
            matches = list(re.finditer(rb'^\d+/\d+ Testing: ([^\r\n]+)\r?$', text, re.M))
            for index, match in enumerate(matches):
                if match[1] in names:
                    end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
                    sections.append((names[match[1]], text[match.start():end]))
            (output / "LastTest.log").write_bytes(b"".join(body for _, body in sections))
        cases = []
        if "junit" in raw:
            original = ET.fromstring(raw["junit"])
            cases = [case for case in original.findall(".//testcase") if case.attrib.get("name") in REQUIRED]
            subset = ET.Element("testsuite", tests=str(len(cases)))
            for case in cases:
                subset.append(case)
            ET.ElementTree(subset).write(output / "results.xml", encoding="utf-8", xml_declaration=True)
        context["nativeSections"] = [name for name, _ in sections]
        context["junitTests"] = [case.attrib.get("name") for case in cases]
        save_context()  # Retain failed/missing native originals before validation.
        if set(raw) != set(sources):
            raise RuntimeError("Full result-store pair input is missing or unreadable")
        tests = json.loads(raw["registration"])["tests"]
        if (len(tests) != 2 or {t["name"] for t in tests} != set(REQUIRED)
                or len(sections) != 2 or {name for name, _ in sections} != set(REQUIRED)
                or len(cases) != 2 or {c.attrib.get("name") for c in cases} != set(REQUIRED)):
            raise RuntimeError("Full result-store pair is missing or duplicated")
        commands = {}
        for test in tests:
            command = test.get("command", [])
            binary = REQUIRED[test["name"]]
            if (len(command) != 2 or not isinstance(command[0], str)
                    or command[0].replace("\\", "/").rsplit("/", 1)[-1] not in (binary, binary + ".exe")
                    or command[1] != FILTER):
                raise RuntimeError("Full result-store pair registration has a wrong binary or filter")
            props = {p["name"]: p["value"] for p in test.get("properties", [])}
            if (len(props) != len(test.get("properties", [])) or props.get("DISABLED")
                    or not 0 < float(props.get("TIMEOUT", 0)) <= 300):
                raise RuntimeError("Full result-store pair is disabled or unbounded")
            commands[test["name"]] = [value.replace("\\", "/") for value in command]
        details, roots = {}, []
        for name, original_body in sections:
            body = original_body.decode("utf-8")
            command_lines = re.findall(r'^Command: ([^\r\n]+)\r?$', body, re.M)
            if len(command_lines) != 1 or shlex.split(command_lines[0].replace("\\", "/")) != commands[name]:
                raise RuntimeError("Full result-store actual command differs from registration")
            case = next(c for c in cases if c.attrib["name"] == name)
            if case.attrib.get("status") != "run" or any(case.find(k) is not None for k in ("failure", "error", "skipped")):
                raise RuntimeError("Full result-store JUnit reports a failure or skip")
            if body.splitlines().count("Test Passed.") != 1 or "Test Failed." in body:
                raise RuntimeError("Full result-store native section did not pass")
            check_result_store_native(body, platform_name)
            counts = re.findall(r'\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed', body)
            if len(counts) != 1 or int(counts[0][0]) <= 0 or counts[0][0] != counts[0][1] or int(counts[0][2]):
                raise RuntimeError("Full result-store native assertions are empty or failed")
            owned = check_result_store_owned_roots(body, platform_name)
            roots.extend(owned)
            details[name] = {"nativeCases": 17, "nativeAssertions": int(counts[0][0]),
                             "command": commands[name], "ownedRoots": owned}
        if len(roots) != len(set(roots)):
            raise RuntimeError("Full result-store programs reused an owned root")
        context["status"] = "passed"
        context["details"] = details
        save_context()
        return context
    except Exception as error:
        context["status"] = "failed"
        context["error"] = str(error)
        save_context()
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("nt", "posix"), required=True)
    args = parser.parse_args()
    context = extract(args.build_dir, args.platform)
    print(json.dumps({"status": context["status"], "tests": context["details"]}, ensure_ascii=True))


if __name__ == "__main__":
    main()
