#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LubanCode 安装资源所有权引擎(Linux/macOS install.sh 调用;Windows 的 install.ps1 内置同一套决策表)。

GitHubRelease自动更新单 §四/§五:文件所有权按"上次可信官方清单"判定,不按
目录名判定。决策表(new=新包清单 old=上次官方清单 disk=盘面实况):

  N  D  O  hd==ho   动作
  1  1  1  是       replace      官方未改 → 换新(先备份);hd==hn 则 skip-current
  1  1  1  否       conflict-modified      本地改过 → 原地保留+备份,新版不落此路径
  1  1  0  -        conflict-collision     未知文件撞上新版同路径 → 保留+备份+报冲突
  1  0  1  -        missing-kept           官方件被本地删掉 → 不悄悄复活,报告
  1  0  0  -        install-new            新版新增 → 装
  0  1  1  是       retire                 官方新版删了它且本地未改 → 备份后退役
  0  1  1  否       keep-modified-retired  本地改过且新版已删 → 保留+报告
  0  1  0  -        keep-unknown           用户/未知文件 → 原地保留,报告
  0  0  1  -        盘面无痕,无事可做
  盘面是符号链接/reparse/目录挡在文件路径上 → conflict-reparse / conflict-kind,不写不删

无基线(旧装没有清单)时:能给出同版本可信原包(--baseline-dir)就现建基线走
上表;拿不到、且新包路径与盘面有相撞 → 完整备份+报 needs-review 拒绝动手,
除非显式 --allow-unknown-replace(先完整备份再整目录替换)。无基线也无相撞
(纯新增,删不动任何现有文件)→ 直接装,旁杂文件按 keep-unknown 保留报告。
来源与目标同一目录时不删目录再搬自己——同路径直接跳过。

子命令:plan(只读预演)/ apply(执行)/ backup-only(完整备份不安装)/
write-state(全新装记档,拷贝由调用方完成)。退出码:0 成功,1 出错,3 needs-review。
"""

import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys

SCHEMA = 1
STATE_NAME = "install-state.json"
MANIFEST_NAME = "manifest.json"
ROLE_TOP_DIRS = ("skills", "docs", "web", "libexec", "licenses", "updater")
ROLE_ROOT_FILES = {
    "lubancode": "exe",
    "lubancode.exe": "exe",
    "LICENSE": "license",
    "THIRD_PARTY_NOTICES.md": "notices",
    "README.md": "readme",
    "README.en.md": "readme",
    "install.ps1": "installer",
    "uninstall.ps1": "installer",
    "install.sh": "installer",
    "install_plan.py": "installer",
}
WINDOWS_RESERVED = {
    "CON", "PRN", "AUX", "NUL",
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
}
INVALID_CHARS = set('<>:"|?*')
RECORD_FILES = {MANIFEST_NAME, STATE_NAME}

# Windows/macOS 盘面大小写不敏感:路径键按折叠比较;清单校验已禁大小写碰撞
CASE_FOLD = os.name != "posix" or sys.platform == "darwin"


def fold(path):
    return path.casefold() if CASE_FOLD else path


def valid_relpath(path):
    """路径规则与 generate_manifest.py / install.ps1 同一契约,别单边改。"""
    if not path or len(path) > 512:
        return False
    if path.startswith("/") or "\\" in path or ":" in path:
        return False
    for seg in path.split("/"):
        if seg == "" or seg in (".", ".."):
            return False
        if seg.endswith(".") or seg.endswith(" "):
            return False
        # 保留名连扩展名一起拒:com1.md 在 Windows 上照样惹祸
        stem = seg.split(".", 1)[0].upper()
        if seg.upper() in WINDOWS_RESERVED or stem in WINDOWS_RESERVED:
            return False
        for ch in seg:
            if ch in INVALID_CHARS or ord(ch) < 0x20:
                return False
    return True


def role_for(path):
    top = path.split("/", 1)[0]
    if top in ROLE_TOP_DIRS:
        return top
    return ROLE_ROOT_FILES.get(top, "asset")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def die(msg):
    print("错误:" + msg, file=sys.stderr)
    sys.exit(1)


# ---------------- 清单构建与读取 ----------------

def build_manifest(root):
    """走一遍 root 给每个普通文件记账(来源无官方清单时现场建,权当新包清单)。"""
    root = os.path.abspath(root)
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if rel == MANIFEST_NAME:
                continue  # 清单不给自己记账
            if not valid_relpath(rel):
                die("来源文件路径不合法,拒绝记账:%s" % rel)
            if os.path.islink(full):
                die("来源含符号链接,拒绝:%s" % rel)
            files.append({
                "path": rel,
                "size": os.path.getsize(full),
                "sha256": sha256_file(full),
                "role": role_for(rel),
            })
    files.sort(key=lambda e: e["path"])
    seen = set()
    for e in files:
        key = fold(e["path"])
        if key in seen:
            die("来源大小写碰撞:%s" % e["path"])
        seen.add(key)
    return {
        "schema": SCHEMA,
        "name": "lubancode",
        "version": None,
        "platform": None,
        "channel": None,
        "algo": "sha256",
        "file_count": len(files),
        "files": files,
    }


def validate_manifest(manifest, where):
    if not isinstance(manifest, dict):
        return ["%s:清单不是 JSON 对象" % where]
    problems = []
    if manifest.get("schema") != SCHEMA:
        problems.append("%s:schema 不认(%r)" % (where, manifest.get("schema")))
    if manifest.get("algo") != "sha256":
        problems.append("%s:algo 不认(%r)" % (where, manifest.get("algo")))
    files = manifest.get("files")
    if not isinstance(files, list):
        return problems + ["%s:files 不是数组" % where]
    seen = set()
    for e in files:
        p = e.get("path") if isinstance(e, dict) else None
        if not isinstance(p, str) or not valid_relpath(p):
            problems.append("%s:路径不合法 %r" % (where, p))
            continue
        if fold(p) in seen:
            problems.append("%s:大小写碰撞 %s" % (where, p))
        seen.add(fold(p))
    return problems


def read_manifest_file(path, where):
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            manifest = json.load(f)
    except (OSError, ValueError) as exc:
        die("%s 读不了(%s)" % (where, exc))
    problems = validate_manifest(manifest, where)
    if problems:
        for p in problems:
            print("错误:" + p, file=sys.stderr)
        sys.exit(1)
    return manifest


def manifest_to_map(manifest):
    m = {}
    for e in manifest.get("files", []):
        m[fold(e["path"])] = {
            "path": e["path"],
            "sha256": e.get("sha256"),
            "size": e.get("size"),
        }
    return m


def dump_json(obj):
    """纯 ASCII 输出:PowerShell 5.1 默认编码读 BOM-less UTF-8 会花,ASCII 谁读都稳。"""
    return json.dumps(obj, ensure_ascii=True, sort_keys=True, indent=2) + "\n"


# ---------------- 盘面实况 ----------------

def scan_disk(maps, record_dir):
    """扫每棵受管树(全递归,含目录,防类型错位)+ 记录目录顶层文件。"""
    disk = {}

    def add(abspath, rel, is_dir, reparse=False):
        disk[fold(rel)] = {"path": rel, "abspath": abspath,
                           "is_dir": is_dir, "reparse": reparse, "sha256": None}

    for tree, dest in sorted(maps.items()):
        dest = os.path.abspath(dest)
        if not os.path.isdir(dest) or os.path.islink(dest):
            continue
        for dirpath, dirnames, filenames in os.walk(dest):
            for d in dirnames:
                full = os.path.join(dirpath, d)
                rel = tree + "/" + os.path.relpath(full, dest).replace(os.sep, "/")
                add(full, rel, True, reparse=os.path.islink(full))
            for name in filenames:
                full = os.path.join(dirpath, name)
                rel = tree + "/" + os.path.relpath(full, dest).replace(os.sep, "/")
                if os.path.islink(full):
                    add(full, rel, False, reparse=True)
                elif os.path.isfile(full):
                    add(full, rel, False)
    record_dir = os.path.abspath(record_dir)
    if os.path.isdir(record_dir):
        for name in sorted(os.listdir(record_dir)):
            full = os.path.join(record_dir, name)
            if name in RECORD_FILES:
                continue
            if os.path.islink(full):
                add(full, name, False, reparse=True)
            elif os.path.isfile(full):
                add(full, name, False)
    return disk


def disk_hash(entry):
    if entry["sha256"] is None and not entry["reparse"] and not entry["is_dir"]:
        try:
            entry["sha256"] = sha256_file(entry["abspath"])
        except OSError:
            entry["reparse"] = True
    return entry["sha256"]


def dest_path_for(rel, maps, record_dir):
    top = rel.split("/", 1)[0]
    if top in maps:
        rest = rel.split("/", 1)[1] if "/" in rel else ""
        return os.path.join(os.path.abspath(maps[top]), *rest.split("/"))
    return os.path.join(os.path.abspath(record_dir), *rel.split("/"))


def src_path_for(rel, source):
    return os.path.join(os.path.abspath(source), *rel.split("/"))


# ---------------- 决策表 ----------------

def build_plan(new_map, old_map, disk):
    """纯决策:三张表进,动作清单出。单测直接打这里,不碰文件系统。"""
    universe = {}
    for k, v in new_map.items():
        universe[k] = {"new": v}
    for k, v in old_map.items():
        universe.setdefault(k, {})["old"] = v
    for k, v in disk.items():
        universe.setdefault(k, {})["disk"] = v

    plan = []
    for key in sorted(universe.keys()):
        cell = universe[key]
        n, o, d = cell.get("new"), cell.get("old"), cell.get("disk")
        rel = (n or o or d)["path"]

        def emit(action, reason, backup=False):
            plan.append({"path": rel, "key": key, "action": action,
                         "reason": reason, "backup": backup})

        if d is not None and (d.get("reparse") or d.get("is_dir")):
            # 普通容器目录(清单没把它当文件)不进计划;链接/reparse 即便清单
            # 不认识也点名留观(绝不去动);只有清单要文件的路径被目录/链接
            # 挡住才是 conflict-kind / conflict-reparse。
            if n is None and o is None and not d.get("reparse"):
                continue
            emit("conflict-reparse" if d.get("reparse") else "conflict-kind",
                 "盘面是非常规文件(链接/目录),不写不删")
            continue
        hd = disk_hash(d) if d is not None else None
        if n is not None:
            if d is None:
                emit("missing-kept" if o is not None else "install-new",
                     "官方件本地已删,不悄悄复活" if o is not None else "新版新增")
            elif o is not None:
                if hd == o.get("sha256"):
                    emit("skip-current" if n.get("sha256") == hd else "replace",
                         "已是新版内容" if n.get("sha256") == hd else "官方未改,换新",
                         backup=n.get("sha256") != hd)
                else:
                    emit("conflict-modified", "本地改过官方件,原件保留+备份", backup=True)
            else:
                emit("conflict-collision", "未知文件撞上新版同路径,保留+备份", backup=True)
        else:
            if d is not None:
                if o is not None:
                    if hd == o.get("sha256"):
                        emit("retire", "官方新版已删且本地未改,随旧版退役", backup=True)
                    else:
                        emit("keep-modified-retired", "本地改过且新版已删,保留")
                else:
                    emit("keep-unknown", "用户/未知文件,保留")
    return plan


def plan_has_conflict(plan):
    return any(e["action"].startswith("conflict") or e["action"] in ("keep-modified-retired", "missing-kept")
               for e in plan)


# ---------------- 执行 ----------------

def make_backup_root(record_dir):
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    # 时间戳只到秒,同秒两笔事务会撞名(copytree 直接炸);加随机后缀防撞
    suffix = os.urandom(4).hex()
    root = os.path.join(os.path.abspath(record_dir), "backups", stamp + "-" + suffix)
    os.makedirs(root, exist_ok=True)
    return root


def backup_file(path, rel, backup_root):
    dest = os.path.join(backup_root, *rel.split("/"))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copy2(path, dest)


def apply_plan(plan, new_manifest, maps, record_dir, source, source_meta, install_mode):
    backup_root = make_backup_root(record_dir)
    source = os.path.abspath(source)
    conflicts = []
    retired_parents = set()

    for e in plan:
        rel = e["path"]
        dst = dest_path_for(rel, maps, record_dir)
        if e["backup"] and os.path.isfile(dst) and not os.path.islink(dst):
            backup_file(dst, rel, backup_root)
        action = e["action"]
        if action in ("install-new", "replace"):
            src = src_path_for(rel, source)
            if os.path.exists(dst) and os.path.samefile(src, dst):
                continue  # 同目录安装,不搬自己
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
        elif action == "retire":
            if os.path.isfile(dst) and not os.path.islink(dst):
                os.remove(dst)
                retired_parents.add(os.path.dirname(dst))
        elif action in ("conflict-modified", "conflict-collision",
                        "conflict-reparse", "conflict-kind", "keep-modified-retired"):
            conflicts.append({
                "path": rel, "kind": action,
                "backup": os.path.relpath(backup_root, os.path.abspath(record_dir)).replace(os.sep, "/"),
            })

    # 退役文件的空父目录收尾:只清这次真正删空的目录,不碰用户内容
    for parent in sorted(retired_parents, key=len, reverse=True):
        try:
            os.rmdir(parent)
        except OSError:
            pass

    write_records(new_manifest, record_dir, source, source_meta,
                  install_mode=install_mode, conflicts=conflicts)
    return backup_root, conflicts


def full_backup(maps, record_dir):
    """无基线时的完整备份:受管树全量 + 记录目录顶层文件,全部原样进备份。"""
    backup_root = make_backup_root(record_dir)
    for tree, dest in sorted(maps.items()):
        dest = os.path.abspath(dest)
        if not os.path.isdir(dest) or os.path.islink(dest):
            continue
        top = os.path.join(backup_root, tree)
        if os.path.abspath(dest) != os.path.abspath(top):
            shutil.copytree(dest, top, symlinks=True)
    record_dir = os.path.abspath(record_dir)
    for name in sorted(os.listdir(record_dir)):
        full = os.path.join(record_dir, name)
        if name in RECORD_FILES or not os.path.isfile(full):
            continue
        shutil.copy2(full, os.path.join(backup_root, name))
    return backup_root


def wholesale_replace(source, maps, record_dir):
    """整目录替换(--allow-unknown-replace):旧脚本行为,但同源同目标不删自己。"""
    source = os.path.abspath(source)
    for tree in sorted(maps):
        src_tree = os.path.join(source, tree)
        if not os.path.isdir(src_tree):
            continue
        dst = os.path.abspath(maps[tree])
        if os.path.abspath(src_tree) == dst:
            print("[plan] 同目录安装,跳过 %s" % tree)
            continue
        parent = os.path.dirname(dst)
        os.makedirs(parent, exist_ok=True)
        stage = os.path.join(parent, ".%s-new-%d" % (tree, os.getpid()))
        shutil.rmtree(stage, ignore_errors=True)
        shutil.copytree(src_tree, stage, symlinks=False)
        shutil.rmtree(dst, ignore_errors=True)
        os.rename(stage, dst)
        print("[plan] 整目录换入 %s" % dst)
    for name in sorted(os.listdir(source)):
        src = os.path.join(source, name)
        dst = os.path.join(os.path.abspath(record_dir), name)
        if name in RECORD_FILES or not os.path.isfile(src):
            continue
        if os.path.exists(dst) and os.path.samefile(src, dst):
            continue
        shutil.copy2(src, dst)
    rg = os.path.join(os.path.abspath(maps.get("libexec", record_dir)), "rg")
    if os.path.isfile(rg):
        try:
            os.chmod(rg, 0o755)
        except OSError:
            pass


def write_records(new_manifest, record_dir, source, source_meta,
                  install_mode, conflicts):
    record_dir = os.path.abspath(record_dir)
    os.makedirs(record_dir, exist_ok=True)
    # manifest.json:官方包里的原件逐字节照抄(同文件不搬自己);现场生成的落转义文本
    src_manifest = os.path.join(os.path.abspath(source), MANIFEST_NAME)
    dst_manifest = os.path.join(record_dir, MANIFEST_NAME)
    if os.path.isfile(src_manifest):
        try:
            same = os.path.samefile(src_manifest, dst_manifest)
        except OSError:
            same = False
        if not same:
            shutil.copy2(src_manifest, dst_manifest)
        provenance = "official-package"
    else:
        with open(dst_manifest, "w", encoding="ascii", newline="\n") as f:
            f.write(dump_json(new_manifest))
        provenance = "generated-from-source"

    version = new_manifest.get("version") or detect_version(os.path.join(record_dir, "lubancode"))
    state = {
        "schema": SCHEMA,
        "installed_at_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "version": version,
        "platform": new_manifest.get("platform"),
        "channel": new_manifest.get("channel"),
        "source": {
            "repo": source_meta.get("repo"),
            "release_tag": source_meta.get("release_tag"),
            "asset_name": source_meta.get("asset_name"),
            "download_url": source_meta.get("download_url"),
        },
        "install_mode": install_mode,
        "manifest_provenance": provenance,
        "installer": "install.sh/install_plan.py",
        "manifest": new_manifest,
        "pending_conflicts": conflicts or [],
    }
    with open(os.path.join(record_dir, STATE_NAME), "w", encoding="ascii", newline="\n") as f:
        f.write(dump_json(state))


def detect_version(exe):
    if not os.path.isfile(exe):
        return None
    try:
        out = subprocess.run([exe, "--version"], capture_output=True, text=True,
                             timeout=30, check=False)
        for tok in (out.stdout or "").strip().split():
            if tok and tok[0].isdigit():
                return tok
    except (OSError, subprocess.SubprocessError):
        pass
    return None


# ---------------- 预演报告 ----------------

def print_plan_report(plan, new_manifest, old_manifest):
    total = new_manifest.get("file_count") or len(new_manifest.get("files", []))
    print("预演:新版 %s 个官方文件;基线:%s" %
          (total, "有(上次官方清单)" if old_manifest else "无(旧安装没有清单)"))
    counts = {}
    for e in plan:
        counts[e["action"]] = counts.get(e["action"], 0) + 1
        print("[plan] %s %s (%s)" % (e["action"], e["path"], e["reason"]))
    print("合计:" + (", ".join("%s=%d" % kv for kv in sorted(counts.items())) or "无事可做"))
    if plan_has_conflict(plan):
        print("注意:存在冲突/本地差异项(见上)。执行安装时这些路径一律保留原件并备份,官方新版不落这些路径。")


def print_needs_review(disk, new_map):
    collisions = sorted(disk[k]["path"] for k in new_map if k in disk)
    others = sorted(v["path"] for k, v in disk.items() if k not in new_map)
    print("needs-review:旧安装没有清单(基线),且新包路径与盘面相撞 %d 项:" % len(collisions))
    for p in collisions[:50]:
        print("  相撞:" + p)
    if others:
        print("另有未知/用户文件 %d 项(不受影响,将保留)。" % len(others))
    print("两条路:")
    print("  1) 提供同版本可信原包目录重跑(--baseline-dir / install.sh --baseline <原包目录>)")
    print("  2) 确认安装目录里没有要保的东西后 --allow-unknown-replace(先完整备份再整目录替换)")


# ---------------- 各子命令 ----------------

def parse_map_args(map_args):
    maps = {}
    for m in map_args or []:
        if "=" not in m:
            die("--map 形如 skills=/目标/路径,收到:%s" % m)
        tree, dest = m.split("=", 1)
        if tree not in ROLE_TOP_DIRS:
            die("--map 只收受管树(skills/docs/web/libexec/licenses/updater),收到:%s" % tree)
        maps[tree] = dest
    return maps


def load_baselines(args, record_dir):
    """返回 (new_manifest, old_manifest_or_None)。"""
    src_manifest = os.path.join(os.path.abspath(args.source), MANIFEST_NAME)
    if os.path.isfile(src_manifest):
        new_manifest = read_manifest_file(src_manifest, "来源清单")
    else:
        new_manifest = build_manifest(args.source)

    if getattr(args, "baseline_manifest", None):
        return new_manifest, read_manifest_file(args.baseline_manifest, "指定基线清单")
    if getattr(args, "baseline_dir", None):
        base = build_manifest(args.baseline_dir)
        problems = validate_manifest(base, "基线目录")
        if problems:
            die("基线目录不干净:" + ";".join(problems))
        return new_manifest, base

    # 默认基线:安装记录里存的上次官方清单;再退一步,根下平铺的旧清单
    state_path = os.path.join(record_dir, STATE_NAME)
    if os.path.isfile(state_path):
        try:
            with open(state_path, "r", encoding="utf-8-sig") as f:
                cand = json.load(f).get("manifest")
            if cand and not validate_manifest(cand, state_path):
                return new_manifest, cand
        except (OSError, ValueError):
            pass
    legacy = os.path.join(record_dir, MANIFEST_NAME)
    if os.path.isfile(legacy) and not os.path.abspath(legacy) == os.path.abspath(src_manifest):
        return new_manifest, read_manifest_file(legacy, "旧版平铺清单")
    return new_manifest, None


def cmd_plan(args):
    maps = parse_map_args(args.map)
    record_dir = os.path.abspath(args.record_dir)
    new_manifest, old_manifest = load_baselines(args, record_dir)
    new_map = manifest_to_map(new_manifest)
    disk = scan_disk(maps, record_dir)
    if old_manifest is None:
        if any(k in disk for k in new_map):
            print_needs_review(disk, new_map)
            return 3
        # 无基线也无相撞:纯新增,现有文件全部按未知保留
    plan = build_plan(new_map, manifest_to_map(old_manifest) if old_manifest else {}, disk)
    print_plan_report(plan, new_manifest, old_manifest)
    return 0


def cmd_apply(args):
    maps = parse_map_args(args.map)
    record_dir = os.path.abspath(args.record_dir)
    source = os.path.abspath(args.source)
    new_manifest, old_manifest = load_baselines(args, record_dir)
    new_map = manifest_to_map(new_manifest)
    disk = scan_disk(maps, record_dir)

    if old_manifest is None and any(k in disk for k in new_map):
        if not args.allow_unknown_replace:
            backup_root = full_backup(maps, record_dir)
            print("needs-review:旧安装没有清单(基线),已先做完整备份,未动安装:%s" % backup_root)
            print_needs_review(disk, new_map)
            return 3
        backup_root = full_backup(maps, record_dir)
        print("完整备份完成:%s" % backup_root)
        wholesale_replace(source, maps, record_dir)
        write_records(new_manifest, record_dir, source, vars_source_meta(args),
                      install_mode="allow-unknown-replace", conflicts=[])
        print("整目录替换完成(备份在 %s)。" % backup_root)
        return 0

    plan = build_plan(new_map, manifest_to_map(old_manifest) if old_manifest else {}, disk)
    backup_root, conflicts = apply_plan(plan, new_manifest, maps, record_dir,
                                        source, vars_source_meta(args), install_mode="baseline")
    print("应用完成。备份根:%s" % backup_root)
    if conflicts:
        print("冲突/本地差异 %d 项(原件保留,已备份):" % len(conflicts))
        for c in conflicts:
            print("  %s %s" % (c["kind"], c["path"]))
        print("处理完冲突后重跑安装即可换上官方新版。")
    return 0


def cmd_backup_only(args):
    maps = parse_map_args(args.map)
    record_dir = os.path.abspath(args.record_dir)
    new_manifest, old_manifest = load_baselines(args, record_dir)
    disk = scan_disk(maps, record_dir)
    if not disk:
        print("安装目录没有受管内容,无需备份。")
        return 0
    backup_root = full_backup(maps, record_dir)
    print("完整备份完成:%s" % backup_root)
    new_map = manifest_to_map(new_manifest)
    unknowns = sorted(v["path"] for k, v in disk.items() if k not in new_map)
    if unknowns:
        print("不在新版清单内的文件 %d 项(已随备份保留):" % len(unknowns))
        for p in unknowns:
            print("  " + p)
    return 0


def cmd_write_state(args):
    parse_map_args(args.map)
    record_dir = os.path.abspath(args.record_dir)
    new_manifest, _ = load_baselines(args, record_dir)
    write_records(new_manifest, record_dir, args.source, vars_source_meta(args),
                  install_mode=args.install_mode or "fresh", conflicts=[])
    print("已记档:%s" % os.path.join(record_dir, STATE_NAME))
    return 0


def vars_source_meta(args):
    return {
        "repo": getattr(args, "source_repo", None),
        "release_tag": getattr(args, "release_tag", None),
        "asset_name": getattr(args, "asset_name", None),
        "download_url": getattr(args, "download_url", None),
    }


def add_common(ap):
    ap.add_argument("--source", required=True, help="发行包根目录(含可执行文件)")
    ap.add_argument("--record-dir", required=True, help="记档目录(exe 旁;install-state.json/manifest.json 落点)")
    ap.add_argument("--map", action="append", metavar="TREE=DIR",
                    help="受管树落点,如 skills=/opt/lubancode/share/lubancode/skills,可重复")
    ap.add_argument("--baseline-manifest", help="显式上次官方清单")
    ap.add_argument("--baseline-dir", help="同版本可信原包目录,现建基线")
    ap.add_argument("--source-repo")
    ap.add_argument("--release-tag")
    ap.add_argument("--asset-name")
    ap.add_argument("--download-url")


def main():
    ap = argparse.ArgumentParser(description="lubancode 安装资源所有权引擎")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_plan = sub.add_parser("plan", help="只读预演")
    add_common(p_plan)

    p_apply = sub.add_parser("apply", help="执行安装(按所有权判定)")
    add_common(p_apply)
    p_apply.add_argument("--allow-unknown-replace", action="store_true",
                         help="无基线时:先完整备份再整目录替换(需显式确认)")

    p_backup = sub.add_parser("backup-only", help="完整备份受管内容,不安装")
    add_common(p_backup)

    p_state = sub.add_parser("write-state", help="全新装记档(文件拷贝由调用方完成)")
    add_common(p_state)
    p_state.add_argument("--install-mode")

    args = ap.parse_args()
    if args.cmd == "plan":
        return cmd_plan(args)
    if args.cmd == "apply":
        return cmd_apply(args)
    if args.cmd == "backup-only":
        return cmd_backup_only(args)
    return cmd_write_state(args)


if __name__ == "__main__":
    sys.exit(main())
