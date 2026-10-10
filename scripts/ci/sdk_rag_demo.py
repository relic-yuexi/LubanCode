"""Seal and inspect the independently built installed-SDK RAG example.

These functions only inspect files/JSON. The installed acceptance driver owns
the actual remote configure, ALL build, registration and executed test log.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil

try:
    from .check_sdk_only_boundary import CLIENT, CPP_TOKENS, STANDARD_HEADERS, includes_in, prepare, read_reply, without_comments
except ImportError:
    try:
        from check_sdk_only_boundary import CLIENT, CPP_TOKENS, STANDARD_HEADERS, includes_in, prepare, read_reply, without_comments
    except ModuleNotFoundError:
        from scripts.ci.check_sdk_only_boundary import CLIENT, CPP_TOKENS, STANDARD_HEADERS, includes_in, prepare, read_reply, without_comments

FILES = {
    "agentic_rag.cpp": "examples/sdk-consumer/agentic_rag.cpp",
    "main.cpp": "examples/sdk-rag/main.cpp",
    "CMakeLists.txt": "examples/sdk-rag/CMakeLists.txt",
    "README.md": "examples/sdk-rag/README.md",
}
COMPILED = {"main.cpp", "agentic_rag.cpp"}


def digest(path: Path) -> str:
    if not path.is_file() or path.is_symlink():
        raise RuntimeError("RAG sealed regular file is missing or linked: " + str(path))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def public_includes(path: Path, required_core: bool) -> list[str]:
    text = path.read_text(encoding="utf-8")
    directives = CPP_TOKENS.sub(lambda match: re.sub(r"[^\n]", " ", match.group())
                               if match.group().startswith('R"') else match.group(), without_comments(text))
    matches = list(includes_in(text))
    if len(re.findall(r"^\s*#\s*include\b", directives, flags=re.M)) != len(matches):
        raise RuntimeError("RAG example has a dynamic include")
    headers = [match.group(1) for match in matches]
    if headers.count("lubancore/core.hpp") != int(required_core):
        raise RuntimeError("RAG example must use the selected installed public core include")
    if any(header not in STANDARD_HEADERS and header != "lubancore/core.hpp" for header in headers):
        raise RuntimeError("RAG example contains a private/nonstandard include")
    if any(not re.match(r"#\s*include\s*<", match.group().lstrip()) for match in matches):
        raise RuntimeError("RAG example includes must use installed/standard search paths")
    return headers


def seal_sources(repo: Path) -> dict:
    files = {}
    for name, relative in FILES.items():
        path = repo / relative
        files[name] = {"source": relative, "sha256": digest(path)}
        if name in COMPILED:
            files[name]["includes"] = public_includes(path, name == "agentic_rag.cpp")
    return {"files": files}


def check_copy(source: Path, seal: dict, *, consumer: bool = False) -> dict:
    if not isinstance(seal, dict) or not isinstance(seal.get("files"), dict) or set(seal["files"]) != set(FILES):
        raise RuntimeError("RAG source seal is missing or has a different roster")
    names = {"agentic_rag.cpp"} if consumer else set(FILES)
    if not consumer and {path.name for path in source.iterdir()} != names:
        raise RuntimeError("RAG flat source copy must contain exactly four sealed files")
    copies = {}
    for name in names:
        expected = seal["files"][name]
        if not isinstance(expected, dict) or expected.get("source") != FILES[name] or not re.fullmatch(r"[0-9a-f]{64}", expected.get("sha256", "")):
            raise RuntimeError("RAG source seal entry is malformed")
        path = source / name
        if digest(path) != expected["sha256"]:
            raise RuntimeError("RAG copied source differs from its seal: " + name)
        if name in COMPILED and public_includes(path, name == "agentic_rag.cpp") != expected.get("includes"):
            raise RuntimeError("RAG copied source has a different include surface")
        copies[name] = {"path": str(path.resolve()), **expected}
    return copies


def copy_demo(repo: Path, source: Path, seal: dict) -> dict:
    source.mkdir(parents=True, exist_ok=False)
    for name, relative in FILES.items():
        shutil.copyfile(repo / relative, source / name)
    return check_copy(source, seal)


def inside(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def _root(value: Path, label: str) -> Path:
    if not value.is_absolute() or value.is_symlink():
        raise RuntimeError("RAG " + label + " must be an explicit absolute unlinked root")
    return value.resolve()


def _reply(reply: Path, reference: dict) -> dict:
    filename = reference.get("jsonFile") if isinstance(reference, dict) else None
    if (not isinstance(filename, str) or not filename or Path(filename).name != filename or
            not (reply / filename).is_file() or (reply / filename).is_symlink()):
        raise RuntimeError("RAG File API reference is missing or escapes its reply directory")
    return read_reply(reply, reference)


def inspect_demo(scratch: Path, source: Path, build: Path, prefix: Path, producer: Path,
                 seal: dict, config: str = "Release", *, producer_build: Path | None = None) -> dict:
    scratch = _root(scratch, "scratch")
    source, build, prefix, producer = (_root(value, label) for value, label in (
        (source, "source"), (build, "build"), (prefix, "installed prefix"), (producer, "producer")))
    roots = (source, build, prefix)
    if (not all(inside(path, scratch) and path != scratch for path in roots) or
            any(inside(left, right) for left in roots for right in roots if left != right) or
            len(set(roots)) != 3):
        raise RuntimeError("RAG independent source/build/install must be distinct roots inside this CI scratch")
    forbidden = [producer]
    if producer_build is not None:
        forbidden.append(_root(producer_build, "producer build"))
    if any(inside(path, root) for path in (scratch, source, build, prefix) for root in forbidden):
        raise RuntimeError("RAG demo must not borrow a producer source/build directory")
    copies = check_copy(source, seal)
    reply = build / ".cmake/api/v1/reply"
    indices = sorted(reply.glob("index-*.json"))
    if not indices:
        raise RuntimeError("RAG demo has no actual File API reply")
    index = json.loads(indices[-1].read_text(encoding="utf-8"))
    references = index.get("reply", {}).get(CLIENT, {})
    if not all(name in references for name in ("codemodel-v2", "cache-v2")):
        raise RuntimeError("RAG File API queries are missing")
    model = _reply(reply, references["codemodel-v2"])
    cache = _reply(reply, references["cache-v2"])
    if (model.get("kind") != "codemodel" or model.get("version", {}).get("major") != 2 or
            cache.get("kind") != "cache" or cache.get("version", {}).get("major") != 2):
        raise RuntimeError("RAG File API kind/version differs")
    if (Path(model.get("paths", {}).get("source", "")).resolve() != source or
            Path(model.get("paths", {}).get("build", "")).resolve() != build):
        raise RuntimeError("RAG File API belongs to a different independent project")
    configurations = [entry for entry in model.get("configurations", []) if entry.get("name") == config]
    if len(configurations) != 1:
        raise RuntimeError("RAG demo requires one actual requested configuration")
    entries = {}
    for entry in cache.get("entries", []):
        if entry.get("name") in entries:
            raise RuntimeError("RAG File API cache contains duplicate keys")
        entries[entry.get("name")] = entry.get("value")
    package_value = entries.get("LubanCore_DIR")
    if not isinstance(package_value, str) or not Path(package_value).is_absolute():
        raise RuntimeError("RAG demo did not resolve an absolute installed SDK package")
    package = Path(package_value).resolve()
    if not inside(package, prefix) or not (package / "LubanCoreConfig.cmake").is_file():
        raise RuntimeError("RAG demo resolved Core outside this relocated prefix")
    if Path(entries.get("CMAKE_HOME_DIRECTORY", "")).resolve() != source:
        raise RuntimeError("RAG demo cache belongs to a different source root")
    targets = []
    identities = set()
    for reference in configurations[0].get("targets", []):
        target = _reply(reply, reference)
        if (not target.get("id") or target.get("id") != reference.get("id") or
                target.get("name") != reference.get("name") or target.get("id") in identities):
            raise RuntimeError("RAG File API contains duplicate or foreign target identity")
        identities.add(target.get("id")); targets.append(target)
    executable_targets = [target for target in targets if target.get("type") == "EXECUTABLE"]
    if len(executable_targets) != 1 or executable_targets[0].get("name") != "lubancore_rag":
        raise RuntimeError("RAG demo must own one actual standalone executable")
    target = executable_targets[0]
    compiled = []
    for owner in targets:
        groups = owner.get("compileGroups", [])
        for entry in owner.get("sources", []):
            if "compileGroupIndex" not in entry:
                continue
            number = entry["compileGroupIndex"]
            if type(number) is not int or not 0 <= number < len(groups) or groups[number].get("language") != "CXX":
                raise RuntimeError("RAG compiled source has no actual CXX group")
            path = Path(entry.get("path", ""))
            path = (path if path.is_absolute() else source / path).resolve()
            if owner is not target or path not in {source / name for name in COMPILED}:
                raise RuntimeError("RAG demo compiled a foreign or unsealed source")
            if digest(path) != seal["files"][path.name]["sha256"]:
                raise RuntimeError("RAG actual compiled source differs from the copied seal")
            compiled.append(path.name)
    if sorted(compiled) != sorted(COMPILED):
        raise RuntimeError("RAG demo must compile each of its two sealed CPP sources once")
    includes = [Path(item.get("path", "")).resolve() for group in target.get("compileGroups", []) for item in group.get("includes", [])]
    if prefix / "include" not in includes or any(inside(path, root) for path in includes for root in forbidden):
        raise RuntimeError("RAG demo did not compile against only this installed public SDK include root")
    fragments = target.get("link", {}).get("commandFragments", [])
    libraries = []
    for fragment in fragments:
        if fragment.get("role") != "libraries":
            continue
        for token in re.findall(r'"[^\"]*"|\S+', fragment.get("fragment", "")):
            value = token.strip('"'); path = Path(value)
            if re.fullmatch(r"(?:lib)?lubancore(?:\.[0-9]+)*\.(?:so(?:\.[0-9]+)*|dylib|lib|dll\.a)", path.name):
                if not path.is_absolute() or not inside(path.resolve(), prefix) or not path.is_file():
                    raise RuntimeError("RAG demo linked Core outside the relocated prefix")
                libraries.append(str(path.resolve()))
    if len(libraries) != 1:
        raise RuntimeError("RAG actual link must name the single relocated Core library")
    artifacts = []
    for entry in target.get("artifacts", []):
        path = Path(entry.get("path", "")); path = (path if path.is_absolute() else build / path).resolve()
        if not inside(path, build):
            raise RuntimeError("RAG demo artifact escaped its independent build")
        if path.name in ("lubancore_rag", "lubancore_rag.exe"):
            if not path.is_file() or not path.stat().st_size:
                raise RuntimeError("RAG actual standalone binary is missing or empty")
            artifacts.append(path)
    if len(artifacts) != 1:
        raise RuntimeError("RAG actual File API must identify one built standalone binary")
    return {"status": "built", "scratch": str(scratch), "source": str(source), "build": str(build),
            "installed_prefix": str(prefix), "producer_source": str(producer), "configuration": config,
            "file_api_index": str(indices[-1]), "copied_files": copies, "compiled_sources": compiled,
            "sdk_package": str(package), "sdk_package_sha256": digest(package / "LubanCoreConfig.cmake"),
            "core_libraries": libraries, "core_library_sha256": digest(Path(libraries[0])),
            "executable": str(artifacts[0]), "executable_sha256": digest(artifacts[0])}


def check_demo_context(context: dict, scratch: Path, prefix: Path) -> None:
    if not isinstance(context, dict) or context.get("status") not in {"built", "passed"}:
        raise RuntimeError("RAG demo context is missing or unbuilt")
    scratch, prefix = scratch.resolve(), prefix.resolve()
    for name in ("scratch", "source", "build", "installed_prefix", "executable", "file_api_index"):
        value = context.get(name)
        if not isinstance(value, str) or not Path(value).is_absolute() or not inside(Path(value).resolve(), scratch):
            raise RuntimeError("RAG demo context path is foreign: " + name)
    if (Path(context["scratch"]).resolve() != scratch or Path(context["installed_prefix"]).resolve() != prefix or
            not inside(Path(context["executable"]).resolve(), Path(context["build"]).resolve()) or
            Path(context["executable"]).name not in ("lubancore_rag", "lubancore_rag.exe") or
            sorted(context.get("compiled_sources", [])) != sorted(COMPILED) or
            set(context.get("copied_files", {})) != set(FILES)):
        raise RuntimeError("RAG demo context differs from this independent installed image")
    if not inside(Path(context["file_api_index"]).resolve(), Path(context["build"]).resolve() / ".cmake/api/v1/reply"):
        raise RuntimeError("RAG demo context has a foreign File API index")
    package = context.get("sdk_package")
    libraries = context.get("core_libraries")
    if (not isinstance(package, str) or not Path(package).is_absolute() or not inside(Path(package).resolve(), prefix) or
            not isinstance(libraries, list) or len(libraries) != 1 or not isinstance(libraries[0], str) or
            not Path(libraries[0]).is_absolute() or not inside(Path(libraries[0]).resolve(), prefix)):
        raise RuntimeError("RAG demo context has a foreign installed SDK package/link")
    if digest(Path(context["executable"])) != context.get("executable_sha256"):
        raise RuntimeError("RAG demo executable differs from its actual built artifact")
    if (digest(Path(package) / "LubanCoreConfig.cmake") != context.get("sdk_package_sha256") or
            digest(Path(libraries[0])) != context.get("core_library_sha256")):
        raise RuntimeError("RAG demo installed SDK bytes differ from its actual built image")


def preserve_demo(evidence: Path, build: Path, source: Path, report: dict) -> None:
    target = evidence / "agentic-rag-demo"
    if target.exists():
        shutil.rmtree(target)
    target.mkdir()
    # Failed configure/builds may not have generated a cache or replies. Keep
    # what actually exists plus the failed context, without fabricating a graph.
    reply = build / ".cmake/api/v1/reply"
    if reply.is_dir():
        shutil.copytree(reply, target / "file-api")
    if (build / "CMakeCache.txt").is_file():
        shutil.copyfile(build / "CMakeCache.txt", target / "CMakeCache.txt")
    if source.is_dir():
        shutil.copytree(source, target / "source")
    (target / "context.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
