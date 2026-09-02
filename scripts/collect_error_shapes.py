#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""错误形态采样器(重试白名单单 P0-2):从轨迹会话里捞实战失败事件,回填账本。

扫的账:
  model.output.failed / turn.failed 两类事件,从 payload.reason 解析形态——
    "HTTP 401: ..."            前缀      -> HttpStatus 401
    "... (code=upstream_error)" 尾巴     -> Api code=upstream_error
    "... (type=xxx[, code=yyy])" 尾巴    -> Api code 取 code,缺则 type
    "模型返回错误: / 请求失败: " 前缀     -> 剥掉再按上面三样解
    连接超时/读超时/连接失败/网络 一类    -> Network(可重试)
    用户按 ESC 打断                      -> Cancelled(不重试)
  provider/model 从同会话同 turn 的 model.request.prepared 补;
  wire 要 --config 指 lubancode config.json 才能对上(provider 名 -> wire),
  没给就记 null——形态去重键照样成立,补跑一次 --config 即可回填。

幂等:形态去重键 = (wire, http_status, api_code)。同形态只留一条,新出处
append 进 sources;已有人工字段(first_seen_*/expected_retryable)一字不动,
出处去重按 (session_id, reason)。current_policy/mismatch 每次重算。

对账(mismatch):expected_retryable 是人录的"这形态现场该不该重试"(瞬时
上游病=true,确定性错误=false),脚本拿镜像白名单算 should_retry,两者相
左才记 mismatch——该重试没重试、不该重试重试了,都算。expected_retryable
缺省(null)的条目跳过对账。

镜像白名单同步义务:下表两张是 src/api/model_request_recovery.cpp
IsRetryableError 的复制品,C++ 侧改表必须同步到这里,否则对账失真。

用法:
  python scripts/collect_error_shapes.py <轨迹根>... [--config ~/.lubancode/config.json]
                                         [--ledger tests/fixtures/api/error_shapes.json] [--check]

轨迹根认三种:workspaces 根(多个 workspace)、单个 workspace、sessions 目录。
--check 只报不写,有新形态未入账时退出码 1(CI 催回填用)。
"""

import argparse
import datetime
import json
import re
import sys
from pathlib import Path

# ---- IsRetryableError 镜像(源:src/api/model_request_recovery.cpp,改动须同步) ----
RETRYABLE_HTTP_STATUSES = {408, 429, 500, 502, 503, 504}
RETRYABLE_API_CODES = {
    "upstream_error", "server_error", "overloaded_error", "overloaded",
    "model_overloaded", "internal_error", "internal_server_error",
}

HTTP_PREFIX = re.compile(r"^HTTP (\d{3}):\s*")
# ComposeErrorMessage(responses/events.cpp)的尾巴:type 空时 "(code=x)",
# type 有值时 "(type=x)" / "(type=x, code=y)"。
SHAPE_TAIL = re.compile(r"\s*\((?:type=([\w.-]+))?(?:,\s*)?(?:code=([\w.-]+))?\)$")
NETWORK_HINTS = ("连接超时", "读超时", "请求超时", "连接失败", "网络",
                 "Connection", "reset", "timed out", "connect")
CANCEL_HINTS = ("ESC", "取消")
PARSE_HINTS = ("协议错误", "SSE", "帧超过")
REASON_PREFIXES = ("模型返回错误: ", "请求失败: ")

LEDGER_SCHEMA = "lubancode.error.shapes.ledger"


def iter_session_files(root: Path):
    """轨迹根 -> 一枚枚 jsonl。三种根形都吃:workspaces 根 / 单 workspace / sessions 目录。"""
    patterns = [
        "sessions/*/main.jsonl", "sessions/*/subagents/*.jsonl",
        "*/sessions/*/main.jsonl", "*/sessions/*/subagents/*.jsonl",
        "main.jsonl", "subagents/*.jsonl",
    ]
    seen = set()
    for pattern in patterns:
        for path in root.glob(pattern):
            key = str(path.resolve())
            if key not in seen:
                seen.add(key)
                yield path


def strip_reason(reason: str) -> str:
    for prefix in REASON_PREFIXES:
        if reason.startswith(prefix):
            return reason[len(prefix):]
    return reason


def classify(reason: str):
    """reason -> (http_status, api_code, kind)。kind ∈ HttpStatus/Api/Network/Cancelled/Parse。"""
    text = strip_reason(reason.strip())
    http_status = None
    api_code = None
    m = HTTP_PREFIX.match(text)
    if m:
        http_status = int(m.group(1))
        text = text[m.end():]
    tail = SHAPE_TAIL.search(text)
    if tail:
        api_code = tail.group(2) or tail.group(1)
    if http_status is not None:
        return http_status, api_code, "HttpStatus"
    if any(hint in text for hint in PARSE_HINTS):
        return None, api_code, "Parse"
    if any(hint in text for hint in CANCEL_HINTS):
        return None, api_code, "Cancelled"
    if any(hint in text for hint in NETWORK_HINTS):
        return None, api_code, "Network"
    return None, api_code, "Api"


def should_retry(http_status, api_code, kind):
    """镜像 IsRetryableError。"""
    if kind == "Network":
        return True
    if kind == "HttpStatus":
        return http_status in RETRYABLE_HTTP_STATUSES
    if kind == "Api":
        return api_code in RETRYABLE_API_CODES
    return False


def reason_code(kind, http_status, api_code):
    if kind == "Network":
        return "network.error"
    if kind == "HttpStatus":
        return "http.%d" % http_status
    if kind == "Parse":
        return "protocol.parse"
    if kind == "Api":
        return "api." + api_code if api_code else "api.error"
    return "cancelled"


def wall_date(event):
    """wall_time_ms -> 本地日期串;缺了给空。"""
    wall = event.get("wall_time_ms")
    if not isinstance(wall, (int, float)):
        return ""
    return datetime.datetime.fromtimestamp(wall / 1000.0).strftime("%Y-%m-%d")


def load_provider_wire(config_path):
    providers = {}
    try:
        data = json.loads(Path(config_path).read_text(encoding="utf-8"))
        for entry in data.get("providers", []):
            name = entry.get("name")
            if name:
                providers[name] = entry.get("wire")
    except (OSError, ValueError) as ex:
        print("warning: config 读不了(%s),wire 记 null" % ex, file=sys.stderr)
    return providers


def harvest(root: Path, provider_wire):
    """一个轨迹根 -> {形态键: 形态条目(采集面)}。"""
    shapes = {}
    files_scanned = 0
    failures_seen = 0
    for path in iter_session_files(root):
        files_scanned += 1
        session_id = path.parent.name if path.name == "main.jsonl" else path.parent.parent.name
        # provider/model 索引:同 turn 最近一次 model.request.prepared。
        prepared_by_turn = {}
        try:
            with path.open(encoding="utf-8") as handle:
                for line in handle:
                    try:
                        event = json.loads(line)
                    except ValueError:
                        continue
                    kind = event.get("kind")
                    turn_id = event.get("turn_id")
                    payload = event.get("payload", {})
                    if kind == "model.request.prepared":
                        prepared_by_turn[turn_id] = (payload.get("provider"), payload.get("model"),
                                                     payload.get("purpose"))
                        continue
                    if kind not in ("model.output.failed", "turn.failed"):
                        continue
                    reason = payload.get("reason", "")
                    if not reason:
                        continue
                    if kind == "turn.failed":
                        # turn.failed 有两个来历:模型失败收口(reason 带"模型
                        # 返回错误:"/"请求失败:"前缀)与轨迹系统自己的失败
                        # (如 trajectory.turn_close_rejected,payload 另带
                        # error_code)。账本只记前者——后者是本地账务事件,
                        # 不是模型请求的错误形态。
                        if "error_code" in payload:
                            continue
                        if not reason.startswith(REASON_PREFIXES):
                            continue
                    failures_seen += 1
                    http_status, api_code, error_kind = classify(reason)
                    provider, model, purpose = prepared_by_turn.get(turn_id, (None, None, None))
                    wire = provider_wire.get(provider)
                    key = (wire, http_status, api_code)
                    entry = shapes.setdefault(key, {
                        "wire": wire, "http_status": http_status, "api_code": api_code,
                        "error_kind": error_kind, "first_date": wall_date(event),
                        "occurrences": 0, "sources": [],
                    })
                    entry["occurrences"] += 1
                    source = {
                        "kind": "trajectory", "session_id": session_id,
                        "event": kind, "reason": reason, "date": wall_date(event),
                    }
                    if provider:
                        source["provider"] = provider
                    if model:
                        source["model"] = model
                    if purpose:
                        source["purpose"] = purpose
                    # 同会话同场(model.output.failed 与 turn.failed 的 reason
                    # 只差一层收口前缀)只留一份。
                    bare = strip_reason(reason)
                    if not any(s.get("session_id") == session_id and strip_reason(s.get("reason", "")) == bare
                               for s in entry["sources"]):
                        entry["sources"].append(source)
        except OSError as ex:
            print("warning: %s 读不了(%s)" % (path, ex), file=sys.stderr)
    return shapes, files_scanned, failures_seen


def shape_id(wire, http_status, api_code):
    parts = ["shape"]
    if http_status is not None:
        parts.append("http-%d" % http_status)
    else:
        parts.append("api-%s" % (api_code or "none"))
    if wire:
        parts.append(wire.replace("/", "-"))
    return "-".join(parts)


def reconcile(entry, retryable_now):
    """按镜像白名单重算 current_policy/mismatch;expected_retryable 缺省跳过对账。"""
    entry["current_policy"] = {
        "retryable": retryable_now,
        "reason_code": entry.get("reason_code"),
        "source": "src/api/model_request_recovery.cpp IsRetryableError",
    }
    expected = entry.get("expected_retryable")
    if expected is None:
        entry["mismatch"] = None
        return
    if expected == retryable_now:
        entry["mismatch"] = None
    else:
        blame = "该重试没重试" if expected else "不该重试重试了"
        entry["mismatch"] = "%s:现场预期 retryable=%s,现行白名单判 %s" % (blame, expected, retryable_now)


def merge_into_ledger(ledger, collected, verbose):
    """采集形态并进账本:同键去重,新键 append;人工字段不动。返回 (新增数, mismatch 数)。"""
    shapes = ledger.setdefault("shapes", [])
    by_key = {(s.get("wire"), s.get("http_status"), s.get("api_code")): s for s in shapes}
    new_count = 0
    for key in sorted(collected, key=str):
        fresh = collected[key]
        entry = by_key.get(key)
        if entry is None:
            entry = {
                "id": shape_id(key[0], key[1], key[2]),
                "wire": key[0],
                "http_status": key[1],
                "api_code": key[2],
                "error_kind": fresh["error_kind"],
                "first_seen_date": fresh["first_date"] or None,
                "first_seen_scene": "采集脚本首录(场景细节待人工补注)",
                "sources": [],
                "origin": "collected",
            }
            shapes.append(entry)
            by_key[key] = entry
            new_count += 1
            if verbose:
                print("  + 新形态 %s" % entry["id"])
        # 出处回填:既有出处(session_id+剥离前缀后的 reason 同)不重复 append。
        known = {(s.get("session_id"), strip_reason(s.get("reason", "")))
                 for s in entry.get("sources", []) if s.get("kind") == "trajectory"}
        for source in fresh["sources"]:
            if (source["session_id"], strip_reason(source["reason"])) not in known:
                entry.setdefault("sources", []).append(source)
    # 全量重算判定与 mismatch(手录条目一并重算,镜像表改了就翻新)。
    mismatch_count = 0
    for entry in shapes:
        error_kind = entry.get("error_kind")
        retryable = should_retry(entry.get("http_status"), entry.get("api_code"), error_kind)
        entry["reason_code"] = reason_code(error_kind, entry.get("http_status"), entry.get("api_code"))
        reconcile(entry, retryable)
        if entry.get("mismatch"):
            mismatch_count += 1
    return new_count, mismatch_count


def main():
    parser = argparse.ArgumentParser(description="实战错误形态采样:轨迹失败事件 -> 账本")
    parser.add_argument("roots", nargs="+", type=Path, help="轨迹根(workspaces 根/单 workspace/sessions 目录)")
    parser.add_argument("--config", type=Path, default=None, help="lubancode config.json(provider->wire)")
    parser.add_argument("--ledger", type=Path, default=None, help="账本路径(默认 tests/fixtures/api/error_shapes.json)")
    parser.add_argument("--check", action="store_true", help="只报不写;有新形态未入账退出码 1")
    args = parser.parse_args()

    script_dir = Path(__file__).resolve().parent
    ledger_path = args.ledger or (script_dir.parent / "tests" / "fixtures" / "api" / "error_shapes.json")
    provider_wire = load_provider_wire(args.config) if args.config else {}

    collected = {}
    total_files = 0
    total_failures = 0
    for root in args.roots:
        shapes, files_scanned, failures = harvest(root, provider_wire)
        total_files += files_scanned
        total_failures += failures
        for key, entry in shapes.items():
            merged = collected.setdefault(key, entry)
            if merged is not entry:
                merged["occurrences"] += entry["occurrences"]
                merged["sources"].extend(entry["sources"])
    print("扫 %d 个 jsonl,撞见 %d 条失败事件,聚成 %d 种形态" %
          (total_files, total_failures, len(collected)))

    ledger = {"schema": LEDGER_SCHEMA, "schema_version": 1, "shapes": []}
    if ledger_path.exists():
        try:
            ledger = json.loads(ledger_path.read_text(encoding="utf-8"))
        except ValueError as ex:
            print("账本坏了(%s),先手修再跑" % ex, file=sys.stderr)
            return 2

    new_count, mismatch_count = merge_into_ledger(ledger, collected, verbose=True)

    for entry in sorted(ledger["shapes"], key=lambda s: s.get("id", "")):
        tag = " [MISMATCH] %s" % entry["mismatch"] if entry.get("mismatch") else ""
        policy = "可重试" if entry["current_policy"]["retryable"] else "零重试"
        print("  %-44s %-18s -> %s%s" % (entry["id"], entry.get("wire") or "wire未知", policy, tag))
    if mismatch_count:
        print("mismatch %d 条(判定与账本不合,见上)" % mismatch_count)

    if args.check:
        if new_count:
            print("check: 有 %d 种新形态未入账(--check 不写,去掉 --check 回填)" % new_count)
            return 1
        print("check: 无新形态,账本齐")
        return 0

    ledger_path.parent.mkdir(parents=True, exist_ok=True)
    ledger_path.write_text(json.dumps(ledger, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("账本落 %s(新增 %d 种,%d 条 mismatch)" % (ledger_path, new_count, mismatch_count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
