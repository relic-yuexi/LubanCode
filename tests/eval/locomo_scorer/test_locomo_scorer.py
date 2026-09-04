#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoCoMo 判分器固定集回归(评测纠偏单 §5.3/§十三 P1,离线零模型零钥匙)。

三层断言:
  1. 严格解析 §5.2:单枚 ASCII 数字之外的形状全 invalid,分型记账;
  2. manifest 夹具:9 个 B mismatch + 9 个"不知道" + no_end_turn 工具场
     + 合成形状(双 end_turn/越界/v1/v3/缺版本/混版/异 schema/截断行),
     逐件对 parse_status/parsed_choice/failure_class;
  3. 结构保证:判分器 API 无 stdout 入口;工具结果几百枚数字改不了
     末答;§5.1 结果账字段齐。

用法(独立跑):
  python test_locomo_scorer.py [--scorer <eval_locomo_score.py>] [--fixtures <dir>]
ctest 注册见 tests/eval/CMakeLists.txt 的 eval.locomo_scorer.fixtures。
"""

import argparse
import importlib.util
import inspect
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DEFAULT_SCORER = os.path.join(REPO, "scripts", "eval_locomo_score.py")

failures = []


def check(name, cond, detail=""):
    if cond:
        return True
    failures.append(f"{name}: {detail}")
    return False


def load_scorer(path):
    spec = importlib.util.spec_from_file_location("eval_locomo_score", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def digit_count_in_tool_results(path):
    n = 0
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        evt = json.loads(line)
        if evt.get("kind") == "tool.result.committed":
            n += len(re.findall(r"\d", json.dumps(evt.get("payload") or {})))
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scorer", default=DEFAULT_SCORER)
    ap.add_argument("--fixtures", default=os.path.join(HERE, "fixtures"))
    args = ap.parse_args()
    s = load_scorer(args.scorer)
    fx = args.fixtures
    manifest = json.load(open(os.path.join(fx, "manifest.json"),
                              encoding="utf-8"))

    # ---- 0) 判分器自检(模块内联,同一份断言) ----
    try:
        s._self_check()
        check("scorer.self_check", True)
    except AssertionError as ex:
        check("scorer.self_check", False, repr(ex))

    # ---- 1) §5.2 严格解析(内联用例,超出夹具覆盖的词面变体) ----
    ok, invalid = s.PARSE_OK, s.PARSE_INVALID
    for text, want in [("7", (7, ok, "")), (" 4\n", (4, ok, "")),
                       ("0", (0, ok, "")), ("9", (9, ok, ""))]:
        check(f"parse[{text!r}]", s.parse_choice(text, 10) == want,
              f"got {s.parse_choice(text, 10)}")
    for text, cls in [("答案是 4", "invalid_format"), ("4.", "invalid_format"),
                      ("Option 4", "invalid_format"), ("12", "invalid_format"),
                      ("４", "invalid_format"), ("4 2", "invalid_format"),
                      ("ANSWER: 4", "invalid_format"),
                      ("", "empty_final_output"), ("  \n ", "empty_final_output"),
                      ("不知道", "refusal_unknown"),
                      ("I don't know.", "refusal_unknown"),
                      ("9", "out_of_range")]:
        got = s.parse_choice(text, 5 if text == "9" else 10)
        want = (None, invalid, cls)
        check(f"parse[{text!r}]", got == want, f"got {got}")

    # ---- 2) 结构保证:判分器没有吃 stdout/stderr 的入口 ----
    for fname, fn in inspect.getmembers(s, inspect.isfunction):
        params = list(inspect.signature(fn).parameters)
        check(f"no-stdout-param[{fname}]",
              not any(p in ("stdout", "stderr", "proc", "process_output",
                            "terminal_output") for p in params),
              f"params={params}")
    soup = open(os.path.join(fx, "synth", "stdout_digit_soup.txt"),
                encoding="utf-8").read()
    check("stdout-soup-has-digits", len(re.findall(r"\b\d{1,4}\b", soup)) >= 40,
          "终端杂字样本本身要有几十枚数字")

    # ---- 3) manifest 夹具逐件判 ----
    fields_51 = ["session_id", "main_jsonl_relpath", "main_jsonl_sha256",
                 "event_id", "run_id", "turn_id", "request_id", "output_id",
                 "raw_final_text", "parsed_choice", "parse_status",
                 "failure_class"]
    checked_ok_fixture = False
    for group in ("real", "synth"):
        for entry in manifest.get(group, []):
            exp = entry.get("expected")
            if not exp:
                continue
            path = os.path.join(fx, entry["file"])
            r = s.score_session_file(path, n_choices=exp.get("n_choices", 10))
            label = entry["file"]
            check(f"[{label}] parse_status",
                  r["parse_status"] == exp["parse_status"],
                  f"got {r['parse_status']} want {exp['parse_status']}"
                  f" (failure_class={r['failure_class']})")
            if "parsed_choice" in exp:
                check(f"[{label}] parsed_choice",
                      r["parsed_choice"] == exp["parsed_choice"],
                      f"got {r['parsed_choice']} want {exp['parsed_choice']}")
            if "failure_class" in exp:
                check(f"[{label}] failure_class",
                      r["failure_class"] == exp["failure_class"],
                      f"got {r['failure_class']} want {exp['failure_class']}")
            if "malformed_lines" in exp:
                check(f"[{label}] malformed_lines",
                      r.get("malformed_lines") == exp["malformed_lines"],
                      f"got {r.get('malformed_lines')}")
            for field in fields_51:
                check(f"[{label}] field[{field}]", field in r,
                      "§5.1 结果账字段缺失")
            if r["parse_status"] == ok and not checked_ok_fixture:
                checked_ok_fixture = True
                for field in ("session_id", "main_jsonl_sha256", "event_id",
                              "run_id", "turn_id", "request_id", "output_id"):
                    check(f"[{label}] field[{field}] nonempty", bool(r[field]),
                          f"{field} 为空")
                check(f"[{label}] relpath sane",
                      r["main_jsonl_relpath"].endswith(".jsonl"))

    # ---- 4) 工具结果数字密度 + 末答不被数字汤带偏 ----
    dense = [e for e in manifest["real"]
             if "5AGLZE" in e["file"]] or [manifest["real"][0]]
    dense_path = os.path.join(fx, dense[0]["file"])
    n_digits = digit_count_in_tool_results(dense_path)
    check("tool-results-digit-dense", n_digits >= 300,
          f"工具结果里只有 {n_digits} 枚数字,不够'数百'")
    r = s.score_session_file(dense_path, n_choices=10)
    check("tool-dense-final-wins", r["parsed_choice"] == 2,
          f"13 轮工具输出 {n_digits} 枚数字后末答应记 2,"
          f"得 {r['parsed_choice']}")

    # ---- 5) 9 个 B mismatch 全数在账(具名化验收) ----
    mismatch_files = [e for e in manifest["real"]
                      if re.search(r"_final\d+_old\d+_", e["file"])]
    check("nine-b-mismatches", len(mismatch_files) == 9,
          f"B mismatch 夹具应 9 件,得 {len(mismatch_files)}")
    dunno_files = [e for e in manifest["real"] if "_dunno_" in e["file"]]
    check("nine-dunno-shapes", len(dunno_files) == 9,
          f"'不知道'形状夹具应 9 件,得 {len(dunno_files)}")

    # ---- 6) runner 装配级:假 subprocess 落真夹具 session,stdout 只当诊断 ----
    # 旧 runner 的病根在装配层(trajectory 读不到就回落 proc.stdout),光测
    # 判分器不够——这里把 subprocess.run 换成假件:落一份末答=4 的真夹具
    # session,同时往 stdout 塞满以 0/1 开头的数字汤。choice 必须是 4。
    import subprocess as _sp
    import tempfile
    spec = importlib.util.spec_from_file_location(
        "eval_locomo_runner", os.path.join(REPO, "scripts",
                                           "eval_locomo_runner.py"))
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    fixture_final4 = os.path.join(
        fx, "real", "b_conv-41_q10_final4_old0_20260904-093706-EWVVEE.jsonl")

    real_run = _sp.run

    state = {"n": 0}  # 各场景共用计数,session 目录不重名(重名会让
    #                    ask_one 的 before/after diff 落空,测不到想测的路)

    def fake_run_factory(scenario):
        def fake_run(cmd, cwd=None, env=None, capture_output=False, text=False,
                     encoding=None, errors=None, timeout=None):
            home = env["USERPROFILE"]
            ws_key = "ws-fixture-stub"
            state["n"] += 1
            sess = f"20260905-FIXTURE-{state['n']:03d}"
            sdir = os.path.join(home, ".lubancode", "workspaces", ws_key,
                                "sessions", sess)
            os.makedirs(sdir, exist_ok=True)
            with open(fixture_final4, encoding="utf-8") as src, \
                    open(os.path.join(sdir, "main.jsonl"), "w",
                         encoding="utf-8") as dst:
                dst.write(src.read())
            if scenario == "timeout":
                raise _sp.TimeoutExpired(cmd, timeout)
            code = 0 if scenario == "ok" else 3
            return _sp.CompletedProcess(cmd, code, stdout=soup, stderr="")
        return fake_run

    with tempfile.TemporaryDirectory() as td:
        home = os.path.join(td, "home_B")
        ws = os.path.join(td, "ws")
        os.makedirs(ws, exist_ok=True)
        # ok 路:末答 4,stdout 全是 0/1 汤——choice 必须 4,不是汤里的 0
        _sp.run = fake_run_factory("ok")
        try:
            r = runner.ask_one("B", home, ws, "fixture question",
                               [f"Option {i}" for i in range(10)])
        finally:
            _sp.run = real_run
        check("runner.ok-choice-from-trajectory", r["choice"] == 4,
              f"choice={r['choice']},被 stdout 汤带偏了")
        check("runner.ok-parse", r["parse_status"] == ok,
              f"parse_status={r['parse_status']}")
        check("runner.ok-session-id",
              r["session_id"] == "20260904-093706-EWVVEE",
              f"session_id={r['session_id']}(夹具事件里的真 session_id)")
        check("runner.ok-diag-tail-kept", r["diag_stdout_tail"] != "",
              "stdout 诊断尾巴应保留")
        check("runner.ok-failure-class-empty", r["failure_class"] == "",
              f"failure_class={r['failure_class']}")
        # timeout 路:进程层失败盖过 trajectory,不给记 choice
        _sp.run = fake_run_factory("timeout")
        try:
            runner.TASK_TIMEOUT_SECS_BACKUP = runner.TASK_TIMEOUT_SECS
            r = runner.ask_one("B", home, ws, "fixture question",
                               [f"Option {i}" for i in range(10)])
        finally:
            _sp.run = real_run
        check("runner.timeout-class", r["failure_class"] == "timeout",
              f"failure_class={r['failure_class']}")
        check("runner.timeout-choice-minus1", r["choice"] == -1,
              f"choice={r['choice']}")
        # 进程非零退:同上,trajectory 字段留诊断但 choice 不给
        _sp.run = fake_run_factory("crash")
        try:
            r = runner.ask_one("B", home, ws, "fixture question",
                               [f"Option {i}" for i in range(10)])
        finally:
            _sp.run = real_run
        check("runner.crash-class", r["failure_class"] == "process_failure",
              f"failure_class={r['failure_class']}")
        check("runner.crash-choice-minus1", r["choice"] == -1,
              f"choice={r['choice']}")
        check("runner.crash-keeps-trace", r["event_id"] != "",
              "崩溃路的 trajectory 指针应留诊断")

        # 无 session 路:进程跑了但没落账
        def fake_no_session(cmd, cwd=None, env=None, capture_output=False,
                            text=False, encoding=None, errors=None,
                            timeout=None):
            return _sp.CompletedProcess(cmd, 0, stdout=soup, stderr="")
        _sp.run = fake_no_session
        try:
            r = runner.ask_one("B", home, ws, "fixture question",
                               [f"Option {i}" for i in range(10)])
        finally:
            _sp.run = real_run
        check("runner.no-session-class", r["failure_class"] == "no_session",
              f"failure_class={r['failure_class']}")
        check("runner.no-session-choice", r["choice"] == -1,
              f"choice={r['choice']}——stdout 汤绝不能当答案")

    # ---- 汇总 ----
    total = len(failures)
    print(f"locomo_scorer fixtures: "
          f"{len(manifest.get('real', []))} real + "
          f"{len(manifest.get('synth', []))} synth entries")
    if failures:
        print(f"FAIL ({total}):")
        for f in failures:
            print("  -", f)
        return 1
    print("locomo_scorer: all green (offline, zero model calls)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
