#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""发布侧更新元数据生成器(GitHubRelease自动更新单 P1,§四/§八)。

集中发布任务(publish)在三平台归档重核通过后跑它:对每个归档重算
SHA-256 与大小,汇总成 update-meta.json 随 Release 挂出。客户端
(lubancode update --check / 更新助手)拿它做资产摘要的可信来源——
GitHub 资产未带 digest 时,以这里为准;两处都没有就不做自动安装(§四:
不静默降级为不校验)。

输出纯 ASCII(P0 同一惯例)。用法:
    compose_update_meta.py --dist dist/ --tag v0.26.300 \
        [--platform windows-x64:zip --platform linux-x64:tar.gz ...]
"""

import argparse
import datetime
import hashlib
import json
import os
import sys

PLATFORMS = (("windows-x64", "zip"), ("linux-x64", "tar.gz"), ("macos-arm64", "tar.gz"))


def sha256_and_size(path):
    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


def main():
    ap = argparse.ArgumentParser(description="汇总发布资产摘要为 update-meta.json")
    ap.add_argument("--dist", required=True, help="归档所在目录")
    ap.add_argument("--tag", required=True, help="Release tag(v 前缀)")
    ap.add_argument("--platform", action="append", metavar="PLATFORM:KIND",
                    help="平台与归档种类;缺省三平台全收")
    ap.add_argument("--out", help="输出路径(默认 <dist>/update-meta.json)")
    args = ap.parse_args()

    tag = args.tag[1:] if args.tag.startswith("v") else args.tag
    channel = "prerelease" if "-" in tag else "stable"
    platforms = []
    for item in (args.platform or ["%s:%s" % p for p in PLATFORMS]):
        platform, _, kind = item.partition(":")
        if not platform or not kind:
            print("错误:--platform 形如 windows-x64:zip,收到 %s" % item, file=sys.stderr)
            return 1
        platforms.append((platform, kind))

    entries = []
    for platform, kind in platforms:
        name = "lubancode-v%s-%s.%s" % (tag, platform, kind)
        path = os.path.join(args.dist, name)
        if not os.path.isfile(path):
            print("错误:归档不在:%s" % path, file=sys.stderr)
            return 1
        digest, size = sha256_and_size(path)
        entries.append({"platform": platform, "archive": name,
                        "size": size, "sha256": digest})

    meta = {
        "schema": 1,
        "name": "lubancode",
        "version": tag,
        "channel": channel,
        "generated_at_utc":
            datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        # 低于这个版本的一键更新器先走包内安装脚本升级一次(更新助手
        # 自升级的分阶段交接另设实现项)
        "minimum_updater_version": "0.26.278",
        "platforms": entries,
    }
    out = args.out or os.path.join(args.dist, "update-meta.json")
    with open(out, "w", encoding="ascii", newline="\n") as f:
        json.dump(meta, f, ensure_ascii=True, sort_keys=True, indent=2)
        f.write("\n")
    print("update-meta.json: %s %s,%d 个平台 -> %s" % (tag, channel, len(entries), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
