#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoCoMo-MC10 判分器(LoCoMo 评测纠偏单 P1 重写,离线零模型)。

旧 runner 两处硬伤(单子 §三):
  1. last_assistant_text 只认 model.response.completed——Trajectory schema
     v2 实写 model.output.completed,248 个 B 态 session 零命中;
  2. 命不中就回落 proc.stdout,extract_choice 扫整段终端文字捞第一枚
     0-9 数字——文件行号/Token 数/耗时统计全成了"答案"。

本件按单子 §四/§五重立合同:
  - 唯一事实源 = trajectory main.jsonl 里钉住的最后一条
    stop_reason=end_turn 的 model.output.completed 文本块;
  - stdout/stderr 永不参与判分(本模块没有任何吃 stdout 的入口);
  - 主答严格匹配单枚 ASCII 数字并落在选项范围,否则 invalid,
    拒答("不知道")、空串、越界、格式错分型记账,不猜不捞;
  - schema v2 才判分;v1 是认得出但判不了的 legacy 账,版本不明/
    缺失/超前/混版一律 fail closed;
  - 两条 end_turn 记 ambiguous_final_output,不擅自挑一条。

事件形状(对齐 src/trajectory/event.hpp 与现存 288 个评测 session):
  {schema:"lubancode.trajectory.event", schema_version:2,
   kind:"model.output.completed", run_id:"main-00NN", turn_id:"turn-1",
   payload:{stop_reason:"end_turn"|"tool_use",
            blocks:[{type:"text",text:"…"}],
            request_id / output_id},
   event_id / session_id / …}
run_id 不固定(实测见过 main-0011:同 workspace 预灌 worker 占号),
单 run 单 turn 的 one_shot session 按"唯一 run/唯一 turn"自动钉,
钉不住或多 run 无指定即 fail closed。

用法(runner 内嵌,亦可独立导入做回归):
  import eval_locomo_score as s
  r = s.score_session_file(path, n_choices=10)
  r["parse_status"] == "ok" and r["parsed_choice"] == 7
"""

import hashlib
import json
import os
import re

SCHEMA_NAME = "lubancode.trajectory.event"
# 只按 v2 规则判分(§十六验收门:scorer 只认 schema v2)。
SCORED_SCHEMA_VERSION = 2
# v1 事件形状同源(src/trajectory/event.hpp:v1/v2 差异只在 usage 事件),
# 但 v1 账不判分——认得出、报 legacy_schema_v1,fail closed。
LEGACY_SCHEMA_VERSIONS = (1,)

PARSE_OK = "ok"
PARSE_INVALID = "invalid"

KIND_OUTPUT_COMPLETED = "model.output.completed"
STOP_END_TURN = "end_turn"

# 拒答词面(全串匹配,剥首尾空白与句读)。判分不猜:词面之外的拒答
# 表述(如长篇解释)按 invalid_format 记,不替模型圆场。
_REFUSAL_PATTERNS = [
    re.compile(r"^(?:不知道|不知道[。.!！?？\s]*)$", re.IGNORECASE),
    re.compile(r"^(?:i\s*(?:do not|don'?t)\s*know|not?\s*answerable"
               r"|cannot\s+(?:determine|answer)|unable\s+to\s+determine"
               r"|not\s+sure)[\s.!。?？]*$", re.IGNORECASE),
]

# 单枚 ASCII 数字(§四合同 2:严格 ^[0-9]$)。
_SINGLE_DIGIT = re.compile(r"^[0-9]$")


# ---------------------------------------------------------------- 严格解析

def parse_choice(raw_text, n_choices):
    """§5.2 严格单数字解析。返回 (parsed_choice|None, parse_status, failure_class)。

    只接受剥掉首尾空白后的一枚 ASCII 数字且落在 [0, n_choices)。
    空串/拒答/越界/格式错分型,一律 invalid,不猜不捞。
    """
    text = (raw_text or "").strip()
    if text == "":
        return None, PARSE_INVALID, "empty_final_output"
    if _SINGLE_DIGIT.match(text):
        value = int(text)
        if 0 <= value < n_choices:
            return value, PARSE_OK, ""
        return None, PARSE_INVALID, "out_of_range"
    for pat in _REFUSAL_PATTERNS:
        if pat.match(text):
            return None, PARSE_INVALID, "refusal_unknown"
    return None, PARSE_INVALID, "invalid_format"


# ---------------------------------------------------------------- 事实源定位

def _base_record(path):
    return {
        "session_id": "",
        "main_jsonl_relpath": "",
        "main_jsonl_sha256": "",
        "event_id": "",
        "run_id": "",
        "turn_id": "",
        "request_id": "",
        "output_id": "",
        "raw_final_text": "",
        "parsed_choice": None,
        "parse_status": PARSE_INVALID,
        "failure_class": "",
    }


def _fail(record, failure_class):
    record["failure_class"] = failure_class
    return record


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def _text_blocks_text(payload):
    blocks = payload.get("blocks") or []
    parts = []
    for b in blocks:
        if isinstance(b, dict) and b.get("type") == "text":
            parts.append(b.get("text") or "")
    return "".join(parts).strip()


def locate_final_output(events, n_choices, expected_run_id=None,
                        expected_turn_id=None):
    """从一列已解出的事件里钉最终末答。events 为 dict 列表(可为空)。

    钉法:先验信封(schema 名/版本),再钉 run/turn,最后取范围内
    end_turn 输出的最后一条;两条即 ambiguous。任何钉不住都 fail closed。
    """
    record = {"run_id": "", "turn_id": "", "event_id": "",
              "request_id": "", "output_id": "", "raw_final_text": "",
              "parsed_choice": None, "parse_status": PARSE_INVALID,
              "failure_class": "", "session_id": ""}
    if not events:
        _fail(record, "no_valid_lines")
        return record

    # -- 信封:schema 名(一行异名即整本拒) --
    for evt in events:
        if evt.get("schema") != SCHEMA_NAME:
            _fail(record, "schema_mismatch")
            return record

    # -- 信封:版本(缺失/不明/超前/混版 fail closed;v1 legacy 认账不判分) --
    versions = {evt.get("schema_version") for evt in events}
    if None in versions or not versions:
        _fail(record, "unknown_schema_version")
        return record
    if len(versions) > 1:
        _fail(record, "mixed_schema_version")
        return record
    version = next(iter(versions))
    if version == SCORED_SCHEMA_VERSION:
        pass
    elif version in LEGACY_SCHEMA_VERSIONS:
        _fail(record, "legacy_schema_v1")
        return record
    else:
        _fail(record, "unknown_schema_version")
        return record

    # -- 钉 run --
    outputs = [e for e in events
               if e.get("kind") == KIND_OUTPUT_COMPLETED]
    if not outputs:
        _fail(record, "no_final_output_event")
        return record
    runs = {e.get("run_id") for e in outputs}
    if expected_run_id is not None:
        if expected_run_id not in runs:
            _fail(record, "run_turn_mismatch")
            return record
        run_id = expected_run_id
    elif len(runs) == 1:
        run_id = next(iter(runs))
    else:
        _fail(record, "run_structure_ambiguous")
        return record

    # -- 钉 turn --
    turns = {e.get("turn_id") for e in outputs if e.get("run_id") == run_id}
    if expected_turn_id is not None:
        if expected_turn_id not in turns:
            _fail(record, "run_turn_mismatch")
            return record
        turn_id = expected_turn_id
    elif len(turns) == 1:
        turn_id = next(iter(turns))
    else:
        _fail(record, "run_structure_ambiguous")
        return record

    pinned = [e for e in outputs
              if e.get("run_id") == run_id and e.get("turn_id") == turn_id]
    end_turns = [e for e in pinned
                 if (e.get("payload") or {}).get("stop_reason") == STOP_END_TURN]
    if not end_turns:
        _fail(record, "no_end_turn")
        return record
    if len(end_turns) > 1:
        _fail(record, "ambiguous_final_output")
        return record

    final = end_turns[-1]
    payload = final.get("payload") or {}
    record["run_id"] = run_id
    record["turn_id"] = turn_id
    record["event_id"] = final.get("event_id") or ""
    record["request_id"] = final.get("request_id") or payload.get("request_id") or ""
    record["output_id"] = payload.get("output_id") or ""
    record["session_id"] = final.get("session_id") or ""
    record["raw_final_text"] = _text_blocks_text(payload)
    choice, status, failure = parse_choice(record["raw_final_text"], n_choices)
    record["parsed_choice"] = choice
    record["parse_status"] = status
    record["failure_class"] = failure
    return record


def score_session_file(main_jsonl, n_choices=10, expected_run_id=None,
                       expected_turn_id=None, rel_root=None):
    """判一场 one_shot 答题。返回 §5.1 结果账字段(超集,含诊断位)。

    stdout/stderr 不在本函数参数里——它们不参与判分,这是结构保证,
    不是约定。malformed_lines 只作诊断计数;末行截断会被当成没有
    end_turn,fail closed。
    """
    record = _base_record(main_jsonl)
    if rel_root:
        try:
            record["main_jsonl_relpath"] = os.path.relpath(main_jsonl, rel_root)
        except ValueError:
            record["main_jsonl_relpath"] = main_jsonl
    else:
        record["main_jsonl_relpath"] = main_jsonl
    if not os.path.isfile(main_jsonl):
        _fail(record, "unreadable_trajectory")
        return record
    record["main_jsonl_sha256"] = _sha256_file(main_jsonl)

    events = []
    malformed = 0
    with open(main_jsonl, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                malformed += 1
    located = locate_final_output(events, n_choices,
                                  expected_run_id, expected_turn_id)
    record.update({k: located[k] for k in
                   ("run_id", "turn_id", "event_id", "request_id",
                    "output_id", "session_id", "raw_final_text",
                    "parsed_choice", "parse_status", "failure_class")})
    if not record["session_id"]:
        record["session_id"] = os.path.basename(
            os.path.dirname(os.path.abspath(main_jsonl)))
    record["malformed_lines"] = malformed
    record["n_choices"] = n_choices
    return record


def unscored_record(failure_class, n_choices=None):
    """无 session 可判时(进程没落账/超时被杀)的占位账。"""
    record = _base_record("")
    record["n_choices"] = n_choices
    record["malformed_lines"] = 0
    return _fail(record, failure_class)


# ---------------------------------------------------------------- 自检

def _self_check():
    """离线自检:不碰网络、不碰真实账。失败抛 AssertionError。"""
    assert parse_choice("7", 10) == (7, PARSE_OK, "")
    assert parse_choice(" 4\n", 10) == (4, PARSE_OK, "")
    assert parse_choice("0", 10) == (0, PARSE_OK, "")
    # §5.2 全部 invalid 形状
    assert parse_choice("答案是 4", 10)[1] == PARSE_INVALID
    assert parse_choice("答案是 4", 10)[2] == "invalid_format"
    assert parse_choice("4.", 10)[2] == "invalid_format"
    assert parse_choice("Option 4", 10)[2] == "invalid_format"
    assert parse_choice("12", 10)[2] == "invalid_format"
    assert parse_choice("４", 10)[2] == "invalid_format"  # 全角,非 ASCII
    assert parse_choice("", 10)[2] == "empty_final_output"
    assert parse_choice("   ", 10)[2] == "empty_final_output"
    assert parse_choice("不知道", 10)[2] == "refusal_unknown"
    assert parse_choice("I don't know.", 10)[2] == "refusal_unknown"
    assert parse_choice("9", 5)[2] == "out_of_range"
    # 事实源:两条 end_turn 不挑
    base = {"schema": SCHEMA_NAME, "schema_version": 2,
            "kind": KIND_OUTPUT_COMPLETED, "run_id": "main-0001",
            "turn_id": "turn-1"}
    one = dict(base, payload={"stop_reason": "end_turn",
                              "blocks": [{"type": "text", "text": "3"}]})
    two = dict(base, payload={"stop_reason": "end_turn",
                              "blocks": [{"type": "text", "text": "5"}]})
    r = locate_final_output([one, two], 10)
    assert r["failure_class"] == "ambiguous_final_output"
    tool_only = dict(base, payload={"stop_reason": "tool_use",
                                    "blocks": [{"type": "text", "text": "9"}]})
    r = locate_final_output([tool_only], 10)
    assert r["failure_class"] == "no_end_turn"
    v1 = dict(one, schema_version=1)
    assert locate_final_output([v1], 10)["failure_class"] == "legacy_schema_v1"
    v3 = dict(one, schema_version=3)
    assert locate_final_output([v3], 10)["failure_class"] == "unknown_schema_version"
    mixed = [one, dict(two, schema_version=1)]
    assert locate_final_output(mixed, 10)["failure_class"] == "mixed_schema_version"
    wrong_schema = dict(one, schema="something.else")
    assert locate_final_output([wrong_schema], 10)["failure_class"] == "schema_mismatch"
    r = locate_final_output([one], 10, expected_run_id="main-0002")
    assert r["failure_class"] == "run_turn_mismatch"
    assert locate_final_output([], 10)["failure_class"] == "no_valid_lines"


if __name__ == "__main__":
    _self_check()
    print("eval_locomo_score: self-check ok (offline, zero model calls)")
