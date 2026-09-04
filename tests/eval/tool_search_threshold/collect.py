"""实验 A(工具检索退化阈值)collect:原始 JSONL → csv + md(+parquet)。

P0 范围:A2 档(40 只工具)× disabled 模式 × T1 任务 × 5 次重复。P2 扩到
五档(12/22/40/70/120)× 三模式(disabled/legacy_expand/proxy_reference)×
三任务(T1/T2/T3)时,本脚本按 (tier, mode, task) 分组的结构原样吃下,
不另立口径。

记账(单子 §三):首 token 延迟、tools/system/总请求字节、模型决策正确率。
统计:每格中位数 + P95;决策正确率按 safe_pct(零分母 unavailable)。

用法:
  python collect.py --raw results/raw_a2_disabled_t1.jsonl --out-dir results
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from collect_common import (  # noqa: E402
    UNAVAILABLE,
    base_parser,
    fmt_cell,
    group_by,
    median,
    numeric_column,
    p95,
    read_jsonl,
    report_written,
    safe_pct,
    write_outputs,
)

# 数值列:每格出 n/median/P95 三件(单子 §三的首 token 延迟与三笔字节账)。
METRIC_KEYS = [
    "first_event_latency_ms",
    "tools_bytes",
    "system_bytes",
    "request_bytes",
]
COLUMNS = [
    "tier",
    "mode",
    "task",
    "n",
    "decision_correct_pct",
    *[f"{key}_median" for key in METRIC_KEYS],
    *[f"{key}_p95" for key in METRIC_KEYS],
]


def summarize(records: list[dict]) -> list[dict]:
    rows: list[dict] = []
    for key, group in sorted(group_by(records, ["tier", "mode", "task"]).items()):
        row: dict[str, object] = {
            "tier": key[0],
            "mode": key[1],
            "task": key[2],
            "n": len(group),
        }
        decisions = [1 if r.get("decision_correct") else 0 for r in group]
        row["decision_correct_pct"] = safe_pct(sum(decisions), len(decisions))
        for metric in METRIC_KEYS:
            values = numeric_column(group, metric)
            row[f"{metric}_median"] = median(values)
            row[f"{metric}_p95"] = p95(values)
        rows.append(row)
    return rows


def main() -> int:
    args = base_parser(__doc__).parse_args()
    records = read_jsonl(args.raw)
    stem = args.out_stem or "summary_a2_disabled_t1"
    notes = [
        f"原始账: {args.raw.name}({len(records)} 行)",
        "口径: 字节是 api::Request 中立投影的 JSON dump 长度(driver 的 "
        "MeasureRequest,P2 报告沿用并注明);首 token 延迟是假 backend "
        "send_stream 进入到首个事件回调的毫秒数(量宿主路径,非真网络)。",
        "零分母规则: 比率类分母为 0 记 " + UNAVAILABLE + ",不填 0。",
    ]
    written = write_outputs(
        summarize(records), COLUMNS, args.out_dir, stem, "实验 A:工具检索退化阈值(P0:A2/disabled/T1)", notes
    )
    return report_written(written, args.raw, len(records))


if __name__ == "__main__":
    raise SystemExit(main())
