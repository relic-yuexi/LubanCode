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
    "sdk.consumer.named_results",
    "sdk.consumer.command_jobs",
    "sdk.consumer.web_fetch",
    "sdk.consumer.authorization",
    "sdk.consumer.packages",
    "sdk.consumer.package_inventory",
    "sdk.consumer.lua_build_profile",
    "sdk.consumer.smoke", "sdk.consumer.isolation", "sdk.consumer.extensions",
    "sdk.consumer.actions",
    "sdk.consumer.event_sink",
    "sdk.consumer.memory_blobs",
    "sdk.consumer.todo_write",
    "sdk.consumer.agentic_rag", "sdk.consumer.agentic_rag_demo",
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
    "include/lubancore/named_results.hpp",
    "include/lubancore/jobs.hpp",
    "include/lubancore/web_fetch.hpp",
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


def check_todo_consumer_source(repo: Path) -> dict:
    """Seal the relocated Todo helper's public/standard-library include surface."""
    try:
        from .check_sdk_only_boundary import CPP_TOKENS, STANDARD_HEADERS, includes_in, without_comments
    except ImportError:
        try:
            from check_sdk_only_boundary import CPP_TOKENS, STANDARD_HEADERS, includes_in, without_comments
        except ModuleNotFoundError:
            from scripts.ci.check_sdk_only_boundary import CPP_TOKENS, STANDARD_HEADERS, includes_in, without_comments
    relative = "examples/sdk-consumer/todo_write.cpp"
    path = repo / relative
    if not path.is_file():
        raise RuntimeError("Todo relocated helper source is missing")
    raw = path.read_bytes()
    text = raw.decode("utf-8")
    # Synthetic directives inside raw literals are not preprocessor includes.
    directives = CPP_TOKENS.sub(lambda match: re.sub(r"[^\n]", " ", match.group())
                               if match.group().startswith('R"') else match.group(), without_comments(text))
    includes = list(includes_in(text))
    if len(re.findall(r"^\s*#\s*include\b", directives, flags=re.M)) != len(includes):
        raise RuntimeError("Todo relocated helper contains a dynamic include")
    headers = [match.group(1) for match in includes]
    if headers.count("lubancore/core.hpp") != 1:
        raise RuntimeError("Todo relocated helper must include the installed SDK core header once")
    if any(header not in STANDARD_HEADERS and header != "lubancore/core.hpp" for header in headers):
        raise RuntimeError("Todo relocated helper exposes a private/nonstandard include")
    if any(not re.match(r"#\s*include\s*<", match.group().lstrip()) for match in includes):
        raise RuntimeError("Todo relocated helper includes must use installed/standard search paths")
    return {"path": relative, "sha256": hashlib.sha256(raw).hexdigest(), "includes": headers}


def check_todo_consumer_copy(source: Path, seal: dict) -> dict:
    path = source / "todo_write.cpp"
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != seal["sha256"]:
        raise RuntimeError("Todo relocated helper differs from its public source seal")
    return {"source": str(path), "sha256": seal["sha256"], "includes": seal["includes"]}


def check_rag_consumer_evidence(listing: dict, sections: list[str], context: dict,
                               scratch: Path, prefix: Path, consumer_build: Path) -> None:
    """Pair the two real RAG registrations with their executed, complete logs."""
    try:
        from .check_sdk_focused import check_agentic_rag_consumer, check_agentic_rag_demo
        from .sdk_rag_demo import inside
    except ImportError:
        try:
            from check_sdk_focused import check_agentic_rag_consumer, check_agentic_rag_demo
            from sdk_rag_demo import inside
        except ModuleNotFoundError:
            from scripts.ci.check_sdk_focused import check_agentic_rag_consumer, check_agentic_rag_demo
            from scripts.ci.sdk_rag_demo import inside
    for name, demo in (("sdk.consumer.agentic_rag", False), ("sdk.consumer.agentic_rag_demo", True)):
        tests = [test for test in listing.get("tests", []) if test.get("name") == name]
        logs = [sections[index + 1] for index in range(1, len(sections), 2) if sections[index] == name]
        if len(tests) != 1 or len(logs) != 1:
            raise RuntimeError("RAG registration and native log must identify one executed test: " + name)
        if any(prop.get("name") == "DISABLED" and prop.get("value") for prop in tests[0].get("properties", [])):
            raise RuntimeError("RAG registration is disabled: " + name)
        command = tests[0].get("command")
        if demo:
            check_agentic_rag_demo(logs[0], command, context, scratch, prefix)
        else:
            check_agentic_rag_consumer(logs[0], command)
            if (not inside(Path(command[0]).resolve(), consumer_build.resolve()) or
                    not Path(command[0]).is_file() or not Path(command[0]).stat().st_size or
                    not inside(Path(command[2]).resolve(), consumer_build.resolve())):
                raise RuntimeError("RAG consumer borrowed a binary/state outside its actual relocated build")


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


def run_demo_command(args: list[str], env: dict[str, str], receipt: Path,
                     *, label: str = "RAG independent") -> None:
    """Retain the actual independent configure/ALL result even when it fails."""
    report = {"argv": args, "status": "running", "returncode": None,
              "output": str(receipt.with_suffix(".log")), "output_encoding": "raw_bytes"}
    receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("+ " + shlex.join(args), flush=True)
    try:
        result = subprocess.run(args, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
    except OSError as error:
        report.update(status="failed", error=str(error))
        receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        raise
    receipt.with_suffix(".log").write_bytes(result.stdout or b"")
    if result.stdout:
        print(result.stdout.decode("utf-8", errors="replace"), flush=True)
    report.update(status="passed" if result.returncode == 0 else "failed", returncode=result.returncode)
    receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{label} command exited with {result.returncode}: {args[0]}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--install-mode", choices=("component", "full"), default="component")
    parser.add_argument("--lua-profile", choices=("on", "off"), default="on")
    parser.add_argument("--lua-cross-profile-context", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    todo_source = check_todo_consumer_source(repo)
    try:
        from . import sdk_rag_demo as rag
    except ImportError:
        import sdk_rag_demo as rag
    rag_source = rag.seal_sources(repo)
    try:
        from . import sdk_web_fetch_fixture as web_fixture
    except ImportError:
        import sdk_web_fetch_fixture as web_fixture
    web_source = web_fixture.seal_helper(repo)
    try:
        from . import sdk_command_jobs as command_jobs
    except ImportError:
        import sdk_command_jobs as command_jobs
    job_sources = command_jobs.seal_sources(repo)
    try:
        from . import sdk_named_results as named_results
    except ImportError:
        import sdk_named_results as named_results
    named_sources = named_results.seal_sources(repo)
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
    demo_source = scratch / "rag-source"
    demo_build = scratch / "rag-build"
    evidence = producer_build / "test-evidence" / "sdk-consumer"
    evidence.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    for name in ("LubanCore_DIR", "LubanCore_ROOT", "CMAKE_PREFIX_PATH", "CMAKE_TOOLCHAIN_FILE"):
        env.pop(name, None)
    job_fixture = command_jobs.prepare_probe_fixture(repo, scratch,
                                                     evidence / "command-jobs-probe-source")
    (evidence / "command-jobs-probe-fixture.json").write_text(
        json.dumps(job_fixture, indent=2) + "\n", encoding="utf-8")
    # The SDK-only product graph intentionally has no testing targets. Build
    # this independent, standard-only fixture on the remote CI runner, then
    # relocate its actual File API artifact. Never enable producer tests here.
    try:
        run_demo_command(job_fixture["configure"], env, evidence / "command-jobs-probe-configure.json",
                         label="Command Jobs private probe configure")
        run_demo_command(job_fixture["buildCommand"], env, evidence / "command-jobs-probe-build.json",
                         label="Command Jobs private probe build")
    finally:
        # Preserve the actual reply before source/target/artifact eligibility.
        # Capture failure must not replace the original configure/build error.
        try:
            observed = command_jobs.preserve_probe_reply(job_fixture["build"],
                                                         evidence / "command-jobs-file-api-observed")
        except Exception as error:
            observed = {"schemaVersion": 1, "githubSha": os.environ.get("GITHUB_SHA"),
                        "producerBuild": job_fixture["build"], "status": "capture_failed",
                        "acceptance": "not_evaluated", "error": str(error)}
        (evidence / "command-jobs-file-api-observed.json").write_text(
            json.dumps(observed, indent=2) + "\n", encoding="utf-8")
    if observed["status"] != "copied":
        raise RuntimeError("Command Jobs private probe original File API evidence unavailable")
    job_probe = command_jobs.copy_probe(job_fixture["source"], job_fixture["build"], scratch,
                                       evidence=evidence / "command-jobs-file-api")
    job_probe["fixture"] = job_fixture
    job_probe["fileApiObserved"] = observed
    (evidence / "command-jobs-probe.json").write_text(json.dumps(job_probe, indent=2) + "\n", encoding="utf-8")
    (evidence / "consumer-context.json").write_text(json.dumps({
        "github_sha": os.environ.get("GITHUB_SHA"),
        "producer_source": str(repo),
        "producer_build": str(producer_build),
        "consumer_source": str(consumer_source),
        "consumer_build": str(consumer_build),
        "installed_prefix": str(prefix),
        "required_tests": sorted(required_tests), "lua_profile": args.lua_profile, "status": "running",
        "install_mode": args.install_mode,
        "todo_consumer_source": todo_source,
        "rag_source": rag_source, "rag_demo": None,
        "web_fetch_source": web_source, "web_fetch_fixture": None,
        "command_jobs_sources": job_sources, "command_jobs_probe": job_probe,
        "named_results_sources": named_sources,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"SDK consumer evidence directory: {scratch}", flush=True)

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
    web_header = web_fixture.check_installed_header(repo, prefix)
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
    (evidence / "todo-consumer-source.json").write_text(
        json.dumps(check_todo_consumer_copy(consumer_source, todo_source), indent=2) + "\n", encoding="utf-8")
    (evidence / "rag-consumer-source.json").write_text(
        json.dumps(rag.check_copy(consumer_source, rag_source, consumer=True), indent=2) + "\n", encoding="utf-8")
    (evidence / "web-fetch-consumer-source.json").write_text(
        json.dumps(web_fixture.check_helper_copy(consumer_source, web_source), indent=2) + "\n", encoding="utf-8")
    job_source_copies = command_jobs.check_copies(consumer_source, prefix, job_sources, job_probe,
                                                 evidence=evidence / "command-jobs-source-copies")
    (evidence / "command-jobs-source-copies.json").write_text(
        json.dumps(job_source_copies, indent=2) + "\n", encoding="utf-8")
    named_source_copies = named_results.check_copies(consumer_source, prefix, named_sources, repo,
                                                    evidence=evidence / "named-results-source-copies")
    (evidence / "named-results-source-copies.json").write_text(
        json.dumps(named_source_copies, indent=2) + "\n", encoding="utf-8")
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

    # A separate source tree and configure/build prove the documented example
    # consumes the relocated package. No add_subdirectory or producer executable.
    rag.copy_demo(repo, demo_source, rag_source)
    rag.prepare(demo_build)
    demo_configure = ["cmake", "-S", str(demo_source), "-B", str(demo_build),
                      "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={prefix}",
                      "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"]
    demo_all_build = ["cmake", "--build", str(demo_build), "--config", "Release", "--parallel", "4"]
    demo_evidence = {"status": "running", "source": str(demo_source), "build": str(demo_build),
                     "scratch": str(scratch), "installed_prefix": str(prefix), "source_seal": rag_source,
                     "configure_argv": demo_configure, "all_build_argv": demo_all_build}
    try:
        run_demo_command(demo_configure, env, evidence / "rag-demo-configure.json")
        run_demo_command(demo_all_build, env, evidence / "rag-demo-all-build.json")
        demo_context = rag.inspect_demo(scratch, demo_source, demo_build, prefix, repo, rag_source,
                                       producer_build=producer_build)
    except Exception as error:
        demo_evidence.update(status="failed", error=str(error))
        # Preserve the original configure/build exception even if diagnostics
        # themselves cannot be copied from a partially generated build tree.
        try:
            rag.preserve_demo(evidence, demo_build, demo_source, demo_evidence)
            context_path = evidence / "consumer-context.json"
            failed_context = json.loads(context_path.read_text(encoding="utf-8"))
            failed_context.update(status="failed", rag_demo=demo_evidence)
            context_path.write_text(json.dumps(failed_context, indent=2) + "\n", encoding="utf-8")
        except OSError as diagnostic_error:
            print("RAG failure evidence copy failed: " + str(diagnostic_error), file=sys.stderr, flush=True)
        raise
    demo_evidence.update(demo_context)
    rag.preserve_demo(evidence, demo_build, demo_source, demo_evidence)

    with web_fixture.FixtureOwner(repo, scratch, evidence / "web-fetch-fixture", env) as fixture:
        run(["cmake", "-S", str(consumer_source), "-B", str(consumer_build),
             "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
             f"-DCMAKE_PREFIX_PATH={prefix}",
             f"-DLUBANCORE_CONSUMER_RESOURCE_ROOT={prefix / 'share/lubancore'}",
             f"-DLUBANCORE_CONSUMER_RAG_DEMO_EXECUTABLE={demo_context['executable']}",
             f"-DLUBANCORE_CONSUMER_COMMAND_PROBE={job_probe['copy']['path']}",
             f"-DLUBANCORE_CONSUMER_WEB_FETCH_BASE_URL={fixture.context['ready']['base_url']}",
             f"-DLUBANCORE_CONSUMER_WEB_FETCH_REQUESTS_FILE={fixture.context['requests_file']}",
             "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
             "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"], fixture.env)
        cache = (consumer_build / "CMakeCache.txt").read_text(encoding="utf-8")
        package_values = [line.split("=", 1)[1] for line in cache.splitlines()
                          if line.startswith("LubanCore_DIR:PATH=")]
        if len(package_values) != 1:
            raise RuntimeError("consumer did not resolve a LubanCore package directory")
        package_dir = Path(package_values[0]).resolve()
        if package_dir != prefix and prefix not in package_dir.parents:
            raise RuntimeError(f"consumer resolved LubanCore outside the installed prefix: {package_dir}")
        print(f"consumer resolved installed package: {package_dir}", flush=True)
        run(["cmake", "--build", str(consumer_build), "--config", "Release", "--parallel", "4"], fixture.env)
        test_listing = fixture.caller("registration", ["ctest", "--test-dir", str(consumer_build), "-C", "Release",
                            "--show-only=json-v1"], consumer_build, evidence / "consumer-results.xml")
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
        web_test = next(test for test in listing["tests"] if test["name"] == "sdk.consumer.web_fetch")
        smoke_test = next(test for test in listing["tests"] if test["name"] == "sdk.consumer.smoke")
        job_tests = [test for test in listing["tests"] if test["name"] == "sdk.consumer.command_jobs"]
        if len(job_tests) != 1 or len(job_tests[0].get("command", [])) != 4:
            raise RuntimeError("Command Jobs consumer registration is missing or malformed")
        job_test = job_tests[0]
        if (job_test["command"][0] != smoke_test["command"][0] or
                not Path(job_test["command"][0]).resolve().is_relative_to(consumer_build.resolve()) or
                Path(job_test["command"][2]).resolve() != (consumer_build / "state-command-jobs").resolve() or
                Path(job_test["command"][3]).resolve() != Path(job_probe["copy"]["path"]).resolve()):
            raise RuntimeError("Command Jobs consumer/probe/state borrowed a foreign path")
        (evidence / "command-jobs-registration.json").write_text(json.dumps(job_test, indent=2) + "\n", encoding="utf-8")
        named_tests = [test for test in listing["tests"] if test["name"] == "sdk.consumer.named_results"]
        if len(named_tests) != 1 or len(named_tests[0].get("command", [])) != 4:
            raise RuntimeError("Named results consumer registration is missing or malformed")
        named_test = named_tests[0]
        if (named_test["command"][0] != smoke_test["command"][0] or
                not Path(named_test["command"][0]).resolve().is_relative_to(consumer_build.resolve()) or
                named_test["command"][1] != "named-results" or
                Path(named_test["command"][2]).resolve() != (consumer_build / "state-named-results").resolve() or
                Path(named_test["command"][3]).resolve() != Path(job_probe["copy"]["path"]).resolve()):
            raise RuntimeError("Named results consumer/probe/state borrowed a foreign path")
        (evidence / "named-results-registration.json").write_text(json.dumps(named_test, indent=2) + "\n", encoding="utf-8")
        fixture.context.update(registration=web_test, consumer_executable=smoke_test["command"][0])
        fixture.save()
        try:
            fixture.caller("execute", ["ctest", "--test-dir", str(consumer_build), "-C", "Release",
                 "--output-on-failure", "--no-tests=error",
                 "--output-junit", str(evidence / "consumer-results.xml")], consumer_build, evidence / "consumer-results.xml")
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
    from check_sdk_focused import check_action_paths, check_event_sink_consumer, check_memory_blob_consumer, check_package_inventory_consumer, check_lua_build_consumer, check_todo_write_consumer
    sections = re.split(r'^\d+/\d+ Testing: ([^\r\n]+)\r?$',
                        (evidence / "LastTest.log").read_text(encoding="utf-8"), flags=re.M)
    check_rag_consumer_evidence(listing, sections, demo_context, scratch, prefix, consumer_build)
    job_sections = [sections[index + 1] for index in range(1, len(sections), 2)
                    if sections[index] == "sdk.consumer.command_jobs"]
    if len(job_sections) != 1:
        raise RuntimeError("Command Jobs native consumer section missing or duplicated")
    job_receipt = command_jobs.check_consumer(job_sections[0], job_test["command"], job_probe)
    command_jobs.check_copies(consumer_source, prefix, job_sources, job_probe)
    (evidence / "command-jobs-acceptance.json").write_text(json.dumps({
        "status": "passed", "registration": job_test, "receipt": job_receipt,
        "probe": job_probe, "sources": job_sources,
        "callers": fixture.context.get("callers", []),
        "scope": "actual registration/JUnit/LastTest plus sealed relocated probe; source assertions check child entry and retirement",
    }, indent=2) + "\n", encoding="utf-8")
    from check_sdk_focused import check_web_fetch_consumer
    named_sections = [sections[index + 1] for index in range(1, len(sections), 2)
                      if sections[index] == "sdk.consumer.named_results"]
    if len(named_sections) != 1:
        raise RuntimeError("Named results native consumer section missing or duplicated")
    named_receipt = named_results.check_consumer(named_sections[0], named_test["command"], job_probe)
    named_results.check_copies(consumer_source, prefix, named_sources, repo)
    (evidence / "named-results-acceptance.json").write_text(json.dumps({
        "status": "passed", "registration": named_test, "receipt": named_receipt,
        "probe": job_probe, "sources": named_sources, "copies": named_source_copies,
        "scope": "actual installed registration/JUnit/LastTest and SDK/STL source/header copies; internal V3 summary witness belongs to original native source",
    }, indent=2) + "\n", encoding="utf-8")
    web_sections = [sections[index + 1] for index in range(1, len(sections), 2)
                    if sections[index] == "sdk.consumer.web_fetch"]
    if len(web_sections) != 1:
        raise RuntimeError("consumer native log must identify exactly one WebFetch invocation")
    check_web_fetch_consumer(web_sections[0], web_test["command"], fixture.context)
    request_receipt = web_fixture.check_requests((evidence / "web-fetch-fixture/requests.tsv").read_bytes())
    web_fixture.check_helper_copy(consumer_source, web_source)
    (evidence / "web-fetch-acceptance.json").write_text(json.dumps({
        "status": "passed", "registration": web_test, "requests": request_receipt,
        "fixture_context": fixture.context, "source_seal": web_source, "public_header": web_header,
    }, indent=2) + "\n", encoding="utf-8")
    rag.check_copy(consumer_source, rag_source, consumer=True)
    after_demo = rag.inspect_demo(scratch, demo_source, demo_build, prefix, repo, rag_source,
                                  producer_build=producer_build)
    if after_demo != demo_context:
        raise RuntimeError("RAG source, build graph, installed package or executable changed during acceptance")
    demo_evidence.update(status="passed", registration=next(test for test in listing["tests"]
                         if test["name"] == "sdk.consumer.agentic_rag_demo"))
    rag.preserve_demo(evidence, demo_build, demo_source, demo_evidence)
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
    todo_sections = [sections[index + 1] for index in range(1, len(sections), 2)
                     if sections[index] == "sdk.consumer.todo_write"]
    todo_tests = [test for test in listing["tests"] if test["name"] == "sdk.consumer.todo_write"]
    if len(todo_sections) != 1 or len(todo_tests) != 1:
        raise RuntimeError("consumer log and registration must identify one Todo write test")
    check_todo_write_consumer(todo_sections[0], todo_tests[0].get("command"))
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
    context.update(status="passed", consumer_executable=profile_command[0], rag_demo=demo_evidence,
                   web_fetch_fixture=fixture.context)
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
