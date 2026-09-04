"""实验 B1(compact 位置探针)collect 骨架:原始 JSONL → csv + md(+parquet)。

P1 才真跑(单子 §五);P0 先立骨架与口径。原始账一行一问(schema 约定,
造稿器/判卷器 P1 落地时照此写):
  treatment: FULL | microcompact | compact   同一底稿换压缩路
  position_pct: 0|5|...|95                    needle 受控深度(20 档)
  probe_kind: recall | conflict               直接召回 | 更新冲突
  lang: zh | en                                事实措辞两形
  repeat: 1..5                                 每档每处理 5 次
  hit: bool                                    判卷结果(确定性判卷)

产出两张表:
  1) 位置-命中曲线:每 (treatment, probe_kind) × 20 档的命中率;
  2) 中段存活率:每 (treatment, probe_kind) 的中段(25%≤pos≤75%)命中率
     均值——单子 B1 的核心数(compact/microcompact 把 FULL 的 U 形压成什么)。
零分母:档无样本记 unavailable,不填 0。

用法:
  python collect.py --raw results/raw_position_probe.jsonl --out-dir results
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from collect_common import (  # noqa: E402
    base_parser,
    group_by,
    numeric_column,
    read_jsonl,
    report_written,
    safe_pct,
    write_outputs,
)

POSITION_STEPS = list(range(0, 100, 5))  # 0,5,...,95
MID_MIN, MID_MAX = 25, 75  # 中段定义(单子 B1:U 形的中段)

CURVE_COLUMNS = ["treatment", "probe_kind", "position_pct", "n", "hit_pct"]
SURVIVAL_COLUMNS = ["treatment", "probe_kind", "mid_n", "mid_hit_pct", "overall_hit_pct"]


def curve_rows(records: list[dict]) -> list[dict]:
    rows = []
    for key, group in sorted(group_by(records, ["treatment", "probe_kind"]).items()):
        by_pos = group_by(group, ["position_pct"])
        for pos in POSITION_STEPS:
            cell = by_pos.get((str(pos),), [])
            hits = [1 if r.get("hit") else 0 for r in cell]
            rows.append(
                {
                    "treatment": key[0],
                    "probe_kind": key[1],
                    "position_pct": pos,
                    "n": len(cell),
                    "hit_pct": safe_pct(sum(hits), len(hits)),
                }
            )
    return rows


def survival_rows(records: list[dict]) -> list[dict]:
    rows = []
    for key, group in sorted(group_by(records, ["treatment", "probe_kind"]).items()):
        mid = [
            r
            for r in group
            if isinstance(r.get("position_pct"), (int, float)) and MID_MIN <= r["position_pct"] <= MID_MAX
        ]
        mid_hits = [1 if r.get("hit") else 0 for r in mid]
        all_hits = [1 if r.get("hit") else 0 for r in group]
        rows.append(
            {
                "treatment": key[0],
                "probe_kind": key[1],
                "mid_n": len(mid),
                "mid_hit_pct": safe_pct(sum(mid_hits), len(mid_hits)),
                "overall_hit_pct": safe_pct(sum(all_hits), len(all_hits)),
            }
        )
    return rows


def main() -> int:
    args = base_parser(__doc__).parse_args()
    records = read_jsonl(args.raw)
    notes = [
        f"原始账: {args.raw.name}({len(records)} 行)",
        "中段定义: 25%<=位置<=75%(U 形中段);存活率=中段命中率。",
        "判卷确定性优先(归一化匹配/正则),LLM judge 只补同义表达(单子 B1)。",
    ]
    written = []
    written += write_outputs(
        curve_rows(records), CURVE_COLUMNS, args.out_dir,
        args.out_stem or "position_curve", "B1 位置-命中曲线(骨架)", notes,
    )
    written += write_outputs(
        survival_rows(records), SURVIVAL_COLUMNS, args.out_dir,
        args.out_stem or "mid_survival", "B1 中段存活率(骨架)", notes,
    )
    return report_written(written, args.raw, len(records))


if __name__ == "__main__":
    raise SystemExit(main())
