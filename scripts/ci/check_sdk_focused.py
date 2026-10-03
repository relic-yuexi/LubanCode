#!/usr/bin/env python3
"""Run all SDK test files and reject missing, skipped or empty native tests."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import xml.etree.ElementTree as ET


REQUIRED = {
    "sdk.focused.session_recovery_view",
    "sdk.focused.lubancore_recovery_view",
    "sdk.focused.lubancore_session",
    "sdk.focused.lubancore_scoped_approval",
    "sdk.focused.lubancore_child_approval",
    "sdk.focused.lubancore_subagents",
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
    listed = subprocess.run(command + ["--show-only=json-v1"], check=True, text=True,
                            encoding="utf-8", capture_output=True)
    (evidence / "tests.json").write_text(listed.stdout, encoding="utf-8")
    tests = json.loads(listed.stdout)["tests"]
    if len(tests) != len(REQUIRED) or {t["name"] for t in tests} != REQUIRED:
        raise RuntimeError("SDK test files are missing, duplicated or unexpected")
    for test in tests:
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        if (props.get("DISABLED") or "sdk-focused" not in props.get("LABELS", [])
                or not 0 < float(props.get("TIMEOUT", 0)) <= 300):
            raise RuntimeError("SDK test is disabled, mislabeled or unbounded: " + test["name"])
        if test["name"] == "sdk.focused.atomic_write":
            check_plan_retry_registration(test.get("command", []))
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
        counts = re.findall(r"\[doctest\] test cases:\s+(\d+)", sections[0])
        if len(counts) != 1 or int(counts[0]) == 0:
            raise RuntimeError("SDK source filter ran no native test cases: " + case.attrib["name"])
        check_recovery_source(case.attrib["name"], sections[0], int(counts[0]))
        if case.attrib["name"] == "sdk.focused.v3_result_store":
            check_result_store_native(sections[0], os.name)
        if case.attrib["name"] == "sdk.focused.atomic_write":
            check_plan_retry_native(sections[0], os.name)
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
