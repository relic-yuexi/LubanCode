#!/usr/bin/env python3
"""Run all SDK test files and reject missing, skipped or empty native tests."""

import argparse
import json
import ntpath
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import xml.etree.ElementTree as ET


REQUIRED = {
    "sdk.focused.package_manifest", "sdk.focused.lubancore_package_manifest",
    "sdk.focused.tool_job_coordinator",
    "sdk.focused.tool_job_start_transaction",
    "sdk.focused.tool_job_hold_recovery",
    "sdk.focused.owned_job_admission",
    "sdk.focused.session_recovery_view",
    "sdk.focused.lubancore_recovery_view",
    "sdk.focused.lubancore_session",
    "sdk.focused.lubancore_scoped_approval",
    "sdk.focused.lubancore_child_approval",
    "sdk.focused.lubancore_subagents",
    "sdk.focused.lubancore_lua",
    "sdk.focused.lua_protected",
    "sdk.focused.lubancore_actions",
    "sdk.focused.lubancore_builtin_search",
    "sdk.focused.lubancore_lifecycle",
    "sdk.focused.lubancore_host_boundary",
    "sdk.focused.lubancore_history",
    "sdk.focused.lubancore_extensions",
    "sdk.focused.lubancore_skills",
    "sdk.focused.lubancore_memory_recall",
    "sdk.focused.lubancore_memory_cas",
    "sdk.focused.lubancore_memory_save",
    "sdk.focused.lubancore_results",
    "sdk.focused.lubancore_result_projection",
    "sdk.focused.session_resources",
    "sdk.focused.session_execution",
    "sdk.focused.execution_owner",
    "sdk.focused.subagent_terminal_receipt",
    "sdk.focused.child_foreground_integration",
    "sdk.focused.child_parent_observation",
    "sdk.focused.child_history_adoption",
    "sdk.focused.scoped_turn_bindings",
    "sdk.focused.atomic_write",
    "sdk.focused.v3_result_store",
}

LUA_PATHS = ("off-and-visible", "four-sessions", "approval", "cancel-and-close",
             "invalid-budget", "bad-declarations", "resume-fresh-vm", "resume-drift", "owned-opening")


def check_native_command(section: str, registered: list[str]):
    if (not isinstance(registered, list) or not registered or
            not all(isinstance(argument, str) for argument in registered) or not registered[0]):
        raise RuntimeError("Native source lacks a valid registered command")
    commands = re.findall(r"^Command: ([^\r\n]*)\r?$", section, flags=re.M)
    if len(commands) != 1:
        raise RuntimeError("Native source command is missing or duplicated")
    try:
        actual = shlex.split(commands[0].replace("\\", "/"))
    except ValueError as error:
        raise RuntimeError("Native source command is not a valid argument list") from error
    if actual != [argument.replace("\\", "/") for argument in registered]:
        raise RuntimeError("Native source command differs from the registered path or arguments")


def check_lua_native(section, *, protected=False, executable="lubancore_sdk_tests"):
    source = "test_lua_protected.cpp" if protected else "test_lubancore_lua.cpp"
    expected = 6 if protected else len(LUA_PATHS)
    commands = re.findall(r"^Command: ([^\r\n]+)\r?$", section, flags=re.M)
    if len(commands) != 1:
        raise RuntimeError("Lua native source command is missing or duplicated")
    command = shlex.split(commands[0].replace("\\", "/"))
    if (len(command) != 2 or command[0].rsplit("/", 1)[-1] not in (executable, executable + ".exe")
            or command[1] != "--source-file=*" + source):
        raise RuntimeError("Lua native command selects a different source or binary")
    cases = re.findall(r"\[doctest\] test cases:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if cases != [(str(expected), str(expected), "0")]:
        raise RuntimeError("Lua native roster was empty, skipped or failed")
    assertions = re.findall(r"\[doctest\] assertions:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(assertions) != 1:
        raise RuntimeError("Lua native assertion summary is missing or duplicated")
    total, passed, failed = map(int, assertions[0])
    if not total or total != passed or failed or section.count("Test Passed.") != 1:
        raise RuntimeError("Lua native assertions did not pass")
    if not protected:
        for path in LUA_PATHS:
            if section.splitlines().count("[sdk-lua-path] " + path) != 1:
                raise RuntimeError("Lua actual Session path did not finish once: " + path)


def check_package_registration(command: list, source: str, executable: str):
    if (len(command) != 2 or not isinstance(command[0], str) or
            command[0].replace("\\", "/").rsplit("/", 1)[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*" + source):
        raise RuntimeError("Package command must select exactly the registered native source")


def check_package_native(native_section: str, expected: int, registered_command: list):
    source = "test_package_manifest.cpp" if expected == 15 else "test_lubancore_package_manifest.cpp"
    if (expected not in (15, 8) or len(registered_command) != 2 or
            not isinstance(registered_command[0], str) or
            not all(isinstance(argument, str) for argument in registered_command)):
        raise RuntimeError("Package actual command lacks a valid registered source")
    executable = registered_command[0].replace("\\", "/").rsplit("/", 1)[-1].removesuffix(".exe")
    if executable not in ("lubancode_tests", "lubancore_sdk_tests"):
        raise RuntimeError("Package actual command lacks the native test executable")
    check_package_registration(registered_command, source, executable)
    commands = re.findall(r"^Command: ([^\r\n]+)\r?$", native_section, flags=re.M)
    if len(commands) != 1:
        raise RuntimeError("Package actual command is missing or duplicated")
    actual_command = shlex.split(commands[0].replace("\\", "/"))
    if actual_command != [argument.replace("\\", "/") for argument in registered_command]:
        raise RuntimeError("Package actual command differs from the registered path or arguments")
    cases = re.findall(r"\[doctest\] test cases:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", native_section)
    assertions = re.findall(r"\[doctest\] assertions:\s+(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", native_section)
    if cases != [(str(expected), str(expected), "0")] or len(assertions) != 1:
        raise RuntimeError("Package native source roster differs from required passing cases")
    total, passed, failed = map(int, assertions[0])
    if not total or total != passed or failed:
        raise RuntimeError("Package native assertions are empty or failed")


ACTION_PATHS = ("chain", "deny", "rewrite-schema", "force-ask", "post-failure",
                "limits", "opening", "resume", "close", "isolation")


def check_action_paths(section: str, *, native: bool):
    lines = section.splitlines()
    for path in ACTION_PATHS:
        if lines.count("[sdk-action-path] " + path) != 1:
            raise RuntimeError("Action actual public path did not finish once: " + path)
    if not native:
        return
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(counts) != 1 or tuple(map(int, counts[0])) != (10, 10, 0):
        raise RuntimeError("Action native roster differs from 10 successful cases")
    for path in ("existing-permission-chain", "summary-stop", "receipt-stop", "binding-opening"):
        if lines.count("[sdk-action-native] " + path) != 1:
            raise RuntimeError("Action actual internal path did not finish once: " + path)


def check_result_store_owned_roots(native_section: str, platform_name: str):
    if platform_name != "nt":
        return []
    prefix = "[result-store-fixture] "
    lines = [line for line in native_section.splitlines() if line.startswith(prefix)]
    if len(lines) != 2:
        raise RuntimeError("Result-store owned cleanup records are missing or duplicated")
    records = []
    for line in lines:
        try:
            record = json.loads(line[len(prefix):])
        except (ValueError, TypeError) as error:
            raise RuntimeError("Result-store owned cleanup record is invalid") from error
        if (not isinstance(record, dict) or set(record) != {"marker", "root", "cleanup"}
                or record["cleanup"] != "removed" or not isinstance(record["root"], str)
                or not record["root"] or "\0" in record["root"]):
            raise RuntimeError("Result-store owned cleanup record is invalid")
        records.append(record)
    if {r["marker"] for r in records} != {"target-extended", "temporary-threshold"}:
        raise RuntimeError("Result-store owned cleanup source markers are wrong")
    roots = [ntpath.normcase(ntpath.normpath(r["root"])) for r in records]
    if len(set(roots)) != len(roots):
        raise RuntimeError("Result-store owned roots were reused")
    return roots


def check_result_store_native(native_section: str, platform_name: str):
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", native_section)
    if len(counts) != 1 or tuple(map(int, counts[0])) != (17, 17, 0):
        raise RuntimeError("Result-store native roster differs from 17 successful cases")
    if platform_name != "nt":
        return
    for name, target, temporary in (("target-extended", 340, 344), ("temporary-threshold", 247, 251)):
        marker = "[result-store-path] " + name
        lengths = f"[result-store-path-length] {name} target={target} temporary={temporary}"
        if native_section.splitlines().count(marker) != 1 or native_section.splitlines().count(lengths) != 1:
            raise RuntimeError("Result-store actual Windows path did not finish once: " + name)
    check_result_store_owned_roots(native_section, platform_name)


def check_memory_cas_paths(native_section: str, platform_name: str):
    if platform_name != "nt":
        return
    for path in ("target-extended", "temporary-threshold"):
        marker = "[memory-cas-path] " + path
        if native_section.splitlines().count(marker) != 1:
            raise RuntimeError("Memory CAS actual Windows path did not finish once: " + path)


RECOVERY_PATHS = {
    "session_recovery_view": ("native-byte-bounds", "metadata-roster-bounds", "immutable-main-reference", "strict-operation-order",
                              "native-existing-prefix", "owned-projection-append", "bounded-preflight-cli-unset", "cli-real-resumed-model"),
    "lubancore_recovery_view": ("public-budget-raise", "locked-memory-owned-view", "completed-versus-partial", "four-session-isolation"),
}


def check_recovery_source(name: str, native_section: str, count: int):
    stem = name.rsplit(".", 1)[-1]
    if stem not in RECOVERY_PATHS:
        return
    paths = RECOVERY_PATHS[stem]
    if count != len(paths):
        raise RuntimeError("Recovery native roster differs from actual source: " + name)
    filters = re.findall(r'--source-file=([^"\s]+)', native_section)
    if filters != ["*test_" + stem + ".cpp"]:
        raise RuntimeError("Recovery native command does not identify the exact source: " + name)
    prefix = "[session-recovery-path] " if stem == "session_recovery_view" else "[sdk-recovery-path] "
    for path in paths:
        if native_section.splitlines().count(prefix + path) != 1:
            raise RuntimeError("Recovery actual path did not finish once: " + name + ":" + path)


PLAN_RETRY_PATHS = ("retry-success", "permanent-stop", "committed-stop", "attempt-budget",
                    "deadline-budget", "native-write")


def check_plan_retry_registration(command, executable="lubancore_sdk_tests"):
    if (len(command) != 2 or not isinstance(command[0], str) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_atomic_write.cpp"):
        raise RuntimeError("SDK plan retry must run the single actual atomic-write source")


def check_plan_retry_native(native_section: str, platform_name: str, executable="lubancore_sdk_tests"):
    commands = re.findall(r"^Command: ([^\r\n]+)$", native_section, flags=re.M)
    if len(commands) != 1:
        raise RuntimeError("SDK plan retry native log must identify one actual command")
    check_plan_retry_registration(shlex.split(commands[0].replace("\\", "/")), executable)
    expected = 25 if platform_name == "nt" else 22
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", native_section)
    if len(counts) != 1 or tuple(map(int, counts[0])) != (expected, expected, 0):
        raise RuntimeError(f"SDK plan retry native roster differs from {expected} successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", native_section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            native_section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("SDK plan retry native assertions did not actually pass")
    paths = (*PLAN_RETRY_PATHS, "windows-sharing-recovery") if platform_name == "nt" else PLAN_RETRY_PATHS
    for path in paths:
        if native_section.splitlines().count("[sdk-plan-retry] " + path) != 1:
            raise RuntimeError("SDK plan retry actual path did not finish once: " + path)
    if platform_name != "nt" and "[sdk-plan-retry] windows-sharing-recovery" in native_section.splitlines():
        raise RuntimeError("SDK plan retry POSIX evidence cannot claim a Windows sharing probe")


JOB_START_PATHS = ("executor-copy", "before-thread", "after-thread", "queued-successor",
                   "unconfirmed-terminal", "capture-close")
JOB_HOLD_PATHS = ("registered-admission", "new-job-isolation", "dispatched-unknown",
                  "terminal-facts", "async-propagation", "legacy-and-owner")
OWNED_JOB_PATHS = ("owned-snapshot", "shared-denials", "ask-cancel-retire",
                   "deferred-capability", "loop-no-fallback", "receipt-unknown-stop")


def check_owned_job_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_owned_job_admission.cpp"):
        raise RuntimeError("Owned Job admission must run the single actual source")


def check_owned_job_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Owned Job admission native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Owned Job admission executable is not the actual native fixture")
    check_owned_job_registration(command, executable)
    check_native_command(section, command)
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(counts) != 1 or tuple(map(int, counts[0])) != (6, 6, 0):
        raise RuntimeError("Owned Job admission native roster differs from 6 successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Owned Job admission native assertions did not actually pass")
    for path in OWNED_JOB_PATHS:
        if section.splitlines().count("[owned-job-path] " + path) != 1:
            raise RuntimeError("Owned Job admission actual path did not finish once: " + path)


def check_job_start_registration(command, source, executable="lubancore_sdk_tests"):
    if source not in ("test_tool_job_coordinator.cpp", "test_tool_job_start_transaction.cpp", "test_tool_job_hold_recovery.cpp"):
        raise RuntimeError("unexpected Job startup source")
    if (len(command) != 2 or not isinstance(command[0], str) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*" + source):
        raise RuntimeError("Job startup must run the single actual source")


def check_job_start_native(section, command, original=False):
    source = "test_tool_job_coordinator.cpp" if original else "test_tool_job_start_transaction.cpp"
    _check_job_native(section, command, source, 16 if original else 6,
                      () if original else JOB_START_PATHS, "job-start-path")


def check_job_hold_native(section, command):
    _check_job_native(section, command, "test_tool_job_hold_recovery.cpp", 6,
                      JOB_HOLD_PATHS, "job-hold-path")


def _check_job_native(section, command, source, expected, paths, marker_prefix):
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe") if command else ""
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Job startup executable is not the actual native fixture")
    check_job_start_registration(command, source, executable)
    lines = re.findall(r"^Command:\s*(.+)$", section, flags=re.M)
    if len(lines) != 1 or shlex.split(lines[0].replace("\\", "/")) != [value.replace("\\", "/") for value in command]:
        raise RuntimeError("Job startup native command differs from full registration argv")
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(counts) != 1 or tuple(map(int, counts[0])) != (expected, expected, 0):
        raise RuntimeError("Job startup native roster differs from its successful source")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(assertions) != 1 or int(assertions[0][0]) <= 0 or int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0:
        raise RuntimeError("Job startup assertions are empty or failed")
    for path in paths:
        if section.splitlines().count("[" + marker_prefix + "] " + path) != 1:
            raise RuntimeError("Job actual path did not finish once: " + path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--sdk-only", action="store_true")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    evidence = build / "test-evidence" / "sdk-focused"
    evidence.mkdir(parents=True, exist_ok=True)
    command = ["ctest", "--test-dir", str(build), "-C", args.config]
    if not args.sdk_only:
        command += ["-L", "^sdk-focused$"]
    registration_command = command + ["--show-only=json-v1"]
    listed = subprocess.run(registration_command, capture_output=True)
    (evidence / "registration.stdout").write_bytes(listed.stdout)
    (evidence / "registration.stderr").write_bytes(listed.stderr)
    (evidence / "registration-result.json").write_text(json.dumps({
        "command": registration_command, "returncode": listed.returncode,
        "githubSha": os.environ.get("GITHUB_SHA"),
    }, indent=2) + "\n", encoding="utf-8")
    (evidence / "tests.json").write_bytes(listed.stdout)
    listed.check_returncode()
    tests = json.loads(listed.stdout)["tests"]
    if len(tests) != len(REQUIRED) or {t["name"] for t in tests} != REQUIRED:
        raise RuntimeError("SDK test files are missing, duplicated or unexpected")
    for test in tests:
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        if (props.get("DISABLED") or "sdk-focused" not in props.get("LABELS", [])
                or not 0 < float(props.get("TIMEOUT", 0)) <= 300):
            raise RuntimeError("SDK test is disabled, mislabeled or unbounded: " + test["name"])
        if test["name"] in ("sdk.focused.tool_job_coordinator", "sdk.focused.tool_job_start_transaction", "sdk.focused.tool_job_hold_recovery"):
            source = "test_" + test["name"].removeprefix("sdk.focused.") + ".cpp"
            check_job_start_registration(test.get("command", []), source)
        if test["name"] == "sdk.focused.owned_job_admission":
            check_owned_job_registration(test.get("command", []))
        if test["name"] == "sdk.focused.atomic_write":
            check_plan_retry_registration(test.get("command", []))
        if test["name"] in ("sdk.focused.package_manifest", "sdk.focused.lubancore_package_manifest"):
            source = "test_package_manifest.cpp" if test["name"] == "sdk.focused.package_manifest" else "test_lubancore_package_manifest.cpp"
            check_package_registration(test.get("command", []), source, "lubancore_sdk_tests")
    (evidence / "context.json").write_text(json.dumps({
        "githubSha": os.environ.get("GITHUB_SHA"), "buildDir": str(build),
        "configuration": args.config, "sdkOnly": args.sdk_only,
        "requiredTests": sorted(REQUIRED),
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
        raise RuntimeError("JUnit does not cover every SDK test file")
    # Successful JUnit output can be truncated before the doctest summary.
    native_sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$',
        (evidence / "LastTest.log").read_text(encoding="utf-8"), flags=re.M)
    for case in cases:
        if case.attrib.get("status") != "run" or any(case.find(k) is not None for k in
                ("failure", "error", "skipped")):
            raise RuntimeError("SDK test was skipped or failed: " + case.attrib["name"])
        sections = [native_sections[index + 1] for index in range(1, len(native_sections), 2)
                    if native_sections[index] == case.attrib["name"]]
        if len(sections) != 1:
            raise RuntimeError("Native log does not identify one SDK source: " + case.attrib["name"])
        commands = [test.get("command", []) for test in tests if test["name"] == case.attrib["name"]]
        if len(commands) != 1:
            raise RuntimeError("SDK source has no unique registered command: " + case.attrib["name"])
        check_native_command(sections[0], commands[0])
        counts = re.findall(r"\[doctest\] test cases:\s+(\d+)", sections[0])
        if len(counts) != 1 or int(counts[0]) == 0:
            raise RuntimeError("SDK source filter ran no native test cases: " + case.attrib["name"])
        if case.attrib["name"] == "sdk.focused.package_manifest":
            registered = next(test["command"] for test in tests if test["name"] == case.attrib["name"])
            check_package_native(sections[0], 15, registered)
        if case.attrib["name"] == "sdk.focused.lubancore_package_manifest":
            registered = next(test["command"] for test in tests if test["name"] == case.attrib["name"])
            check_package_native(sections[0], 8, registered)
        if case.attrib["name"] in ("sdk.focused.tool_job_coordinator", "sdk.focused.tool_job_start_transaction"):
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_job_start_native(sections[0], registered["command"],
                case.attrib["name"] == "sdk.focused.tool_job_coordinator")
        if case.attrib["name"] == "sdk.focused.tool_job_hold_recovery":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_job_hold_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.owned_job_admission":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_owned_job_native(sections[0], registered["command"])
        check_recovery_source(case.attrib["name"], sections[0], int(counts[0]))
        if case.attrib["name"] == "sdk.focused.v3_result_store":
            check_result_store_native(sections[0], os.name)
        if case.attrib["name"] == "sdk.focused.atomic_write":
            check_plan_retry_native(sections[0], os.name)
        if case.attrib["name"] in ("sdk.focused.lubancore_lua", "sdk.focused.lua_protected"):
            check_lua_native(sections[0], protected=case.attrib["name"] == "sdk.focused.lua_protected")
        if case.attrib["name"] == "sdk.focused.lubancore_memory_cas" and int(counts[0]) != 10:
            raise RuntimeError("Memory CAS native roster differs from 10 cases")
        if case.attrib["name"] == "sdk.focused.lubancore_memory_cas":
            check_memory_cas_paths(sections[0], os.name)
        if case.attrib["name"] == "sdk.focused.lubancore_memory_save" and int(counts[0]) != 12:
            raise RuntimeError("SDK memory-save native roster differs from 12 cases")
        if case.attrib["name"] == "sdk.focused.execution_owner" and int(counts[0]) != 7:
            raise RuntimeError("Shared execution-owner native roster differs from 7 cases")
        if case.attrib["name"] == "sdk.focused.subagent_terminal_receipt" and int(counts[0]) != 7:
            raise RuntimeError("Child terminal receipt native roster differs from 7 cases")
        if case.attrib["name"] == "sdk.focused.subagent_terminal_receipt":
            for path in ("cancel", "budget", "success", "close-failed", "child-unknown"):
                marker = "[child-terminal-path] foreground." + path
                if sections[0].splitlines().count(marker) != 1:
                    raise RuntimeError("Child terminal actual foreground path did not finish once: " + path)
        if case.attrib["name"] == "sdk.focused.child_foreground_integration":
            if int(counts[0]) != 2:
                raise RuntimeError("Child foreground integration roster differs from 2 cases")
            for path in ("isolation", "cancel-close-failed"):
                marker = "[child-integration-path] " + path
                if sections[0].splitlines().count(marker) != 1:
                    raise RuntimeError("Child integration actual path did not finish once: " + path)
        if case.attrib["name"] == "sdk.focused.lubancore_scoped_approval" and int(counts[0]) != 14:
            raise RuntimeError("Scoped approval native roster differs from 14 cases")
        if case.attrib["name"] == "sdk.focused.lubancore_child_approval" and int(counts[0]) != 14:
            raise RuntimeError("Actual child approval native roster differs from 14 cases")
        if case.attrib["name"] == "sdk.focused.lubancore_subagents" and int(counts[0]) != 12:
            raise RuntimeError("Public SDK child assembly native roster differs from 12 cases")
        if case.attrib["name"] == "sdk.focused.lubancore_actions":
            check_action_paths(sections[0], native=True)
        if case.attrib["name"] == "sdk.focused.child_parent_observation":
            if int(counts[0]) != 8:
                raise RuntimeError("Child parent observation roster differs from 8 cases")
            for path in ("adopted", "observation-unknown", "summary-halted", "summary-healthy",
                         "capture-unknown", "rewrite-unknown", "commit-unknown", "rewrite-exception",
                         "mid-batch-summary-halted", "cancel-close-failed", "source-gap", "ledger-summary"):
                marker = "[child-observation-path] " + path
                if sections[0].splitlines().count(marker) != 1:
                    raise RuntimeError("Child observation actual path did not finish once: " + path)
            if os.name != "nt" and sections[0].splitlines().count("[child-observation-path] source-owner-alias") != 1:
                raise RuntimeError("Child observation actual Unix owner alias path did not finish once")
        if case.attrib["name"] == "sdk.focused.child_history_adoption":
            if int(counts[0]) != 8:
                raise RuntimeError("Child history adoption roster differs from 8 cases")
            for path in ("complete", "post-hook", "historical-chain", "observation-gap",
                         "source-gap", "artifact-gap", "adoption-gap", "scope-reuse"):
                marker = "[child-adoption-path] " + path
                if sections[0].splitlines().count(marker) != 1:
                    raise RuntimeError("Child adoption actual path did not finish once: " + path)
    print(f"SDK focused: all {len(REQUIRED)} registered test files executed nonempty native test cases")


if __name__ == "__main__":
    main()
