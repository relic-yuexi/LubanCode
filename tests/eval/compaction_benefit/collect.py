"""实验 B2(compact 真实对照)collect 骨架:原始 JSONL → csv + md(+parquet)。

P3 才真跑(单子 §五);P0 先立骨架与口径。原始账一行一会话×处理:
  session: 场次 id(8 场 dogfooding)
  treatment: FULL | compact          同一快照/截点起跑的两路
  ledger: UserContract | WorkState | ToolEvidence   三本账(一条事实只落一本)
  kind: constraint | acceptance | superseded | verified_fact | failed_attempt
       | open_item | next_action | tool_result | side_effect
  key_fact_recall / false_retention / contradiction / evidence_attribution:
       每会话各账的命中/误留/矛盾/归因率(0-1)
  downstream_task_success: bool       结构化验收判的续跑成败
  compaction_benefit_ratio: float     compact 省出的 token 比(FULL/compact 口径)
  cold_zone: bool                     事实落冷区/热区
  turns_from_cutoff: int              距截点 turn 数

产出:每 treatment 的指标中位+P95 表 + compaction_benefit_ratio 分桶账。
零分母:指标分母为 0 记 unavailable,不填 0(单子 §二 B)。

注意:本目录的 Lost in Conversation 备料四件套(lock/取数/README/smoke)
是 B2 主基准的原料,与这份 collect 的会话账并行——LiC 逐题判卷的汇总
P3 另立脚本,不混进这里。

用法:
  python collect.py --raw results/raw_compaction_benefit.jsonl --out-dir results
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
    write_outputs,
)

METRIC_KEYS = [
    "key_fact_recall",
    "false_retention",
    "contradiction",
    "evidence_attribution",
    "compaction_benefit_ratio",
    "turns_from_cutoff",
]
COLUMNS = ["treatment", "ledger", "n"] + [f"{k}_{stat}" for k in METRIC_KEYS for stat in ("median", "p95")]


def rows_from(records: list[dict]) -> list[dict]:
    rows = []
    for key, group in sorted(group_by(records, ["treatment", "ledger"]).items()):
        row: dict[str, object] = {"treatment": key[0], "ledger": key[1], "n": len(group)}
        for metric in METRIC_KEYS:
            values = numeric_column(group, metric)
            row[f"{metric}_median"] = median(values)
            row[f"{metric}_p95"] = p95(values)
        rows.append(row)
    return rows


def main() -> int:
    args = base_parser(__doc__).parse_args()
    records = read_jsonl(args.raw)
    notes = [
        f"原始账: {args.raw.name}({len(records)} 行)",
        "must-retain recall 门槛(单子 B2):UserContract/WorkState >=95%,"
        "active todo 100%——门槛判在报告层,这里只出原始分布。",
        "零分母规则: 比率类分母为 0 记 unavailable,不填 0。",
    ]
    written = write_outputs(
        rows_from(records), COLUMNS, args.out_dir,
        args.out_stem or "ledger_summary", "实验 B2:三本账指标(骨架)", notes,
    )
    return report_written(written, args.raw, len(records))


if __name__ == "__main__":
    raise SystemExit(main())
