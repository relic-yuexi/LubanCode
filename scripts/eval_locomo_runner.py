#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoCoMo-MC10 端到端 off/on 配对 runner(记忆系统评测单 §E2,烧模型,抽样起步)。

管道形态(参考 scripts/deferred_quality_compare.py 趟通的路):
  阶段一 灌对话:每场对话一个 workspace 目录,python 按 EnqueueJob 同款
    JSON 写 memory job(与 EnqueueSave 产的字段一字不差),主 exe
    --memory-worker 落盘——只灌 haystack sessions,QA 对永不进记忆。
  阶段二 答题:同 seed 抽样题集,A/B 两态各跑一遍——
    A(裸底): memory.enabled=false,one_shot 只给题与选项;
    B(记忆在岗): memory.enabled=true,one_shot 走生产召回路注入记忆。
    同模型(ccmoon/gpt-5.6-sol)、温度 0、one_shot 每题新进程=新会话,
    不带对话上下文(写入与答题硬隔离,§2.2)。
  判分:trajectory main.jsonl 里最后一条 assistant 文本抓选项号,对
  perturbed 的 correct_choice_index(扰动不改位次)。答题流程永不读
  ledger/answers——判分在跑完之后,由本脚本按 qid 查 answers.json。

钥匙安全:真 config.json 拷进临时 USERPROFILE 下的 _run/home_X/.lubancode/,
_run/ 整体 gitignore,绝不进 git。

用法:
  python scripts/eval_locomo_runner.py --smoke          # 1 场每类 1 题,验管线
  python scripts/eval_locomo_runner.py                  # 2 场 x 五类各 10 题
"""

import argparse
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE_EVAL = os.environ.get("LOCOMO_EVAL_DIR") or os.path.join(HERE, "eval", "locomo")
# workspace 目录必须放在干净的盘级路径:ResolveWorkspaceIdentity 四级
# 裁决会爬最近边界——ws 在主仓 D:/lubancode 树内会撞 git common dir,
# 在 %TEMP% 下会撞用户主目录的 .lubancode config 标记,两者都让
# one_shot 与预灌记忆的 workspace 错开、B 组记忆注不进去(实测都踩过)。
# D:/ 盘根无 .git/.lubancode,裁决落到 cwd 回退级,与 python 复刻一致。
WS_ROOT = r"D:\locomo_e2_ws"
REAL_CONFIG = os.path.join(os.environ.get("USERPROFILE", r"C:\Users\moontidef"),
                          ".lubancode", "config.json")
# EXE 可用 LOCOMO_EXE 覆盖(复跑指向改进分支的构建;默认主仓产物)。
EXE = os.environ.get("LOCOMO_EXE") or r"D:\lubancode\build\release\Release\lubancode.exe"
PROVIDER_NAME = "ccmoon"
TEMPERATURE = 0
# 用户日常档是 high,但 A 组(无记忆)high 空推理实测 420s+ 不封顶,放量跑
# 不动;两组对称降档(可经 LOCOMO_EFFORT 覆盖),口径可复现,报告注明与日常档差异。
REASONING_EFFORT = os.environ.get("LOCOMO_EFFORT", "medium")
TASK_TIMEOUT_SECS = 420
CATEGORIES = ("single_hop", "multi_hop", "temporal_reasoning", "open_domain",
              "adversarial")
MODES = ("A", "B")  # A=memory off 裸底;B=memory on


# ---------------------------------------------------------------- workspace key
# 复刻 src/workspace/identity.cpp 的 cwd 回退级:path: 前缀+规范绝对路径
# (正斜杠、ASCII 小写折叠)做 sha256,前 16 位;SafeName(basename,48)。
def _safe_name(value: str, max_bytes: int = 48) -> str:
    out = []
    dash = False
    for ch in value.encode("utf-8"):
        c = chr(ch)
        if len(out) >= max_bytes:
            break
        if ch >= 0x80 or c.isalnum() or c in "_-":
            out.append(c)
            dash = False
        elif not dash and out:
            out.append("-")
            dash = True
    name = "".join(out).rstrip("-")
    return name or "project"


def workspace_key(project_root: str) -> str:
    norm = os.path.abspath(project_root).replace("\\", "/").lower()
    if len(norm) > 1 and norm.endswith("/"):
        norm = norm[:-1]
    seed = "path:" + norm
    digest = hashlib.sha256(seed.encode("utf-8")).hexdigest()
    display = _safe_name(os.path.basename(norm))
    return display + "-" + digest[:16]


# ---------------------------------------------------------------- 配置与环境

def build_home(mode: str) -> str:
    home = os.path.join(HERE_EVAL, "_run", "home_" + mode)
    shutil.rmtree(home, ignore_errors=True)
    luban = os.path.join(home, ".lubancode")
    os.makedirs(luban, exist_ok=True)
    cfg = json.load(open(REAL_CONFIG, encoding="utf-8"))
    # 钉死评测口径:active_provider 一并钉在评测 provider 上——用户真 config 的
    # 默认 provider 日常会切(实测被切到 minimax 后,整批答题悄悄换了模型,账全废)。
    cfg["active_provider"] = PROVIDER_NAME
    # A/B 唯一差异:memory.enabled。B 组 user 层不动(默认关),只开项目层。
    cfg["memory"] = dict(cfg.get("memory") or {})
    cfg["memory"]["enabled"] = (mode == "B")
    for prov in cfg.get("providers", []):
        if prov.get("name") == PROVIDER_NAME:
            extra = prov.get("extra_body") or {}
            extra["temperature"] = TEMPERATURE
            prov["extra_body"] = extra
            prov["model_reasoning_effort"] = REASONING_EFFORT
    with open(os.path.join(luban, "config.json"), "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=1)
    return home


# ---------------------------------------------------------------- 灌入(job 正门)

def stamp(index: int) -> str:
    return time.strftime("%Y%m%d-%H%M%S") + f"-{index:04d}"


def ingest_conv(home: str, conv: dict) -> tuple:
    """一场对话灌成 session 粒度 topics。返回 (ws_dir, n_topics)。"""
    cid = conv["conv_id"]
    ws_dir = ws_dir_of(cid)
    os.makedirs(ws_dir, exist_ok=True)
    key = workspace_key(ws_dir)
    luban = os.path.join(home, ".lubancode")
    workspace_dir = os.path.join(luban, "workspaces", key)
    memory_dir = os.path.join(workspace_dir, "memory")
    os.makedirs(memory_dir, exist_ok=True)
    speakers = []
    for s in conv["sessions"]:
        for line in s["lines"]:
            m = re.match(r"^\[([A-Z][A-Z ]*)\]:", line)
            if m:
                nice = m.group(1).capitalize()
                if nice not in speakers:
                    speakers.append(nice)
        if len(speakers) >= 2:
            break
    pending = os.path.join(luban, "memory-jobs", "pending")
    os.makedirs(pending, exist_ok=True)
    for i, s in enumerate(conv["sessions"]):
        no = s["no"]
        content = "\n".join(s["lines"])[:8192]
        title = (f"Chat session {no} ({s['datetime'][:10]}) "
                 f"{speakers[0]} & {speakers[1]}" if len(speakers) == 2
                 else f"Chat session {no} ({s['datetime'][:10]}) chat")[:200]
        job = {
            "schema": 1,
            "operation": "upsert",
            "workspace_key": key,
            "display_name": os.path.basename(ws_dir),
            "project_root": ws_dir,
            "workspace_dir": workspace_dir,
            "memory_dir": memory_dir,
            "source_event_ref": f"workspace_key={key}/session_id=locomo-eval-{cid}/run_id=none/event_id=none",
            "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "id": f"fact.locomo-{cid}-s{no}",
            "kind": "fact",
            "title": title,
            "summary": s["summary"][:500],
            "content": content,
            "keywords": speakers,
            "paths": [],
            "source_session": f"locomo-eval-{cid}",
            "confidence": "verified",
            # 时间线锚点(记忆写入侧改进单):session datetime 是材料自带
            # 的时间,不是推算——灌进 occurred_at,注入侧排成时间线。
            "occurred_at": s["datetime"][:10],
        }
        with open(os.path.join(pending, stamp(i) + ".json"), "w",
                  encoding="utf-8") as f:
            json.dump(job, f, ensure_ascii=False, indent=1)
    proc = subprocess.run([EXE, "--memory-worker", luban],
                          capture_output=True, text=True, timeout=120)
    if proc.returncode != 0:
        raise RuntimeError(f"memory-worker 失败: {proc.stdout} {proc.stderr}")
    catalog = os.path.join(memory_dir, ".state", "catalog.json")
    n_topics = len(conv["sessions"])
    if not os.path.exists(catalog):
        raise RuntimeError(f"{cid}: catalog 未生成,灌入失败")
    return ws_dir, n_topics


# ---------------------------------------------------------------- 答题

def build_prompt(question: str, choices: list) -> str:
    lines = [question, "", "Options:"]
    for i, c in enumerate(choices):
        lines.append(f"{i}) {c}")
    lines.append("")
    lines.append("Reply with ONLY the option number (a single digit "
                 f"0-{len(choices) - 1}). No other text.")
    return "\n".join(lines)


def list_session_dirs(home: str) -> set:
    out = set()
    ws_root = os.path.join(home, ".lubancode", "workspaces")
    if not os.path.isdir(ws_root):
        return out
    for w in os.listdir(ws_root):
        sdir = os.path.join(ws_root, w, "sessions")
        if not os.path.isdir(sdir):
            continue
        for d in os.listdir(sdir):
            if os.path.isfile(os.path.join(sdir, d, "main.jsonl")):
                out.add(os.path.join(sdir, d))
    return out


def last_assistant_text(main_jsonl: str) -> str:
    text = ""
    with open(main_jsonl, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                evt = json.loads(line)
            except json.JSONDecodeError:
                continue
            if evt.get("kind") != "model.response.completed":
                continue
            payload = evt.get("payload") or {}
            msg = payload.get("message") or payload.get("assistant_message") or {}
            blocks = msg.get("content") or []
            parts = [b.get("text", "") for b in blocks
                     if isinstance(b, dict) and b.get("type") == "text"]
            joined = "".join(parts).strip()
            if joined:
                text = joined
    return text


def extract_choice(text: str, n_choices: int) -> int:
    m = re.search(r"\b(?:ANSWER|answer)\s*[:=]?\s*(\d+)\b", text)
    if not m:
        digits = re.findall(r"\b(\d{1,2})\b", text)
        for d in digits:
            if 0 <= int(d) < n_choices:
                m = type("M", (), {"group": lambda self, x=None: d})()
                break
    if m:
        v = int(m.group(1))
        if 0 <= v < n_choices:
            return v
    return -1


def ask_one(mode: str, home: str, ws_dir: str, question: str, choices: list) -> dict:
    env = dict(os.environ)
    env["USERPROFILE"] = home
    env["PYTHONIOENCODING"] = "utf-8"
    before = list_session_dirs(home)
    prompt = build_prompt(question, choices)
    t0 = time.time()
    failure = ""
    out_text = ""
    try:
        proc = subprocess.run([EXE, "--yes", prompt], cwd=ws_dir, env=env,
                              capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=TASK_TIMEOUT_SECS)
        if proc.returncode != 0:
            failure = f"exit={proc.returncode}: {(proc.stdout + proc.stderr)[-200:]}"
    except subprocess.TimeoutExpired:
        failure = "timeout"
    wall = time.time() - t0
    new = sorted(list_session_dirs(home) - before)
    main_jsonl = os.path.join(new[-1], "main.jsonl") if new else ""
    if main_jsonl and not failure:
        out_text = last_assistant_text(main_jsonl)
    if not out_text and not failure:
        out_text = proc.stdout or ""
    choice = extract_choice(out_text, len(choices)) if out_text else -1
    return {"raw_tail": (out_text or "")[-160:], "choice": choice,
            "failure": failure, "wall": round(wall, 1)}


# ---------------------------------------------------------------- 主流程

def sample_questions(convs: list, per_cat: int, seed: int, categories=None) -> list:
    rng = random.Random(seed)
    cats = tuple(categories) if categories else CATEGORIES
    picked = []
    for conv in convs:
        by_cat = {}
        for q in conv["questions"]:
            by_cat.setdefault(q["category"], []).append(q)
        for cat in cats:
            pool = by_cat.get(cat, [])
            for q in rng.sample(pool, min(per_cat, len(pool))):
                picked.append((conv["conv_id"], q))
    return picked


def per_mode_path(mode: str, suffix: str = "") -> str:
    return os.path.join(HERE_EVAL, f"e2_per_question_{mode}{suffix}.json")


def run_batch(modes, convs, tasks, answers, suffix="", max_attempts=0):
    """逐态跑批,每态独立账文件——A/B 两组 home 互相独立,可两个进程分别
    --modes A / --modes B 并行,互不写同一份文件(汇总时合并)。max_attempts
    >0 时,账里已失败这么多次的 qid 不再重试(留最后一笔失败账进文件),
    防断线续跑在 hopeless 超时题上无限烧钟。"""
    for mode in modes:
        home = build_home(mode)
        for conv in convs:
            ws_dir, n = ingest_conv(home, conv)
            print(f"[{mode}] {conv['conv_id']}: 灌入 {n} topics -> {ws_dir}", flush=True)
        account_path = per_mode_path(mode, suffix)
        results = []
        done = set()
        attempts = {}
        if os.path.exists(account_path):
            for r in json.load(open(account_path, encoding="utf-8")):
                # 熔断只数 timeout(裸底空推理 420s 不封顶,重试也没救);
                # 上游 502/连接抖动是暂态病,不计数,续跑时该重试还重试。
                if r.get("failure") == "timeout":
                    attempts[r["qid"]] = attempts.get(r["qid"], 0) + 1
                results.append(r)  # 失败行也留账:熔断计数与失败明细都靠它
                if not r.get("failure") and r.get("choice", -1) >= 0:
                    done.add(r["qid"])
        for cid, q in tasks:
            if q["qid"] in done:
                continue
            if max_attempts and attempts.get(q["qid"], 0) >= max_attempts:
                print(f"[{mode}] {q['qid']} 已失败 {attempts[q['qid']]} 次,熔断不再重试",
                      flush=True)
                continue
            r = ask_one(mode, home, ws_dir_of(cid), q["question"], q["choices"])
            correct = answers[q["qid"]]
            choice_text = q["choices"][r["choice"]] if 0 <= r["choice"] < len(q["choices"]) else ""
            r.update({"mode": mode, "qid": q["qid"], "category": q["category"],
                      "conv": cid, "correct": correct,
                      "choice_text": choice_text,
                      "hit": 1 if r["choice"] == correct else 0})
            results.append(r)
            with open(account_path, "w", encoding="utf-8") as f:
                json.dump(results, f, ensure_ascii=False, indent=1)
            print(f"[{mode}] {q['qid']} [{q['category']}] "
                  f"choice={r['choice']} correct={correct} "
                  f"{'HIT' if r['hit'] else 'MISS'} wall={r['wall']}s"
                  + (f" failure={r['failure']}" if r["failure"] else ""), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--convs", default="conv-26,conv-41")
    ap.add_argument("--per-category", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20260903)
    ap.add_argument("--modes", default="A,B", help="只跑指定态(如 A),A/B 可两进程并行")
    ap.add_argument("--smoke", action="store_true", help="每类 1 题,验管线")
    ap.add_argument("--report-only", action="store_true",
                    help="不跑题,只按已有逐题账产报告")
    ap.add_argument("--limit", type=int, default=0, help="只跑前 N 题(冒烟用)")
    ap.add_argument("--categories", default="",
                    help="只跑指定类别(逗号分隔,如 temporal_reasoning);空=五类全跑")
    ap.add_argument("--suffix", default="",
                    help="账与报告文件名后缀(如 _t1):不同批次/不同构建的账分开落盘,"
                         "互不覆盖、可分别复算")
    ap.add_argument("--max-attempts", type=int, default=0,
                    help=">0 时同一 qid 失败这么多次后熔断,不再重试(防断线续跑"
                         "在 hopeless 超时题上无限烧钟)")
    ap.add_argument("--qids", default="",
                    help="只跑这些 qid(逗号分隔,如 conv-49_q1,conv-49_q2);"
                         "抽样仍按 --convs/--seed 原口径定题集后再过滤——"
                         "单场续跑必须保持五场列表的 rng 顺序,不能只给单场")
    args = ap.parse_args()
    per_cat = 1 if args.smoke else args.per_category
    run_modes = tuple(m.strip() for m in args.modes.split(",") if m.strip())
    cats = [c.strip() for c in args.categories.split(",") if c.strip()] or None
    suffix = args.suffix if args.suffix == "" or args.suffix.startswith("_") \
        else "_" + args.suffix

    convs_all = {}
    with open(os.path.join(HERE_EVAL, "perturbed.jsonl"), encoding="utf-8") as f:
        for line in f:
            rec = json.loads(line)
            convs_all[rec["conv_id"]] = rec
    answers = json.load(open(os.path.join(HERE_EVAL, "answers.json"), encoding="utf-8"))
    want = [c.strip() for c in args.convs.split(",") if c.strip()]
    convs = [convs_all[c] for c in want]

    tasks = sample_questions(convs, per_cat, args.seed, cats)
    if args.limit:
        tasks = tasks[:args.limit]
    if args.qids:
        allow = {q.strip() for q in args.qids.split(",") if q.strip()}
        tasks = [t for t in tasks if t[1]["qid"] in allow]
        print(f"qid 白名单过滤: 剩 {len(tasks)} 题")
    print(f"题集: {len(tasks)} 题(每类 {per_cat},类别 {cats or '全部'}),本次态 {run_modes}")

    if not args.report_only:
        run_batch(run_modes, convs, tasks, answers, suffix, args.max_attempts)

    # 合并各态账
    results = []
    for mode in MODES:
        p = per_mode_path(mode, suffix)
        if os.path.exists(p):
            results.extend(json.load(open(p, encoding="utf-8")))
    merged = os.path.join(HERE_EVAL, f"e2_per_question{suffix}.json")
    with open(merged, "w", encoding="utf-8") as f:
        json.dump(results, f, ensure_ascii=False, indent=1)
    print(f"逐题账(合并): {merged}")

    # ---- 汇总 ----
    lines = ["# LoCoMo-MC10 端到端配对(E2 抽样首批)", ""]
    lines.append(f"题集: {want} x 五类各 {per_cat} = {len(tasks)} 题,双态; "
                 f"模型 {PROVIDER_NAME}(温度 {TEMPERATURE},推理档 "
                 f"{REASONING_EFFORT}——日常 high 会让 A 组空推理 420s+ 不封顶,"
                 "两组对称降档,口径可复现); A=memory off,B=memory on。"
                 "判分对扰动后 correct_choice_index(扰动不改位次)。")
    lines.append("")
    lines.append("| 类别 | n(A/B) | A 准确 | B 准确 | B-A | A 选Not answerable | B 选Not answerable |")
    lines.append("|---|---|---|---|---|---|---|")

    def acc(rs):
        return sum(r["hit"] for r in rs) / len(rs) if rs else 0.0

    def na(rs):
        return sum(1 for r in rs if "not answerable" in
                   (r.get("choice_text") or "").lower()) / len(rs) if rs else 0.0

    for cat in CATEGORIES:
        a = [r for r in results if r["category"] == cat and r["mode"] == "A"]
        b = [r for r in results if r["category"] == cat and r["mode"] == "B"]
        if not a and not b:
            continue
        delta = f"{acc(b) - acc(a):+.3f}" if (a and b) else "-"
        lines.append(f"| {cat} | {len(a)}/{len(b)} | "
                     f"{acc(a):.3f}{'*' * (not a)} | {acc(b):.3f}{'*' * (not b)} | "
                     f"{delta} | {na(a):.3f}{'*' * (not a)} | {na(b):.3f}{'*' * (not b)} |")
    lines.append("")
    lines.append("(* = 该态此桶暂无完成账,数字为空桶占位,不可引用)")
    all_a = [r for r in results if r["mode"] == "A"]
    all_b = [r for r in results if r["mode"] == "B"]
    delta_all = f"{acc(all_b) - acc(all_a):+.3f}" if (all_a and all_b) else "-"
    lines.append(f"| **合计** | {len(all_a)}/{len(all_b)} | {acc(all_a):.3f} | "
                 f"{acc(all_b):.3f} | {delta_all} | - | - |")
    lines.append("")
    fails = [r for r in results if r.get("failure")]
    unanswered = [r for r in results if r.get("choice", -1) < 0 and not r.get("failure")]
    if fails or unanswered:
        lines.append(f"失败调用 {len(fails)} 次、判不出选项 {len(unanswered)} 次"
                     "(逐题账有明细,未计入分母的按 MISS 计)")
    lines.append("")
    report = os.path.join(HERE_EVAL, f"report{suffix}.md")
    with open(report, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    print(f"\nwritten: {report}")


def ws_dir_of(cid: str) -> str:
    return os.path.join(WS_ROOT, "ws-" + cid)


if __name__ == "__main__":
    sys.exit(main())
