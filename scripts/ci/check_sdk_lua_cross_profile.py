"""Run two already verified relocated consumer images in separate CI processes."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def validate_image(context: dict, profile: str, sha: str, scratch: Path) -> tuple[Path, Path]:
    if (not isinstance(context, dict) or context.get("status") != "passed" or
            context.get("lua_profile") != profile or context.get("github_sha") != sha or
            not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{40}", sha)):
        raise RuntimeError("Lua cross image lacks passed same-head profile evidence")
    values = []
    for key in ("installed_prefix", "consumer_build", "consumer_executable"):
        raw = context.get(key)
        if not isinstance(raw, str) or not raw or "\0" in raw or not Path(raw).is_absolute():
            raise RuntimeError("Lua cross image has a malformed absolute " + key)
        value = Path(raw).resolve()
        if scratch not in value.parents:
            raise RuntimeError("Lua cross image is outside the CI scratch root")
        values.append(value)
    prefix, build, executable = values
    if (not prefix.is_dir() or not build.is_dir() or not executable.is_file() or
            build not in executable.parents or executable.name not in ("lubancore_consumer", "lubancore_consumer.exe")):
        raise RuntimeError("Lua cross image is not the actual relocated consumer executable")
    return prefix, executable


def check_phase_output(stdout: str, mode: str, profile: str) -> None:
    marker = "[sdk-lua-build-cross] " + mode + " " + profile + " complete"
    if [line for line in stdout.splitlines() if line.startswith("[sdk-lua-build-cross] ")] != [marker]:
        raise RuntimeError("Lua cross image did not finish its actual phase once")


def tree_fingerprints(root: Path) -> dict[str, str]:
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(root.rglob("*")) if path.is_file()}


def run_cross_images(on_context_path: Path, off_context_path: Path, evidence: Path) -> dict:
    scratch_env, sha = os.environ.get("RUNNER_TEMP"), os.environ.get("GITHUB_SHA", "")
    if not scratch_env or not Path(scratch_env).is_absolute():
        raise RuntimeError("Lua cross image requires the absolute CI RUNNER_TEMP")
    scratch = Path(scratch_env).resolve()
    contexts = {"on": json.loads(on_context_path.read_text(encoding="utf-8")),
                "off": json.loads(off_context_path.read_text(encoding="utf-8"))}
    images = {profile: validate_image(context, profile, sha, scratch) for profile, context in contexts.items()}
    if images["on"] == images["off"] or contexts["on"]["producer_build"] == contexts["off"]["producer_build"]:
        raise RuntimeError("Lua cross image requires two different producer/consumer images")
    if contexts["on"]["producer_source"] != contexts["off"]["producer_source"]:
        raise RuntimeError("Lua cross images do not share the same producer source")
    headers = {profile: tree_fingerprints(prefix / "include/lubancore")
               for profile, (prefix, _) in images.items()}
    if not headers["on"] or headers["on"] != headers["off"]:
        raise RuntimeError("Lua cross images changed the installed public header bytes")
    state = Path(tempfile.mkdtemp(prefix="sdk-lua-cross-", dir=scratch)).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    report = {"githubSha": sha, "stateRoot": str(state), "phases": [], "status": "running",
              "publicHeaderFingerprints": headers["on"]}
    def phase(mode, profile):
        prefix, executable = images[profile]
        env = os.environ.copy()
        for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"):
            env.pop(key, None)
        blocked = {Path(context[key]).resolve() for context in contexts.values()
                   for key in ("producer_source", "producer_build", "installed_prefix", "consumer_build")}
        entries = [value for value in env.get("PATH", "").split(os.pathsep) if value and
                   not any(Path(value).resolve() == path or path in Path(value).resolve().parents for path in blocked)]
        env["PATH"] = os.pathsep.join(entries)
        if sys.platform == "win32": env["PATH"] = str(prefix / "bin") + os.pathsep + env["PATH"]
        elif sys.platform.startswith("linux"): env["LD_LIBRARY_PATH"] = os.pathsep.join(map(str, (prefix / "lib", prefix / "lib64")))
        command = [str(executable), mode, str(state)]
        result = subprocess.run(command, env=env, capture_output=True, text=True, encoding="utf-8", timeout=120)
        (evidence / (mode + "-" + profile + ".stdout")).write_text(result.stdout, encoding="utf-8")
        (evidence / (mode + "-" + profile + ".stderr")).write_text(result.stderr, encoding="utf-8")
        report["phases"].append({"command": command, "profile": profile, "returncode": result.returncode,
                                 "imagePrefix": str(prefix), "executableSha256": hashlib.sha256(executable.read_bytes()).hexdigest()})
        if result.returncode: raise RuntimeError("Lua cross image phase failed: " + mode)
        check_phase_output(result.stdout, mode, profile)
    try:
        phase("lua-build-seed-off", "on")
        plans = list((state / "disabled/data").rglob("sdk-lua-plan.json"))
        if len(plans) != 1: raise RuntimeError("Lua cross disabled seed did not create one real frozen plan")
        disabled_plan = plans[0]
        disabled_bytes = disabled_plan.read_bytes()
        phase("lua-build-resume-off", "off")
        if disabled_plan.read_bytes() != disabled_bytes: raise RuntimeError("OFF restore rewrote disabled frozen plan")
        phase("lua-build-resume-off", "on")
        if disabled_plan.read_bytes() != disabled_bytes: raise RuntimeError("ON restore rewrote disabled frozen plan")
        phase("lua-build-seed-on", "on")
        original = tree_fingerprints(state / "enabled/data")
        phase("lua-build-reject-on", "off")
        current = tree_fingerprints(state / "enabled/data")
        if current != original: raise RuntimeError("OFF refusal modified enabled original state")
        report.update(status="passed", disabledPlanSha256=hashlib.sha256(disabled_bytes).hexdigest(),
                      enabledOriginalFingerprints=original, enabledRefusedFingerprints=current)
        return report
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (evidence / "cross-profile.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
