"""实验 B1 全链管道(ctest 入口:eval.compact_position.pipeline)。

造稿(generate.py)→ 三处理驱动(eval_compact_position,进程内自检先行)
→ 问答判卷落账(grounded 假后端读三版视图作答,判卷比对回答)→ collect
四件表,一条链跑完,链上任一环 exit 非 0 即红。装置阶段全程假后端:不发
真请求、不读真钥匙、不写真 ~/.lubancode。

链上断言(不只是"跑完没报错",装置自身的账也要对得上):
  - 金账行数 = langs × repeats × 位置档数 × (evidence recall 每档数 +
    evidence conflict 1 + contract recall 1 + contract conflict 1);
  - 分层各半:evidence 与 contract 的 needle 数相等(铁律 3"两类各半");
  - 原始账行数 = 金账 needle 数 × 3 处理;
  - 问答账字段齐:每行都有 question 与 model_answer(铁律 1 判卷三件);
  - FULL 的全 needle 问答复 hit(零处理基线,grounded 答不对即装置坏
    ——驱动里也断,双保险);
  - microcompact 至少折掉一例 long 档 evidence needle(真语义信号在场);
  - 问答判卷三型齐现:全链 stale>0 且 lost>0(负路径在主链真红过,
    驱动自检另有显式失败注入);
  - collect 四件表都在,曲线表覆盖三处理 × 两层 × 两类探针。

手动跑:
  python pipeline.py --python python --driver <build>/eval_compact_position.exe \
      --root <repo>/tests/eval/compact_position
"""

from __future__ import annotations

import argparse
import doctest
import json
import os
import subprocess
import sys
from pathlib import Path


def run_step(name: str, command: list[str]) -> None:
    print(f"pipeline: [{name}] {' '.join(str(c) for c in command)}", flush=True)
    env = dict(os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
    result = subprocess.run([str(c) for c in command], env=env)
    if result.returncode != 0:
        sys.exit(f"pipeline: [{name}] exit {result.returncode}——全链到此为止")


def read_jsonl(path: Path) -> list[dict]:
    rows = []
    if not path.exists():
        return rows
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--python", default=sys.executable, help="python 解释器")
    parser.add_argument("--driver", required=True, help="eval_compact_position 可执行件")
    parser.add_argument("--root", default=Path(__file__).resolve().parent,
                        type=Path, help="compact_position 目录(造稿参数与 results 落此)")
    parser.add_argument("--skip-generate", action="store_true",
                        help="复用已有底稿,只跑驱动+collect")
    args = parser.parse_args()

    root: Path = args.root
    results = root / "results"
    sys.path.insert(0, str(root))
    import generate  # noqa: E402  取默认参数对账(段数/档表/重复数)

    # collect 的判卷/分桶口径先自检(doctest 钉中段区间、计数与反救公式)。
    sys.path.insert(0, str(root.parent))
    failures = doctest.testmod(__import__("collect")).failed
    if failures:
        sys.exit(f"pipeline: collect doctest 挂了 {failures} 条")

    # 1) 造稿:默认参数(96 段 × 20 档 × 2 措辞 × 5 重复,每档 4 枚 needle)。
    if not args.skip_generate:
        run_step("generate", [args.python, root / "generate.py",
                              "--out-dir", results])
    gold = read_jsonl(results / "needle_gold.jsonl")
    expected_gold = (len(generate.DEFAULT_LANGS) * generate.DEFAULT_REPEATS
                     * len(generate.DEFAULT_POSITIONS)
                     * (generate.DEFAULT_NEEDLES_PER_POSITION + 3))
    if len(gold) != expected_gold:
        sys.exit(f"pipeline: 金账 {len(gold)} 行,期望 {expected_gold}"
                 "(langs×repeats×档×[evidence recall+evidence conflict+contract recall+contract conflict])")
    evidence_n = sum(1 for r in gold if r.get("layer") == "evidence")
    contract_n = sum(1 for r in gold if r.get("layer") == "contract")
    if evidence_n != contract_n or evidence_n + contract_n != len(gold):
        sys.exit(f"pipeline: 分层不各半(evidence {evidence_n} / contract {contract_n})")

    # 2) 三处理驱动(问答判卷与管道自检在驱动进程内,自检不过 exit 1)。
    run_step("driver", [args.driver, "--drafts", results / "drafts",
                        "--results", results])
    raw = read_jsonl(results / "raw_position_probe.jsonl")
    if len(raw) != len(gold) * 3:
        sys.exit(f"pipeline: 原始账 {len(raw)} 行,期望 {len(gold) * 3}(needle×3 处理)")
    if any(not r.get("question") or not isinstance(r.get("model_answer"), str)
           for r in raw):
        sys.exit("pipeline: 问答账字段缺(question/model_answer)——铁律 1 判卷三件不全")

    full_rows = [r for r in raw if r["treatment"] == "FULL"]
    if not full_rows or not all(r["hit"] for r in full_rows):
        sys.exit("pipeline: FULL 问答判失存在——零处理基线破了,装置坏")
    micro_lost_long = [r for r in raw if r["treatment"] == "microcompact" and not r["hit"]
                       and r.get("seg_length_class") == "long"]
    if not micro_lost_long:
        sys.exit("pipeline: microcompact 没折掉任何 long 档 needle——真语义信号缺位")
    stale_n = sum(1 for r in raw if r.get("verdict") == "stale")
    lost_n = sum(1 for r in raw if r.get("verdict") == "lost")
    if stale_n == 0 or lost_n == 0:
        sys.exit(f"pipeline: 问答判卷三型未齐现(stale {stale_n} / lost {lost_n})"
                 "——失败注入没在主链真红过")

    # 3) collect:位置-命中曲线 + 中段存活率 + compact 反救 + 折叠分桶。
    run_step("collect", [args.python, root / "collect.py",
                         "--raw", results / "raw_position_probe.jsonl",
                         "--out-dir", results])
    curve_path = results / "position_curve.csv"
    if not curve_path.exists():
        sys.exit("pipeline: collect 没落 position_curve.csv")
    curve = [line.split(",") for line in curve_path.read_text(encoding="utf-8").splitlines()[1:]]
    combos = {(row[0], row[1], row[2]) for row in curve}
    expected_combos = {(t, layer, k) for t in ("FULL", "microcompact", "compact")
                       for layer in ("evidence", "contract")
                       for k in ("recall", "conflict")}
    if combos != expected_combos:
        sys.exit(f"pipeline: 曲线表缺处理×层×探针组合: {expected_combos - combos}")
    for stem in ("mid_survival", "compact_rescue", "fold_survival"):
        if not (results / f"{stem}.csv").exists():
            sys.exit(f"pipeline: collect 没落 {stem}.csv")

    treatments = len({(r["draft_id"], r["treatment"]) for r in raw})
    hits = sum(1 for r in raw if r["hit"])
    print(f"pipeline: 全链绿——{len(gold)} needle(分层各半 {evidence_n}/{contract_n})"
          f" × {treatments} 处理视图 = {len(raw)} 案,hit {hits},stale {stale_n},"
          f"lost {lost_n};假后端零真请求,问答判卷三型齐现。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
