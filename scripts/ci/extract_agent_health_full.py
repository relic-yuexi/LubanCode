#!/usr/bin/env python3
"""Retain and check AgentHealth's actual full-run source; never runs native tests."""
import argparse
import hashlib
import json
import ntpath
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import xml.etree.ElementTree as ET

from check_sdk_focused import check_native_command

NAME = "unit.runtime.agent_thread_lifetime"
SOURCE = "tests/unit/runtime/test_agent_thread_lifetime.cpp"
FILTER = "--source-file=*test_agent_thread_lifetime.cpp"
EXPECTED_CASES = 5
OUTPUT_FILES = ("context.json", "registration.json", "LastTest.log", "results.xml",
                "junit-unparsed.xml", "manifest.json")


def check_registration(command, executable="lubancode_tests"):
    """Validate the exact source argv, including an absolute executable path."""
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(argument, str) and argument for argument in command) or
            any("\x00" in argument or "\n" in argument or "\r" in argument for argument in command)):
        raise RuntimeError("AgentHealth registration command is not a two-string argument list")
    normalized = [argument.replace("\\", "/") for argument in command]
    if (not (PurePosixPath(normalized[0]).is_absolute() or ntpath.isabs(normalized[0])) or
            normalized[0].rsplit("/", 1)[-1] not in (executable, executable + ".exe") or
            normalized[1] != FILTER):
        raise RuntimeError("AgentHealth registration must select the absolute native binary and exact source")
    return normalized


def check_source_native(name, section, platform, registration_command=None):
    """Check one source section; optional argv pairs it with actual registration.

    Callers omitting registration_command must already have paired the section
    with their actual registration via check_native_command.
    """
    if name != NAME or platform not in ("nt", "posix") or not isinstance(section, str):
        raise RuntimeError("AgentHealth native source name, platform or section is invalid")
    commands = re.findall(r"^Command: ([^\r\n]*)\r?$", section, re.M)
    if len(commands) != 1:
        raise RuntimeError("AgentHealth native command is missing or duplicated")
    try:
        actual = shlex.split(commands[0].replace("\\", "/"))
    except ValueError as error:
        raise RuntimeError("AgentHealth native command has invalid quoting") from error
    check_registration(actual)
    if registration_command is not None:
        check_registration(registration_command)
        check_native_command(section, registration_command)
    if (section.splitlines().count("Test Passed.") != 1 or
            re.search(r"^Test (?:Failed|Timeout|Not Run|Skipped)\b", section, re.M) or
            re.search(r"\bSKIP(?:PED)?\s*:|skipped test case", section, re.I)):
        raise RuntimeError("AgentHealth native section failed, timed out or skipped")
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if cases != [(str(EXPECTED_CASES), str(EXPECTED_CASES), "0")]:
        raise RuntimeError("AgentHealth native source requires exactly five successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(assertions) != 1:
        raise RuntimeError("AgentHealth assertion summary is missing or duplicated")
    total, passed, failed = map(int, assertions[0])
    if total <= 0 or total != passed or failed:
        raise RuntimeError("AgentHealth assertions are empty or failed")
    return {"nativeCases": EXPECTED_CASES, "nativeAssertions": total}


def _registered_command(raw):
    try:
        tests = json.loads(raw).get("tests")
    except (AttributeError, ValueError, UnicodeError) as error:
        raise RuntimeError("AgentHealth registration JSON is invalid") from error
    if (not isinstance(tests, list) or len(tests) != 1 or not isinstance(tests[0], dict) or
            tests[0].get("name") != NAME):
        raise RuntimeError("AgentHealth registration source is missing, foreign or duplicated")
    test = tests[0]
    command = check_registration(test.get("command"))
    properties = test.get("properties", [])
    if (not isinstance(properties, list) or
            any(not isinstance(prop, dict) or not isinstance(prop.get("name"), str) or
                "value" not in prop for prop in properties)):
        raise RuntimeError("AgentHealth registration properties are invalid")
    props = {prop["name"]: prop["value"] for prop in properties}
    if len(props) != len(properties) or props.get("DISABLED"):
        raise RuntimeError("AgentHealth registration is duplicated or disabled")
    timeout = props.get("TIMEOUT")
    if isinstance(timeout, bool):
        raise RuntimeError("AgentHealth registration must retain its actual 180-second timeout")
    try:
        timeout = float(timeout)
    except (TypeError, ValueError) as error:
        raise RuntimeError("AgentHealth registration timeout is invalid") from error
    if timeout != 180:
        raise RuntimeError("AgentHealth registration must retain its actual 180-second timeout")
    return command


def extract(build, platform_name):
    build = Path(build).resolve()
    output = build / "test-evidence" / "agent-health-full"
    output.mkdir(parents=True, exist_ok=True)
    # Replace only this helper's prior derived files; never remove an input.
    for name in OUTPUT_FILES:
        (output / name).unlink(missing_ok=True)
    context = {"githubSha": os.environ.get("GITHUB_SHA"), "platform": platform_name,
               "requiredTests": [NAME], "source": SOURCE, "inputs": {},
               "status": "collecting", "stage": "read-inputs"}
    sources = {"registration": build / "agent-health-full-registration.json",
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
        (output / "manifest.json").write_text(json.dumps({
            "status": context["status"], "stage": context["stage"], "files": files}, indent=2) + "\n",
            encoding="utf-8")

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
                context["inputs"][key] = {
                    "state": "missing" if isinstance(error, FileNotFoundError) else "io_error", "error": str(error)}
        context["stage"] = "retain-source"
        save_context()
        if "registration" in raw:
            (output / "registration.json").write_bytes(raw["registration"])
        sections = []
        if "native" in raw:
            matches = list(re.finditer(rb"^\d+/\d+ Testing: ([^\r\n]+)\r?$", raw["native"], re.M))
            for index, match in enumerate(matches):
                if match[1] == NAME.encode("ascii"):
                    end = matches[index + 1].start() if index + 1 < len(matches) else len(raw["native"])
                    sections.append(raw["native"][match.start():end])
            (output / "LastTest.log").write_bytes(b"".join(sections))
        cases = []
        junit_error = None
        if "junit" in raw:
            try:
                original = ET.fromstring(raw["junit"])
                cases = [case for case in original.findall(".//testcase") if case.attrib.get("name") == NAME]
                subset = ET.Element("testsuite", tests=str(len(cases)))
                for case in cases:
                    subset.append(case)
                ET.ElementTree(subset).write(output / "results.xml", encoding="utf-8", xml_declaration=True)
            except (ET.ParseError, ValueError) as error:
                junit_error = error
                # Malformed XML cannot be scoped. Retain its original bytes and say so.
                (output / "junit-unparsed.xml").write_bytes(raw["junit"])
                context["junitScope"] = "unparsed-original"
        context["nativeSections"] = len(sections)
        context["junitTests"] = len(cases)
        save_context()
        save_manifest()
        if set(raw) != set(sources):
            raise RuntimeError("AgentHealth full input is missing or unreadable")
        if junit_error is not None:
            raise RuntimeError("AgentHealth full JUnit XML is invalid") from junit_error
        context["stage"] = "validate-registration"
        save_context()
        command = _registered_command(raw["registration"])
        if not Path(command[0]).resolve().is_relative_to(build):
            raise RuntimeError("AgentHealth registered executable belongs to a foreign build path")
        if len(sections) != 1 or len(cases) != 1:
            raise RuntimeError("AgentHealth full native or JUnit source is missing or duplicated")
        context["stage"] = "validate-junit"
        save_context()
        case = cases[0]
        if case.attrib.get("status") != "run" or any(
                case.find(kind) is not None for kind in ("failure", "error", "skipped")):
            raise RuntimeError("AgentHealth full JUnit reports failure or skip")
        context["stage"] = "validate-native"
        save_context()
        details = check_source_native(NAME, sections[0].decode("utf-8"), platform_name, command)
        context["details"] = {NAME: {**details, "command": command}}
        context["status"] = "passed"
        context["stage"] = "complete"
        save_context()
        save_manifest()
        return context
    except Exception as error:
        context["status"] = "failed"
        context["error"] = str(error)
        save_context()
        save_manifest()
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
