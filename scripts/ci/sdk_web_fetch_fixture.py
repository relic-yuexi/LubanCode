"""Own the installed WebFetch HTTP fixture and preserve its actual receipts.

Only FixtureOwner.start/close launch or retire a process, in remote CI. Source,
registration, request-ledger and lifecycle validators below are pure data/file
checks. Native reference tests use their existing in-process FakeHttpServer.
"""
from __future__ import annotations

from collections import Counter
import hashlib
import json
import ntpath
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import time


HELPER = "examples/sdk-consumer/web_fetch.cpp"
HEADER = "include/lubancore/web_fetch.hpp"
SERVER = "tests/fixtures/sdk_web_fetch_http.py"
PROXY_KEYS = ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy")
ENVIRONMENT = {**{name: None for name in PROXY_KEYS}, "NO_PROXY": "127.0.0.1", "no_proxy": "127.0.0.1"}
ROUTES = (
    "/admission/plain", "/content/html", "/content/utf8", "/content/binary", "/content/status",
    "/limits/exact", "/limits/body", "/limits/gzip", "/limits/header", "/limits/output",
    "/limits/cumulative-body", "/limits/body-final", "/limits/cumulative-header", "/limits/header-final",
    "/redirects/relative", "/redirects/final", "/redirects/loop-a", "/redirects/loop-b",
    "/redirects/limit", "/redirects/scheme", "/redirects/duplicate",
    "/cancel/wait", "/cancel/close", "/cancel/shutdown", "/cancel/timeout",
)
EXPECTED_REQUESTS = Counter({(route, "lubancore"): 1 for route in ROUTES})
EXPECTED_REQUESTS.update({("/isolation/" + str(i), "isolation-" + str(i)): 2 if i == 1 else 1
                          for i in range(4)})


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def absolute(value):
    return isinstance(value, str) and bool(value) and (
        PurePosixPath(value.replace("\\", "/")).is_absolute() or
        (ntpath.isabs(value) and bool(ntpath.splitdrive(value)[0])))


def digest(path: Path) -> str:
    require(path.is_file() and not path.is_symlink(), "WebFetch evidence source is absent or linked: " + str(path))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_url(value):
    require(isinstance(value, str), "WebFetch fixture URL must be text")
    match = re.fullmatch(r"http://127\.0\.0\.1:([1-9][0-9]{0,4})", value)
    require(match is not None and int(match[1]) <= 65535, "WebFetch fixture URL must name one exact loopback listener")


def seal_helper(repo: Path) -> dict:
    try:
        from .sdk_rag_demo import public_includes
    except ImportError:
        from sdk_rag_demo import public_includes
    path = repo / HELPER
    return {"source": HELPER, "sha256": digest(path), "includes": public_includes(path, True)}


def check_installed_header(repo: Path, prefix: Path) -> dict:
    try:
        from .check_sdk_only_boundary import CPP_TOKENS, STANDARD_HEADERS, includes_in, without_comments
    except ImportError:
        from check_sdk_only_boundary import CPP_TOKENS, STANDARD_HEADERS, includes_in, without_comments
    def public_includes(path):
        text = path.read_text(encoding="utf-8")
        directives = CPP_TOKENS.sub(lambda match: re.sub(r"[^\n]", " ", match.group())
                                   if match.group().startswith('R"') else match.group(), without_comments(text))
        matches = list(includes_in(text))
        headers = [match.group(1) for match in matches]
        require(len(re.findall(r"^\s*#\s*include\b", directives, flags=re.M)) == len(matches) and
                headers.count("lubancore/api.hpp") == 1 and
                all(header in STANDARD_HEADERS or header == "lubancore/api.hpp" for header in headers) and
                all(re.match(r"#\s*include\s*<", match.group().lstrip()) for match in matches),
                "WebFetch public header may include only its public API types and standard headers")
        return headers
    original, installed = repo / HEADER, prefix / HEADER
    headers = public_includes(original)
    require(digest(installed) == digest(original) and public_includes(installed) == headers,
            "WebFetch installed header changed or imported a nonstandard dependency")
    return {"source": HEADER, "sha256": digest(original), "includes": headers, "installed": str(installed)}


def check_helper_copy(source: Path, seal: dict) -> dict:
    require(isinstance(seal, dict) and seal.get("source") == HELPER and
            re.fullmatch(r"[0-9a-f]{64}", seal.get("sha256", "")), "WebFetch helper seal is malformed")
    try:
        from .sdk_rag_demo import public_includes
    except ImportError:
        from sdk_rag_demo import public_includes
    path = source / "web_fetch.cpp"
    require(digest(path) == seal["sha256"] and public_includes(path, True) == seal.get("includes"),
            "WebFetch relocated helper differs from its public source seal")
    return {"path": str(path.resolve()), **seal}


def fixture_environment(inherited):
    env = inherited.copy()
    for name, value in ENVIRONMENT.items():
        if value is None:
            env.pop(name, None)
        else:
            env[name] = value
    return env


def check_ready(value, pid, requests: Path, stop: Path):
    require(isinstance(value, dict) and set(value) == {"schemaVersion", "base_url", "pid", "requests_file", "stop_file"},
            "WebFetch fixture ready record has a different schema")
    require(type(value["schemaVersion"]) is int and value["schemaVersion"] == 1 and
            type(value["pid"]) is int and value["pid"] == pid and pid > 0,
            "WebFetch ready record does not belong to the actual child PID")
    check_url(value["base_url"])
    require(value["requests_file"] == str(requests) and value["stop_file"] == str(stop),
            "WebFetch ready record changed its owned files")
    require(requests.is_file() and not requests.is_symlink(), "WebFetch fixture did not create its request ledger")


def check_requests(raw: bytes):
    require(0 < len(raw) <= 65536 and raw.endswith(b"\n"), "WebFetch request ledger is empty, truncated or unbounded")
    try:
        lines = raw.decode("ascii").split("\n")[:-1]
    except UnicodeError as error:
        raise RuntimeError("WebFetch request ledger is not the fixture's ASCII protocol") from error
    pairs = []
    for line in lines:
        fields = line.split("\t")
        require(len(fields) == 2 and all(fields), "WebFetch request ledger has a malformed record")
        pairs.append(tuple(fields))
    require(Counter(pairs) == EXPECTED_REQUESTS,
            "WebFetch actual request ledger has missing, duplicate, foreign or wrong-agent requests")
    return {"records": len(pairs), "sha256": hashlib.sha256(raw).hexdigest(),
            "requests": [{"target": target, "user_agent": agent} for target, agent in pairs]}


def check_closed_context(context):
    require(isinstance(context, dict) and type(context.get("schemaVersion")) is int and context["schemaVersion"] == 1,
            "WebFetch fixture context is missing")
    require(type(context.get("pid")) is int and context["pid"] > 0 and
            context.get("ready", {}).get("pid") == context["pid"], "WebFetch actual fixture PID differs")
    require(context.get("status") == "closed" and context.get("stop_written") is True and
            context.get("forced") is False and type(context.get("returncode")) is int and
            context["returncode"] == 0 and context.get("alive_after_wait") is False and
            context.get("early_exit") is False and not context.get("start_error") and not context.get("cleanup_error"),
            "WebFetch fixture did not retire normally after its explicit stop")
    require(context.get("environment") == ENVIRONMENT, "WebFetch fixture/caller environment declaration differs")
    ready = context["ready"]
    check_url(ready.get("base_url"))
    require(ready.get("requests_file") == context.get("requests_file") and
            ready.get("stop_file") == context.get("stop_file"), "WebFetch lifecycle files differ from ready receipt")
    argv = context.get("argv", [])
    require(isinstance(argv, list) and len(argv) == 8 and all(isinstance(value, str) and value for value in argv) and
            absolute(argv[0]) and argv[1] == context.get("fixture_source") and
            argv[2:] == ["--ready-file", context.get("ready_file"), "--requests-file", context.get("requests_file"),
                         "--stop-file", context.get("stop_file")], "WebFetch fixture startup argv differs")
    require(re.fullmatch(r"[0-9a-f]{64}", context.get("fixture_sha256", "")) is not None,
            "WebFetch fixture source seal is absent")
    require(absolute(context.get("cwd")) and absolute(context.get("caller_cwd")), "WebFetch owned directories must be absolute")
    normalized = lambda value: value.replace("\\", "/").rstrip("/")
    owned_root = normalized(context["cwd"])
    require(owned_root == normalized(context["caller_cwd"]) + "/web-fetch-fixture", "WebFetch fixture root is outside its caller scratch")
    for key, filename in (("ready_file", "ready.json"), ("requests_file", "requests.tsv"),
                          ("stop_file", "stop"), ("fixture_source", "sdk_web_fetch_http.py")):
        require(absolute(context.get(key)) and normalized(context[key]) == owned_root + "/" + filename,
                "WebFetch lifecycle file escaped its explicit fixture owner: " + key)


def check_caller_receipts(context):
    check_closed_context(context)
    callers = context.get("callers", [])
    require(isinstance(callers, list) and all(isinstance(item, dict) for item in callers) and
            [item.get("phase") for item in callers] == ["registration", "execute"],
            "WebFetch fixture lacks the actual registration/execution callers")
    for item in callers:
        require(item.get("status") == "completed" and type(item.get("returncode")) is int and item["returncode"] == 0 and
                item.get("environment") == ENVIRONMENT and item.get("cwd") == context.get("caller_cwd"),
                "WebFetch caller did not succeed under its declared environment")
        argv = item.get("argv", [])
        require(isinstance(argv, list) and len(argv) >= 6 and all(isinstance(value, str) and value for value in argv) and
                absolute(argv[0]) and
                Path(argv[0]).name.lower() in ("ctest", "ctest.exe") and
                argv[1:3] == ["--test-dir", context.get("consumer_build")] and argv[3:5] == ["-C", "Release"],
                "WebFetch caller argv differs from the actual installed test directory")
        if item["phase"] == "registration":
            require(argv[5:] == ["--show-only=json-v1"], "WebFetch registration caller filtered the installed tests")
        else:
            require(argv[5:] == ["--output-on-failure", "--no-tests=error", "--output-junit", context.get("consumer_junit")],
                    "WebFetch execution caller filtered or changed the installed test command")
        require(re.fullmatch(r"[0-9a-f]{64}", item.get("stdout_sha256", "")) is not None and
                type(item.get("stdout_bytes")) is int and item["stdout_bytes"] > 0,
                "WebFetch actual caller receipt lacks its original output")


class FixtureOwner:
    def __init__(self, repo: Path, scratch: Path, evidence: Path, env: dict):
        self.repo, self.scratch, self.evidence = repo.resolve(), scratch.resolve(), evidence.resolve()
        self.root = self.scratch / "web-fetch-fixture"
        self.env = fixture_environment(env)
        self.process = None
        self.handles = []
        self.context = {"schemaVersion": 1, "status": "preparing", "pid": None,
                        "githubSha": os.environ.get("GITHUB_SHA"), "environment": ENVIRONMENT,
                        "forced": False, "stop_written": False, "returncode": None, "alive_after_wait": None,
                        "callers": [], "caller_cwd": str(self.scratch)}

    def save(self):
        self.evidence.mkdir(parents=True, exist_ok=True)
        (self.evidence / "context.json").write_text(json.dumps(self.context, indent=2) + "\n", encoding="utf-8")
        for name in ("ready.json", "requests.tsv", "stop", "fixture.stdout", "fixture.stderr", "sdk_web_fetch_http.py"):
            path = self.root / name
            if path.is_file() and not path.is_symlink():
                shutil.copyfile(path, self.evidence / name)

    def start(self):
        self.root.mkdir(parents=True, exist_ok=False)
        source = self.root / "sdk_web_fetch_http.py"
        original = self.repo / SERVER
        original_hash = digest(original)
        shutil.copyfile(original, source)
        require(digest(source) == original_hash, "WebFetch fixture copy differs from its source")
        ready, requests, stop = (self.root / name for name in ("ready.json", "requests.tsv", "stop"))
        argv = [sys.executable, str(source), "--ready-file", str(ready), "--requests-file", str(requests), "--stop-file", str(stop)]
        self.context.update(argv=argv, cwd=str(self.root), fixture_source=str(source), fixture_sha256=original_hash,
                            ready_file=str(ready), requests_file=str(requests), stop_file=str(stop), status="starting")
        self.save()
        try:
            output, errors = (self.root / name for name in ("fixture.stdout", "fixture.stderr"))
            self.handles = [output.open("wb"), errors.open("wb")]
            flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
            self.process = subprocess.Popen(argv, cwd=self.root, env=self.env, stdin=subprocess.DEVNULL,
                                            stdout=self.handles[0], stderr=self.handles[1], creationflags=flags)
            self.context["pid"] = self.process.pid
            self.save()
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                require(self.process.poll() is None, "WebFetch fixture exited before ready")
                if ready.is_file():
                    require(not ready.is_symlink() and ready.stat().st_size <= 4096, "WebFetch ready file is linked or oversized")
                    raw = ready.read_bytes()
                    if raw.endswith(b"\n"):
                        value = json.loads(raw)
                        check_ready(value, self.process.pid, requests, stop)
                        self.context.update(ready=value, status="ready")
                        self.save()
                        return self
                time.sleep(0.02)
            raise RuntimeError("WebFetch fixture did not publish its actual ready receipt")
        except BaseException as error:
            self.context.update(status="failed", start_error=str(error))
            try:
                self.close()
            except Exception as cleanup_error:
                self.context["cleanup_error"] = str(cleanup_error)
                self.save()
            raise

    def close(self):
        if self.context.get("status") == "closed":
            return
        started = time.monotonic()
        early = self.process is not None and self.process.poll() is not None
        if self.process is not None:
            try:
                if not early:
                    Path(self.context["stop_file"]).write_bytes(b"stop\n")
                    self.context["stop_written"] = True
                try:
                    self.context["returncode"] = self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.context["forced"] = True
                    self.process.terminate()
                    try:
                        self.context["returncode"] = self.process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        self.process.kill()
                        self.context["returncode"] = self.process.wait(timeout=2)
            finally:
                self.context["alive_after_wait"] = self.process.poll() is None
        for handle in self.handles:
            handle.close()
        self.handles.clear()
        self.context.update(status="closed", early_exit=early,
                            close_elapsed_ms=int((time.monotonic() - started) * 1000))
        self.save()
        check_closed_context(self.context)
        require(not early, "WebFetch fixture exited before its owned stop")
        require(digest(Path(self.context["fixture_source"])) == self.context["fixture_sha256"],
                "WebFetch fixture source changed during acceptance")

    def caller(self, phase, argv, consumer_build: Path, junit: Path):
        require(phase in ("registration", "execute") and
                phase not in [item["phase"] for item in self.context["callers"]], "WebFetch caller phase duplicated")
        require(self.process is not None and self.process.poll() is None and self.context["status"] == "ready",
                "WebFetch fixture must be alive for its actual caller")
        command = list(argv)
        command[0] = shutil.which(command[0], path=self.env.get("PATH")) or command[0]
        require(absolute(command[0]), "WebFetch caller must resolve the actual CTest executable")
        self.context.update(consumer_build=str(consumer_build), consumer_junit=str(junit))
        print("WebFetch fixture owns caller: " + " ".join(command), flush=True)
        receipt = {"phase": phase, "argv": command, "cwd": str(self.scratch),
                   "environment": ENVIRONMENT, "status": "running", "returncode": None}
        self.context["callers"].append(receipt)
        self.save()
        try:
            result = subprocess.run(command, cwd=self.scratch, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except OSError as error:
            receipt.update(status="spawn-failed", error=str(error))
            self.save()
            raise
        (self.evidence / ("caller-" + phase + ".stdout")).write_bytes(result.stdout)
        (self.evidence / ("caller-" + phase + ".stderr")).write_bytes(result.stderr)
        receipt.update({"status": "completed", "returncode": result.returncode,
            "stdout_bytes": len(result.stdout), "stdout_sha256": hashlib.sha256(result.stdout).hexdigest(),
            "stderr_bytes": len(result.stderr), "stderr_sha256": hashlib.sha256(result.stderr).hexdigest()})
        self.save()
        if phase == "execute":
            print(result.stdout.decode("utf-8", errors="replace"), end="", flush=True)
        if result.stderr:
            print(result.stderr.decode("utf-8", errors="replace"), file=sys.stderr, end="", flush=True)
        require(result.returncode == 0, "installed SDK caller failed with exit " + str(result.returncode))
        return result.stdout.decode("utf-8")

    def __enter__(self):
        return self.start()

    def __exit__(self, exception_type, error, traceback):
        try:
            self.close()
        except Exception as cleanup_error:
            self.context["cleanup_error"] = str(cleanup_error)
            self.save()
            if error is None:
                raise
            print("WebFetch fixture cleanup failed after caller failure: " + str(cleanup_error), file=sys.stderr)
        return False
