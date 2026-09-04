"""实验 C(Subagent 失败分布)collect 骨架:原始 JSONL → csv + md(+parquet)。

P4 才真跑(单子 §五);P0 先立骨架与口径。原始账一行一跑:
  arm: baseline | one_explore | three_parallel    三对照
  task_kind: readonly | rewrite | longcommand     12 任务三类
  task_index: 1..4                                 每类 4 题
  wall_ms / tokens_total / tokens_cached           墙钟与 token 两账
  failure_type: none | background_unavailable | trajectory.subagent_start_failed
              | admission_depth_limit | admission_active_limit | hook_deny
              | tool_error                          失败类型(单子 §四)
  progressless_steps: int                          无进展指纹连续 step 数
  review_complete: bool                            独立评审完成率

产出两张表:
  1) 三对照账:每 arm 的墙钟/token 中位+P95、失败类型分布、评审完成率;
  2) 派工价值/成本:每 (task_kind, subagent arm) 的
       派工价值 = (baseline_wall - subagent_wall) / baseline_wall
       派工成本 = (subagent_tokens - baseline_tokens) / baseline_tokens
     两中位数(单子 §四验收);baseline 缺位记 unavailable,不填 0。

用法:
  python collect.py --raw results/raw_subagent_failure.jsonl --out-dir results
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from collect_common import (  # noqa: E402
    base_parser,
    group_by,
    median,
    numeric_column,
    p95,
    read_jsonl,
    report_written,
    safe_pct,
    safe_ratio,
    write_outputs,
)

FAILURE_TYPES = [
    "none",
    "background_unavailable",
    "trajectory.subagent_start_failed",
    "admission_depth_limit",
    "admission_active_limit",
    "hook_deny",
    "tool_error",
]
SUBAGENT_ARMS = ["one_explore", "three_parallel"]

ARM_COLUMNS = ["arm", "n", "wall_ms_median", "wall_ms_p95", "tokens_median", "tokens_p95",
               "review_complete_pct", *[f"fail_{kind}" for kind in FAILURE_TYPES]]
VALUE_COLUMNS = ["task_kind", "arm", "n", "dispatch_value_median", "dispatch_cost_median"]


def arm_rows(records: list[dict]) -> list[dict]:
    rows = []
    for key, group in sorted(group_by(records, ["arm"]).items()):
        row: dict[str, object] = {"arm": key[0], "n": len(group)}
        for metric, col in (("wall_ms", "wall_ms"), ("tokens_total", "tokens")):
            values = numeric_column(group, metric)
            row[f"{col}_median"] = median(values)
            row[f"{col}_p95"] = p95(values)
        reviews = [1 if r.get("review_complete") else 0 for r in group]
        row["review_complete_pct"] = safe_pct(sum(reviews), len(reviews))
        for kind in FAILURE_TYPES:
            row[f"fail_{kind}"] = sum(1 for r in group if r.get("failure_type") == kind)
        rows.append(row)
    return rows


def value_rows(records: list[dict]) -> list[dict]:
    baselines: dict[tuple, list[float]] = {}
    for key, group in group_by(records, ["task_kind", "arm"]).items():
        if key[1] == "baseline":
            baselines[key[:1]] = numeric_column(group, "wall_ms"), numeric_column(group, "tokens_total")
    rows = []
    for key, group in sorted(group_by(records, ["task_kind", "arm"]).items()):
        if key[1] not in SUBAGENT_ARMS:
            continue
        walls = numeric_column(group, "wall_ms")
        tokens = numeric_column(group, "tokens_total")
        base = baselines.get(key[:1])
        row: dict[str, object] = {"task_kind": key[0], "arm": key[1], "n": len(group)}
        if base is None or not base[0] or not base[1]:
            row["dispatch_value_median"] = "unavailable"
            row["dispatch_cost_median"] = "unavailable"
        else:
            base_wall = median(base[0])
            base_tokens = median(base[1])
            sub_wall = median(walls)
            sub_tokens = median(tokens)
            value = safe_ratio(base_wall - sub_wall, base_wall) if sub_wall is not None else "unavailable"
            cost = safe_ratio(sub_tokens - base_tokens, base_tokens) if sub_tokens is not None else "unavailable"
            row["dispatch_value_median"] = value
            row["dispatch_cost_median"] = cost
        rows.append(row)
    return rows


def main() -> int:
    args = base_parser(__doc__).parse_args()
    records = read_jsonl(args.raw)
    notes = [
        f"原始账: {args.raw.name}({len(records)} 行)",
        "派工价值/成本按 task_kind 分桶、arm 内取中位后再对 baseline 求 "
        "unavailable。基线缺位或分母为 0 记 unavailable。",
    ]
    written = []
    written += write_outputs(
        arm_rows(records), ARM_COLUMNS, args.out_dir,
        args.out_stem or "arm_summary", "实验 C:三对照账(骨架)", notes,
    )
    written += write_outputs(
        value_rows(records), VALUE_COLUMNS, args.out_dir,
        args.out_stem or "dispatch_value", "实验 C:派工价值/成本(骨架)", notes,
    )
    return report_written(written, args.raw, len(records))


if __name__ == "__main__":
    raise SystemExit(main())
