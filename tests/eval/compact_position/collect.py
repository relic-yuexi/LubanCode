"""实验 B1(compact 位置探针)collect:原始 JSONL → csv + md(+parquet)。

吃 eval_driver.cpp 落的 raw_position_probe.jsonl(一行一判,**问答判卷**:
判卷比对的是模型回答,不是视图原文),出四张表:
  1) 位置-命中曲线 position_curve:treatment × layer × probe_kind × 位置档
     的问答命中率与 stale/lost 细分(单子 §二 B1 记账三铁律:按层分列,
     "设计如此"与"意外丢失"不混锅);
  2) 中段存活率 mid_survival:treatment × layer × probe_kind 的中段
     (30%<=位置<=70%)命中率与总体命中率;
  3) compact 反救中段 compact_rescue:layer × probe_kind 的 compact 臂
     中段命中率 vs FULL 臂同档——摘要落点固定在上下文尾部(注意力好区),
     compact 是否反救中段,子串判卷对此全瞎,问答判卷才可见(单子 §二 B1
     单列的一问);
  4) 折叠分桶 fold_survival(evidence 层):treatment × probe_kind × 段长
     档命中率——microcompact 折叠与上下文位置无关、与 needle 落段长度强
     相关(长段中段事实被换成头尾 256B 预览即真丢)。
零分母:格无样本记 unavailable,不填 0(单子记账规则)。

原始账 schema(eval_driver.cpp 落,关键字段):
  treatment: FULL | microcompact | compact     同一底稿换压缩路
  layer: evidence | contract                   证据类(被摘要)| 合同类(该进 manifest)
  probe_kind: recall | conflict                直接召回 | 更新冲突
  carrier: tool_result | user_turn             needle 落点(工具结果 | user 消息)
  position_pct: 0|5|...|95                     needle 设计档(20 档)
  lang: zh | en;repeat: 1..5
  question/model_answer/verdict: 问答判卷三件——题面、模型回答、判定
  verdict: hit | stale | lost                  答新值=hit;答旧值=stale;答不出=lost
  new_in_view/old_in_view: 视图级子串在场,辅助诊断列(FULL/microcompact
      机械性信号;compact 臂成绩只认问答,不据此记)
  seg_length_class: short | medium | long      evidence needle 落段长度档
  summary_fake: bool(compact 臂摘要为假,只验管道)

注意:compact 臂 summary_fake=true——六栏摘要是假后端替身,该臂装置数字
只证管道+热区保留形状,语义命中率待真模型跑。FULL 在 grounded 问答下恒
全 hit(它是装置完整性锚;U 形是模型现象,留给真模型问答)。

用法:
  python collect.py --raw results/raw_position_probe.jsonl --out-dir results
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from collect_common import (  # noqa: E402
    UNAVAILABLE,
    base_parser,
    group_by,
    read_jsonl,
    report_written,
    safe_pct,
    write_outputs,
)

# 中段定义(装置单口径):位置 30%~70% 档——U 形的腰部。
MID_MIN, MID_MAX = 30, 70

LAYERS = ["evidence", "contract"]
PROBE_KINDS = ["recall", "conflict"]

CURVE_COLUMNS = ["treatment", "layer", "probe_kind", "position_pct", "n", "hit_n", "hit_pct",
                 "stale_n", "lost_n"]
SURVIVAL_COLUMNS = ["treatment", "layer", "probe_kind", "mid_n", "mid_hit_pct", "mid_stale_pct",
                    "overall_n", "overall_hit_pct", "view_new_present_pct"]
RESCUE_COLUMNS = ["layer", "probe_kind", "full_mid_n", "full_mid_hit_pct",
                  "compact_mid_n", "compact_mid_hit_pct", "rescue_delta_pp", "summary_fake"]
FOLD_COLUMNS = ["treatment", "probe_kind", "seg_length_class", "n", "hit_n", "hit_pct",
                "stale_n", "view_new_present_pct"]


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


def view_new_present_pct(cell: list[dict]) -> float | str:
    """视图级子串在场率(辅助诊断列):FULL/microcompact 给数,compact 不给。

    >>> view_new_present_pct([{"treatment": "FULL", "new_in_view": True},
    ...                       {"treatment": "FULL", "new_in_view": True}])
    100.0
    >>> view_new_present_pct([{"treatment": "compact", "new_in_view": True}])
    'unavailable'
    """
    if not cell or any(r.get("treatment") == "compact" for r in cell):
        return UNAVAILABLE
    return safe_pct(sum(1 for r in cell if r.get("new_in_view")), len(cell))


def layer_of(record: dict) -> str:
    """分层字段;缺层(旧账兼容)按 evidence 归。

    >>> layer_of({"layer": "contract"})
    'contract'
    >>> layer_of({})
    'evidence'
    """
    layer = record.get("layer")
    return layer if isinstance(layer, str) and layer else "evidence"


def observed_positions(records: list[dict]) -> list[int]:
    """数据里真跑过的位置档(升序去重)——位置表可参数化,不硬钉 20 档。"""
    positions = {r.get("position_pct") for r in records
                 if isinstance(r.get("position_pct"), (int, float))
                 and not isinstance(r.get("position_pct"), bool)}
    return sorted(int(p) for p in positions)


def curve_rows(records: list[dict]) -> list[dict]:
    """位置-命中曲线:treatment × layer × probe_kind × 位置档(铁律 3 按层分列)。"""
    rows = []
    positions = observed_positions(records)
    for key, group in sorted(group_by(records, ["treatment", "layer", "probe_kind"]).items()):
        by_pos = group_by(group, ["position_pct"])
        for pos in positions:
            cell = by_pos.get((str(pos),), [])
            n, hits, stale, lost = verdict_counts(cell)
            rows.append({
                "treatment": key[0],
                "layer": key[1],
                "probe_kind": key[2],
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
    for key, group in sorted(group_by(records, ["treatment", "layer", "probe_kind"]).items()):
        mid = [r for r in group if is_mid(r.get("position_pct"))]
        mid_n, mid_hits, mid_stale, _ = verdict_counts(mid)
        all_n, all_hits, _, _ = verdict_counts(group)
        rows.append({
            "treatment": key[0],
            "layer": key[1],
            "probe_kind": key[2],
            "mid_n": mid_n,
            "mid_hit_pct": safe_pct(mid_hits, mid_n),
            "mid_stale_pct": safe_pct(mid_stale, mid_n),
            "overall_n": all_n,
            "overall_hit_pct": safe_pct(all_hits, all_n),
            "view_new_present_pct": view_new_present_pct(mid),
        })
    return rows


def rescue_rows(records: list[dict]) -> list[dict]:
    """compact 反救中段:compact 臂中段命中率 - FULL 臂同档命中率(百分点)。

    delta > 0 = compact 反救中段(摘要落点在尾部好区,把 U 形腰部抬起来);
    delta < 0 = compact 压塌中段。装置阶段 compact 摘要为假(summary_fake),
    该列只证管道形态;真跑后这就是反转效应的观测点。

    >>> rows = rescue_rows([{"treatment": "FULL", "layer": "evidence", "probe_kind": "recall",
    ...                      "position_pct": 40, "verdict": "hit"},
    ...                     {"treatment": "FULL", "layer": "evidence", "probe_kind": "recall",
    ...                      "position_pct": 60, "verdict": "hit"},
    ...                     {"treatment": "compact", "layer": "evidence", "probe_kind": "recall",
    ...                      "position_pct": 40, "verdict": "hit", "summary_fake": True},
    ...                     {"treatment": "compact", "layer": "evidence", "probe_kind": "recall",
    ...                      "position_pct": 60, "verdict": "lost", "summary_fake": True}])
    >>> len(rows)  # 全矩阵:层 2 × 探针 2,零分母 unavailable 不填 0
    4
    >>> rows[0] == {"layer": "evidence", "probe_kind": "recall", "full_mid_n": 2,
    ...             "full_mid_hit_pct": 100.0, "compact_mid_n": 2,
    ...             "compact_mid_hit_pct": 50.0, "rescue_delta_pp": -50.0,
    ...             "summary_fake": True}
    True
    """
    rows: list[dict] = []
    for layer in LAYERS:
        for kind in PROBE_KINDS:
            def mid_hits(treatment: str) -> tuple[int, float | str]:
                cell = [r for r in records if r.get("treatment") == treatment
                        and layer_of(r) == layer and r.get("probe_kind") == kind
                        and is_mid(r.get("position_pct"))]
                n, hits, _, _ = verdict_counts(cell)
                return n, safe_pct(hits, n)

            full_n, full_pct = mid_hits("FULL")
            compact_n, compact_pct = mid_hits("compact")
            delta: float | str = UNAVAILABLE
            if isinstance(full_pct, float) and isinstance(compact_pct, float):
                delta = round(compact_pct - full_pct, 2)
            rows.append({
                "layer": layer,
                "probe_kind": kind,
                "full_mid_n": full_n,
                "full_mid_hit_pct": full_pct,
                "compact_mid_n": compact_n,
                "compact_mid_hit_pct": compact_pct,
                "rescue_delta_pp": delta,
                "summary_fake": any(r.get("treatment") == "compact" and r.get("summary_fake")
                                    for r in records),
            })
    return rows


def fold_rows(records: list[dict]) -> list[dict]:
    """折叠分桶(evidence 层专属:contract needle 落 user 消息,无段长档)。"""
    rows = []
    evidence = [r for r in records if layer_of(r) == "evidence"]
    classes = ["short", "medium", "long"]
    for key, group in sorted(group_by(evidence, ["treatment", "probe_kind"]).items()):
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
                "view_new_present_pct": view_new_present_pct(cell),
            })
    return rows


def main() -> int:
    args = base_parser(__doc__).parse_args()
    records = read_jsonl(args.raw)
    notes = [
        f"原始账: {args.raw.name}({len(records)} 行)",
        f"中段定义: 位置 {MID_MIN}%~{MID_MAX}%(含端点);存活率=中段问答命中率。",
        "问答判卷(铁律 1):verdict 比对的是模型回答,不是视图原文——hit=答新值/"
        "期望值;stale=答已被 superseded 的旧值;lost=答不出。",
        "分层(铁律 3):evidence=工具输出里的事实(本分是被摘要);contract=用户"
        "约定(落 user 消息,按产品设计该进 manifest 逐字收编)。",
        "view_new_present_pct 是视图级子串在场率,仅作 FULL/microcompact 臂的折叠"
        "机械性诊断;compact 臂成绩只认问答,该列不给数。",
        "compact 臂 summary_fake=true:六栏摘要是假后端替身,该臂装置数字只证管道+"
        "热区保留形状,语义命中率(含 compact_rescue 的反转效应)待真模型跑。",
        "判卷确定性优先(归一化匹配),LLM judge 只补同义表达(单子 B1)。",
    ]
    written = []
    written += write_outputs(
        curve_rows(records), CURVE_COLUMNS, args.out_dir,
        args.out_stem or "position_curve", "B1 位置-命中曲线(处理×层×探针×位置档)", notes,
    )
    written += write_outputs(
        survival_rows(records), SURVIVAL_COLUMNS, args.out_dir,
        args.out_stem or "mid_survival", "B1 中段存活率(处理×层×探针)", notes,
    )
    written += write_outputs(
        rescue_rows(records), RESCUE_COLUMNS, args.out_dir,
        args.out_stem or "compact_rescue", "B1 compact 反救中段(compact vs FULL,问答判卷)",
        notes,
    )
    written += write_outputs(
        fold_rows(records), FOLD_COLUMNS, args.out_dir,
        args.out_stem or "fold_survival", "B1 折叠分桶存活率(evidence 层,段长档)", notes,
    )
    return report_written(written, args.raw, len(records))


if __name__ == "__main__":
    raise SystemExit(main())
