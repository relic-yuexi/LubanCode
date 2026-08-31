#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Kimi 保留式思考真机探针(《Kimi 保留式思考、工具循环与历史回传兼容设计》P2)。

不进常规 ctest:钥匙只认 env MOONSHOT_API_KEY 或用户 config
(%USERPROFILE%\\.lubancode\\config.json 里 providers 列表 name=="moonshot" 的
api_key)。钥匙不打印、不落盘、不进任何证据文件。

两个子命令:

  direct  直打真端点四案 + 模型清单(每案 max_tokens<=200,一两发):
            case k3_two_turn   K3 两轮纯对话:第二轮请求体里第一轮
                               assistant.reasoning_content 原样在场
            case k26_tool_loop K2.6 两步工具调用:第二轮把 assistant
                               (reasoning_content+tool_calls)+tool 结果回传
            case k26_keep_all  K2.6 thinking.keep="all" 跨轮:服务端接受
                               keep 字段与回传历史
            case k27_two_turn  K2.7-code 两轮纯对话:零请求参数 + Always 回传
            case models        GET /models,与 catalog/providers.json 对账
  resume   真 lubancode.exe + 临时 USERPROFILE(只带 moonshot 一个 provider)
           + 本地录制代理:管道跑一轮 /exit,--continue 续聊一轮,抓第二笔
           请求体核对 assistant.reasoning_content 与第一轮流式响应逐字一致。

三层报账(不许并成一句"支持"):
  L1 accepted         HTTP 200 + 请求体含期望字段
  L2 emitted          响应 reasoning_content 非空
  L3 echoed           下一笔请求体的 assistant.reasoning_content 与原响应逐字一致
                     (resume 案还要求服务端接受这笔回传后的请求)

证据落盘(仓库 gitignore 内):build/smoke/kimi-preserved-thinking/
  *_request*.json / *_response*.json / resume_exchange_*.json  原始报文(含
  reasoning 正文,只留本地);summary.json 只有长度/sha256/逐字相等/usage,
  不含 reasoning 正文——普通报告只许引用后者。

用法:
  python tests/manual/model_probe_kimi_preserved_thinking.py direct
  python tests/manual/model_probe_kimi_preserved_thinking.py resume \
      --exe build/debug/Debug/lubancode.exe
  python tests/manual/model_probe_kimi_preserved_thinking.py resume --analyze-only
      (零网络复析已录证据:改了解析逻辑后复跑账,不花一枚 token)
  可配 env:MOONSHOT_API_KEY / KIMI_PROBE_BASE_URL(默认
  https://api.moonshot.cn/v1)/ KIMI_PROBE_EVIDENCE_DIR。
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_BASE_URL = "https://api.moonshot.cn/v1"
MAX_TOKENS = 200  # 探测成本纪律:全部 <=200
HTTP_TIMEOUT = 180

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


# ---------------------------------------------------------------------------
# 钥匙与端点
# ---------------------------------------------------------------------------


def load_api_key() -> str | None:
    key = os.environ.get("MOONSHOT_API_KEY", "").strip()
    if key:
        return key
    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    path = os.path.join(home, ".lubancode", "config.json")
    try:
        with open(path, encoding="utf-8") as handle:
            cfg = json.load(handle)
    except (OSError, ValueError):
        return None
    for entry in cfg.get("providers", []):
        if isinstance(entry, dict) and entry.get("name") == "moonshot":
            value = str(entry.get("api_key") or "").strip()
            if value:
                return value
    return None


def evidence_dir() -> str:
    root = os.environ.get("KIMI_PROBE_EVIDENCE_DIR") or os.path.join(
        REPO_ROOT, "build", "smoke", "kimi-preserved-thinking"
    )
    os.makedirs(root, exist_ok=True)
    return root


def base_url() -> str:
    return os.environ.get("KIMI_PROBE_BASE_URL", "").strip().rstrip("/") or DEFAULT_BASE_URL


def sha256_short(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:12]


def write_json(name: str, payload) -> str:
    path = os.path.join(evidence_dir(), name)
    with open(path, "w", encoding="utf-8") as handle:
        if isinstance(payload, str):
            handle.write(payload)
        else:
            json.dump(payload, handle, ensure_ascii=False, indent=1)
    return path


def load_json(name: str):
    with open(os.path.join(evidence_dir(), name), encoding="utf-8") as handle:
        return json.load(handle)


# ---------------------------------------------------------------------------
# 直打 HTTP
# ---------------------------------------------------------------------------


def http_json(method: str, url: str, key: str, payload=None):
    """发一笔请求。返回 (status, 响应文本, 实际发出的请求体字节)。"""
    data = None
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={"Authorization": "Bearer " + key, "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
            return response.status, response.read().decode("utf-8", "replace"), data
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode("utf-8", "replace"), data
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        return 0, f"{{\"probe_network_error\": {json.dumps(str(error))}}}", data


def chat(base: str, key: str, payload) -> tuple[int, dict | None, str, bytes]:
    status, text, sent = http_json("POST", base + "/chat/completions", key, payload)
    parsed = None
    try:
        parsed = json.loads(text)
    except ValueError:
        parsed = None
    return status, parsed, text, sent or b""


def message_of(parsed) -> dict | None:
    try:
        return parsed["choices"][0]["message"]
    except (KeyError, IndexError, TypeError):
        return None


def finish_reason(parsed) -> str:
    try:
        return str(parsed["choices"][0].get("finish_reason") or "")
    except (KeyError, IndexError, TypeError):
        return ""


def usage_line(parsed) -> str:
    usage = (parsed or {}).get("usage")
    if not isinstance(usage, dict):
        return "usage 不可得(响应无 usage 对象)"
    prompt = usage.get("prompt_tokens", "?")
    completion = usage.get("completion_tokens", "?")
    details = usage.get("completion_tokens_details") or {}
    prompt_details = usage.get("prompt_tokens_details") or {}
    reasoning = details.get("reasoning_tokens", "不可得")
    cached = prompt_details.get("cached_tokens", "不可得")
    return (
        f"usage prompt={prompt} completion={completion} "
        f"reasoning_tokens={reasoning} cached_tokens={cached}"
    )


def layer_line(name: str, ok: bool | None, detail: str) -> str:
    verdict = "PASS" if ok is True else ("FAIL" if ok is False else "SKIP")
    return f"[{verdict}] {name}: {detail}"


def transient_status(status: int) -> bool:
    """429/5xx 是限流或服务端抖动,不是契约答案——记'暂不可测',不记 FAIL。"""
    return status == 429 or 500 <= status < 600


def mark_transient(case: dict, where: str, status: int) -> dict:
    detail = f"http={status}({where}),限流/服务端抖动,本案暂不可测,不发第二笔"
    for name in ("L1_accepted", "L2_emitted_reasoning", "L3_echoed_back"):
        case["layers"][name] = {"ok": None, "detail": detail}
    case["usage"] = {"turn1": "不可得(未收到响应)", "turn2": "不可得(未发第二笔)"}
    return case


# ---------------------------------------------------------------------------
# direct: 三案
# ---------------------------------------------------------------------------


def case_k3_two_turn(key: str) -> dict:
    """K3 两轮纯对话:第二轮请求体第一轮 assistant.reasoning_content 原样在场。"""
    case = {"case": "k3_two_turn", "model": "kimi-k3", "layers": {}}
    u1 = "用一句话说明什么是二进制,不超过二十个字。"
    u2 = "接着上面说的,再一句话:计算机里它用来做什么?"
    body1 = {
        "model": "kimi-k3",
        "messages": [{"role": "user", "content": u1}],
        "max_tokens": MAX_TOKENS,
        "reasoning_effort": "low",
    }
    status1, parsed1, text1, sent1 = chat(base_url(), key, body1)
    write_json("k3_two_turn_request1.json", json.loads(sent1.decode("utf-8")))
    write_json("k3_two_turn_response1.json", text1)
    msg1 = message_of(parsed1)
    rc1 = (msg1 or {}).get("reasoning_content") or ""
    content1 = (msg1 or {}).get("content") or ""
    if transient_status(status1):
        return mark_transient(case, "第一笔", status1)

    l1 = status1 == 200 and msg1 is not None
    l2 = bool(rc1)
    case["layers"]["L1_accepted"] = {
        "ok": l1,
        "detail": f"http={status1} choices_present={msg1 is not None} "
        f"request(reasoning_effort=low, thinking 键={'有' if 'thinking' in body1 else '无'})",
    }
    case["layers"]["L2_emitted_reasoning"] = {
        "ok": l2,
        "detail": f"len={len(rc1)} sha256={sha256_short(rc1)}" if rc1 else "reasoning_content 为空",
    }

    body2 = {
        "model": "kimi-k3",
        "messages": [
            {"role": "user", "content": u1},
            {"role": "assistant", "reasoning_content": rc1, "content": content1},
            {"role": "user", "content": u2},
        ],
        "max_tokens": MAX_TOKENS,
        "reasoning_effort": "low",
    }
    status2, parsed2, text2, sent2 = chat(base_url(), key, body2)
    write_json("k3_two_turn_request2.json", json.loads(sent2.decode("utf-8")))
    write_json("k3_two_turn_response2.json", text2)
    # 逐字比对:对准实际发出的 wire 字节,不信内存里的同源对象。
    wire_rc = ""
    try:
        wire_rc = json.loads(sent2.decode("utf-8"))["messages"][1]["reasoning_content"]
    except (ValueError, KeyError, IndexError, TypeError):
        wire_rc = ""
    identical = wire_rc == rc1 and len(wire_rc) == len(rc1)
    case["layers"]["L3_echoed_back"] = {
        "ok": identical and status2 == 200,
        "detail": (
            f"第二笔请求体 assistant.reasoning_content 逐字一致={identical} "
            f"len={len(wire_rc)} sha256={sha256_short(wire_rc)};第二笔 http={status2}"
        ),
    }
    case["usage"] = {
        "turn1": usage_line(parsed1),
        "turn2": usage_line(parsed2),
        "turn2_reasoning_nonempty": bool((message_of(parsed2) or {}).get("reasoning_content")),
        "finish1": finish_reason(parsed1),
        "finish2": finish_reason(parsed2),
    }
    return case


def case_k26_tool_loop(key: str) -> dict:
    """K2.6 两步工具调用:第二轮回传 assistant(reasoning+tool_calls)+tool 结果。"""
    case = {"case": "k26_tool_loop", "model": "kimi-k2.6", "layers": {}}
    tools = [
        {
            "type": "function",
            "function": {
                "name": "read_notes",
                "description": "读取一份本地笔记文件的内容(无害只读)",
                "parameters": {
                    "type": "object",
                    "properties": {"path": {"type": "string", "description": "文件路径"}},
                    "required": ["path"],
                },
            },
        }
    ]

    def ask(directive: str):
        body = {
            "model": "kimi-k2.6",
            "messages": [{"role": "user", "content": directive}],
            "tools": tools,
            "thinking": {"type": "enabled"},
            "max_tokens": MAX_TOKENS,
        }
        return (body, *chat(base_url(), key, body))

    # 第一发:诱导模型思考后调工具。没出 tool_calls 允许换更硬的指令重试一发。
    body1, status1, parsed1, text1, sent1 = ask("请调用 read_notes 工具读取 notes.txt,先想清楚再调用,不要直接回答。")
    msg1 = message_of(parsed1)
    if status1 == 200 and msg1 is not None and not msg1.get("tool_calls"):
        body1, status1, parsed1, text1, sent1 = ask(
            "必须调用 read_notes 工具,path 填 notes.txt。禁止直接给答案。"
        )
        msg1 = message_of(parsed1)
    write_json("k26_tool_loop_request1.json", json.loads(sent1.decode("utf-8")))
    write_json("k26_tool_loop_response1.json", text1)
    if transient_status(status1):
        return mark_transient(case, "第一笔", status1)

    rc1 = (msg1 or {}).get("reasoning_content") or ""
    content1 = (msg1 or {}).get("content") or ""
    tool_calls1 = (msg1 or {}).get("tool_calls") or []
    l1 = status1 == 200 and msg1 is not None
    l2 = bool(rc1)
    case["layers"]["L1_accepted"] = {
        "ok": l1,
        "detail": f"http={status1} finish={finish_reason(parsed1)} tool_calls={len(tool_calls1)}",
    }
    case["layers"]["L2_emitted_reasoning"] = {
        "ok": l2,
        "detail": f"len={len(rc1)} sha256={sha256_short(rc1)}" if rc1 else "reasoning_content 为空",
    }
    if not tool_calls1:
        case["layers"]["L3_echoed_back"] = {
            "ok": False,
            "detail": "模型未发起 tool_calls,两步循环不成立(证据已存 response1)",
        }
        case["usage"] = {"turn1": usage_line(parsed1), "turn2": "不可得(循环未成立)"}
        return case

    assistant_echo = {
        "role": "assistant",
        "reasoning_content": rc1,
        "content": content1,
        "tool_calls": tool_calls1,
    }
    tool_result = {
        "role": "tool",
        "tool_call_id": tool_calls1[0]["id"],
        "content": "第一行:今天天气晴。\n第二行:适合散步。\n第三行:完。",
    }
    body2 = {
        "model": "kimi-k2.6",
        "messages": [
            {"role": "user", "content": body1["messages"][0]["content"]},
            assistant_echo,
            tool_result,
        ],
        "tools": tools,
        "thinking": {"type": "enabled"},
        "max_tokens": MAX_TOKENS,
    }
    status2, parsed2, text2, sent2 = chat(base_url(), key, body2)
    write_json("k26_tool_loop_request2.json", json.loads(sent2.decode("utf-8")))
    write_json("k26_tool_loop_response2.json", text2)
    wire_rc = ""
    try:
        wire_rc = json.loads(sent2.decode("utf-8"))["messages"][1]["reasoning_content"]
    except (ValueError, KeyError, IndexError, TypeError):
        wire_rc = ""
    identical = wire_rc == rc1 and len(wire_rc) == len(rc1)
    no_effort = "reasoning_effort" not in json.loads(sent2.decode("utf-8"))
    case["layers"]["L3_echoed_back"] = {
        "ok": identical and status2 == 200 and no_effort,
        "detail": (
            f"第二笔请求体 assistant.reasoning_content 逐字一致={identical} "
            f"len={len(wire_rc)} sha256={sha256_short(wire_rc)};第二笔 http={status2} "
            f"finish={finish_reason(parsed2)};第二笔无 reasoning_effort={no_effort}"
        ),
    }
    case["usage"] = {
        "turn1": usage_line(parsed1),
        "turn2": usage_line(parsed2),
        "turn2_tool_calls": len((message_of(parsed2) or {}).get("tool_calls") or []),
    }
    return case


def case_k26_keep_all(key: str) -> dict:
    """K2.6 thinking.keep="all":服务端接受 keep 字段与跨轮回传历史。"""
    case = {"case": "k26_keep_all", "model": "kimi-k2.6", "layers": {}}
    u1 = "一句话:江南最大的城市是哪座?"
    u2 = "接着刚才说的,再一句话:它在哪个省?"
    body1 = {
        "model": "kimi-k2.6",
        "messages": [{"role": "user", "content": u1}],
        "thinking": {"type": "enabled"},
        "max_tokens": MAX_TOKENS,
    }
    status1, parsed1, text1, sent1 = chat(base_url(), key, body1)
    write_json("k26_keep_all_request1.json", json.loads(sent1.decode("utf-8")))
    write_json("k26_keep_all_response1.json", text1)
    msg1 = message_of(parsed1)
    rc1 = (msg1 or {}).get("reasoning_content") or ""
    content1 = (msg1 or {}).get("content") or ""
    if transient_status(status1):
        return mark_transient(case, "第一笔", status1)

    l1 = status1 == 200 and msg1 is not None
    l2 = bool(rc1)
    case["layers"]["L1_accepted"] = {"ok": l1, "detail": f"http={status1} finish={finish_reason(parsed1)}"}
    case["layers"]["L2_emitted_reasoning"] = {
        "ok": l2,
        "detail": f"len={len(rc1)} sha256={sha256_short(rc1)}" if rc1 else "reasoning_content 为空",
    }

    body2 = {
        "model": "kimi-k2.6",
        "thinking": {"type": "enabled", "keep": "all"},
        "messages": [
            {"role": "user", "content": u1},
            {"role": "assistant", "reasoning_content": rc1, "content": content1},
            {"role": "user", "content": u2},
        ],
        "max_tokens": MAX_TOKENS,
    }
    status2, parsed2, text2, sent2 = chat(base_url(), key, body2)
    write_json("k26_keep_all_request2.json", json.loads(sent2.decode("utf-8")))
    write_json("k26_keep_all_response2.json", text2)
    wire_rc = ""
    try:
        wire_rc = json.loads(sent2.decode("utf-8"))["messages"][1]["reasoning_content"]
    except (ValueError, KeyError, IndexError, TypeError):
        wire_rc = ""
    identical = wire_rc == rc1 and len(wire_rc) == len(rc1)
    keep_sent = False
    try:
        keep_sent = json.loads(sent2.decode("utf-8"))["thinking"]["keep"] == "all"
    except (ValueError, KeyError, TypeError):
        keep_sent = False
    case["layers"]["L3_echoed_back"] = {
        "ok": identical and keep_sent and status2 == 200,
        "detail": (
            f"thinking.keep=all 已发={keep_sent};第二笔请求体 assistant."
            f"reasoning_content 逐字一致={identical} len={len(wire_rc)} "
            f"sha256={sha256_short(wire_rc)};第二笔 http={status2}"
        ),
    }
    case["usage"] = {"turn1": usage_line(parsed1), "turn2": usage_line(parsed2)}
    return case


def case_k27_two_turn(key: str) -> dict:
    """K2.7-code 两轮纯对话:零请求参数(effort/thinking 都不发)+ Always 回传。"""
    case = {"case": "k27_two_turn", "model": "kimi-k2.7-code", "layers": {}}
    u1 = "用一句话说明什么是质数,不超过二十个字。"
    u2 = "接着上面说的,再一句话:最小的质数是几?"
    body1 = {
        "model": "kimi-k2.7-code",
        "messages": [{"role": "user", "content": u1}],
        "max_tokens": MAX_TOKENS,
    }
    status1, parsed1, text1, sent1 = chat(base_url(), key, body1)
    write_json("k27_two_turn_request1.json", json.loads(sent1.decode("utf-8")))
    write_json("k27_two_turn_response1.json", text1)
    msg1 = message_of(parsed1)
    rc1 = (msg1 or {}).get("reasoning_content") or ""
    content1 = (msg1 or {}).get("content") or ""
    if transient_status(status1):
        return mark_transient(case, "第一笔", status1)
    sent1_keys = sorted(json.loads(sent1.decode("utf-8")).keys())

    l1 = status1 == 200 and msg1 is not None
    l2 = bool(rc1)
    case["layers"]["L1_accepted"] = {
        "ok": l1,
        "detail": f"http={status1};第一笔顶层键={sent1_keys}(契约:无 thinking 无 reasoning_effort)",
    }
    case["layers"]["L2_emitted_reasoning"] = {
        "ok": l2,
        "detail": f"len={len(rc1)} sha256={sha256_short(rc1)}" if rc1 else "reasoning_content 为空",
    }

    body2 = {
        "model": "kimi-k2.7-code",
        "messages": [
            {"role": "user", "content": u1},
            {"role": "assistant", "reasoning_content": rc1, "content": content1},
            {"role": "user", "content": u2},
        ],
        "max_tokens": MAX_TOKENS,
    }
    status2, parsed2, text2, sent2 = chat(base_url(), key, body2)
    write_json("k27_two_turn_request2.json", json.loads(sent2.decode("utf-8")))
    write_json("k27_two_turn_response2.json", text2)
    sent2_obj = json.loads(sent2.decode("utf-8"))
    wire_rc = ""
    try:
        wire_rc = sent2_obj["messages"][1]["reasoning_content"]
    except (KeyError, IndexError, TypeError):
        wire_rc = ""
    identical = wire_rc == rc1 and len(wire_rc) == len(rc1)
    bare = not ("thinking" in sent2_obj or "reasoning_effort" in sent2_obj)
    case["layers"]["L3_echoed_back"] = {
        "ok": identical and status2 == 200 and bare,
        "detail": (
            f"第二笔请求体 assistant.reasoning_content 逐字一致={identical} "
            f"len={len(wire_rc)} sha256={sha256_short(wire_rc)};第二笔 http={status2};"
            f"第二笔顶层干净(无 thinking/reasoning_effort)={bare}"
        ),
    }
    case["usage"] = {"turn1": usage_line(parsed1), "turn2": usage_line(parsed2)}
    return case


def case_models(key: str) -> dict:
    """GET /models 对账 catalog/providers.json 的 moonshot 模型清单。"""
    status, text, _ = http_json("GET", base_url() + "/models", key)
    served = []
    try:
        payload = json.loads(text)
        served = sorted(
            str(item["id"]) for item in payload.get("data", []) if "kimi" in str(item.get("id", ""))
        )
    except (ValueError, AttributeError, TypeError):
        served = []
    write_json("models.json", text)
    catalog_path = os.path.join(REPO_ROOT, "catalog", "providers.json")
    cataloged = []
    try:
        with open(catalog_path, encoding="utf-8") as handle:
            catalog = json.load(handle)
        cataloged = sorted(catalog["providers"]["moonshot"]["models"].keys())
    except (OSError, ValueError, KeyError, TypeError):
        cataloged = []
    only_served = [m for m in served if m not in cataloged]
    only_catalog = [m for m in cataloged if m not in served]
    case = {
        "case": "models",
        "layers": {
            "L1_accepted": {"ok": status == 200, "detail": f"http={status} kimi 模型 {len(served)} 枚"},
            "catalog_diff": {
                "served_not_in_catalog": only_served,
                "catalog_not_served": only_catalog,
            },
        },
        "served": served,
        "cataloged": cataloged,
    }
    return case


def cmd_direct(_args) -> int:
    key = load_api_key()
    if not key:
        print("SKIP: 没有 MOONSHOT_API_KEY,用户 config 里也没找到 moonshot 的 api_key。")
        print("     探针不发一笔请求,不算 PASS。")
        return 0
    print(f"端点: {base_url()}  key: sk-***  证据目录: {evidence_dir()}")
    started = time.strftime("%Y-%m-%d %H:%M:%S")
    cases = []
    for runner in (case_models, case_k3_two_turn, case_k26_tool_loop, case_k26_keep_all, case_k27_two_turn):
        print(f"\n== {runner.__name__} ==")
        result = runner(key)
        cases.append(result)
        for name, layer in result["layers"].items():
            if name == "catalog_diff" and isinstance(layer, dict):
                print(f"  catalog 对账: 端点比目录多 {layer.get('served_not_in_catalog') or '无'}; "
                      f"目录比端点多 {layer.get('catalog_not_served') or '无'}")
                continue
            print("  " + layer_line(name, layer.get("ok"), layer.get("detail", "")))
        for usage_key, usage_value in (result.get("usage") or {}).items():
            print(f"  {usage_key}: {usage_value}")

    failed = [
        case["case"]
        for case in cases
        if any(
            layer.get("ok") is False
            for layer in case["layers"].values()
            if isinstance(layer, dict) and "ok" in layer
        )
    ]
    summary = {
        "probe": "manual.model_probe_kimi_preserved_thinking",
        "mode": "direct",
        "started": started,
        "finished": time.strftime("%Y-%m-%d %H:%M:%S"),
        "base_url": base_url(),
        "key": "sk-***",
        "max_tokens_cap": MAX_TOKENS,
        "cases": cases,
        "failed_cases": failed,
    }
    write_json("summary.json", summary)
    print(f"\n判定: {'FAIL ' + ', '.join(failed) if failed else '全部通过'}")
    print("summary.json 已落盘(只含 hash/长度/逐字相等/usage,不含 reasoning 正文)。")
    return 1 if failed else 0


# ---------------------------------------------------------------------------
# resume: 真 binary + 录制代理
# ---------------------------------------------------------------------------


class RecordingProxy:
    """本地 HTTP 录制代理:转发到 Moonshot 真端点,逐笔记账请求/响应。

    下游(临时 config)拿假钥匙,代理在出口换成真钥匙——真钥匙不进临时
    目录、不进任何落盘文件。
    """

    def __init__(self, upstream_base: str, real_key: str):
        self.upstream_base = upstream_base.rstrip("/")
        self.real_key = real_key
        self.records: list[dict] = []
        self.lock = threading.Lock()
        outer = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):  # 静音:证据自己记,不刷控制台
                return

            def _forward(self, method: str):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length) if length else b""
                path = self.path
                if path.startswith("/v1/"):
                    url = outer.upstream_base + path[3:]
                else:
                    url = outer.upstream_base + path
                headers = {"Content-Type": self.headers.get("Content-Type", "application/json")}
                request = urllib.request.Request(url, data=body if body else None, method=method,
                                                 headers=headers)
                request.add_header("Authorization", "Bearer " + outer.real_key)
                status = 502
                text = ""
                try:
                    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
                        status = response.status
                        text = response.read().decode("utf-8", "replace")
                except urllib.error.HTTPError as error:
                    status = error.code
                    text = error.read().decode("utf-8", "replace")
                except (urllib.error.URLError, TimeoutError, OSError) as error:
                    text = json.dumps({"probe_proxy_error": str(error)})
                payload = text.encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                with outer.lock:
                    outer.records.append(
                        {
                            "method": method,
                            "path": path,
                            "status": status,
                            "request_body": body.decode("utf-8", "replace"),
                            "response_body": text,
                        }
                    )

            def do_POST(self):  # noqa: N802 (http.server 命名)
                self._forward("POST")

            def do_GET(self):  # noqa: N802
                self._forward("GET")

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()

    @property
    def local_base(self) -> str:
        return f"http://127.0.0.1:{self.port}/v1"


def parse_sse(text: str) -> dict:
    """把 SSE 流收成 {reasoning, content, usage, finish}——resume 案的响应侧证据。"""
    reasoning_parts: list[str] = []
    content_parts: list[str] = []
    usage = None
    finish = ""
    tool_call_seen = False
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("data:"):
            continue
        payload = line[5:].strip()
        if not payload or payload == "[DONE]":
            continue
        try:
            chunk = json.loads(payload)
        except ValueError:
            continue
        if isinstance(chunk.get("usage"), dict) and chunk["usage"]:
            usage = chunk["usage"]
        try:
            choice = chunk["choices"][0]
        except (KeyError, IndexError, TypeError):
            continue
        # Moonshot 流式末块把 usage 塞在 choices[0].usage 里(与 OpenAI
        # 顶层的口径不同),两处都看,只记非空那份。
        if isinstance(choice.get("usage"), dict) and choice["usage"]:
            usage = choice["usage"]
        if choice.get("finish_reason"):
            finish = str(choice["finish_reason"])
        delta = choice.get("delta") or {}
        if delta.get("reasoning_content"):
            reasoning_parts.append(delta["reasoning_content"])
        if delta.get("content"):
            content_parts.append(delta["content"])
        if delta.get("tool_calls"):
            tool_call_seen = True
    return {
        "reasoning": "".join(reasoning_parts),
        "content": "".join(content_parts),
        "usage": usage,
        "finish": finish,
        "tool_call_seen": tool_call_seen,
    }


def analyze_resume(root: str, records: list[dict], home: str) -> tuple[dict, int]:
    """resume 链收账:纯读证据,不碰网络——改了解析逻辑可零成本复析。"""
    chats = [
        record
        for record in records
        if record["method"] == "POST" and record["path"].endswith("/chat/completions")
    ]
    layers: dict[str, dict] = {}
    sessions_dir = os.path.join(home, ".lubancode", "sessions")
    session_files = (
        sorted(
            (os.path.join(sessions_dir, name) for name in os.listdir(sessions_dir) if name.endswith(".jsonl")),
            key=os.path.getmtime,
        )
        if os.path.isdir(sessions_dir)
        else []
    )
    session_has_thinking = False
    if session_files:
        with open(session_files[-1], encoding="utf-8") as handle:
            session_text = handle.read()
        for line in session_text.splitlines():
            if '"type":"thinking"' in line or '"type": "thinking"' in line:
                session_has_thinking = True
                break
        shutil.copyfile(session_files[-1], os.path.join(root, "resume_session_copy.jsonl"))
    layers["session_thinking_persisted"] = {
        "ok": session_has_thinking,
        "detail": (
            f"最新会话档 {os.path.basename(session_files[-1]) if session_files else '无'} "
            f"含 type=thinking 事件={session_has_thinking}"
        ),
    }

    if len(chats) < 2:
        layers["L1_accepted"] = {"ok": False, "detail": f"代理只录到 {len(chats)} 笔 chat 请求,恢复链不成立"}
        layers["L2_emitted_reasoning"] = {"ok": False, "detail": "无第一笔响应可收账"}
        layers["L3_echoed_back"] = {"ok": False, "detail": "无第二笔请求可比对"}
        layers["usage"] = {"ok": None, "detail": "不可得(恢复链未成立)"}
        return layers, len(chats)

    first, second = chats[0], chats[-1]
    status1, status2 = first["status"], second["status"]
    sse1, sse2 = parse_sse(first["response_body"]), parse_sse(second["response_body"])
    rc1 = sse1["reasoning"]
    request2 = None
    try:
        request2 = json.loads(second["request_body"])
    except ValueError:
        request2 = None

    layers["L1_accepted"] = {
        "ok": status1 == 200 and status2 == 200,
        "detail": (
            f"第一笔 http={status1}(finish={sse1['finish'] or '未知'});"
            f"续聊第二笔 http={status2}(finish={sse2['finish'] or '未知'})"
        ),
    }
    layers["L2_emitted_reasoning"] = {
        "ok": bool(rc1),
        "detail": (
            f"第一轮流式 reasoning_content 拼接 len={len(rc1)} sha256={sha256_short(rc1)}"
            if rc1
            else "第一轮流里没有 reasoning_content 增量"
        ),
    }
    echo_rc = ""
    thinking_key_sent = request2 is not None and "thinking" in request2
    if request2:
        for message in request2.get("messages", []):
            if message.get("role") == "assistant" and "reasoning_content" in message:
                echo_rc = message["reasoning_content"]
                break
    identical = echo_rc == rc1 and len(echo_rc) == len(rc1)
    layers["L3_echoed_back"] = {
        "ok": identical and status2 == 200 and not thinking_key_sent,
        "detail": (
            f"恢复链第二笔请求体 assistant.reasoning_content 逐字一致={identical} "
            f"len={len(echo_rc)} sha256={sha256_short(echo_rc)};"
            f"第二笔响应有正文={bool(sse2['content'])};"
            f"K3 契约:第二笔未发送 thinking 键={not thinking_key_sent}"
        ),
    }

    def usage_of(usage: dict) -> str:
        if not usage:
            return "不可得(流内无 usage 块)"
        details = usage.get("completion_tokens_details") or {}
        cached = usage.get("cached_tokens")
        if cached is None:
            cached = (usage.get("prompt_tokens_details") or {}).get("cached_tokens", "不可得")
        return (
            f"prompt={usage.get('prompt_tokens', '?')} "
            f"completion={usage.get('completion_tokens', '?')} "
            f"reasoning_tokens={details.get('reasoning_tokens', '不可得')} "
            f"cached_tokens={cached}"
        )

    usage1, usage2 = sse1.get("usage") or {}, sse2.get("usage") or {}
    layers["usage"] = {
        "ok": bool(usage1 and usage2) or None,
        "detail": f"turn1 {usage_of(usage1)}; turn2 {usage_of(usage2)}",
    }
    return layers, len(chats)


def cmd_resume(args) -> int:
    root = evidence_dir()
    home = os.path.join(root, "resume-home")
    if args.analyze_only:
        # 离线复析:只读已录 exchanges 与临时 HOME 里的 session 档,零网络。
        records = []
        for path in sorted(glob.glob(os.path.join(root, "resume_exchange_*.json"))):
            with open(path, encoding="utf-8") as handle:
                records.append(json.load(handle))
        if not records:
            print(f"FAIL: 证据目录里没有 resume_exchange_*.json: {root}")
            return 1
        print(f"离线复析: {len(records)} 笔已录 exchanges;临时 HOME: {home}(不发网络请求)")
        layers, chats_count = analyze_resume(root, records, home)
        return report_resume(root, layers, chats_count, exe="(offline)", proxy_note="已录证据")

    key = load_api_key()
    if not key:
        print("SKIP: 没有 MOONSHOT_API_KEY,用户 config 里也没找到 moonshot 的 api_key。")
        return 0
    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        print(f"FAIL: 找不到 lubancode.exe: {exe}")
        return 1

    root = evidence_dir()
    home = os.path.join(root, "resume-home")
    if os.path.isdir(home):
        shutil.rmtree(home)
    os.makedirs(os.path.join(home, ".lubancode"), exist_ok=True)
    scratch = os.path.join(root, "resume-scratch")
    if os.path.isdir(scratch):
        shutil.rmtree(scratch)
    os.makedirs(scratch, exist_ok=True)

    proxy = RecordingProxy(base_url(), key)
    proxy.start()
    # 临时 config 只带 moonshot 一个 provider;钥匙是假值,真钥匙只在代理出口。
    temp_config = {
        "active_provider": "moonshot",
        "language": "zh",
        "providers": [
            {
                "name": "moonshot",
                "base_url": proxy.local_base,
                "auth": "inline",
                "api_key": "sk-local-probe-only",
                "wire": "openai-chat-completions",
                "model": "kimi-k3",
                "default_model": "kimi-k3",
                "models": ["kimi-k3"],
                "stream_usage": True,
            }
        ],
    }
    with open(os.path.join(home, ".lubancode", "config.json"), "w", encoding="utf-8") as handle:
        json.dump(temp_config, handle, ensure_ascii=False, indent=1)

    env = dict(os.environ)
    env["USERPROFILE"] = home  # Windows 路径,平台层 HomeDir() 只认这个
    env.pop("LUBANCODE_API_KEY", None)

    def run_once(label: str, extra_args: list[str], stdin_text: str) -> subprocess.CompletedProcess:
        print(f"-- {label}: {' '.join([os.path.basename(exe)] + extra_args)}")
        started = time.time()
        result = subprocess.run(
            [exe] + extra_args,
            input=stdin_text.encode("utf-8"),
            capture_output=True,
            cwd=scratch,
            env=env,
            timeout=args.timeout,
        )
        elapsed = time.time() - started
        with open(os.path.join(root, f"resume_{label}_stdout.txt"), "wb") as handle:
            handle.write(result.stdout)
        with open(os.path.join(root, f"resume_{label}_stderr.txt"), "wb") as handle:
            handle.write(result.stderr)
        print(
            f"   exit={result.returncode} 耗时={elapsed:.1f}s "
            f"stdout={len(result.stdout)}B stderr={len(result.stderr)}B"
        )
        return result

    print(f"端点: {base_url()} 经本地录制代理 127.0.0.1:{proxy.port};临时 HOME: {home}")
    try:
        run_once("turn1", [], "用一句话回答:中国最长的河流是哪条?\n/exit\n")
        run_once("turn2_continue", ["--continue"], "接着上面说的,用一句话回答:它流经哪座著名城市?\n/exit\n")
    finally:
        proxy.stop()

    for index, record in enumerate(proxy.records):
        write_json(f"resume_exchange_{index:02d}.json", record)

    layers, chats_count = analyze_resume(root, list(proxy.records), home)
    return report_resume(root, layers, chats_count, exe=exe, proxy_note=f"127.0.0.1:{proxy.port}(已关)")


def report_resume(root: str, layers: dict, chats_count: int, exe: str, proxy_note: str) -> int:
    print()
    ok_flags = []
    for name, layer in layers.items():
        print("  " + layer_line(name, layer.get("ok"), layer.get("detail", "")))
        if layer.get("ok") is not None:
            ok_flags.append(layer.get("ok") is True)
    summary = {
        "probe": "manual.model_probe_kimi_preserved_thinking",
        "mode": "resume",
        "exe": exe,
        "upstream": base_url(),
        "via_local_proxy": proxy_note,
        "finished": time.strftime("%Y-%m-%d %H:%M:%S"),
        "key": "sk-***",
        "layers": layers,
        "chat_requests_recorded": chats_count,
    }
    write_json("resume_summary.json", summary)
    failed = [flag for flag in ok_flags if not flag]
    print(f"\n判定: {'FAIL' if failed else '通过'}(证据目录 {root})")
    return 1 if failed else 0


# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description="Kimi 保留式思考真机探针(P2,opt-in)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("direct", help="直打真端点三案 + 模型清单对账")
    resume = sub.add_parser("resume", help="真 lubancode.exe 恢复链(临时 USERPROFILE + 录制代理)")
    resume.add_argument("--exe", default=os.path.join(REPO_ROOT, "build", "debug", "Debug", "lubancode.exe"))
    resume.add_argument("--timeout", type=int, default=300, help="每轮 binary 超时秒数")
    resume.add_argument("--analyze-only", action="store_true",
                        help="零网络复析已录证据(改了解析逻辑后复跑账)")
    args = parser.parse_args()
    if args.command == "direct":
        return cmd_direct(args)
    return cmd_resume(args)


if __name__ == "__main__":
    sys.exit(main())
