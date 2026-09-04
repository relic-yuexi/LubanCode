"""实验 B1(compact 位置探针)collect:原始 JSONL → csv + md(+parquet)。

吃 eval_driver.cpp 落的 raw_position_probe.jsonl(一行一判),出三张表:
  1) 位置-命中曲线 position_curve:treatment × probe_kind × 位置档的命中率
     与 stale/lost 细分(单子 §二 B1 的核心账);
  2) 中段存活率 mid_survival:treatment × probe_kind 的中段(30%<=位置<=70%,
     装置单口径)命中率与总体命中率——compact/microcompact 把 FULL 的 U 形
     压成什么样,先看这一格;
  3) 折叠分桶 fold_survival:treatment × probe_kind × 段长档命中率——
     microcompact 折叠与上下文位置无关、与 needle 落段长度强相关(长段中段
     事实被换成头尾 256B 预览即真丢),这一桶是装置的第一笔真语义信号。
零分母:格无样本记 unavailable,不填 0(单子记账规则)。

原始账 schema(eval_driver.cpp 落,关键字段):
  treatment: FULL | microcompact | compact     同一底稿换压缩路
  probe_kind: recall | conflict                直接召回 | 更新冲突
  position_pct: 0|5|...|95                     needle 设计档(20 档)
  lang: zh | en;repeat: 1..5;verdict: hit | stale | lost
  seg_length_class: short | medium | long      needle 落段长度档
  summary_fake: bool(compact 臂摘要为假,只验管道)

注意:compact 臂 summary_fake=true——六栏摘要是假后端替身,语义待真模型跑,
读表时该臂只看"管道+热区保留"形状,不读语义命中率。FULL 在视图判卷阶段
恒全存活(U 形是模型现象,留给真模型问答阶段),它是装置完整性锚。

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
    read_jsonl,
    report_written,
    safe_pct,
    write_outputs,
)

# 中段定义(装置单口径):位置 30%~70% 档——U 形的腰部。
MID_MIN, MID_MAX = 30, 70

CURVE_COLUMNS = ["treatment", "probe_kind", "position_pct", "n", "hit_n", "hit_pct",
                 "stale_n", "lost_n"]
SURVIVAL_COLUMNS = ["treatment", "probe_kind", "mid_n", "mid_hit_pct", "mid_stale_pct",
                    "overall_n", "overall_hit_pct"]
FOLD_COLUMNS = ["treatment", "probe_kind", "seg_length_class", "n", "hit_n", "hit_pct", "stale_n"]


def is_mid(position_pct: object) -> bool:
    """位置档是否落中段(30%~70%,含端点);非数值记 False,由零分母规则兜底。

    >>> is_mid(30)
    True
    >>> is_mid(70)
    True
    >>> is_mid(29)
    False
    >>> is_mid(71)
    False
    >>> is_mid("unavailable")
    False
    """
    if not isinstance(position_pct, (int, float)) or isinstance(position_pct, bool):
        return False
    return MID_MIN <= position_pct <= MID_MAX


def verdict_counts(cell: list[dict]) -> tuple[int, int, int, int]:
    """一格样本的 (n, hit_n, stale_n, lost_n)。

    >>> verdict_counts([{"verdict": "hit"}, {"verdict": "hit"},
    ...                 {"verdict": "stale"}, {"verdict": "lost"}])
    (4, 2, 1, 1)
    >>> verdict_counts([])
    (0, 0, 0, 0)
    """
    hits = sum(1 for r in cell if r.get("verdict") == "hit")
    stale = sum(1 for r in cell if r.get("verdict") == "stale")
    lost = sum(1 for r in cell if r.get("verdict") == "lost")
    return len(cell), hits, stale, lost


def observed_positions(records: list[dict]) -> list[int]:
    """数据里真跑过的位置档(升序去重)——位置表可参数化,不硬钉 20 档。"""
    positions = {r.get("position_pct") for r in records
                 if isinstance(r.get("position_pct"), (int, float))
                 and not isinstance(r.get("position_pct"), bool)}
    return sorted(int(p) for p in positions)


def curve_rows(records: list[dict]) -> list[dict]:
    rows = []
    positions = observed_positions(records)
    for key, group in sorted(group_by(records, ["treatment", "probe_kind"]).items()):
        by_pos = group_by(group, ["position_pct"])
        for pos in positions:
            cell = by_pos.get((str(pos),), [])
            n, hits, stale, lost = verdict_counts(cell)
            rows.append({
                "treatment": key[0],
                "probe_kind": key[1],
                "position_pct": pos,
                "n": n,
                "hit_n": hits,
                "hit_pct": safe_pct(hits, n),
                "stale_n": stale,
                "lost_n": lost,
            })
    return rows


def survival_rows(records: list[dict]) -> list[dict]:
    rows = []
    for key, group in sorted(group_by(records, ["treatment", "probe_kind"]).items()):
        mid = [r for r in group if is_mid(r.get("position_pct"))]
        mid_n, mid_hits, mid_stale, _ = verdict_counts(mid)
        all_n, all_hits, _, _ = verdict_counts(group)
        rows.append({
            "treatment": key[0],
            "probe_kind": key[1],
            "mid_n": mid_n,
            "mid_hit_pct": safe_pct(mid_hits, mid_n),
            "mid_stale_pct": safe_pct(mid_stale, mid_n),
            "overall_n": all_n,
            "overall_hit_pct": safe_pct(all_hits, all_n),
        })
    return rows


def fold_rows(records: list[dict]) -> list[dict]:
    rows = []
    classes = ["short", "medium", "long"]
    for key, group in sorted(group_by(records, ["treatment", "probe_kind"]).items()):
        by_class = group_by(group, ["seg_length_class"])
        for cls in classes:
            cell = by_class.get((cls,), [])
            n, hits, stale, _ = verdict_counts(cell)
            rows.append({
                "treatment": key[0],
                "probe_kind": key[1],
                "seg_length_class": cls,
                "n": n,
                "hit_n": hits,
                "hit_pct": safe_pct(hits, n),
                "stale_n": stale,
            })
    return rows


def main() -> int:
    args = base_parser(__doc__).parse_args()
    records = read_jsonl(args.raw)
    notes = [
        f"原始账: {args.raw.name}({len(records)} 行)",
        f"中段定义: 位置 {MID_MIN}%~{MID_MAX}%(含端点);存活率=中段命中率。",
        "verdict:hit=新值/期望值在视图;stale=仅旧值在(更新冲突,旧值 superseded "
        "不得作答,单列不算 hit);lost=两值都不在。",
        "compact 臂 summary_fake=true:六栏摘要是假后端替身,该臂只看管道+热区"
        "保留形状,语义命中率待真模型跑。",
        "判卷确定性优先(归一化匹配),LLM judge 只补同义表达(单子 B1)。",
    ]
    written = []
    written += write_outputs(
        curve_rows(records), CURVE_COLUMNS, args.out_dir,
        args.out_stem or "position_curve", "B1 位置-命中曲线", notes,
    )
    written += write_outputs(
        survival_rows(records), SURVIVAL_COLUMNS, args.out_dir,
        args.out_stem or "mid_survival", "B1 中段存活率", notes,
    )
    written += write_outputs(
        fold_rows(records), FOLD_COLUMNS, args.out_dir,
        args.out_stem or "fold_survival", "B1 折叠分桶存活率(段长档)", notes,
    )
    return report_written(written, args.raw, len(records))


if __name__ == "__main__":
    raise SystemExit(main())
