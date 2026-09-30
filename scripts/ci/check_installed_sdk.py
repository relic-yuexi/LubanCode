#!/usr/bin/env python3
"""Build and run the SDK consumer from its installed prefix on a CI runner."""

from __future__ import annotations

import argparse
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


REQUIRED_TESTS = {
    "sdk.consumer.smoke", "sdk.consumer.isolation", "sdk.consumer.seed", "sdk.consumer.resume",
    "sdk.consumer.recovery_seed", "sdk.consumer.recovery_resume",
}


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
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    producer_build = args.build_dir.resolve()
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
        "required_tests": sorted(REQUIRED_TESTS),
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
    if args.install_mode == "full":
        public_headers = {"include/" + path.relative_to(repo / "include").as_posix()
                          for path in (repo / "include" / "lubancore").rglob("*.hpp")}
        missing_headers = public_headers - set(installed_files)
        if not public_headers or missing_headers:
            raise RuntimeError("full SDK install is missing public headers: " + ", ".join(sorted(missing_headers)))
        package_files = re.compile(r"lib(?:64)?/cmake/LubanCore/LubanCore(?:Config(?:Version)?|Targets(?:-[A-Za-z0-9_]+)?)\.cmake")
        library_files = re.compile(r"lib(?:64)?/(?:liblubancore(?:\.so(?:\.[0-9]+)*|(?:\.[0-9]+)*\.dylib|\.dll\.a)|lubancore\.lib)")
        for relative in installed_files:
            allowed = (relative in public_headers or relative == "share/lubancore/lubancore-sdk.md" or
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
    missing = REQUIRED_TESTS - enabled
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
    if not results or not REQUIRED_TESTS <= executed:
        raise RuntimeError("consumer JUnit is missing required executed tests")
    if len(executed) != len(results) or any(case.find("skipped") is not None or
                                         case.find("failure") is not None or case.find("error") is not None
                                         for case in results):
        raise RuntimeError("consumer JUnit contains duplicate, skipped or failed tests")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"SDK consumer check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
