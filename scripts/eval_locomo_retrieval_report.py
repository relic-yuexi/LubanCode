#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoCoMo-MC10 检索层评测汇总(记忆系统评测单 §E1)。

吃 locomo_retrieval_driver 的逐题排级账 + 扰动数据集,产五类 Recall@k
基线表 eval/locomo/retrieval_baseline.md(进 git,不含原文与答案)。

口径(报告里写死,复算不再猜):
- evidence session 定位:answer_text(扰动后)在 session 正文或官方摘要中
  子串命中的 session 集合。多数 LoCoMo 答案是推理产物,子串命不中的题
  归"不可定位",不进 Recall 分母——主口径只算可定位题。
- Recall@k = ranked 前 k 条 topic 里含任一 evidence topic 的题占比
  (driver 的 ranked 即 BuildTurnContext 排级序,含未过门槛条目)。
- adversarial 题答案为 "Not answerable",无 evidence,单列注入面统计。
- 零命中 = trace.entries 为空(整库无一条有得分)。

用法:
  python scripts/eval_locomo_retrieval_report.py <trace.json> [--out md路径]
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
K_VALUES = (1, 3, 5)


def topic_id(conv: str, no: int) -> str:
    return f"fact.locomo-{conv}-s{no}"


def locate_evidence(conv_record: dict, answer_text: str) -> list:
    if not answer_text or answer_text == "Not answerable":
        return []
    hits = []
    for s in conv_record["sessions"]:
        body = "\n".join(s["lines"]) + "\n" + s["summary"]
        if answer_text in body:
            hits.append(s["no"])
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace", help="locomo_retrieval_driver 输出的 trace.json")
    ap.add_argument("--perturbed",
                    default=os.path.join(HERE, "eval", "locomo", "perturbed.jsonl"))
    ap.add_argument("--out", default=os.path.join(HERE, "eval", "locomo", "retrieval_baseline.md"))
    args = ap.parse_args()

    perturbed = {}
    with open(args.perturbed, encoding="utf-8") as f:
        for line in f:
            rec = json.loads(line)
            perturbed[rec["conv_id"]] = rec
    trace = json.load(open(args.trace, encoding="utf-8"))

    # bucket: category -> {n, locatable, hits@k, zero, injected_bytes, injected_cnt}
    buckets = {}
    unmatched_evidence = 0
    for conv_result in trace:
        cid = conv_result["conv"]
        conv_rec = perturbed[cid]
        answer_by_qid = {}
        for q in conv_rec["questions"]:
            answer_by_qid[q["qid"]] = q["answer_text"]
        for item in conv_result["questions"]:
            cat = item["category"]
            b = buckets.setdefault(cat, {
                "n": 0, "locatable": 0, "zero": 0,
                **{f"hit{k}": 0 for k in K_VALUES},
                "bytes": [], "injected_cnt": [],
                "below": 0, "budget": 0,
            })
            b["n"] += 1
            b["bytes"].append(item["injected_bytes"])
            b["injected_cnt"].append(item["injected_count"])
            b["below"] += item["below_threshold"]
            b["budget"] += item["budget_dropped"]
            if item["zero_recall"]:
                b["zero"] += 1
            evidence = locate_evidence(conv_rec, answer_by_qid.get(item["qid"], ""))
            if not evidence:
                continue
            ev_topics = {topic_id(cid, no) for no in evidence}
            ranked = item["ranked"]
            if not any(t in ev_topics for t in ranked):
                unmatched_evidence += 1
                continue  # 可定位但排级榜上无名:计入分母,不计命中
            b["locatable"] += 1
            for k in K_VALUES:
                if any(t in ev_topics for t in ranked[:k]):
                    b[f"hit{k}"] += 1

    # ---- 产表 ----
    lines = []
    lines.append("# LoCoMo-MC10 检索层基线(E1)")
    lines.append("")
    lines.append("数据:扰动后(实体改名+日期平移+题目改写)十场对话,每 session 一条")
    lines.append("fact 主题,走 LubanCode memory 正门(EnqueueSave→worker 落盘→")
    lines.append("BuildTurnContext 生产召回路,Options 生产默认 max_results=3、")
    lines.append("max_retrieval_bytes=8KiB)。判据口径见 scripts/eval_locomo_retrieval_report.py")
    lines.append("头部注释;逐题排级账在 trace.json(gitignore,本地复算用)。")
    lines.append("")
    lines.append("## 五类 Recall@k(分母=可定位题)")
    lines.append("")
    lines.append("| 类别 | 题数 | 可定位 | R@1 | R@3 | R@5 |")
    lines.append("|---|---|---|---|---|---|")
    total_n = total_loc = 0
    total_hits = {k: 0 for k in K_VALUES}
    for cat in sorted(buckets):
        b = buckets[cat]
        loc = b["locatable"]
        total_n += b["n"]
        total_loc += loc
        cells = []
        for k in K_VALUES:
            total_hits[k] += b[f"hit{k}"]
            cells.append(f"{b[f'hit{k}'] / loc:.3f}" if loc else "-")
        lines.append(f"| {cat} | {b['n']} | {loc} | " + " | ".join(cells) + " |")
    cells = [f"{total_hits[k] / total_loc:.3f}" if total_loc else "-" for k in K_VALUES]
    lines.append(f"| **合计** | {total_n} | {total_loc} | " + " | ".join(cells) + " |")
    lines.append("")
    lines.append("## 注入面与拦截(全量题)")
    lines.append("")
    lines.append("| 类别 | 题数 | 零命中 | 平均注入字节 | 平均注入条数 | 门槛拦截 | 预算拦截 |")
    lines.append("|---|---|---|---|---|---|---|")
    for cat in sorted(buckets):
        b = buckets[cat]
        n = b["n"]
        mean_bytes = sum(b["bytes"]) / n if n else 0
        mean_cnt = sum(b["injected_cnt"]) / n if n else 0
        lines.append(f"| {cat} | {n} | {b['zero']}/{n} | {mean_bytes:.0f} | "
                     f"{mean_cnt:.2f} | {b['below']} | {b['budget']} |")
    lines.append("")
    lines.append(f"可定位但排级榜无名的题(evidence topic 整库零得分): {unmatched_evidence}")
    lines.append("")
    lines.append("adversarial 无 evidence(答案 Not answerable),不进 Recall 表,")
    lines.append("只看注入面——B 组幻觉率在 E2 端到端量。")
    lines.append("")

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"\nwritten: {args.out}")


if __name__ == "__main__":
    sys.exit(main())
