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

try:
    from .sdk_result_immutable import check_result_immutable_registration, check_result_immutable_native
except ImportError:
    try:
        from sdk_result_immutable import check_result_immutable_registration, check_result_immutable_native
    except ModuleNotFoundError:
        from scripts.ci.sdk_result_immutable import check_result_immutable_registration, check_result_immutable_native

try:
    from .sdk_memory_handoff import check_memory_handoff_registration, check_memory_handoff_native
except ImportError:
    try:
        from sdk_memory_handoff import check_memory_handoff_registration, check_memory_handoff_native
    except ModuleNotFoundError:
        from scripts.ci.sdk_memory_handoff import check_memory_handoff_registration, check_memory_handoff_native

try:
    from .sdk_managed_opening import SOURCES as MANAGED_OPENING_SOURCES, check_managed_opening_registration, check_managed_opening_native
except ImportError:
    try:
        from sdk_managed_opening import SOURCES as MANAGED_OPENING_SOURCES, check_managed_opening_registration, check_managed_opening_native
    except ModuleNotFoundError:
        from scripts.ci.sdk_managed_opening import SOURCES as MANAGED_OPENING_SOURCES, check_managed_opening_registration, check_managed_opening_native

try:
    from .sdk_owned_job_deadline import check_owned_job_deadline_registration, check_owned_job_deadline_native
except ImportError:
    try:
        from sdk_owned_job_deadline import check_owned_job_deadline_registration, check_owned_job_deadline_native
    except ModuleNotFoundError:
        from scripts.ci.sdk_owned_job_deadline import check_owned_job_deadline_registration, check_owned_job_deadline_native


try:
    from .sdk_lua_profile import focused_roster, read_lua_profile
except ImportError:
    try:
        from sdk_lua_profile import focused_roster, read_lua_profile
    except ModuleNotFoundError:
        from scripts.ci.sdk_lua_profile import focused_roster, read_lua_profile



try:
    from . import sdk_command_jobs as command_jobs
except ImportError:
    try:
        import sdk_command_jobs as command_jobs
    except ModuleNotFoundError:
        from scripts.ci import sdk_command_jobs as command_jobs

try:
    from . import sdk_named_results as named_results
except ImportError:
    try:
        import sdk_named_results as named_results
    except ModuleNotFoundError:
        from scripts.ci import sdk_named_results as named_results

try:
    from . import sdk_journal_owner as journal_owner
except ImportError:
    try:
        import sdk_journal_owner as journal_owner
    except ModuleNotFoundError:
        from scripts.ci import sdk_journal_owner as journal_owner

try:
    from . import sdk_model_input as model_input
except ImportError:
    try:
        import sdk_model_input as model_input
    except ModuleNotFoundError:
        from scripts.ci import sdk_model_input as model_input

try:
    from . import sdk_owned_file_paths as owned_file_paths
except ImportError:
    try:
        import sdk_owned_file_paths as owned_file_paths
    except ModuleNotFoundError:
        from scripts.ci import sdk_owned_file_paths as owned_file_paths

REQUIRED = {
    "sdk.focused.lubancore_owned_file_paths",
    "sdk.focused.lubancore_model_input",
    "sdk.focused.lubancore_model_sampling",
    "sdk.focused.lubancore_sampling_lifetime",
    "sdk.focused.lubancore_memory_extraction_core",
    "sdk.focused.v3_journal_owner",
    "sdk.focused.lubancore_journal_owner",
    "sdk.focused.lubancore_journal_owner_guards",
    "sdk.focused.lubancore_named_results",
    "sdk.focused.lubancore_named_result_guards",
    "sdk.focused.v3_result_immutable_publication",
    "sdk.focused.lubancore_command_jobs",
    "sdk.focused.lubancore_command_job_guards",
    "sdk.focused.memory_project_commit_handoff",
    "sdk.focused.managed_session_ownership",
    "sdk.focused.managed_session_reservation",
    "sdk.focused.lubancore_owned_job_deadline",
    "sdk.focused.lubancore_web_fetch",
    "sdk.focused.lubancore_web_search",
    "sdk.focused.lubancore_authorization",
    "sdk.focused.package_manifest", "sdk.focused.lubancore_package_manifest",
    "sdk.focused.tool_job_coordinator",
    "sdk.focused.tool_job_start_transaction",
    "sdk.focused.tool_job_hold_recovery",
    "sdk.focused.tool_job_owned_registration",
    "sdk.focused.tool_job_owned_adoption",
    "sdk.focused.tool_job_post_live_invocation",
    "sdk.focused.owned_job_admission",
    "sdk.focused.middleware_native_receipts",
    "sdk.focused.middleware_dispatch_cause",
    "sdk.focused.middleware_job_post_contract",
    "sdk.focused.lubancore_memory_blob_spi",
    "sdk.focused.lubancore_todo_write",
    "sdk.focused.lubancore_agentic_rag",
    "sdk.focused.lubancore_operation_turn_binding",
    "sdk.focused.journal_native_receipts",
    "sdk.focused.v3_journal_receipts",
    "sdk.focused.lubancore_job_operations",
    "sdk.focused.lubancore_package_inventory",
    "sdk.focused.lubancore_lua_build_profile",
    "sdk.focused.run_command_execution_limits",
    "sdk.focused.session_recovery_view",
    "sdk.focused.lubancore_recovery_view",
    "sdk.focused.lubancore_session",
    "sdk.focused.lubancore_scoped_approval",
    "sdk.focused.lubancore_child_approval",
    "sdk.focused.lubancore_subagents",
    "sdk.focused.lubancore_lua",
    "sdk.focused.lua_protected",
    "sdk.focused.lubancore_actions",
    "sdk.focused.lubancore_event_sink",
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
PREPARED_JOB_PATHS = ("stage", "rewrite", "owner", "capacity", "receipts", "close-isolation")


def check_prepared_job_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_tool_job_owned_registration.cpp"):
        raise RuntimeError("Prepared Job registration must run the single actual source")


def check_prepared_job_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Prepared Job native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Prepared Job executable is not the actual native fixture")
    check_prepared_job_registration(command, executable)
    check_native_command(section, command)
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(counts) != 1 or tuple(map(int, counts[0])) != (6, 6, 0):
        raise RuntimeError("Prepared Job native roster differs from 6 successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Prepared Job native assertions did not actually pass")
    for path in PREPARED_JOB_PATHS:
        if section.splitlines().count("[job-owned-registration-path] " + path) != 1:
            raise RuntimeError("Prepared Job actual path did not finish once: " + path)


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


COMMAND_LIMITS_PATHS = ("boundary", "timeout", "cancel", "four-contexts", "invalid-and-background", "legacy")


def check_command_limits_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(part, str) for part in command) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_run_command_execution_limits.cpp"):
        raise RuntimeError("Command limits must run the single actual native source")


def check_command_limits_native(section, command, platform_name=None):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(part, str) for part in command)):
        raise RuntimeError("Command limits fixture lacks a valid registered argument list")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Command limits fixture executable is not registered")
    check_command_limits_registration(command, executable)
    check_native_command(section, command)
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if counts != [("6", "6", "0")]:
        raise RuntimeError("Command limits native roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            assertions[0][0] != assertions[0][1] or assertions[0][2] != "0" or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Command limits native assertions are empty or failed")
    for path in COMMAND_LIMITS_PATHS:
        if section.splitlines().count("[command-limits-path] " + path) != 1:
            raise RuntimeError("Command limits actual path did not finish once: " + path)
    prefix = "[command-limits-shell] "
    shell_lines = [line[len(prefix):] for line in section.splitlines() if line.startswith(prefix)]
    if len(shell_lines) != 2:
        raise RuntimeError("Command limits require both actual shell boundary records")
    records = []
    fields = {"shell", "cwd", "exact_command", "excess_command", "timeout_ms", "max_output_bytes",
              "exact_request_bytes", "excess_request_bytes", "exact_outcome", "excess_error_code"}
    for line in shell_lines:
        try:
            record = json.loads(line)
        except (ValueError, TypeError) as error:
            raise RuntimeError("Command limits shell boundary record is invalid") from error
        if (not isinstance(record, dict) or set(record) != fields or
                any(not isinstance(record[key], str) or not record[key] or "\0" in record[key]
                    for key in ("shell", "cwd", "exact_command", "excess_command")) or
                any(type(record[key]) is not int or record[key] != value for key, value in
                    (("timeout_ms", 15000), ("max_output_bytes", 128),
                     ("exact_request_bytes", 128), ("excess_request_bytes", 129))) or
                record["exact_outcome"] != "succeeded" or record["excess_error_code"] != "process.output_limit"):
            raise RuntimeError("Command limits shell boundary record differs from actual passing boundary")
        records.append(record)
    expected_shells = {"cmd", "powershell"} if (platform_name or os.name) == "nt" else {"sh", "bash"}
    if {record["shell"] for record in records} != expected_shells:
        raise RuntimeError("Command limits actual boundary records do not cover both platform shells")


JOB_ADOPTION_PATHS = ("held-provenance", "parent-chain", "native-raw-post",
                      "sessions-cancel-limits", "startup-close", "native-gap-hold")


def check_job_adoption_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_tool_job_owned_adoption.cpp"):
        raise RuntimeError("Owned Job adoption registration must run the single actual source")


def check_job_adoption_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Owned Job adoption native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Owned Job adoption executable is not the actual native fixture")
    check_job_adoption_registration(command, executable)
    check_native_command(section, command)
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(counts) != 1 or tuple(map(int, counts[0])) != (6, 6, 0):
        raise RuntimeError("Owned Job adoption native roster differs from 6 successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Owned Job adoption native assertions did not actually pass")
    for path in JOB_ADOPTION_PATHS:
        if section.splitlines().count("[job-owned-adoption-path] " + path) != 1:
            raise RuntimeError("Owned Job adoption actual path did not finish once: " + path)


MIDDLEWARE_RECEIPT_PATHS = ("completed", "skipped", "failure-policy", "effects", "writer-fault", "owner-lifetime")


def check_middleware_receipt_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_middleware_native_receipts.cpp"):
        raise RuntimeError("Middleware native receipt registration must run the single actual source")


def check_middleware_receipt_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Middleware native receipt native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Middleware native receipt executable is not the actual native fixture")
    check_middleware_receipt_registration(command, executable)
    check_native_command(section, command)
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(counts) != 1 or tuple(map(int, counts[0])) != (6, 6, 0):
        raise RuntimeError("Middleware native receipt native roster differs from 6 successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Middleware native receipt native assertions did not actually pass")
    for path in MIDDLEWARE_RECEIPT_PATHS:
        if section.splitlines().count("[middleware-native-receipts-path] " + path) != 1:
            raise RuntimeError("Middleware native receipt actual path did not finish once: " + path)


MIDDLEWARE_CAUSE_PATHS = ("policies", "cancellation", "denied", "propagation", "exceptions", "snapshot")


def check_middleware_cause_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_middleware_dispatch_cause.cpp"):
        raise RuntimeError("Middleware dispatch cause registration must run the single actual source")


def check_middleware_cause_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Middleware dispatch cause native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Middleware dispatch cause executable is not the actual native fixture")
    check_middleware_cause_registration(command, executable)
    check_native_command(section, command)
    counts = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(counts) != 1 or tuple(map(int, counts[0])) != (6, 6, 0):
        raise RuntimeError("Middleware dispatch cause native roster differs from 6 successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Middleware dispatch cause native assertions did not actually pass")
    for path in MIDDLEWARE_CAUSE_PATHS:
        if section.splitlines().count("[middleware-dispatch-cause-path] " + path) != 1:
            raise RuntimeError("Middleware dispatch cause actual path did not finish once: " + path)


JOB_POST_PATHS = ("valid", "payload", "budget", "observer", "native-gap", "compatibility")
JOB_POST_SDK_PATHS = ("pre-action", "post-action")


def check_job_post_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_middleware_job_post_contract.cpp"):
        raise RuntimeError("Job Post return registration must run the single absolute native source")


def check_job_post_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Job Post return native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Job Post return executable is not the actual native fixture")
    check_job_post_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("Job Post return native roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Job Post return native assertions did not actually pass")
    for prefix, paths in (("[middleware-job-post-contract-path] ", JOB_POST_PATHS),
                          ("[middleware-job-post-sdk] ", JOB_POST_SDK_PATHS)):
        for path in paths:
            if section.splitlines().count(prefix + path) != 1:
                raise RuntimeError("Job Post return actual path did not finish once: " + path)


JOB_POST_LIVE_PATHS = ("legacy", "actual", "isolation", "drain", "gap", "passive")


def check_job_post_live_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_tool_job_post_live_invocation.cpp"):
        raise RuntimeError("Job live Post registration must run the single absolute native source")


def check_job_post_live_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Job live Post native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Job live Post executable is not the actual native fixture")
    check_job_post_live_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("Job live Post native roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Job live Post native assertions did not actually pass")
    for path in JOB_POST_LIVE_PATHS:
        if section.splitlines().count("[job-post-live-invocation-path] " + path) != 1:
            raise RuntimeError("Job live Post actual path did not finish once: " + path)


EVENT_SINK_PATHS = ("default", "actual", "overflow", "error", "drain", "isolation")


def check_event_sink_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_lubancore_event_sink.cpp"):
        raise RuntimeError("EventSink registration must run the single absolute native source")


def check_opening_start_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe") not in
            ("lubancore_sdk_tests", "lubancode_tests") or
            command[1] != "--source-file=*test_lubancore_lifecycle.cpp"):
        raise RuntimeError("Opening startup must run the complete actual native lifecycle source")
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (19, 19, 0):
        raise RuntimeError("Opening startup and original lifecycle roster must all pass")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Opening startup native assertions did not actually pass")
    for path in ("standard", "nonstandard", "repeated", "isolation", "shutdown"):
        if section.splitlines().count("[sdk-opening-start-path] " + path) != 1:
            raise RuntimeError("Opening startup actual path did not finish once: " + path)

    for path in ("background", "mutual", "late-deleter", "session-deleter", "exceptions", "nested", "allocation-rollback"):
        if section.splitlines().count("[sdk-backend-owner-path] " + path) != 1:
            raise RuntimeError("SDK Backend actual owner path did not finish once: " + path)


def check_event_sink_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("EventSink native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("EventSink executable is not the actual native fixture")
    check_event_sink_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("EventSink native roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("EventSink native assertions did not actually pass")
    for path in EVENT_SINK_PATHS:
        if section.splitlines().count("[sdk-event-sink-path] " + path) != 1:
            raise RuntimeError("EventSink actual path did not finish once: " + path)


def check_event_sink_consumer(section, command):
    if (not isinstance(command, list) or len(command) != 3 or
            not all(isinstance(value, str) and value for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in ("lubancore_consumer", "lubancore_consumer.exe") or
            command[1] != "event-sink"):
        raise RuntimeError("EventSink consumer must run its actual relocated command")
    check_native_command(section, command)
    if (section.splitlines().count("[sdk-event-sink-consumer] actual-owned-queue") != 1 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("EventSink relocated consumer did not finish its actual owned queue")


MEMORY_BLOB_PATHS = ('actual', 'resume', 'receipt', 'isolation', 'drain', 'opening')


def check_memory_blob_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_lubancore_memory_blob_spi.cpp"):
        raise RuntimeError("Memory blob registration must run the single absolute native source")


def check_memory_blob_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Memory blob native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Memory blob executable is not the actual native fixture")
    check_memory_blob_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("Memory blob native roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Memory blob native assertions did not actually pass")
    for path in MEMORY_BLOB_PATHS:
        if section.splitlines().count("[sdk-memory-blob-path] " + path) != 1:
            raise RuntimeError("Memory blob actual path did not finish once: " + path)


def check_memory_blob_consumer(section, command):
    if (not isinstance(command, list) or len(command) != 3 or
            not all(isinstance(value, str) and value for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in ("lubancore_consumer", "lubancore_consumer.exe") or
            command[1] != "memory-blobs"):
        raise RuntimeError("Memory blob consumer must run its actual relocated command")
    check_native_command(section, command)
    if (section.splitlines().count("[sdk-memory-blob-consumer] actual-owned-store") != 1 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Memory blob relocated consumer did not finish its actual owned queue")


TODO_WRITE_PATHS = ('admission', 'replacement', 'validation', 'isolation', 'recovery', 'lifetime')


def check_todo_write_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_lubancore_todo_write.cpp"):
        raise RuntimeError("Todo write registration must run the single absolute native source")


def check_todo_write_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Todo write native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Todo write executable is not the actual native fixture")
    check_todo_write_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("Todo write native roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Todo write native assertions did not actually pass")
    for path in TODO_WRITE_PATHS:
        if section.splitlines().count("[sdk-todo-write-path] " + path) != 1:
            raise RuntimeError("Todo write actual path did not finish once: " + path)


def check_todo_write_consumer(section, command):
    if (not isinstance(command, list) or len(command) != 3 or
            not all(isinstance(value, str) and value for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in ("lubancore_consumer", "lubancore_consumer.exe") or
            command[1] != "todo-write" or
            not (command[2].startswith("/") or
                 (ntpath.isabs(command[2]) and bool(ntpath.splitdrive(command[2])[0])))):
        raise RuntimeError("Todo write consumer must run its actual relocated command")
    check_native_command(section, command)
    if (section.splitlines().count("[sdk-todo-write-consumer] complete") != 1 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Todo write relocated consumer did not finish its actual rounds")
    for path in TODO_WRITE_PATHS:
        if section.splitlines().count("[sdk-todo-write-path] " + path) != 1:
            raise RuntimeError("Todo write relocated path did not finish once: " + path)


AGENTIC_RAG_PATHS = ('retrieval', 'sources', 'preview', 'isolation', 'recovery', 'lifetime')

WEB_FETCH_REQUEST_COUNTS = {'admission': 1, 'content': 4, 'limits': 9, 'redirects': 7, 'cancel': 4, 'isolation': 5}


def check_web_fetch_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) and value for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_lubancore_web_fetch.cpp"):
        raise RuntimeError("WebFetch registration must run one absolute native source with its full filter")


def check_web_fetch_environment(properties):
    environments = [item.get("value") for item in properties if item.get("name") == "ENVIRONMENT"]
    if len(environments) != 1 or not isinstance(environments[0], list) or not all(isinstance(item, str) for item in environments[0]):
        raise RuntimeError("WebFetch native registration lacks its explicit fixture environment")
    for name in ("NO_PROXY", "no_proxy"):
        if [value for value in environments[0] if value.startswith(name + "=")] != [name + "=127.0.0.1"]:
            raise RuntimeError("WebFetch native loopback proxy exception is missing, duplicated or changed")


def check_web_fetch_paths(section):
    for path in WEB_FETCH_REQUEST_COUNTS:
        if section.splitlines().count("[sdk-web-fetch-path] " + path) != 1:
            raise RuntimeError("WebFetch actual path did not finish once: " + path)


def check_web_fetch_decoder(section):
    lines = [line.removeprefix("[sdk-web-fetch-decoder] ") for line in section.splitlines()
             if line.startswith("[sdk-web-fetch-decoder] ")]
    if len(lines) != 1:
        raise RuntimeError("WebFetch must retain one actual owned decoder capability/result receipt")
    try:
        value = json.loads(lines[0])
    except (ValueError, UnicodeError) as error:
        raise RuntimeError("WebFetch decoder receipt is not JSON") from error
    if (not isinstance(value, dict) or set(value) != {"gzip_decoding", "outcome"} or
            type(value.get("gzip_decoding")) is not bool or
            value["outcome"] != ("download_limit" if value["gzip_decoding"] else "unsupported_encoding")):
        raise RuntimeError("WebFetch gzip result does not match its actual decoder capability")


def check_web_fetch_native(section, command):
    if not isinstance(command, list) or len(command) != 2 or not all(isinstance(value, str) for value in command):
        raise RuntimeError("WebFetch native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("WebFetch executable is not the actual native fixture")
    check_web_fetch_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0) or len(assertions) != 1 or
            int(assertions[0][0]) <= 0 or int(assertions[0][0]) != int(assertions[0][1]) or
            int(assertions[0][2]) != 0 or section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("WebFetch native source must finish six nonempty successful cases")
    check_web_fetch_paths(section)
    check_web_fetch_decoder(section)
    lines = [line for line in section.splitlines() if line.startswith("[sdk-web-fetch-fixture] ")]
    if len(lines) != len(WEB_FETCH_REQUEST_COUNTS):
        raise RuntimeError("WebFetch must retain six actual owned HTTP fixture retirement receipts")
    seen = set()
    for line in lines:
        try:
            value = json.loads(line.removeprefix("[sdk-web-fetch-fixture] "))
        except (ValueError, UnicodeError) as error:
            raise RuntimeError("WebFetch HTTP fixture receipt is not JSON") from error
        if (not isinstance(value, dict) or set(value) != {"path", "quiescent", "requests", "stop_elapsed_ms"} or
                not isinstance(value.get("path"), str) or value["path"] not in WEB_FETCH_REQUEST_COUNTS or value["path"] in seen or
                value.get("quiescent") is not True or type(value.get("requests")) is not int or
                value["requests"] != WEB_FETCH_REQUEST_COUNTS[value["path"]] or
                type(value.get("stop_elapsed_ms")) is not int or not 0 <= value["stop_elapsed_ms"] < 3000):
            raise RuntimeError("WebFetch HTTP fixture did not prove its exact requests and completed retirement")
        seen.add(value["path"])


def check_web_fetch_consumer(section, command, context):
    if (not isinstance(command, list) or len(command) != 5 or
            not all(isinstance(value, str) and value for value in command) or command[1] != "web-fetch" or
            any(not (value.startswith("/") or (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0])))
                for value in (command[0], command[2], command[4])) or
            command[0].replace("\\", "/").split("/")[-1] not in ("lubancore_consumer", "lubancore_consumer.exe")):
        raise RuntimeError("WebFetch consumer must run its actual relocated five-argument command")
    try:
        from .sdk_web_fetch_fixture import check_caller_receipts
    except ImportError:
        from sdk_web_fetch_fixture import check_caller_receipts
    check_caller_receipts(context)
    normalize = lambda value: value.replace("\\", "/")
    if (command[3] != context["ready"]["base_url"] or normalize(command[4]) != normalize(context["requests_file"]) or
            normalize(command[0]) != normalize(context.get("consumer_executable", "")) or
            normalize(command[2]) != normalize(context["consumer_build"].rstrip("/\\") + "/state-web-fetch")):
        raise RuntimeError("WebFetch consumer argv is not bound to its owned actual fixture and installed target")
    check_native_command(section, command)
    if (section.splitlines().count("[sdk-web-fetch-consumer] complete") != 1 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("WebFetch installed consumer did not finish its actual calls")
    check_web_fetch_paths(section)
    check_web_fetch_decoder(section)


def check_agentic_rag_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_lubancore_agentic_rag.cpp"):
        raise RuntimeError("Agentic RAG registration must run the single absolute native source")


def check_agentic_rag_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Agentic RAG native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Agentic RAG executable is not the actual native fixture")
    check_agentic_rag_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("Agentic RAG native roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Agentic RAG native assertions did not actually pass")
    for path in AGENTIC_RAG_PATHS:
        if section.splitlines().count("[sdk-agentic-rag-path] " + path) != 1:
            raise RuntimeError("Agentic RAG actual path did not finish once: " + path)


def check_agentic_rag_consumer(section, command):
    if (not isinstance(command, list) or len(command) != 3 or
            not all(isinstance(value, str) and value for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in ("lubancore_consumer", "lubancore_consumer.exe") or
            command[1] != "agentic-rag" or
            not (command[2].startswith("/") or
                 (ntpath.isabs(command[2]) and bool(ntpath.splitdrive(command[2])[0])))):
        raise RuntimeError("Agentic RAG consumer must run its actual relocated command")
    check_native_command(section, command)
    if (section.splitlines().count("[sdk-agentic-rag-consumer] complete") != 1 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Agentic RAG relocated consumer did not finish its actual rounds")
    for path in AGENTIC_RAG_PATHS:
        if section.splitlines().count("[sdk-agentic-rag-path] " + path) != 1:
            raise RuntimeError("Agentic RAG relocated path did not finish once: " + path)


def check_agentic_rag_demo(section, command, context, scratch: Path, prefix: Path):
    try:
        from .sdk_rag_demo import check_demo_context, inside
    except ImportError:
        try:
            from sdk_rag_demo import check_demo_context, inside
        except ModuleNotFoundError:
            from scripts.ci.sdk_rag_demo import check_demo_context, inside
    check_demo_context(context, scratch, prefix)
    if (not isinstance(command, list) or len(command) != 3 or
            not all(isinstance(value, str) and value for value in command) or
            not Path(command[0]).is_absolute() or Path(command[0]).resolve() != Path(context["executable"]).resolve() or
            command[1] != "--fixture" or not Path(command[2]).is_absolute() or
            not inside(Path(command[2]).resolve(), scratch.resolve()) or
            any(inside(Path(command[2]).resolve(), Path(context[key]).resolve()) for key in ("source", "installed_prefix"))):
        raise RuntimeError("Agentic RAG demo must run its actual independent binary and an absolute scratch state root")
    check_native_command(section, command)
    if (section.splitlines().count("[sdk-agentic-rag-consumer] complete") != 1 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Agentic RAG standalone example did not finish its actual rounds")
    for path in AGENTIC_RAG_PATHS:
        if section.splitlines().count("[sdk-agentic-rag-path] " + path) != 1:
            raise RuntimeError("Agentic RAG standalone path did not finish once: " + path)


OPERATION_TURN_BINDING_PATHS = ("actual", "source", "gap", "history", "relation", "isolation")


def check_operation_turn_binding_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_lubancore_operation_turn_binding.cpp"):
        raise RuntimeError("Operation turn binding must register the single absolute native source")


def check_operation_turn_binding_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Operation turn binding native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Operation turn binding executable is not the actual native fixture")
    check_operation_turn_binding_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("Operation turn binding roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Operation turn binding native assertions did not actually pass")
    for path in OPERATION_TURN_BINDING_PATHS:
        if section.splitlines().count("[sdk-operation-turn-binding-path] " + path) != 1:
            raise RuntimeError("Operation turn binding actual path did not finish once: " + path)


JOURNAL_RECEIPT_PATHS = ('committed', 'before-io', 'append-gap', 'flush-gap', 'close', 'move')


def check_journal_receipt_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_journal_native_receipts.cpp"):
        raise RuntimeError("Journal receipt registration must run the single absolute native source")


def check_journal_receipt_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Journal receipt native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Journal receipt executable is not the actual native fixture")
    check_journal_receipt_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("Journal receipt native roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Journal receipt native assertions did not actually pass")
    for path in JOURNAL_RECEIPT_PATHS:
        if section.splitlines().count("[journal-native-receipt-path] " + path) != 1:
            raise RuntimeError("Journal receipt actual path did not finish once: " + path)


V3_JOURNAL_WITNESS_PATHS = ('committed', 'before-io', 'append-gap', 'flush-gap', 'cache', 'lifetime')


def check_v3_journal_witness_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_v3_journal_receipts.cpp"):
        raise RuntimeError("V3 journal witness must register the single absolute native source")


def check_v3_journal_witness_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("V3 journal witness native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("V3 journal witness executable is not the actual native fixture")
    check_v3_journal_witness_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("V3 journal witness roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("V3 journal witness native assertions did not actually pass")
    for path in V3_JOURNAL_WITNESS_PATHS:
        if section.splitlines().count("[v3-journal-witness-path] " + path) != 1:
            raise RuntimeError("V3 journal witness actual path did not finish once: " + path)


JOB_OPERATION_PATHS = ('source', 'gap', 'history', 'relation', 'isolation', 'lifetime')


def check_job_operation_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_lubancore_job_operations.cpp"):
        raise RuntimeError("Job operation must register the single absolute native source")


def check_job_operation_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Job operation native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Job operation executable is not the actual native fixture")
    check_job_operation_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (6, 6, 0):
        raise RuntimeError("Job operation roster differs from six successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Job operation native assertions did not actually pass")
    for path in JOB_OPERATION_PATHS:
        if section.splitlines().count("[sdk-job-operations-path] " + path) != 1:
            raise RuntimeError("Job operation actual path did not finish once: " + path)


PACKAGE_INVENTORY_PATHS = ('owned', 'fingerprint', 'manifest', 'input', 'limits', 'links', 'changed', 'isolation')


def check_package_inventory_registration(command, executable="lubancore_sdk_tests"):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command) or
            not (command[0].startswith("/") or
                 (ntpath.isabs(command[0]) and bool(ntpath.splitdrive(command[0])[0]))) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_lubancore_package_inventory.cpp"):
        raise RuntimeError("Package inventory must register the single absolute native source")


def check_package_inventory_native(section, command):
    if (not isinstance(command, list) or len(command) != 2 or
            not all(isinstance(value, str) for value in command)):
        raise RuntimeError("Package inventory native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Package inventory executable is not the actual native fixture")
    check_package_inventory_registration(command, executable)
    check_native_command(section, command)
    cases = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if len(cases) != 1 or tuple(map(int, cases[0])) != (8, 8, 0):
        raise RuntimeError("Package inventory roster differs from eight successful cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or
            int(assertions[0][0]) != int(assertions[0][1]) or int(assertions[0][2]) != 0 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Package inventory native assertions did not actually pass")
    for path in PACKAGE_INVENTORY_PATHS:
        if section.splitlines().count("[sdk-package-inventory-path] " + path) != 1:
            raise RuntimeError("Package inventory actual path did not finish once: " + path)


def check_package_inventory_consumer(section, command):
    def absolute(value):
        return (isinstance(value, str) and bool(value) and "\0" not in value and
                (value.startswith("/") or (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0]))))
    if (not isinstance(command, list) or len(command) != 3 or
            not all(isinstance(value, str) and value for value in command) or
            not absolute(command[0]) or not absolute(command[2]) or
            command[0].replace("\\", "/").split("/")[-1] not in ("lubancore_consumer", "lubancore_consumer.exe") or
            command[1] != "package-inventory"):
        raise RuntimeError("Package inventory consumer must run its absolute relocated command and state")
    check_native_command(section, command)
    if (section.splitlines().count("[sdk-package-inventory-consumer] complete") != 1 or
            section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Package inventory consumer did not finish the actual owned capture")


LUA_BUILD_PATHS = ("default-off", "selection", "disabled-resume", "frozen-plan", "isolation", "lifetime")

def check_lua_build_registration(command, executable="lubancore_sdk_tests"):
    absolute = lambda value: isinstance(value, str) and bool(value) and "\0" not in value and (
        value.startswith("/") or (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0])))
    if (not isinstance(command, list) or len(command) != 2 or not absolute(command[0]) or
            command[0].replace("\\", "/").split("/")[-1] not in (executable, executable + ".exe") or
            command[1] != "--source-file=*test_lubancore_lua_build_profile.cpp"):
        raise RuntimeError("Lua build profile must register its actual absolute native source")

def check_lua_build_native(section, command, enabled=True):
    if not isinstance(command, list) or not command or not isinstance(command[0], str):
        raise RuntimeError("Lua build native argv is malformed")
    executable = command[0].replace("\\", "/").split("/")[-1].removesuffix(".exe")
    if executable not in ("lubancore_sdk_tests", "lubancode_tests"):
        raise RuntimeError("Lua build profile ran a foreign native executable")
    check_lua_build_registration(command, executable)
    check_native_command(section, command)
    summaries = re.findall(r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if summaries != [("6", "6", "0")]:
        raise RuntimeError("Lua build native roster differs from six actual cases")
    assertions = re.findall(r"\[doctest\] assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed", section)
    if (len(assertions) != 1 or int(assertions[0][0]) <= 0 or assertions[0][0] != assertions[0][1] or
            int(assertions[0][2]) != 0 or section.splitlines().count("Test Passed.") != 1):
        raise RuntimeError("Lua build native assertions did not pass")
    profile = "on" if enabled else "off"
    observed = [line for line in section.splitlines() if line.startswith("[sdk-lua-build-case-profile] ")]
    if sorted(observed) != sorted("[sdk-lua-build-case-profile] " + path + " " + profile for path in LUA_BUILD_PATHS):
        raise RuntimeError("Lua native evidence contains a different or duplicate configured profile")
    for path in LUA_BUILD_PATHS:
        if (section.splitlines().count("[sdk-lua-build-path] " + path) != 1 or
                section.splitlines().count("[sdk-lua-build-case-profile] " + path + " " + profile) != 1):
            raise RuntimeError("Lua actual case/profile did not finish once: " + path)

def check_lua_build_consumer(section, command, enabled=True):
    absolute = lambda value: isinstance(value, str) and bool(value) and "\0" not in value and (
        value.startswith("/") or (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0])))
    if (not isinstance(command, list) or len(command) != 3 or not absolute(command[0]) or not absolute(command[2]) or
            command[0].replace("\\", "/").split("/")[-1] not in ("lubancore_consumer", "lubancore_consumer.exe") or
            command[1] != "lua-build-profile"):
        raise RuntimeError("Lua build consumer must run its actual absolute relocated command")
    check_native_command(section, command)
    profile = "on" if enabled else "off"
    if [line for line in section.splitlines() if line.startswith("[sdk-lua-build-profile] ")] != ["[sdk-lua-build-profile] " + profile]:
        raise RuntimeError("Lua consumer evidence contains a different or duplicate configured profile")
    if any(section.splitlines().count(marker) != 1 for marker in (
            "[sdk-lua-build-profile] " + profile, "[sdk-lua-build-consumer] complete", "Test Passed.")):
        raise RuntimeError("Lua actual installed profile did not finish once")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--sdk-only", action="store_true")
    parser.add_argument("--lua-profile", choices=("on", "off"), default="on")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    evidence = build / "test-evidence" / "sdk-focused"
    evidence.mkdir(parents=True, exist_ok=True)
    source_root = Path(__file__).resolve().parents[2]
    owned_file_paths.capture_source(source_root, evidence)
    owned_source = owned_file_paths.read_source_evidence(source_root, evidence)
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
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    entries = {line.split(":", 1)[0]: line.split("=", 1)[1] for line in cache.splitlines()
               if ":" in line and "=" in line and not line.startswith(("#", "//"))}
    with_lua = read_lua_profile(entries, args.lua_profile)
    required = focused_roster(REQUIRED, with_lua)
    tests = json.loads(listed.stdout)["tests"]
    if len(tests) != len(required) or {t["name"] for t in tests} != required:
        raise RuntimeError("SDK test files are missing, duplicated or unexpected")
    for test in tests:
        job_stem = test["name"].removeprefix("sdk.focused.")
        if job_stem in command_jobs.SOURCES:
            command_jobs.check_registration(test.get("command"), job_stem)
        if job_stem in named_results.SOURCES:
            named_results.check_registration(test.get("command"), job_stem)
        if job_stem in journal_owner.SOURCES:
            journal_owner.check_registration(test.get("command"), job_stem)
        if job_stem in model_input.SOURCES:
            model_input.check_registration(test.get("command"), job_stem)
        if job_stem == owned_file_paths.STEM:
            owned_file_paths.check_registration(test.get("command"))
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        if job_stem == owned_file_paths.STEM and (type(props.get("TIMEOUT")) not in (int, float) or props["TIMEOUT"] != 300):
            raise RuntimeError("Owned file paths source must retain its original300s timeout")
        if (props.get("DISABLED") or "sdk-focused" not in props.get("LABELS", [])
                or not 0 < float(props.get("TIMEOUT", 0)) <= 300):
            raise RuntimeError("SDK test is disabled, mislabeled or unbounded: " + test["name"])
        if test["name"] in ("sdk.focused.tool_job_coordinator", "sdk.focused.tool_job_start_transaction", "sdk.focused.tool_job_hold_recovery"):
            source = "test_" + test["name"].removeprefix("sdk.focused.") + ".cpp"
            check_job_start_registration(test.get("command", []), source)
        if test["name"] == "sdk.focused.owned_job_admission":
            check_owned_job_registration(test.get("command", []))
        if test["name"] == "sdk.focused.tool_job_owned_registration":
            check_prepared_job_registration(test.get("command", []))
        if test["name"] == "sdk.focused.tool_job_owned_adoption":
            check_job_adoption_registration(test.get("command", []))
        if test["name"] == "sdk.focused.middleware_native_receipts":
            check_middleware_receipt_registration(test.get("command", []))
        if test["name"] == "sdk.focused.middleware_dispatch_cause":
            check_middleware_cause_registration(test.get("command", []))
        if test["name"] == "sdk.focused.middleware_job_post_contract":
            check_job_post_registration(test.get("command", []))
        if test["name"] == "sdk.focused.tool_job_post_live_invocation":
            check_job_post_live_registration(test.get("command", []))
        if test["name"] == "sdk.focused.lubancore_event_sink":
            check_event_sink_registration(test.get("command", []))
        if test["name"] == "sdk.focused.lubancore_memory_blob_spi":
            check_memory_blob_registration(test.get("command", []))
        if test["name"] == "sdk.focused.lubancore_todo_write":
            check_todo_write_registration(test.get("command", []))
        if test["name"] == "sdk.focused.lubancore_agentic_rag":
            check_agentic_rag_registration(test.get("command", []))
        if test["name"] == "sdk.focused.lubancore_web_fetch":
            check_web_fetch_registration(test.get("command", []))
            check_web_fetch_environment(test.get("properties", []))
        if test["name"] == "sdk.focused.lubancore_operation_turn_binding":
            check_operation_turn_binding_registration(test.get("command", []))
        if test["name"] == "sdk.focused.journal_native_receipts":
            check_journal_receipt_registration(test.get("command", []))
        if test["name"] == "sdk.focused.lubancore_lua_build_profile":
            check_lua_build_registration(test.get("command", []))
        if test["name"] == "sdk.focused.lubancore_package_inventory":
            check_package_inventory_registration(test.get("command", []))
        if test["name"] == "sdk.focused.lubancore_job_operations":
            check_job_operation_registration(test.get("command", []))
        if test["name"] == "sdk.focused.memory_project_commit_handoff":
            check_memory_handoff_registration(test.get("command", []))
        if test["name"] == "sdk.focused.v3_result_immutable_publication":
            check_result_immutable_registration(test.get("command", []))
        if test["name"].removeprefix("sdk.focused.") in MANAGED_OPENING_SOURCES:
            check_managed_opening_registration(test.get("command", []), test["name"].removeprefix("sdk.focused."))
        if test["name"] == "sdk.focused.lubancore_owned_job_deadline":
            check_owned_job_deadline_registration(test.get("command", []))
        if test["name"] == "sdk.focused.v3_journal_receipts":
            check_v3_journal_witness_registration(test.get("command", []))
        if test["name"] == "sdk.focused.run_command_execution_limits":
            check_command_limits_registration(test.get("command", []))
        if test["name"] == "sdk.focused.atomic_write":
            check_plan_retry_registration(test.get("command", []))
        if test["name"] in ("sdk.focused.package_manifest", "sdk.focused.lubancore_package_manifest"):
            source = "test_package_manifest.cpp" if test["name"] == "sdk.focused.package_manifest" else "test_lubancore_package_manifest.cpp"
            check_package_registration(test.get("command", []), source, "lubancore_sdk_tests")
    (evidence / "context.json").write_text(json.dumps({
        "githubSha": os.environ.get("GITHUB_SHA"), "buildDir": str(build),
        "configuration": args.config, "sdkOnly": args.sdk_only, "luaProfile": args.lua_profile,
        "requiredTests": sorted(required),
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
    if len(cases) != len(required) or {c.attrib["name"] for c in cases} != required:
        raise RuntimeError("JUnit does not cover every SDK test file")
    # Successful JUnit output can be truncated before the doctest summary.
    native_sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$',
        (evidence / "LastTest.log").read_text(encoding="utf-8"), flags=re.M)
    command_job_reports = {}
    named_result_reports = {}
    journal_owner_reports = {}
    model_input_reports = {}
    owned_file_path_reports = {}
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
        job_stem = case.attrib["name"].removeprefix("sdk.focused.")
        if job_stem in command_jobs.SOURCES:
            command_job_reports[case.attrib["name"]] = command_jobs.check_native(sections[0], commands[0], job_stem, os.name)
            (evidence / "command-jobs.json").write_text(json.dumps(command_job_reports, indent=2) + "\n", encoding="utf-8")
        if job_stem in named_results.SOURCES:
            named_result_reports[case.attrib["name"]] = named_results.check_native(sections[0], commands[0], job_stem)
            (evidence / "named-results.json").write_text(json.dumps(named_result_reports, indent=2) + "\n", encoding="utf-8")
        if job_stem in journal_owner.SOURCES:
            journal_owner_reports[case.attrib["name"]] = journal_owner.check_native(sections[0], commands[0], job_stem)
            (evidence / "journal-owner.json").write_text(json.dumps(journal_owner_reports, indent=2) + "\n", encoding="utf-8")
        if job_stem in model_input.SOURCES:
            model_input_reports[case.attrib["name"]] = model_input.check_native(sections[0], commands[0], job_stem)
            (evidence / "model-input.json").write_text(json.dumps(model_input_reports, indent=2) + "\n", encoding="utf-8")
        if job_stem == owned_file_paths.STEM:
            owned_file_path_reports[case.attrib["name"]] = owned_file_paths.check_native(
                sections[0], commands[0], owned_source['record'], owned_source['bytes'], owned_source['head'])
            (evidence / "owned-file-paths.json").write_text(json.dumps(owned_file_path_reports, indent=2) + "\n", encoding="utf-8")
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
        if case.attrib["name"] == "sdk.focused.tool_job_owned_registration":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_prepared_job_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.tool_job_owned_adoption":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_job_adoption_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.middleware_native_receipts":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_middleware_receipt_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.middleware_dispatch_cause":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_middleware_cause_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.middleware_job_post_contract":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_job_post_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.tool_job_post_live_invocation":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_job_post_live_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.lubancore_event_sink":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_event_sink_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.lubancore_lifecycle":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_opening_start_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.lubancore_memory_extraction_core":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            if registered["command"][1:] != ["--source-file=*test_lubancore_memory_extraction_core.cpp"]:
                raise RuntimeError("Memory extraction must run the actual shared-SDK native source")
            check_native_command(sections[0], registered["command"])
            if int(counts[0]) != 10:
                raise RuntimeError("Memory extraction requires all ten actual shared-module cases")
            for path in ("parser", "transcript", "prompt", "request", "usage", "finish", "native-connection", "learning-text-gate", "learning-evidence-gate", "learning-reason-names"):
                if sections[0].splitlines().count("[memory-extraction-core-path] shared-sdk " + path) != 1:
                    raise RuntimeError("Memory extraction actual shared-module path missing: " + path)
        if case.attrib["name"] == "sdk.focused.lubancore_sampling_lifetime":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            if registered["command"][1:] != ["--source-file=*test_lubancore_sampling_lifetime.cpp"]:
                raise RuntimeError("Sampling lifetime must run the actual shared-SDK native source")
            check_native_command(sections[0], registered["command"])
            if int(counts[0]) != 6:
                raise RuntimeError("Sampling lifetime requires all six actual shared-module cases")
            for path in ("startup", "exceptions", "preflight", "gates", "isolation", "cancellation"):
                if sections[0].splitlines().count("[sample-lifetime-path] shared-sdk " + path) != 1:
                    raise RuntimeError("Sampling lifetime actual shared-module path missing: " + path)
        if case.attrib["name"] == "sdk.focused.lubancore_model_sampling":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            if registered["command"][1:] != ["--source-file=*test_lubancore_model_sampling.cpp"]:
                raise RuntimeError("Sampling must run the actual complete native source")
            check_native_command(sections[0], registered["command"])
            if int(counts[0]) != 6:
                raise RuntimeError("Sampling native roster must contain all six cases")
            for path in ("defaults", "effort", "finish", "invalid-request", "invalid-reply", "errors"):
                if sections[0].splitlines().count("[sdk-model-sampling-path] " + path) != 1:
                    raise RuntimeError("Sampling actual path did not execute once: " + path)
        if case.attrib["name"] == "sdk.focused.lubancore_memory_blob_spi":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_memory_blob_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.lubancore_todo_write":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_todo_write_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.lubancore_agentic_rag":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_agentic_rag_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.lubancore_web_fetch":
            check_web_fetch_native(sections[0], commands[0])
        if case.attrib["name"] == "sdk.focused.lubancore_operation_turn_binding":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_operation_turn_binding_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.journal_native_receipts":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_journal_receipt_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.lubancore_lua_build_profile":
            check_lua_build_native(sections[0], commands[0], with_lua)
        if case.attrib["name"] == "sdk.focused.lubancore_package_inventory":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_package_inventory_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.lubancore_job_operations":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_job_operation_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.memory_project_commit_handoff":
            check_memory_handoff_native(sections[0], commands[0])
        if case.attrib["name"] == "sdk.focused.v3_result_immutable_publication":
            check_result_immutable_native(sections[0], commands[0])
        if case.attrib["name"].removeprefix("sdk.focused.") in MANAGED_OPENING_SOURCES:
            check_managed_opening_native(sections[0], commands[0], case.attrib["name"].removeprefix("sdk.focused."))
        if case.attrib["name"] == "sdk.focused.lubancore_owned_job_deadline":
            check_owned_job_deadline_native(sections[0], commands[0])
        if case.attrib["name"] == "sdk.focused.v3_journal_receipts":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_v3_journal_witness_native(sections[0], registered["command"])
        if case.attrib["name"] == "sdk.focused.run_command_execution_limits":
            registered = next(test for test in tests if test["name"] == case.attrib["name"])
            check_command_limits_native(sections[0], registered["command"])
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
    print(f"SDK focused: all {len(required)} registered test files executed nonempty native test cases")


if __name__ == "__main__":
    main()
