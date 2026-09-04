#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从真评测 session 产 LoCoMo 判分器回归夹具(评测纠偏单 P1)。

夹具的来历(单子 §十三 P1 第 4 条:"用现存错例做回归夹具,覆盖 9 个
B mismatch 与'不知道'样本形状"):
  - 248 个 B 态 session 与 40 个 A 态 trajectory 是只读输入,一份不改;
  - 本脚本把其中 20 份错例/边角样本复制成**脱敏夹具**落进 fixtures/real/,
    信封字段(schema/kind/seq/event_id/run_id/turn_id/hash/时间戳)逐字
    保留、可溯源;载荷里的大段题面/工具正文替换成合成文字——.gitignore
    对 eval/locomo 的约定是"进 git 的只有聚合报告(无原文无答案)",夹具
    不能破这条;
  - 最终 end_turn 文本块(判分对象)一字不动;
  - 工具轮正文替换成**数字密集的合成汤**(行号/Token/秒数形状),保住
    "前置输出塞满数字也改不了末答"这条要验的性质;
  - 合成形状(双 end_turn/越界/v1/v3/缺版本/混版/异 schema/截断行/终端
    杂字)没有真实样本,按 schema v2 信封手造,标 synthetic。

qid↔session 与旧账 choice 不手抄:构建时从 input.received 题面前缀对
perturbed.jsonl 定 qid,从 e2_per_question_B_r1.json 定旧账,再与本文件
烤死的审计表(2026-09-05 排查结果)核对——对不上即拒绝出夹具。

原始账不在本机时,--synth 只(重)建合成件;--real 需
eval/locomo/_run(永不进 git)。

用法:
  python make_fixtures.py --real --out fixtures   # 有原始账时全量重建
  python make_fixtures.py --synth --out fixtures  # 只重建合成件
"""

import argparse
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
EVAL_DIR = os.path.join(REPO, "eval", "locomo")
HOME_B = os.path.join(EVAL_DIR, "_run", "home_B")

# ---------------------------------------------------------------- 烤死审计表
# 2026-09-05 对 248 个 B 态 session 逐场读最后 end_turn、对旧账
# e2_per_question_B_r1.json 的排查结果(单子 §3.2 的实锤明细)。
# qid -> (真末答, 旧账 choice)。旧账全是从终端杂字里捞的数。
MISMATCH_AUDIT = {  # 模型答了数字,账记成另一个数
    "conv-41_q10":  ("4", 0),
    "conv-41_q122": ("6", 5),
    "conv-43_q22":  ("2", 0),
    "conv-43_q68":  ("9", 0),
    "conv-43_q95":  ("4", 0),
    "conv-47_q37":  ("5", 8),
    "conv-47_q63":  ("1", 6),
    "conv-49_q12":  ("4", 2),
    "conv-49_q57":  ("4", 0),
}
DUNNO_AUDIT = {  # 模型答"不知道",旧账照样捞出合法选项
    "conv-41_q45": ("不知道", 1),
    "conv-41_q72": ("不知道", 1),
    "conv-41_q93": ("不知道", 1),
    "conv-43_q50": ("不知道", 0),
    "conv-43_q94": ("不知道", 1),
    "conv-47_q17": ("不知道", 1),
    "conv-47_q31": ("不知道", 1),
    "conv-47_q5":  ("不知道", 1),
    "conv-49_q70": ("不知道", 1),
}
# 边角样本:13 轮工具密集场(mismatch conv-43_q22 同源)与 7 轮 tool_use
# 后超时、从未到 end_turn 的场(conv-47_q7,旧账 timeout 行 choice=-1)。
TOOL_DENSE_SESSION = "20260904-094148-5AGLZE"
NO_END_TURN_SESSION = "20260904-095236-83DGB9"
NO_END_TURN_EXPECTED = ("conv-47_q7", -1)  # (qid, 旧账 choice)

# ---------------------------------------------------------------- 脱敏

_SYNTH_PROMPT = (
    "SANITIZED question text (fixture; original wording kept out of git)"
    "\n\nOptions:\n" + "\n".join(f"{i}) Option {i} (sanitized)" for i in range(10))
    + "\n\nReply with ONLY the option number (a single digit 0-9)."
    " No other text.\n")

_SOUP = ("  12| read 345 lines, 67 matches at row 890\n"
         "  tokens: in 1234 out 567 cache 8901 elapsed 23.45s\n"
         "  file.cs:987 col 65 tool call 3 of 13 batch 4 attempt 2\n")


def _digit_soup(length):
    """合成数字密集汤:行号/Token/耗时形状,长度对齐原文(封顶 2KB)。"""
    out, n = [], 0
    total = min(length, 2000)
    while sum(len(s) for s in out) < total:
        out.append(f"[sanitized tool output {n:03d}] {_SOUP}")
        n += 1
    return "".join(out)[:total]


# 工具参数里可能裹着题面原文的键(search 的 pattern、子代理的 prompt…);
# path/glob/mode/limit 等操作形状保留(越界路径本身是要留证的形状)。
_SENSITIVE_ARG_KEYS = {"pattern", "query", "prompt", "content", "text",
                       "question", "command", "description"}


def _scrub_args(holder):
    if not isinstance(holder, dict):
        return
    for k, v in list(holder.items()):
        if isinstance(v, str) and (k in _SENSITIVE_ARG_KEYS or len(v) > 60):
            holder[k] = "sanitized-" + str(k)
        elif isinstance(v, dict):
            _scrub_args(v)


def sanitize_event(evt, is_final_end_turn):
    """就地脱敏一枚事件的载荷。信封(含 hash/ids/时间戳)不动。"""
    kind = evt.get("kind")
    payload = evt.get("payload")
    if not isinstance(payload, dict):
        return evt
    if kind == "input.received":
        for b in payload.get("content") or []:
            if isinstance(b, dict) and b.get("type") == "text":
                b["text"] = _SYNTH_PROMPT
    elif kind == "model.request.prepared":
        payload["request_snapshot_ref"] = {"sanitized": True,
                                           "reason": "locomo fixture"}
        payload.pop("request_snapshot_sha256", None)
        payload.pop("system_ref", None)
    elif kind == "model.output.completed":
        if is_final_end_turn:
            return evt  # 判分对象,一字不动
        for b in payload.get("blocks") or []:
            if not isinstance(b, dict):
                continue
            if b.get("type") == "text":
                b["text"] = ("[sanitized assistant text; mentions rows 0-9, "
                             "counts 17/42/256 tokens before the next tool "
                             "call] " * 2)
            elif b.get("type") in ("tool_use", "tool_call"):
                _scrub_args(b.get("input"))
                _scrub_args(b.get("arguments"))
    elif kind == "tool.input.effective":
        _scrub_args(payload.get("effective_arguments"))
        for k in ("rewritten_by", "source_instance"):
            payload.pop(k, None)
    elif kind == "tool.result.committed":
        for b in payload.get("content") or []:
            if isinstance(b, dict) and b.get("type") == "text":
                b["text"] = _digit_soup(len(b.get("text") or ""))
        ref = payload.get("result_ref")
        if isinstance(ref, dict) and ref.get("kind") == "inline":
            ref["content"] = "[sanitized]"
    return evt


def final_end_turn_text(parsed_events):
    """与判分器同口径:单 run 单 turn 的最后一条 end_turn 文本。"""
    outputs = [e for e in parsed_events
               if e.get("kind") == "model.output.completed"]
    runs = {e.get("run_id") for e in outputs}
    if len(runs) != 1:
        return None, None
    run_id = next(iter(runs))
    pinned = [e for e in outputs if e.get("run_id") == run_id]
    end_turns = [e for e in pinned
                 if (e.get("payload") or {}).get("stop_reason") == "end_turn"]
    if len(end_turns) != 1:
        return None, None
    evt = end_turns[-1]
    blocks = (evt.get("payload") or {}).get("blocks") or []
    text = "".join(b.get("text", "") for b in blocks
                   if isinstance(b, dict) and b.get("type") == "text").strip()
    return text, evt.get("seq")


def build_real_fixture(src_path, out_path):
    parsed = [json.loads(l) for l in open(src_path, encoding="utf-8")
              if l.strip()]
    text, final_seq = final_end_turn_text(parsed)
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        for evt in parsed:
            f.write(json.dumps(sanitize_event(evt, evt.get("seq") == final_seq),
                               ensure_ascii=False) + "\n")
    sha = hashlib.sha256(open(src_path, "rb").read()).hexdigest()
    return text, sha


def session_map():
    """qid -> (main.jsonl 路径, session_id);按 input.received 题面前缀对题。"""
    questions = {}
    with open(os.path.join(EVAL_DIR, "perturbed.jsonl"), encoding="utf-8") as f:
        for line in f:
            rec = json.loads(line)
            for q in rec["questions"]:
                questions[q["qid"]] = q["question"][:60]
    out = {}
    for dp, _dirs, files in os.walk(HOME_B):
        if "main.jsonl" not in files:
            continue
        fp = os.path.join(dp, "main.jsonl")
        qtext = None
        for line in open(fp, encoding="utf-8"):
            line = line.strip()
            if not line:
                continue
            evt = json.loads(line)
            if evt.get("kind") == "input.received":
                for b in (evt.get("payload") or {}).get("content") or []:
                    if isinstance(b, dict) and b.get("type") == "text":
                        qtext = b.get("text", "")
                        break
                break
        if not qtext:
            continue
        for qid, prefix in questions.items():
            if qtext.startswith(prefix):
                out[qid] = (fp, os.path.basename(dp))
                break
    return out


def old_account():
    path = os.path.join(EVAL_DIR, "e2_per_question_B_r1.json")
    return {r["qid"]: r for r in json.load(open(path, encoding="utf-8"))}


def find_session(session_id):
    for dp, _dirs, files in os.walk(HOME_B):
        if os.path.basename(dp) == session_id and "main.jsonl" in files:
            return os.path.join(dp, "main.jsonl")
    return None


# ---------------------------------------------------------------- 合成形状

def _envelope(seq, kind, payload, schema_version=2, run_id="main-0001",
              turn_id="turn-1", schema="lubancode.trajectory.event"):
    return {"actor": "model", "event_hash": f"synthetic-{seq:040x}",
            "event_id": f"{run_id}:evt-{seq:08d}", "kind": kind,
            "monotonic_ns": 1_000_000_000_000 + seq, "origin": "provider_model",
            "payload": payload, "plane": "conversation",
            "prev_hash": f"synthetic-{max(seq - 1, 0):040x}",
            "run_id": run_id, "run_kind": "one_shot", "schema": schema,
            "schema_version": schema_version, "seq": seq,
            "session_id": "SYNTHETIC-FIXTURE", "training_policy": "exclude",
            "turn_id": turn_id, "visibility": ["host_only"],
            "wall_time_ms": 1788514260000 + seq}


def _output(text, stop="end_turn", seq=8, **kw):
    blocks = [] if text is None else [{"type": "text", "text": text}]
    return _envelope(seq, "model.output.completed",
                     {"blocks": blocks, "output_id": "output-1",
                      "stop_reason": stop}, **kw)


def _write(path, events):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for e in events:
            f.write(json.dumps(e, ensure_ascii=False) + "\n")


def build_synthetic(synth_dir):
    entries = []

    def add(fname, events, expected, note):
        _write(os.path.join(synth_dir, fname), events)
        entries.append({"file": f"synth/{fname}", "shape": note,
                        "expected": expected})

    run_start = _envelope(1, "run.started",
                          {"min_reader_version": 2, "run_kind": "one_shot",
                           "start_reason": "process_launch",
                           "writer_version": "trajectory-recorder-v1"},
                          turn_id=None)
    turn_start = _envelope(3, "turn.started", {"trigger": "user"})

    add("no_end_turn.jsonl",
        [run_start, turn_start,
         _output("checking rows 3 and 7", "tool_use", 8),
         _output("still searching, 41 hits", "tool_use", 17)],
        {"n_choices": 10, "parse_status": "invalid",
         "failure_class": "no_end_turn"},
        "两轮 tool_use,从未 end_turn(§5.2'工具输出后没有最终 end_turn')")
    add("two_end_turns.jsonl",
        [run_start, turn_start, _output("3", seq=8), _output("5", seq=17)],
        {"n_choices": 10, "parse_status": "invalid",
         "failure_class": "ambiguous_final_output"},
        "两条 end_turn(3 与 5),必须记 ambiguous,不擅自挑")
    add("out_of_range_mc5.jsonl",
        [run_start, turn_start, _output("7", seq=8)],
        {"n_choices": 5, "parse_status": "invalid",
         "failure_class": "out_of_range"},
        "单数字 7 但 n_choices=5,越界")
    add("v1_legacy.jsonl",
        [_envelope(1, "run.started", {"run_kind": "one_shot"},
                   schema_version=1, turn_id=None),
         _envelope(3, "turn.started", {"trigger": "user"}, schema_version=1),
         _output("4", seq=8, schema_version=1)],
        {"n_choices": 10, "parse_status": "invalid",
         "failure_class": "legacy_schema_v1"},
        "整本 schema_version=1 的 legacy 账:认得出,不判分(fail closed)")
    add("v3_unknown.jsonl",
        [_envelope(1, "run.started", {"run_kind": "one_shot"},
                   schema_version=3, turn_id=None),
         _envelope(3, "turn.started", {"trigger": "user"}, schema_version=3),
         _output("4", seq=8, schema_version=3)],
        {"n_choices": 10, "parse_status": "invalid",
         "failure_class": "unknown_schema_version"},
        "整本 schema_version=3 超前,版本不明 fail closed")
    missing = _output("4", seq=8)
    missing.pop("schema_version")
    add("missing_version.jsonl", [run_start, turn_start, missing],
        {"n_choices": 10, "parse_status": "invalid",
         "failure_class": "unknown_schema_version"},
        "信封缺 schema_version,fail closed")
    add("mixed_versions.jsonl",
        [run_start, turn_start, _output("4", seq=8),
         _output("9", seq=17, schema_version=1)],
        {"n_choices": 10, "parse_status": "invalid",
         "failure_class": "mixed_schema_version"},
        "v2 与 v1 混在同一 stream,fail closed")
    add("wrong_schema_name.jsonl",
        [run_start, turn_start,
         _output("4", seq=8, schema="some.other.stream")],
        {"n_choices": 10, "parse_status": "invalid",
         "failure_class": "schema_mismatch"},
        "schema 名不是 lubancode.trajectory.event")
    good = [run_start, turn_start, _output("4", seq=8)]
    path = os.path.join(synth_dir, "truncated_last_line.jsonl")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for e in good:
            f.write(json.dumps(e, ensure_ascii=False) + "\n")
        f.write(json.dumps(_output("9", seq=17), ensure_ascii=False)[:120])
    entries.append({
        "file": "synth/truncated_last_line.jsonl",
        "shape": "末行截断(半行 JSON)——完整可判事件只有 seq=8 的 4,"
                 "malformed_lines=1 只作诊断;以实际可判事件为准",
        "expected": {"n_choices": 10, "parse_status": "ok",
                     "parsed_choice": 4, "malformed_lines": 1}})

    soup = ("-- Worked for 26s ----------\n"
            "read 345 lines, 67 matches, row 890\n"
            "tokens: in 1234 out 567, 291 requests, 2.58M cache\n"
            "exit 0 after 420s, progress 7/248\n") * 4
    with open(os.path.join(synth_dir, "stdout_digit_soup.txt"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(soup)
    entries.append({
        "file": "synth/stdout_digit_soup.txt",
        "shape": "终端杂字样本(行号/Token/秒数/进度,几十枚数字)。测试断言"
                 "判分器 API 无 stdout 入口,该汤在时末答仍按 trajectory 记",
        "expected": None})
    return entries


# ---------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--real", action="store_true", help="重建真源夹具(需原始账)")
    ap.add_argument("--synth", action="store_true", help="重建合成件")
    ap.add_argument("--out", default="fixtures")
    args = ap.parse_args()
    if not (args.real or args.synth):
        args.real = args.synth = True
    out = os.path.join(HERE, args.out)
    real_dir = os.path.join(out, "real")
    synth_dir = os.path.join(out, "synth")
    os.makedirs(real_dir, exist_ok=True)
    os.makedirs(synth_dir, exist_ok=True)
    manifest_path = os.path.join(out, "manifest.json")

    manifest = {
        "note": "LoCoMo 判分器固定集(单子 §5.3/§十三 P1)。real/=真 session"
                " 脱敏件(信封逐字保留可溯源,载荷去题面/正文,末轮 end_turn "
                "一字不动);synth/=手造形状。expected.n_choices 覆盖默认 10。",
        "real": [], "synth": []}
    # 增量重建:--synth 单独跑时保留已有 real 段(反之亦然)。
    if os.path.isfile(manifest_path):
        try:
            prev = json.load(open(manifest_path, encoding="utf-8"))
            if not args.real:
                manifest["real"] = prev.get("real", [])
            if not args.synth:
                manifest["synth"] = prev.get("synth", [])
        except (json.JSONDecodeError, OSError):
            pass

    if args.synth:
        manifest["synth"] = build_synthetic(synth_dir)
        print(f"synth fixtures -> {synth_dir}")

    if args.real:
        smap = session_map()
        acct = old_account()
        picked = {}
        for qid, (expected_text, old_choice) in {**MISMATCH_AUDIT,
                                                 **DUNNO_AUDIT}.items():
            if qid not in smap:
                sys.exit(f"原始账里找不到 {qid} 的 session——审计表过期?")
            fp, sid = smap[qid]
            recorded = acct.get(qid, {}).get("choice")
            if recorded != old_choice:
                sys.exit(f"{qid} 旧账 choice={recorded},审计表记 {old_choice}"
                         "——对不上,拒绝出夹具")
            picked[qid] = (fp, sid, expected_text, old_choice)
        # 边角:工具密集场(在 mismatch 里)+ no_end_turn 场
        if MISMATCH_AUDIT["conv-43_q22"][0] != "2":
            sys.exit("conv-43_q22 审计表被改过,工具密集场校验失效")
        for qid, (fp, sid, expected_text, old_choice) in sorted(picked.items()):
            tag = "dunno" if expected_text == "不知道" else f"final{expected_text}"
            fname = f"b_{qid}_{tag}_old{old_choice}_{sid}.jsonl"
            final_text, src_sha = build_real_fixture(fp, os.path.join(real_dir, fname))
            if final_text != expected_text:
                sys.exit(f"{qid} 真末答 {final_text!r},审计表记 "
                         f"{expected_text!r}——对不上,拒绝出夹具")
            origin = {"session_id": sid, "qid": qid,
                      "source_main_jsonl_sha256": src_sha,
                      "old_runner_recorded_choice": old_choice}
            if sid == TOOL_DENSE_SESSION:
                origin["shape"] = ("13 轮工具往返,工具结果(脱敏后仍是数字"
                                   "密集汤)里几百枚数字;末答只有最后一条 "
                                   "end_turn 的 2")
            manifest["real"].append({
                "file": f"real/{fname}",
                "origin": origin,
                "expected": {"n_choices": 10, "parse_status": "ok"
                             if expected_text != "不知道" else "invalid",
                             **({"parsed_choice": int(expected_text)}
                                if expected_text != "不知道"
                                else {"failure_class": "refusal_unknown"})}})
        # no_end_turn 场(独立 session,qid=conv-47_q7,旧账为 timeout 行)
        no_end_fp = find_session(NO_END_TURN_SESSION)
        if not no_end_fp:
            sys.exit(f"找不到 no_end_turn 场 {NO_END_TURN_SESSION}")
        fname = f"b_no_end_turn_{NO_END_TURN_SESSION}.jsonl"
        final_text, src_sha = build_real_fixture(no_end_fp,
                                                 os.path.join(real_dir, fname))
        if final_text is not None:
            sys.exit("no_end_turn 场居然有末答——审计表过期")
        manifest["real"].append({
            "file": f"real/{fname}",
            "origin": {"session_id": NO_END_TURN_SESSION,
                       "qid": NO_END_TURN_EXPECTED[0],
                       "source_main_jsonl_sha256": src_sha,
                       "shape": "7 轮 tool_use(含 Explore 子代理)后超时,"
                                "从未出现 end_turn;旧账记 timeout(choice=-1)",
                       "old_runner_recorded_choice": NO_END_TURN_EXPECTED[1]},
            "expected": {"n_choices": 10, "parse_status": "invalid",
                         "failure_class": "no_end_turn"}})
        print(f"real fixtures -> {real_dir} ({len(manifest['real'])} 件)")

    with open(manifest_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)
    print(f"manifest -> {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
