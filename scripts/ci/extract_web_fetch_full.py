#!/usr/bin/env python3
"""Retain the same full Test's WebFetch pair; never execute native tests."""
import argparse
import hashlib
import json
import ntpath
import os
from pathlib import Path, PurePosixPath
import re
import xml.etree.ElementTree as ET

from check_sdk_focused import (
    check_web_fetch_native, check_web_fetch_registration, check_web_fetch_environment,
)

REQUIRED = {
    "sdk.focused.lubancore_web_fetch": ("lubancore_sdk_tests", 300),
    "integration.sdk.lubancore_web_fetch": ("lubancode_tests", 300),
}
SOURCE = "tests/integration/sdk/test_lubancore_web_fetch.cpp"
FILTER = "--source-file=*test_lubancore_web_fetch.cpp"
OUTPUT_FILES = ("context.json", "registration.json", "LastTest.log", "results.xml",
                "junit-unparsed.xml", "native-unscoped.log", "manifest.json")


def check_registration(name, command):
    if not isinstance(name, str) or name not in REQUIRED:
        raise RuntimeError("WebFetch registration has a foreign source name")
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(argument, str) and argument for argument in command) or
            any(any(byte in argument for byte in ("\0", "\n", "\r")) for argument in command)):
        raise RuntimeError("WebFetch registration requires two nonempty string arguments")
    normalized = [argument.replace("\\", "/") for argument in command]
    if not (PurePosixPath(normalized[0]).is_absolute() or ntpath.isabs(normalized[0])):
        raise RuntimeError("WebFetch registered executable must be absolute")
    check_web_fetch_registration(normalized, REQUIRED[name][0])
    return normalized


def check_source_native(name, section, platform, command):
    if platform not in ("nt", "posix") or not isinstance(section, str):
        raise RuntimeError("WebFetch full native platform or section is invalid")
    command = check_registration(name, command)
    if (re.search(r"^Test (?:Failed|Timeout|Not Run|Skipped)\b", section, re.M) or
            re.search(r"\bSKIP(?:PED)?\s*:|skipped test case", section, re.I)):
        raise RuntimeError("WebFetch full native source failed, timed out or skipped")
    check_web_fetch_native(section, command)
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    return {"nativeCases": 6, "nativeAssertions": int(assertions[0][0]), "command": command}


def _registered_commands(raw, build, platform_name):
    if platform_name not in ("nt", "posix"):
        raise RuntimeError("WebFetch full registration platform is invalid")
    try:
        document = json.loads(raw)
    except (ValueError, UnicodeError) as error:
        raise RuntimeError("WebFetch full registration JSON is invalid") from error
    tests = document.get("tests") if isinstance(document, dict) else None
    if (not isinstance(tests, list) or len(tests) != 2 or
            not all(isinstance(test, dict) and isinstance(test.get("name"), str) and
                    test["name"] in REQUIRED for test in tests) or
            {test["name"] for test in tests} != set(REQUIRED)):
        raise RuntimeError("WebFetch full registration source pair is missing or duplicated")
    commands = {}
    for test in tests:
        name = test["name"]
        command = check_registration(name, test.get("command"))
        if not Path(command[0]).resolve().is_relative_to(build):
            raise RuntimeError("WebFetch registered executable belongs to a foreign build path")
        properties = test.get("properties")
        if (not isinstance(properties, list) or not all(isinstance(item, dict) and
                isinstance(item.get("name"), str) and "value" in item for item in properties)):
            raise RuntimeError("WebFetch registration properties are invalid")
        props = {item["name"]: item["value"] for item in properties}
        if len(props) != len(properties) or props.get("DISABLED"):
            raise RuntimeError("WebFetch registration properties are duplicated or disabled")
        timeout = props.get("TIMEOUT")
        if type(timeout) not in (int, float) or timeout != REQUIRED[name][1]:
            raise RuntimeError("WebFetch registration changed its actual source timeout")
        check_web_fetch_environment(properties)
        commands[name] = command
    return commands


def extract(build, platform_name):
    build = Path(build).resolve()
    output = build / "test-evidence" / "web-fetch-full"
    output.mkdir(parents=True, exist_ok=True)
    # Only our prior derived files are replaced. Never remove a Test input.
    for name in OUTPUT_FILES:
        (output / name).unlink(missing_ok=True)
    context = {"githubSha": os.environ.get("GITHUB_SHA"), "platform": platform_name,
               "requiredTests": sorted(REQUIRED), "source": SOURCE, "inputs": {},
               "status": "collecting", "stage": "read-inputs"}
    sources = {"registration": build / "web-fetch-full-registration.json",
               "junit": build / "result-store-full-results.xml",
               "native": build / "Testing/Temporary/LastTest.log"}
    raw = {}

    def save_context():
        (output / "context.json").write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")

    def save_manifest():
        files = {}
        for name in OUTPUT_FILES:
            path = output / name
            if name == "manifest.json" or not path.is_file():
                continue
            data = path.read_bytes()
            files[name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        (output / "manifest.json").write_text(json.dumps({"status": context["status"],
            "stage": context["stage"], "files": files}, indent=2) + "\n", encoding="utf-8")

    save_context()
    try:
        context["sourceSha256"] = hashlib.sha256(
            (Path(__file__).resolve().parents[2] / SOURCE).read_bytes()).hexdigest()
        for key, path in sources.items():
            try:
                raw[key] = path.read_bytes()
                context["inputs"][key] = {"state": "value", "bytes": len(raw[key]),
                    "sha256": hashlib.sha256(raw[key]).hexdigest()}
            except OSError as error:
                context["inputs"][key] = {"state": "missing" if isinstance(error, FileNotFoundError)
                    else "io_error", "error": str(error)}
        context["stage"] = "retain-source"
        save_context()
        if "registration" in raw:
            (output / "registration.json").write_bytes(raw["registration"])
        sections = []
        if "native" in raw:
            matches = list(re.finditer(rb"^\d+/\d+ Testing: ([^\r\n]+)\r?$", raw["native"], re.M))
            names = {name.encode("ascii"): name for name in REQUIRED}
            for index, match in enumerate(matches):
                if match[1] in names:
                    end = matches[index + 1].start() if index + 1 < len(matches) else len(raw["native"])
                    sections.append((names[match[1]], raw["native"][match.start():end]))
            (output / "LastTest.log").write_bytes(b"".join(body for _, body in sections))
            if not sections:
                (output / "native-unscoped.log").write_bytes(raw["native"])
                context["nativeScope"] = "unscoped-original"
        cases = []
        junit_error = None
        if "junit" in raw:
            try:
                original = ET.fromstring(raw["junit"])
                cases = [case for case in original.findall(".//testcase") if case.attrib.get("name") in REQUIRED]
                subset = ET.Element("testsuite", tests=str(len(cases)))
                subset.extend(cases)
                ET.ElementTree(subset).write(output / "results.xml", encoding="utf-8", xml_declaration=True)
            except (ET.ParseError, ValueError) as error:
                junit_error = error
                (output / "junit-unparsed.xml").write_bytes(raw["junit"])
                context["junitScope"] = "unparsed-original"
        context["nativeSections"] = [name for name, _ in sections]
        context["junitTests"] = [case.attrib.get("name") for case in cases]
        save_context()
        save_manifest()  # Preserve failure/timeout/abort bytes before validating.
        if set(raw) != set(sources):
            raise RuntimeError("WebFetch full input is missing or unreadable")
        if junit_error is not None:
            raise RuntimeError("WebFetch full JUnit XML is invalid") from junit_error
        context["stage"] = "validate-registration"
        save_context()
        commands = _registered_commands(raw["registration"], build, platform_name)
        if (len(sections) != 2 or {name for name, _ in sections} != set(REQUIRED) or
                len(cases) != 2 or {case.attrib.get("name") for case in cases} != set(REQUIRED)):
            raise RuntimeError("WebFetch full native or JUnit source pair is missing or duplicated")
        context["stage"] = "validate-junit"
        save_context()
        for case in cases:
            if case.attrib.get("status") != "run" or any(case.find(kind) is not None
                    for kind in ("failure", "error", "skipped")):
                raise RuntimeError("WebFetch full JUnit reports failure or skip")
        context["stage"] = "validate-native"
        save_context()
        context["details"] = {name: check_source_native(name, body.decode("utf-8"),
            platform_name, commands[name]) for name, body in sections}
        context["status"], context["stage"] = "passed", "complete"
        save_context()
        save_manifest()
        return context
    except Exception as error:
        context["status"], context["error"] = "failed", str(error)
        save_context()
        save_manifest()
        raise


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=("nt", "posix"), required=True)
    args = parser.parse_args()
    report = extract(args.build_dir, args.platform)
    print(json.dumps({"status": report["status"], "tests": report["details"]}, ensure_ascii=True))


if __name__ == "__main__":
    main()
