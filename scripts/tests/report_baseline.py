#!/usr/bin/env python3
"""测试基线记录器(T16/V3-ADD-02,SessionV3 清理单 §九 勾五)。

运行测试前把"这一轮跑的到底是什么"记成机器可读的一笔账:
版本、HEAD、工作区 diff 指纹、夹具树 hash、构建产物、测试环境。
基线更新时 --compare 对旧账,变化项点名——旧通过数字不随基线漂移
复用,要复跑才作数。

用法:
  python scripts/tests/report_baseline.py --out baseline.json \
      [--build-dir build] [--label "macos-clang PR#xxx"]   # 记账
  python scripts/tests/report_baseline.py --out new.json \
      --compare old.json                                    # 记账+对旧账

--compare 只点名差异并写进输出 JSON 的 "changes" 字段,退出码恒 0:
判绿是 CI 测试步的事,本工具只保证"跑没跑、跑的什么"有账可查。
git 不可用/非工作区时如实记 unavailable,不伪装成干净基线。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

# 同 ctest_format_registry:Windows 控制台 cp1252 编不出中文,切 UTF-8 防文案炸红。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

REPO_ROOT = Path(__file__).resolve().parents[2]
VERSION_HPP = REPO_ROOT / "src" / "app" / "version.hpp"
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"

VERSION_RE = re.compile(r'kVersion\s*=\s*"([^"]+)"')


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_version() -> str:
    try:
        text = VERSION_HPP.read_text(encoding="utf-8")
        match = VERSION_RE.search(text)
        return match.group(1) if match else "unknown"
    except OSError:
        return "unavailable"


def git_output(*args: str) -> str | None:
    """跑一条 git 命令;失败(非仓库/无 git)回 None,不抛。"""
    try:
        result = subprocess.run(
            ["git", *args], cwd=REPO_ROOT, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout if result.returncode == 0 else None


def collect_git() -> dict:
    head = git_output("rev-parse", "HEAD")
    if head is None:
        return {"available": False,
                "reason": "git 不可用或不在工作区——HEAD/diff 无账,如实记"}
    status = git_output("status", "--porcelain") or ""
    diff = git_output("diff", "HEAD") or ""
    staged = git_output("diff", "--cached") or ""
    return {
        "available": True,
        "head": head.strip(),
        "branch": (git_output("rev-parse", "--abbrev-ref", "HEAD") or "").strip(),
        "status_porcelain": status.splitlines(),
        "worktree_diff_sha256": hashlib.sha256(diff.encode("utf-8")).hexdigest(),
        "staged_diff_sha256": hashlib.sha256(staged.encode("utf-8")).hexdigest(),
        "dirty": bool(status.strip()),
    }


def collect_fixtures() -> dict:
    """夹具树逐文件 hash;聚合出整树指纹(内容寻址,不含时间戳)。"""
    files: dict[str, str] = {}
    if not FIXTURES_DIR.is_dir():
        return {"available": False, "reason": f"{FIXTURES_DIR} 不存在"}
    for path in sorted(FIXTURES_DIR.rglob("*")):
        if path.is_file():
            files[path.relative_to(FIXTURES_DIR).as_posix()] = sha256_file(path)
    tree = hashlib.sha256()
    for name, digest in sorted(files.items()):
        tree.update(name.encode("utf-8") + b"\0" + digest.encode() + b"\0")
    return {"available": True, "file_count": len(files), "tree_sha256": tree.hexdigest(),
            "files": files}


def collect_build(build_dir: Path | None) -> dict:
    if build_dir is None:
        return {"recorded": False}
    artifacts = []
    for candidate in (
            build_dir / "Release" / "lubancode.exe", build_dir / "lubancode",
            build_dir / "Release" / "lubancode_tests.exe",
            build_dir / "lubancode_tests",
            build_dir / "tests" / "Release" / "lubancode_tests.exe",
            build_dir / "tests" / "lubancode_tests"):
        if candidate.is_file():
            stat = candidate.stat()
            artifacts.append({
                "path": str(candidate), "size_bytes": stat.st_size,
                "mtime_utc": datetime.fromtimestamp(
                    stat.st_mtime, tz=timezone.utc).isoformat()})
    return {"recorded": True, "build_dir": str(build_dir),
            "artifacts_found": artifacts}


def compare(old: dict, new: dict) -> dict:
    """对旧账点名变化(只记事实,不判绿红)。"""
    changes: dict[str, str] = {}
    for key in ("version", "git_head"):
        old_value = old.get(key)
        new_value = new.get(key)
        if old_value != new_value:
            changes[key] = f"{old_value} -> {new_value}"
    old_tree = (old.get("fixtures") or {}).get("tree_sha256")
    new_tree = (new.get("fixtures") or {}).get("tree_sha256")
    if old_tree != new_tree:
        old_files = (old.get("fixtures") or {}).get("files", {})
        new_files = (new.get("fixtures") or {}).get("files", {})
        added = sorted(set(new_files) - set(old_files))
        removed = sorted(set(old_files) - set(new_files))
        changed = sorted(name for name in set(old_files) & set(new_files)
                         if old_files[name] != new_files[name])
        changes["fixtures_tree"] = (
            f"{old_tree} -> {new_tree}"
            f"(+{len(added)} -{len(removed)} ~{len(changed)};"
            f"改:{changed[:10]}{'…' if len(changed) > 10 else ''})")
    return changes


def main() -> int:
    parser = argparse.ArgumentParser(description="测试基线记录器(T16 勾五)")
    parser.add_argument("--out", type=Path, required=True,
                        help="基线 JSON 落此文件")
    parser.add_argument("--build-dir", type=Path,
                        help="构建目录(记产物路径/大小/mtime)")
    parser.add_argument("--label", default="",
                        help="人读标签(腿名/PR 号等)")
    parser.add_argument("--compare", type=Path,
                        help="旧基线 JSON:对账并把变化写进输出")
    args = parser.parse_args()

    git_info = collect_git()
    baseline = {
        "label": args.label,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "version": read_version(),
        "git_head": git_info.get("head") if git_info.get("available") else None,
        "git": git_info,
        "fixtures": collect_fixtures(),
        "build": collect_build(args.build_dir),
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": sys.version.split()[0],
            "system": platform.system(),
            "repo_root": str(REPO_ROOT),
        },
    }
    if args.compare and args.compare.is_file():
        old = json.loads(args.compare.read_text(encoding="utf-8"))
        baseline["compared_against"] = {
            "label": old.get("label"),
            "generated_at_utc": old.get("generated_at_utc"),
        }
        baseline["changes"] = compare(old, baseline)
        if baseline["changes"]:
            print("基线变化(须重跑相应测试,不复用旧通过数字):")
            for key, value in baseline["changes"].items():
                print(f"  {key}: {value}")
        else:
            print("基线无变化(版本/HEAD/夹具树一致)")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        json.dumps(baseline, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"基线已写 {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
