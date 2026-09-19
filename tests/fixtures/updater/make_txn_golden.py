#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""更新器 C++ 化·批二第③单:事务账黄金文件生成器(一次性夹具,进仓留档)。

拿 scripts/updater.py 的 Transaction 真跑一遍固定时钟的全相流转,落盘两份
黄金样例,供 tests/unit/updater/test_updater_txn.cpp 逐字节对拍——C++ 侧的
schema 1 账与 python 版逐字相同是本单验收口径。时钟钉死
2026-09-20T12:00:00Z、txn id 钉死 20260920T120000Z-1a2b3c4d,两边同样
喂给 C++ 的 clock/id seam,产物即确定。

用法: python make_txn_golden.py <install_root> <out_committed> <out_failed>
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "scripts"))
import updater  # noqa: E402

FIXED_NOW = "2026-09-20T12:00:00Z"


def make_target():
    digest = "b" * 64
    return {
        "version": "0.26.280",
        "tag": "v0.26.280",
        "exe_version": "0.26.280",
        "platform": "windows-x64",
        "dirname": updater.version_dirname("0.26.280", digest),
        "digest_hex": digest,
        "repo": "relic-yuexi/LubanCode",
        "release_id": 123456789,
        "asset_id": 987654321,
        "asset_name": "lubancode-0.26.280-windows-x64.zip",
        "asset_size": 536870912,
        "download_url": None,
    }


def drive(root, out_path, abnormal):
    updater.utcnow = lambda: FIXED_NOW  # 钉死时钟
    target = make_target()
    txn = updater.Transaction(root, "20260920T120000Z-1a2b3c4d")
    txn.create(target)
    if abnormal:
        # 异常册:checking -> downloading -> needs-review(blocking)->(处理后
        # 续跑)activating -> failed(reason)。覆盖 blocking/reason 字段形状。
        txn.transition("downloading")
        txn.note("下载完成,核对摘要通过")
        txn.transition("needs-review", blocking=[
            {"path": "skills/lubancode-config/SKILL.md", "action": "conflict-modified"},
            {"path": "docs/README.md", "action": "conflict-collision"},
        ])
        txn.transition("activating")
        txn.transition("failed", reason="激活中途失败: 旧根 EXE 挪不进备份")
    else:
        # 全相册:checking -> downloading -> verified -> staged ->
        # waiting-for-idle(rollback_current/layout_before)-> activating ->
        # healthy(health_probe)-> committed(committed_at_utc),外加两条 note。
        txn.transition("downloading")
        txn.transition("verified", archive_sha256="sha256:" + target["digest_hex"])
        txn.note("解包与逐文件核对通过")
        txn.transition("staged")
        txn.transition("waiting-for-idle",
                       rollback_current="0.26.279-aaaaaaaa",
                       layout_before="flat")
        txn.transition("activating")
        txn.transition("healthy", health_probe="lubancode 0.26.280")
        txn.transition("committed", committed_at_utc=FIXED_NOW)
    with open(out_path, "wb") as f:
        f.write(open(txn.path, "rb").read())


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    root, out_committed, out_failed = sys.argv[1], sys.argv[2], sys.argv[3]
    drive(root, out_committed, abnormal=False)
    drive(root, out_failed, abnormal=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
