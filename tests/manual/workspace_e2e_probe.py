#!/usr/bin/env python3
"""Workspace 收官验收·真二进制 E2E 探针(单子 §一第 7 条)。

两幕,全程临时 USERPROFILE,不碰真主目录:

  empty   空主目录冷启动——trajectory usage/doctor 对空 root 干净报错
          (稳定码,不崩),且不在主目录造任何旧式目录;
  upgrade 带旧数据升级——用 tests/fixtures/workspace 的脱敏夹具铺出
          旧主目录(平铺会话/旧项目记忆/旧全局记忆/旧 job 队列),升级后
          的二进制只认新根:旧场列不出/验不了(resume 找不到),新根全新
          开张,旧树一字不动(前后文件清单+字节对账)。

用法:
  python tests/manual/workspace_e2e_probe.py <lubancode.exe> [empty|upgrade|all]

退出码 0 = 全过;非 0 = 有幕失败(输出里逐条点名)。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIXTURES = REPO_ROOT / "tests" / "fixtures" / "workspace"

PASSED: list[str] = []
FAILED: list[str] = []


def check(name: str, ok: bool, detail: str = "") -> None:
    if ok:
        PASSED.append(name)
        print(f"  [PASS] {name}")
    else:
        FAILED.append(name)
        print(f"  [FAIL] {name} {detail}")


def run(exe: Path, args: list[str], userprofile: Path, cwd: Path) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    env["USERPROFILE"] = str(userprofile)
    env.pop("LUBANCODE_HOME", None)
    # Windows 下子进程可能按控制台页输出;按 UTF-8 读、坏字节替换,不炸探针。
    return subprocess.run(
        [str(exe), *args],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=env,
        cwd=str(cwd),
        timeout=120,
    )


def tree_digest(root: Path) -> dict[str, tuple[int, str]]:
    """旧树对账:相对路径 -> (字节数, sha256)。"""
    out: dict[str, tuple[int, str]] = {}
    if not root.exists():
        return out
    for path in sorted(root.rglob("*")):
        if path.is_file():
            data = path.read_bytes()
            out[path.relative_to(root).as_posix()] = (len(data), hashlib.sha256(data).hexdigest())
    return out


def plant_legacy_home(home: Path) -> None:
    """夹具造旧主目录(映射关系见 fixtures/workspace/README.md)。"""
    shutil.copytree(FIXTURES / "legacy", home / "sessions")
    shutil.copytree(FIXTURES / "memory" / "project", home / "projects" / "old-key" / "memory")
    shutil.copytree(FIXTURES / "memory" / "user", home / "memory" / "user")
    shutil.copytree(FIXTURES / "memory-jobs", home / "memory-jobs")


def act_empty(exe: Path) -> None:
    print("幕一: 空主目录冷启动")
    with tempfile.TemporaryDirectory(prefix="lubancode-e2e-empty-") as tmp:
        home = Path(tmp) / "home"
        home.mkdir()
        workdir = Path(tmp) / "repo"
        (workdir / ".git").mkdir(parents=True)

        # usage/doctor 对空 root:稳定报错不崩(退出码 1 = 用法/找不到)。
        for verb, key in (("usage", "demo-0000000000000000"), ("doctor", "demo-0000000000000000")):
            result = run(exe, ["trajectory", verb, key], home, workdir)
            check(
                f"empty/{verb}: 干净报错不崩",
                result.returncode in (0, 1) and "Traceback" not in result.stderr,
                f"rc={result.returncode} stderr={result.stderr[:200]}",
            )
        # verify 找不到场:稳定码 session/trajectory not found 类,退出码 1。
        result = run(exe, ["trajectory", "verify", "20990101-000000-ZZZZZZ"], home, workdir)
        check(
            "empty/verify: 找不到旧场如实报",
            result.returncode == 1,
            f"rc={result.returncode} out={result.stdout[:200]}",
        )
        # 主目录不冒出旧式目录。
        leftovers = [p.name for p in home.iterdir()]
        check(
            "empty: 主目录不造旧式目录",
            all(name not in ("sessions", "projects", "trajectories") for name in leftovers),
            f"leftovers={leftovers}",
        )


def act_upgrade(exe: Path) -> None:
    print("幕二: 带旧数据升级(夹具造旧家)")
    with tempfile.TemporaryDirectory(prefix="lubancode-e2e-upgrade-") as tmp:
        home = Path(tmp) / "home"
        home.mkdir()
        plant_legacy_home(home)
        before = {
            "sessions": tree_digest(home / "sessions"),
            "projects": tree_digest(home / "projects"),
        }
        workdir = Path(tmp) / "repo"
        (workdir / ".git").mkdir(parents=True)

        # 旧场(平铺档的 session id)在新账里找不到。
        legacy_ids = []
        for jsonl in (home / "sessions").glob("*.jsonl"):
            legacy_ids.append(jsonl.stem)
        check("upgrade: 夹具里有旧场", bool(legacy_ids), "没有可用的夹具场")
        for session_id in legacy_ids[:2]:
            result = run(exe, ["trajectory", "verify", session_id], home, workdir)
            check(
                f"upgrade/verify {session_id[:16]}…: 旧场验不到",
                result.returncode == 1,
                f"rc={result.returncode} out={result.stdout[:160]}",
            )
            result = run(exe, ["trajectory", "replay", session_id], home, workdir)
            check(
                f"upgrade/replay {session_id[:16]}…: 旧场折不了",
                result.returncode in (1, 2),
                f"rc={result.returncode}",
            )

        # doctor(全局记忆健康)带旧 job 队列跑得动,不崩。
        result = run(exe, ["trajectory", "doctor", "old-key"], home, workdir)
        check(
            "upgrade/doctor: 稳定退出不崩",
            result.returncode in (0, 1, 2) and "Traceback" not in result.stderr,
            f"rc={result.returncode} stderr={result.stderr[:200]}",
        )

        # 旧树一字不动。
        for name, digest in before.items():
            after = tree_digest(home / name)
            check(
                f"upgrade: 旧 {name}/ 一字不动",
                after == digest,
                f"{len(after)} vs {len(digest)} 项",
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path, help="lubancode.exe 路径")
    parser.add_argument("act", choices=["empty", "upgrade", "all"], nargs="?", default="all")
    args = parser.parse_args()
    if not args.exe.is_file():
        print(f"找不到二进制: {args.exe}")
        return 2
    if args.act in ("empty", "all"):
        act_empty(args.exe)
    if args.act in ("upgrade", "all"):
        act_upgrade(args.exe)
    print(f"\n合计: {len(PASSED)} 过 / {len(FAILED)} 败")
    if FAILED:
        print("失败项: " + ", ".join(FAILED))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
