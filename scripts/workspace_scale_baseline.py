#!/usr/bin/env python3
"""Workspace 量级基线:跑 workspace_scale_driver 并渲染 Markdown 基线表。

流程:
  1. 构建(或指)workspace_scale_driver;
  2. 跑 1/1k/10k session 与 1/1k/10k memory 档(可 --sessions/--memories 改);
  3. 把 driver 的 JSON 渲染成 Markdown 表(冷/热耗时、峰值内存、造数成本),
     供 docs/development/workspace-storage-v2/scale-baseline.md 引用或替换。

用法:
  python scripts/workspace_scale_baseline.py <workspace_scale_driver> [--out 输出.md]
      [--json 输出.json] [--sessions 1,1000,10000] [--memories 1,1000,10000]

不写死预算:数字全部来自本机实跑,表头带机器/日期。
"""
from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
import tempfile
from datetime import date
from pathlib import Path


def fmt_ms(value: float) -> str:
    if value >= 1000:
        return f"{value / 1000:.2f} s"
    return f"{value:.1f} ms"


def render(report: dict) -> str:
    lines: list[str] = []
    lines.append("## Workspace 量级基线(本机实跑)\n")
    lines.append(
        f"- 日期:{date.today().isoformat()};平台:{report.get('platform', '?')}"
        f";机器:{platform.machine()} / {platform.system()}\n"
    )
    template = report.get("template", {})
    lines.append(
        f"- 模板场:{template.get('main_events', '?')} 事件,"
        f"main.jsonl {template.get('main_bytes', 0) / 1024:.1f} KiB\n"
    )
    lines.append("")
    lines.append("| sessions | list 冷 | list 热 |")
    lines.append("| ---: | ---: | ---: |")
    for row in report.get("session_rows", []):
        lines.append(
            f"| {row['sessions']} | {fmt_ms(row['list_cold_ms'])} | {fmt_ms(row['list_hot_ms'])} |"
        )
    lines.append("")
    lines.append("| memory 条目 | 造数 | rebuild | 召回冷 | 召回热 | 注入字节 |")
    lines.append("| ---: | ---: | ---: | ---: | ---: | ---: |")
    for row in report.get("memory_rows", []):
        lines.append(
            f"| {row['memories']} | {fmt_ms(row['topics_write_ms'])} | "
            f"{fmt_ms(row['rebuild_ms'])} | {fmt_ms(row['recall_cold_ms'])} | "
            f"{fmt_ms(row['recall_hot_ms'])} | {row['recall_bytes']} |"
        )
    lines.append("")
    replay = report.get("replay", {})
    if replay:
        lines.append(
            f"- 单场 replay:折叠 {fmt_ms(replay['fold_ms'])},整场验账 "
            f"{fmt_ms(replay['verify_ms'])}({replay['events']} 事件)"
        )
    peak = report.get("peak_rss_kb", 0)
    tree = report.get("workspace_bytes", 0)
    lines.append(
        f"- 峰值内存:{peak / 1024:.1f} MiB;workspace 树 {tree / 1024 / 1024:.1f} MiB\n"
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("driver", type=Path, help="workspace_scale_driver 可执行文件")
    parser.add_argument("--out", type=Path, default=None, help="Markdown 输出路径")
    parser.add_argument("--json", type=Path, default=None, help="原始 JSON 输出路径")
    parser.add_argument("--sessions", default="1,1000,10000")
    parser.add_argument("--memories", default="1,1000,10000")
    args = parser.parse_args()

    if not args.driver.is_file():
        print(f"找不到 driver: {args.driver}")
        return 2

    with tempfile.TemporaryDirectory(prefix="lubancode-scale-") as tmp:
        json_path = Path(tmp) / "scale.json"
        cmd = [
            str(args.driver),
            str(json_path),
            "--sessions",
            args.sessions,
            "--memories",
            args.memories,
        ]
        print("$ " + " ".join(cmd))
        result = subprocess.run(cmd)
        if result.returncode != 0:
            print(f"driver 退出码 {result.returncode}")
            return result.returncode
        report = json.loads(json_path.read_text(encoding="utf-8"))
        if args.json:
            args.json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                 encoding="utf-8")
            print(f"原始 JSON 落 {args.json}")
        markdown = render(report)
        if args.out:
            args.out.write_text(markdown + "\n", encoding="utf-8")
            print(f"基线表落 {args.out}")
        else:
            print(markdown)
    return 0


if __name__ == "__main__":
    sys.exit(main())
