#!/usr/bin/env python3
"""Install, relocate, and exercise real Worker/Runner processes on remote CI."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


@dataclass(frozen=True)
class Host:
    name: str
    source_dir: str
    script: str
    components: tuple[str, ...]
    required_cases: frozenset[str]
    resource_dir: str | None = None

    @property
    def test_name(self) -> str:
        return f"{self.name}.process.lifecycle"

    @property
    def label(self) -> str:
        return f"{self.name}-process"


HOSTS = {
    "worker": Host("worker", "tests/integration/worker_host", "test_worker_process.py",
                   ("LubanCore", "WorkerHost"), frozenset({
                       "health", "same_cwd_sessions", "operation_idempotency", "approval_routing",
                       "cancel_routing", "detach_reattach", "stale_attachment", "eof_cleanup",
                       "killed_worker_resume", "wire_payload_boundary", "assistant_full_text",
                   }), "share/lubancore"),
    "runner": Host("runner", "tests/runner", "test_runner_process.py", ("ExperimentRunner",),
                   frozenset({
                       "worker_exit_survival", "node_exit_survival", "same_project_sessions",
                       "idempotent_start", "session_nonce_fencing", "runner_crash_indeterminate",
                       "starting_window_indeterminate", "cancel_process_group", "state_directory_fencing",
                       "launch_failure_durable", "exit_status_and_logs",
                   })),
}


def run(command: list[str], env: dict[str, str], *, cwd: Path,
        log: Path | None = None) -> str:
    print("+ " + shlex.join(command), flush=True)
    try:
        result = subprocess.run(command, cwd=cwd, env=env, text=True, encoding="utf-8",
                                errors="replace", stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False, timeout=600)
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        if log is not None:
            log.write_text(output, encoding="utf-8")
        raise RuntimeError(f"command exceeded 600 seconds: {command[0]}") from error
    output = result.stdout
    if log is not None:
        log.write_text(output, encoding="utf-8")
    if output:
        print(output, end="" if output.endswith("\n") else "\n", flush=True)
    if result.returncode:
        raise RuntimeError(f"command exited with {result.returncode}: {command[0]}")
    return output


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def check_registration(host: Host, build: Path, repo: Path, env: dict[str, str], evidence: Path) -> None:
    listing = run(["ctest", "--test-dir", str(build), "-C", "Release", "-L",
                   f"^{host.label}$", "--show-only=json-v1"], env, cwd=repo,
                  log=evidence / "registered-tests.json")
    tests = json.loads(listing).get("tests", [])
    matches = [test for test in tests if test.get("name") == host.test_name]
    if len(matches) != 1:
        raise RuntimeError(f"expected exactly one registered {host.test_name} test")
    test = matches[0]
    properties = {prop["name"]: prop["value"] for prop in test.get("properties", [])}
    if properties.get("DISABLED"):
        raise RuntimeError(f"{host.test_name} is disabled")
    if host.label not in properties.get("LABELS", []):
        raise RuntimeError(f"{host.test_name} has no {host.label} label")
    timeout = properties.get("TIMEOUT", 0)
    if not isinstance(timeout, (int, float)) or not 0 < timeout <= 600:
        raise RuntimeError(f"{host.test_name} must have a bounded timeout of at most 600 seconds")
    expected_script = repo / host.source_dir / host.script
    if not any(Path(argument).resolve() == expected_script for argument in test.get("command", [])):
        raise RuntimeError(f"{host.test_name} does not run its real process test script")


def isolated_environment(repo: Path, build: Path, staging: Path, prefix: Path) -> dict[str, str]:
    env = os.environ.copy()
    # Loading from the producer checkout or stale build would conceal an
    # incomplete installed package. Keep ordinary system tool directories.
    blocked = (repo, build, staging)
    path_entries = []
    for entry in env.get("PATH", "").split(os.pathsep):
        if not entry:
            continue
        resolved = Path(entry).resolve()
        if not any(resolved == root or root in resolved.parents for root in blocked):
            path_entries.append(entry)
    env["PATH"] = os.pathsep.join([str(prefix / "bin"), *path_entries])
    for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH",
                "LubanCore_DIR", "LubanCore_ROOT", "CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE"):
        env.pop(key, None)
    env["PYTHONUNBUFFERED"] = "1"
    return env


def suppress_windows_dialogs() -> None:
    if sys.platform != "win32":
        return
    import ctypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetErrorMode.argtypes = []
    kernel32.GetErrorMode.restype = ctypes.c_uint
    kernel32.SetErrorMode.argtypes = [ctypes.c_uint]
    kernel32.SetErrorMode.restype = ctypes.c_uint
    kernel32.SetErrorMode(kernel32.GetErrorMode() | 0x0001 | 0x0002 | 0x8000)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--host", choices=sorted(HOSTS), required=True)
    args = parser.parse_args()
    host = HOSTS[args.host]
    repo = Path(__file__).resolve().parents[2]
    build = args.build_dir.resolve()
    runner_temp = os.environ.get("RUNNER_TEMP")
    if not runner_temp:
        raise RuntimeError("RUNNER_TEMP is required for this remote CI check")
    scratch = Path(tempfile.mkdtemp(prefix=f"{host.name}-ci-", dir=runner_temp)).resolve()
    if scratch == repo or repo in scratch.parents:
        raise RuntimeError("Host relocation must take place outside the producer checkout")
    evidence = build / "test-evidence" / host.label
    evidence.mkdir(parents=True, exist_ok=True)
    staging = scratch / "staging"
    prefix = scratch / "installed"
    test_source = scratch / "process-tests"
    binary = prefix / "bin" / (f"luban-{host.name}" + (".exe" if sys.platform == "win32" else ""))
    resources = prefix / host.resource_dir if host.resource_dir else None
    context = {
        "schemaVersion": 1,
        "githubSha": os.environ.get("GITHUB_SHA"),
        "platform": sys.platform,
        "producerSource": str(repo),
        "producerBuild": str(build),
        "installedPrefix": str(prefix),
        "executable": str(binary),
        "resourceRoot": str(resources) if resources else None,
        "host": host.name,
        "requiredScenarios": sorted(host.required_cases),
        "status": "pending",
    }
    context_path = evidence / "context.json"
    write_json(context_path, context)
    suppress_windows_dialogs()
    env = os.environ.copy()
    check_registration(host, build, repo, env, evidence)
    for component in host.components:
        run(["cmake", "--install", str(build), "--config", "Release", "--prefix",
             str(staging), "--component", component], env, cwd=repo,
            log=evidence / f"install-{component}.log")
    if staging.resolve().parent != scratch or prefix.resolve().parent != scratch:
        raise RuntimeError("Host relocation must stay inside its CI scratch directory")
    staging.rename(prefix)
    if not binary.is_file() or (resources is not None and not resources.is_dir()):
        raise RuntimeError("Host installation is missing its executable or resource root")
    context["executableSha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    write_json(context_path, context)
    shutil.copytree(repo / host.source_dir, test_source)
    report_path = evidence / "process-results.json"
    command = [sys.executable, str(test_source / host.script), f"--{host.name}", str(binary),
               "--report", str(report_path)]
    if resources is not None:
        command.extend(["--resource-root", str(resources)])
    run(command,
        isolated_environment(repo, build, staging, prefix), cwd=scratch,
        log=evidence / "process-test.log")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    tests = report.get("tests") if isinstance(report, dict) else None
    if not isinstance(report, dict) or report.get("schemaVersion") != 1 or not isinstance(tests, list) or not tests:
        raise RuntimeError("Host process test did not report any executed scenarios")
    names = set()
    for test in tests:
        if not isinstance(test, dict):
            raise RuntimeError("Host process report contains a malformed scenario")
        name = test.get("name")
        if not isinstance(name, str) or not name or name in names:
            raise RuntimeError("Host process report has an empty or duplicate scenario name")
        names.add(name)
        if test.get("status") != "passed":
            raise RuntimeError(f"Host process scenario did not pass: {name}")
    missing = host.required_cases - names
    if missing:
        raise RuntimeError("Required process scenarios were not executed: " + ", ".join(sorted(missing)))
    context["status"] = "passed"
    context["passedScenarios"] = sorted(names)
    write_json(context_path, context)
    print(f"Relocated {host.name} passed {len(names)} real process scenarios", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, TypeError) as error:
        print(f"Process host installation check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
