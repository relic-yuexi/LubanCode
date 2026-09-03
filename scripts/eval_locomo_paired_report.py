#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoCoMo E2 配对报告(记忆写入侧改进单 §二:E2 放量与区间分布)。

吃 runner 的逐题账(e2_per_question_A/B*.json,A/B 各一册,同 suffix),产:
- 五类配对表:n、A/B 准确、配对增益 B-A、只B对/只A对(逐题同 qid 配对);
- 增益的 bootstrap 95% 区间(按题重采样,10000 次,种子固定可复算);
- 逐场分布(每场每类配对增益,不只报平均);
- adversarial 幻觉率(选了实选项的比例)与"Not answerable"率,A/B 分列。

只读逐题账,不碰答案原文;输出 markdown 进 eval/locomo/。

用法:
  python scripts/eval_locomo_paired_report.py --suffix _t1 [--out md路径] \
      [--title "标题行"]
"""

import argparse
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE_EVAL = os.environ.get("LOCOMO_EVAL_DIR") or os.path.join(HERE, "eval", "locomo")
CATEGORIES = ("single_hop", "multi_hop", "temporal_reasoning", "open_domain",
              "adversarial")
BOOTSTRAP_ROUNDS = 10000
BOOTSTRAP_SEED = 20260903


def load_mode(suffix: str, mode: str) -> list:
    path = os.path.join(HERE_EVAL, f"e2_per_question_{mode}{suffix}.json")
    if not os.path.exists(path):
        return []
    return json.load(open(path, encoding="utf-8"))


def done_records(records: list) -> dict:
    """qid -> 完成账(choice>=0 且无 failure);失败/判不出的不进配对分母,
    计数另报。"""
    out = {}
    for r in records:
        if not r.get("failure") and r.get("choice", -1) >= 0:
            out[r["qid"]] = r
    return out


def bootstrap_ci(pairs: list, rounds=BOOTSTRAP_ROUNDS, seed=BOOTSTRAP_SEED):
    """配对增益的 95% 区间:每对是 (b_hit - a_hit) ∈ {0, 1, -1}。"""
    if not pairs:
        return None
    rng = random.Random(seed)
    n = len(pairs)
    means = []
    for _ in range(rounds):
        sample = [pairs[rng.randrange(n)] for _ in range(n)]
        means.append(sum(sample) / n)
    means.sort()
    return means[int(rounds * 0.025)], means[int(rounds * 0.975)]


def hallucination_rate(records: list, category: str) -> float:
    """幻觉率:adversarial 题里选了实选项(非 Not answerable)的比例。"""
    rows = [r for r in records if r.get("category") == category]
    rows = [r for r in rows if not r.get("failure") and r.get("choice", -1) >= 0]
    if not rows:
        return 0.0
    picked_real = sum(1 for r in rows
                      if "not answerable" not in (r.get("choice_text") or "").lower())
    return picked_real / len(rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--suffix", default="", help="账文件后缀(与 runner --suffix 一致)")
    ap.add_argument("--out", default="", help="输出 md 路径(默认 eval/locomo/paired<suffix>.md)")
    ap.add_argument("--title", default="LoCoMo E2 配对报告")
    args = ap.parse_args()

    a_all = load_mode(args.suffix, "A")
    b_all = load_mode(args.suffix, "B")
    a_done = done_records(a_all)
    b_done = done_records(b_all)
    paired_qids = sorted(set(a_done) & set(b_done))

    lines = [f"# {args.title}", ""]
    lines.append(f"账:e2_per_question_A/B{args.suffix}.json(同 suffix 双态);配对口径 = "
                 f"同 qid 两态都有完成账(A 完成 {len(a_done)}、B 完成 {len(b_done)}、"
                 f"配对 {len(paired_qids)})。失败/判不出不计入分母,只报计数。")
    lines.append("")

    # ---- 五类配对表 ----
    lines.append("## 五类配对(A=裸底,B=记忆在岗;同题同选项)")
    lines.append("")
    lines.append("| 类别 | n | A 准确 | B 准确 | B-A | 95%区间 | 只B对 | 只A对 |")
    lines.append("|---|---|---|---|---|---|---|---|")
    total_pairs = []
    for cat in CATEGORIES:
        qids = [q for q in paired_qids if a_done[q]["category"] == cat]
        a_hits = [a_done[q]["hit"] for q in qids]
        b_hits = [b_done[q]["hit"] for q in qids]
        pairs = [b - a for b, a in zip(b_hits, a_hits)]
        total_pairs.extend(pairs)
        only_b = sum(1 for d in pairs if d > 0)
        only_a = sum(1 for d in pairs if d < 0)
        acc_a = sum(a_hits) / len(qids) if qids else 0.0
        acc_b = sum(b_hits) / len(qids) if qids else 0.0
        ci = bootstrap_ci(pairs)
        ci_text = f"[{ci[0]:+.3f}, {ci[1]:+.3f}]" if ci else "-"
        lines.append(f"| {cat} | {len(qids)} | {acc_a:.3f} | {acc_b:.3f} | "
                     f"{acc_b - acc_a:+.3f} | {ci_text} | {only_b} | {only_a} |")
    acc_all_a = sum(a_done[q]["hit"] for q in paired_qids) / len(paired_qids) if paired_qids else 0
    acc_all_b = sum(b_done[q]["hit"] for q in paired_qids) / len(paired_qids) if paired_qids else 0
    ci = bootstrap_ci(total_pairs)
    ci_text = f"[{ci[0]:+.3f}, {ci[1]:+.3f}]" if ci else "-"
    lines.append(f"| **合计** | {len(paired_qids)} | {acc_all_a:.3f} | {acc_all_b:.3f} | "
                 f"{acc_all_b - acc_all_a:+.3f} | {ci_text} | "
                 f"{sum(1 for d in total_pairs if d > 0)} | "
                 f"{sum(1 for d in total_pairs if d < 0)} |")
    lines.append("")

    # ---- 逐场分布 ----
    lines.append("## 逐场分布(可答四桶合计配对增益,按场分列)")
    lines.append("")
    lines.append("| 场 | 配对 n | A | B | B-A | adversarial 幻觉率 A→B |")
    lines.append("|---|---|---|---|---|---|")
    convs = sorted({a_done[q]["conv"] for q in paired_qids})
    answerable = [c for c in CATEGORIES if c != "adversarial"]
    for conv in convs:
        qids = [q for q in paired_qids
                if a_done[q]["conv"] == conv and a_done[q]["category"] in answerable]
        a_hits = [a_done[q]["hit"] for q in qids]
        b_hits = [b_done[q]["hit"] for q in qids]
        acc_a = sum(a_hits) / len(qids) if qids else 0.0
        acc_b = sum(b_hits) / len(qids) if qids else 0.0
        hall_a = hallucination_rate(list(a_done.values()), "adversarial")
        hall_b = hallucination_rate(list(b_done.values()), "adversarial")
        # 幻觉率是全账口径,逐场只在行内注明场次账
        conv_a = [r for r in a_all if r.get("conv") == conv and not r.get("failure")]
        conv_b = [r for r in b_all if r.get("conv") == conv and not r.get("failure")]
        hall_a = hallucination_rate(conv_a, "adversarial")
        hall_b = hallucination_rate(conv_b, "adversarial")
        lines.append(f"| {conv} | {len(qids)} | {acc_a:.3f} | {acc_b:.3f} | "
                     f"{acc_b - acc_a:+.3f} | {hall_a:.3f} → {hall_b:.3f} |")
    lines.append("")

    # ---- 幻觉率(全量) ----
    lines.append("## adversarial 幻觉率(选了实选项的比例;越低越好)")
    lines.append("")
    lines.append(f"- A(裸底): {hallucination_rate(a_all, 'adversarial'):.3f}")
    lines.append(f"- B(记忆): {hallucination_rate(b_all, 'adversarial'):.3f}")
    lines.append("")

    # ---- 失败账 ----
    a_fail = sum(1 for r in a_all if r.get("failure"))
    b_fail = sum(1 for r in b_all if r.get("failure"))
    if a_fail or b_fail:
        lines.append(f"失败调用:A {a_fail} 次、B {b_fail} 次(逐题账有明细)。")
        lines.append("")

    out = args.out or os.path.join(HERE_EVAL, f"paired{args.suffix}.md")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"\nwritten: {out}")


if __name__ == "__main__":
    sys.exit(main())
