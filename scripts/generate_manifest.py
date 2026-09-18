#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""发行包官方清单(manifest.json)生成器。

GitHubRelease自动更新单 §四:打包时给每个官方文件记账——相对路径、大小、
SHA-256、用途,覆盖 EXE、skills、docs、web、libexec、许可证与安装脚本。
清单跟着包走(install.ps1 / install_plan.py 拿它做文件所有权判定),包外
由 release.yml 在打包后重算对账(--check),对不上即红,不发糊涂包。

路径规则(与 install_plan.py / install.ps1 同一套契约,别单边改):
  - 只认包内相对路径,正斜杠分隔;
  - 拒绝绝对路径、反斜杠、冒号(盘符/UNC/Windows ADS 一并挡住)、
    空段、"." 与 ".." 段、结尾点/空格、Windows 保留名、非法字符;
  - 大小写折叠后不得重复(Windows 盘面大小写不敏感,防两路撞车)。

输出为纯 ASCII(非 ASCII 一律 \uXXXX 转义):PowerShell 5.1 的
Get-Content 默认编码吃 BOM-less UTF-8 会变摩斯码,ASCII 文件哪家读都稳。
清单不给自己记账(哈希不了自己),manifest.json 由装/校两侧特殊照顾。
"""

import argparse
import hashlib
import json
import os
import sys

SCHEMA = 1
MANIFEST_NAME = "manifest.json"

# 树内目录 → role;根级文件按文件名 → role;其余 asset
ROLE_TOP_DIRS = ("skills", "docs", "web", "libexec", "licenses")
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


def valid_relpath(path):
    """返回 (ok, reason)。规则见文件头注释。"""
    if not path:
        return False, "空路径"
    if len(path) > 512:
        return False, "路径过长(>512)"
    if path.startswith("/"):
        return False, "绝对路径"
    if "\\" in path:
        return False, "反斜杠分隔"
    if ":" in path:
        return False, "含冒号(盘符/UNC/ADS)"
    for seg in path.split("/"):
        if seg == "":
            return False, "空路径段"
        if seg in (".", ".."):
            return False, "点段(. / ..)"
        if seg.endswith(".") or seg.endswith(" "):
            return False, "段结尾是点或空格"
        # 保留名连扩展名一起拒:com1.md 在 Windows 上照样惹祸
        stem = seg.split(".", 1)[0].upper()
        if seg.upper() in WINDOWS_RESERVED or stem in WINDOWS_RESERVED:
            return False, "Windows 保留名:" + seg
        for ch in seg:
            if ch in INVALID_CHARS or ord(ch) < 0x20:
                return False, "非法字符 U+%04X" % ord(ch)
    return True, ""


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


def build_manifest(root, version, platform, channel):
    """走一遍 root,给每个普通文件记账。符号链接直接红——发行包不该有。"""
    root = os.path.abspath(root)
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if rel == MANIFEST_NAME:
                continue  # 清单不给自己记账
            ok, reason = valid_relpath(rel)
            if not ok:
                raise SystemExit("manifest 路径不合法:%s(%s)" % (rel, reason))
            if os.path.islink(full):
                raise SystemExit("manifest 不收符号链接:%s" % rel)
            if not os.path.isfile(full):
                raise SystemExit("manifest 只收普通文件:%s" % rel)
            files.append({
                "path": rel,
                "size": os.path.getsize(full),
                "sha256": sha256_file(full),
                "role": role_for(rel),
            })
    files.sort(key=lambda e: e["path"])
    seen = {}
    for e in files:
        key = e["path"].casefold()
        if key in seen:
            raise SystemExit("manifest 大小写碰撞:%s 与 %s" % (seen[key], e["path"]))
        seen[key] = e["path"]
    return {
        "schema": SCHEMA,
        "name": "lubancode",
        "version": version,
        "platform": platform,
        "channel": channel,
        "algo": "sha256",
        "file_count": len(files),
        "files": files,
    }


def validate_manifest(manifest, where):
    """读回来先验再信:schema/algo/字段齐不齐、路径合不合法、大小写撞不撞。"""
    problems = []
    if not isinstance(manifest, dict):
        return ["%s:清单不是 JSON 对象" % where]
    if manifest.get("schema") != SCHEMA:
        problems.append("%s:schema 不认(期望 %d,得 %r)" % (where, SCHEMA, manifest.get("schema")))
    if manifest.get("algo") != "sha256":
        problems.append("%s:algo 不认(期望 sha256)" % where)
    files = manifest.get("files")
    if not isinstance(files, list):
        problems.append("%s:files 不是数组" % where)
        return problems
    seen = {}
    for e in files:
        if not isinstance(e, dict):
            problems.append("%s:files 成员不是对象" % where)
            continue
        p = e.get("path")
        if not isinstance(p, str):
            problems.append("%s:path 不是字符串:%r" % (where, p))
            continue
        ok, reason = valid_relpath(p)
        if not ok:
            problems.append("%s:路径不合法 %s(%s)" % (where, p, reason))
        key = p.casefold()
        if key in seen:
            problems.append("%s:大小写碰撞 %s 与 %s" % (where, seen[key], p))
        seen[key] = p
        if not isinstance(e.get("size"), int) or e["size"] < 0:
            problems.append("%s:size 不对:%s" % (where, p))
        sha = e.get("sha256")
        if not isinstance(sha, str) or len(sha) != 64 or any(c not in "0123456789abcdef" for c in sha):
            problems.append("%s:sha256 不是 64 位小写十六进制:%s" % (where, p))
    if manifest.get("file_count") != len(files):
        problems.append("%s:file_count(%r)与 files 长度(%d)不合" % (where, manifest.get("file_count"), len(files)))
    return problems


def dump(manifest):
    return json.dumps(manifest, ensure_ascii=True, sort_keys=True, indent=2) + "\n"


def main():
    ap = argparse.ArgumentParser(description="lubancode 发行包官方清单生成器")
    ap.add_argument("--root", required=True, help="包根目录(含 lubancode 可执行文件)")
    ap.add_argument("--version", required=True, help="版本号(不带 v 前缀)")
    ap.add_argument("--platform", required=True, help="windows-x64 / linux-x64 / macos-arm64")
    ap.add_argument("--channel", default="stable", help="stable / prerelease")
    ap.add_argument("--out", help="输出路径(默认 <root>/manifest.json)")
    ap.add_argument("--check", action="store_true",
                    help="重算与现有清单对账,不一致退非零(发布流水线验包用)")
    args = ap.parse_args()

    out = args.out or os.path.join(args.root, MANIFEST_NAME)
    manifest = build_manifest(args.root, args.version, args.platform, args.channel)
    problems = validate_manifest(manifest, "生成")
    if problems:
        for p in problems:
            print("错误:" + p, file=sys.stderr)
        return 1
    text = dump(manifest)

    if args.check:
        if not os.path.isfile(out):
            print("错误:%s 不存在,--check 无从对账" % out, file=sys.stderr)
            return 1
        with open(out, "r", encoding="utf-8-sig") as f:
            existing = json.load(f)
        problems = validate_manifest(existing, out)
        if problems:
            for p in problems:
                print("错误:" + p, file=sys.stderr)
            return 1
        if dump(existing) != text:
            print("错误:%s 与包内实际内容对不上账" % out, file=sys.stderr)
            return 1
        print("manifest 对账通过:%d 个文件,%s %s" % (manifest["file_count"], args.platform, args.version))
        return 0

    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write(text)
    print("已生成 %s:%d 个文件,%s %s" % (out, manifest["file_count"], args.platform, args.version))
    return 0


if __name__ == "__main__":
    sys.exit(main())
