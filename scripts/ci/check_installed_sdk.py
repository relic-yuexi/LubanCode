#!/usr/bin/env python3
"""Build and run the SDK consumer from its installed prefix on a CI runner."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


try:
    from .sdk_lua_profile import consumer_roster, read_lua_profile
except ImportError:
    try:
        from sdk_lua_profile import consumer_roster, read_lua_profile
    except ModuleNotFoundError:
        from scripts.ci.sdk_lua_profile import consumer_roster, read_lua_profile


REQUIRED_TESTS = {
    "sdk.consumer.authorization",
    "sdk.consumer.packages",
    "sdk.consumer.package_inventory",
    "sdk.consumer.lua_build_profile",
    "sdk.consumer.smoke", "sdk.consumer.isolation", "sdk.consumer.extensions",
    "sdk.consumer.actions",
    "sdk.consumer.event_sink",
    "sdk.consumer.memory_blobs",
    "sdk.consumer.builtin_search",
    "sdk.consumer.results",
    "sdk.consumer.skills_seed", "sdk.consumer.skills_resume",
    "sdk.consumer.memory_seed", "sdk.consumer.memory_resume",
    "sdk.consumer.memory_save_seed", "sdk.consumer.memory_save_resume",
    "sdk.consumer.subagents", "sdk.consumer.subagent_seed", "sdk.consumer.subagent_resume",
    "sdk.consumer.lua", "sdk.consumer.lua_seed", "sdk.consumer.lua_resume",
    "sdk.consumer.result_seed", "sdk.consumer.result_resume",
    "sdk.consumer.seed", "sdk.consumer.resume",
    "sdk.consumer.recovery_seed", "sdk.consumer.recovery_resume",
}
REQUIRED_PUBLIC_HEADERS = {
    "include/lubancore/authorization.hpp",
    "include/lubancore/packages.hpp",
    "include/lubancore/api.hpp", "include/lubancore/core.hpp", "include/lubancore/extensions.hpp",
    "include/lubancore/results.hpp",
    "include/lubancore/skills.hpp",
    "include/lubancore/memory.hpp",
    "include/lubancore/subagents.hpp",
    "include/lubancore/lua.hpp",
    "include/lubancore/events.hpp",
    "include/lubancore/memory_blobs.hpp",
}


def check_search_resources(repo: Path, prefix: Path, staged_dir: Path, platform: str) -> dict:
    """Check relocation preserves exactly the staged backend and license/manifest."""
    binary = "rg.exe" if platform == "win32" else "rg"
    pairs = {
        f"share/lubancore/libexec/{binary}": staged_dir / binary,
        "share/lubancore/licenses/ripgrep/LICENSE-MIT": repo / "third_party/ripgrep/LICENSE-MIT",
        "share/lubancore/ripgrep-manifest.json": repo / "third_party/ripgrep/manifest.json",
    }
    hashes = {}
    for relative, original in pairs.items():
        installed = prefix / relative
        if not original.is_file() or not installed.is_file():
            raise RuntimeError("SDK search resource is missing: " + relative)
        original_hash = hashlib.sha256(original.read_bytes()).hexdigest()
        installed_hash = hashlib.sha256(installed.read_bytes()).hexdigest()
        if not installed.stat().st_size or installed_hash != original_hash:
            raise RuntimeError("SDK search resource differs from its staged/source input: " + relative)
        if relative.endswith("/" + binary) and platform != "win32" and not os.access(installed, os.X_OK):
            raise RuntimeError("SDK search backend lost executable permission: " + relative)
        hashes[relative] = installed_hash
    return hashes


def check_public_headers(repo: Path, installed_files: list[str], install_mode: str) -> set[str]:
    """Validate the public header set for either relocated install mode."""
    public_headers = {"include/" + path.relative_to(repo / "include").as_posix()
                      for path in (repo / "include" / "lubancore").rglob("*.hpp")}
    missing_source = REQUIRED_PUBLIC_HEADERS - public_headers
    if missing_source:
        raise RuntimeError("SDK source is missing required public headers: " +
                           ", ".join(sorted(missing_source)))
    missing_headers = public_headers - set(installed_files)
    if missing_headers:
        raise RuntimeError(f"{install_mode} SDK install is missing public headers: " +
                           ", ".join(sorted(missing_headers)))
    return public_headers


def run(args: list[str], env: dict[str, str], *, capture: bool = False) -> str:
    print("+ " + shlex.join(args), flush=True)
    result = subprocess.run(
        args, env=env, text=True, stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None, check=False,
    )
    if result.returncode:
        if capture and result.stdout:
            print(result.stdout, flush=True)
        raise RuntimeError(f"command exited with {result.returncode}: {args[0]}")
    return result.stdout if capture else ""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--install-mode", choices=("component", "full"), default="component")
    parser.add_argument("--lua-profile", choices=("on", "off"), default="on")
    parser.add_argument("--lua-cross-profile-context", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    producer_build = args.build_dir.resolve()
    profile_cache = (producer_build / "CMakeCache.txt").read_text(encoding="utf-8")
    profile_entries = {line.split(":", 1)[0]: line.split("=", 1)[1] for line in profile_cache.splitlines()
                       if ":" in line and "=" in line and not line.startswith(("#", "//"))}
    with_lua = read_lua_profile(profile_entries, args.lua_profile)
    required_tests = consumer_roster(REQUIRED_TESTS, with_lua)
    if not with_lua and args.lua_cross_profile_context is None:
        raise RuntimeError("Lua OFF installed acceptance requires a passed ON consumer context")
    runner_temp = os.environ.get("RUNNER_TEMP")
    if not runner_temp:
        raise RuntimeError("RUNNER_TEMP is required for this remote CI check")
    if sys.platform == "win32":
        # Children inherit this process error mode. Missing runtime DLLs or
        # crashes must fail CI rather than wait for an invisible system dialog.
        # Preserve inherited flags; this changes only this check and its children.
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetErrorMode.argtypes = []
        kernel32.GetErrorMode.restype = ctypes.c_uint
        kernel32.SetErrorMode.argtypes = [ctypes.c_uint]
        kernel32.SetErrorMode.restype = ctypes.c_uint
        # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
        kernel32.SetErrorMode(kernel32.GetErrorMode() | 0x0001 | 0x0002 | 0x8000)
    scratch = Path(tempfile.mkdtemp(prefix="lubancore-consumer-", dir=runner_temp)).resolve()
    if scratch == repo or repo in scratch.parents:
        raise RuntimeError("consumer must be configured outside the producer source tree")
    staging = scratch / "staging"
    prefix = scratch / "installed"
    consumer_source = scratch / "source"
    consumer_build = scratch / "build"
    evidence = producer_build / "test-evidence" / "sdk-consumer"
    evidence.mkdir(parents=True, exist_ok=True)
    (evidence / "consumer-context.json").write_text(json.dumps({
        "github_sha": os.environ.get("GITHUB_SHA"),
        "producer_source": str(repo),
        "producer_build": str(producer_build),
        "consumer_source": str(consumer_source),
        "consumer_build": str(consumer_build),
        "installed_prefix": str(prefix),
        "required_tests": sorted(required_tests), "lua_profile": args.lua_profile, "status": "running",
        "install_mode": args.install_mode,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"SDK consumer evidence directory: {scratch}", flush=True)

    env = os.environ.copy()
    for name in ("LubanCore_DIR", "LubanCore_ROOT", "CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE"):
        env.pop(name, None)
    install = ["cmake", "--install", str(producer_build), "--config", "Release", "--prefix", str(staging)]
    if args.install_mode == "component":
        install.extend(["--component", "LubanCore"])
    else:
        producer_cache = (producer_build / "CMakeCache.txt").read_text(encoding="utf-8")
        cache_values = {line.split(":", 1)[0]: line.split("=", 1)[1]
                        for line in producer_cache.splitlines() if ":" in line and "=" in line and
                        not line.startswith(("#", "//"))}
        if (cache_values.get("LUBANCODE_BUILD_CLI", "").upper() not in {"OFF", "FALSE", "NO", "0"} or
                cache_values.get("LUBANCODE_BUILD_SDK", "").upper() not in {"ON", "TRUE", "YES", "1"}):
            raise RuntimeError("full SDK install requires CLI=OFF and SDK=ON")
    run(install, env)
    # Relocate the whole package before consuming it; both paths stay in our CI scratch.
    if staging.resolve().parent != scratch or prefix.resolve().parent != scratch:
        raise RuntimeError("SDK relocation must stay inside the CI scratch directory")
    staging.rename(prefix)
    installed_files = sorted(path.relative_to(prefix).as_posix() for path in prefix.rglob("*")
                             if path.is_file() or path.is_symlink())
    (evidence / "installed-files.json").write_text(json.dumps(installed_files, indent=2) + "\n", encoding="utf-8")
    public_headers = check_public_headers(repo, installed_files, args.install_mode)
    producer_cache = (producer_build / "CMakeCache.txt").read_text(encoding="utf-8")
    staged_entries = [line.split("=", 1)[1] for line in producer_cache.splitlines()
                      if line.startswith("LUBANCODE_BUNDLED_RG_DIR:PATH=")]
    if len(staged_entries) != 1 or not staged_entries[0]:
        raise RuntimeError("SDK search consumer requires an explicit bundled-rg stage")
    resource_hashes = check_search_resources(repo, prefix, Path(staged_entries[0]), sys.platform)
    (evidence / "search-resources.json").write_text(json.dumps({
        "githubSha": os.environ.get("GITHUB_SHA"), "resourceRoot": str(prefix / "share/lubancore"),
        "hashes": resource_hashes,
        "scope": "Relocated bytes match the CI-staged backend and repository license/manifest; archive verification is performed by fetch_ripgrep.sh.",
    }, indent=2) + "\n", encoding="utf-8")
    if args.install_mode == "full":
        package_files = re.compile(r"lib(?:64)?/cmake/LubanCore/LubanCore(?:Config(?:Version)?|Targets(?:-[A-Za-z0-9_]+)?)\.cmake")
        library_files = re.compile(r"lib(?:64)?/(?:liblubancore(?:\.so(?:\.[0-9]+)*|(?:\.[0-9]+)*\.dylib|\.dll\.a)|lubancore\.lib)")
        for relative in installed_files:
            allowed = (relative in public_headers or relative in resource_hashes or relative == "share/lubancore/lubancore-sdk.md" or
                       package_files.fullmatch(relative) or library_files.fullmatch(relative) or
                       (sys.platform == "win32" and re.fullmatch(r"bin/[^/]+\.dll", relative, re.IGNORECASE)))
            if not allowed:
                raise RuntimeError("full SDK install contains a host/private-development artifact: " + relative)
    def normalized(value: str) -> str:
        return re.sub(r"/+", "/", value.replace(chr(92), "/")).casefold()
    forbidden = [normalized(str(path)) for path in (repo, producer_build, staging)]
    installed_configs = list(prefix.rglob("*.cmake"))
    if not installed_configs:
        raise RuntimeError("SDK install did not provide any CMake package files")
    for config in installed_configs:
        contents = normalized(config.read_text(encoding="utf-8"))
        if any(path in contents for path in forbidden):
            raise RuntimeError(f"installed CMake package leaks a producer or staging path: {config}")
    shutil.copytree(repo / "examples" / "sdk-consumer", consumer_source)
    # Neither inherited loader variables nor a producer PATH may rescue a
    # broken installed package. Ordinary system compiler/tool directories stay.
    blocked = (repo, producer_build, staging)
    path_entries = []
    for entry in env.get("PATH", "").split(os.pathsep):
        if not entry:
            continue
        resolved = Path(entry).resolve()
        if not any(resolved == root or root in resolved.parents for root in blocked):
            path_entries.append(entry)
    env["PATH"] = os.pathsep.join(path_entries)
    for name in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"):
        env.pop(name, None)
    if sys.platform == "win32":
        env["PATH"] = str(prefix / "bin") + os.pathsep + env.get("PATH", "")
    elif sys.platform.startswith("linux"):
        library_dirs = [str(prefix / "lib"), str(prefix / "lib64")]
        env["LD_LIBRARY_PATH"] = os.pathsep.join(library_dirs)

    run(["cmake", "-S", str(consumer_source), "-B", str(consumer_build),
         "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
         f"-DCMAKE_PREFIX_PATH={prefix}",
         f"-DLUBANCORE_CONSUMER_RESOURCE_ROOT={prefix / 'share/lubancore'}",
         "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
         "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"], env)
    cache = (consumer_build / "CMakeCache.txt").read_text(encoding="utf-8")
    package_values = [line.split("=", 1)[1] for line in cache.splitlines()
                      if line.startswith("LubanCore_DIR:PATH=")]
    if len(package_values) != 1:
        raise RuntimeError("consumer did not resolve a LubanCore package directory")
    package_dir = Path(package_values[0]).resolve()
    if package_dir != prefix and prefix not in package_dir.parents:
        raise RuntimeError(f"consumer resolved LubanCore outside the installed prefix: {package_dir}")
    print(f"consumer resolved installed package: {package_dir}", flush=True)
    run(["cmake", "--build", str(consumer_build), "--config", "Release", "--parallel", "4"], env)
    test_listing = run(["ctest", "--test-dir", str(consumer_build), "-C", "Release",
                        "--show-only=json-v1"], env, capture=True)
    (evidence / "consumer-tests.json").write_text(test_listing, encoding="utf-8")
    listing = json.loads(test_listing)
    enabled = {test["name"] for test in listing["tests"] if not any(
        prop["name"] == "DISABLED" and prop["value"]
        for prop in test.get("properties", []))}
    if len(listing["tests"]) != len(required_tests) or {test["name"] for test in listing["tests"]} != required_tests:
        raise RuntimeError("installed consumer registration differs from its exact Lua profile roster")
    missing = required_tests - enabled
    if missing:
        raise RuntimeError("installed consumer is missing enabled tests: " + ", ".join(sorted(missing)))
    print("installed consumer tests: " + ", ".join(sorted(enabled)), flush=True)
    try:
        run(["ctest", "--test-dir", str(consumer_build), "-C", "Release",
             "--output-on-failure", "--no-tests=error",
             "--output-junit", str(evidence / "consumer-results.xml")], env)
    finally:
        # The consumer lives outside the checkout; keep its diagnostic logs in
        # the producer's artifact directory even when one of its tests fails.
        # Session data stays in scratch and is not part of the CI artifact.
        for name in ("LastTest.log", "LastTestsFailed.log"):
            log = consumer_build / "Testing" / "Temporary" / name
            if log.is_file():
                shutil.copy2(log, evidence / name)
    results = ET.parse(evidence / "consumer-results.xml").getroot().findall(".//testcase")
    executed = {case.attrib.get("name") for case in results}
    if not results or executed != required_tests:
        raise RuntimeError("consumer JUnit is missing required executed tests")
    if len(executed) != len(results) or any(case.attrib.get("status") != "run" or case.find("skipped") is not None or
                                         case.find("failure") is not None or case.find("error") is not None
                                         for case in results):
        raise RuntimeError("consumer JUnit contains duplicate, skipped or failed tests")
    from check_sdk_focused import check_action_paths, check_event_sink_consumer, check_memory_blob_consumer, check_package_inventory_consumer, check_lua_build_consumer
    sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$',
                        (evidence / "LastTest.log").read_text(encoding="utf-8"), flags=re.M)
    action_sections = [sections[index + 1] for index in range(1, len(sections), 2)
                       if sections[index] == "sdk.consumer.actions"]
    if len(action_sections) != 1:
        raise RuntimeError("consumer native log does not identify one Action test")
    check_action_paths(action_sections[0], native=False)
    event_sections = [sections[index + 1] for index in range(1, len(sections), 2)
                      if sections[index] == "sdk.consumer.event_sink"]
    event_tests = [test for test in listing["tests"] if test["name"] == "sdk.consumer.event_sink"]
    if len(event_sections) != 1 or len(event_tests) != 1:
        raise RuntimeError("consumer native log and registration must identify one EventSink test")
    check_event_sink_consumer(event_sections[0], event_tests[0].get("command"))
    memory_sections = [sections[index + 1] for index in range(1, len(sections), 2)
                       if sections[index] == "sdk.consumer.memory_blobs"]
    memory_tests = [test for test in listing["tests"] if test["name"] == "sdk.consumer.memory_blobs"]
    if len(memory_sections) != 1 or len(memory_tests) != 1:
        raise RuntimeError("consumer native log and registration must identify one Memory blob test")
    check_memory_blob_consumer(memory_sections[0], memory_tests[0].get("command"))
    package_sections = [sections[index + 1] for index in range(1, len(sections), 2)
                        if sections[index] == "sdk.consumer.package_inventory"]
    package_tests = [test for test in listing["tests"] if test["name"] == "sdk.consumer.package_inventory"]
    if len(package_sections) != 1 or len(package_tests) != 1:
        raise RuntimeError("consumer log and registration must identify one Package inventory test")
    check_package_inventory_consumer(package_sections[0], package_tests[0].get("command"))
    profile_sections = [sections[index + 1] for index in range(1, len(sections), 2)
                        if sections[index] == "sdk.consumer.lua_build_profile"]
    profile_tests = [test for test in listing["tests"] if test["name"] == "sdk.consumer.lua_build_profile"]
    if len(profile_sections) != 1 or len(profile_tests) != 1:
        raise RuntimeError("consumer log and registration must identify one Lua build profile test")
    profile_command = profile_tests[0].get("command")
    check_lua_build_consumer(profile_sections[0], profile_command, with_lua)
    installed_profile = prefix / "lib" / "cmake" / "LubanCore" / "LubanCoreConfig.cmake"
    if not installed_profile.is_file():
        candidates = list(prefix.rglob("LubanCoreConfig.cmake"))
        if len(candidates) != 1:
            raise RuntimeError("installed Lua profile config is missing or ambiguous")
        installed_profile = candidates[0]
    profile_declaration = re.findall(r'set\(LubanCore_WITH_LUA "(ON|OFF)"\)', installed_profile.read_text(encoding="utf-8"))
    if profile_declaration != ["ON" if with_lua else "OFF"]:
        raise RuntimeError("installed Lua profile differs from the actual producer")
    context_path = evidence / "consumer-context.json"
    context = json.loads(context_path.read_text(encoding="utf-8"))
    context.update(status="passed", consumer_executable=profile_command[0])
    context_path.write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    if not with_lua:
        from check_sdk_lua_cross_profile import run_cross_images
        try:
            run_cross_images(args.lua_cross_profile_context.resolve(), context_path, evidence / "lua-cross-profile")
        except Exception as error:
            context.update(status="failed", lua_cross_profile_error=str(error))
            context_path.write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
            raise


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"SDK consumer check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
