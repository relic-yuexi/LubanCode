#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Claude 原生工具搜索(defer_loading + server tool search)真机 cache usage 对照。

动态工具 PromptCache 守恒单 P3 §十三第 5 条的对照夹具。两栏口径(单子
§11.2):structural_expected(客户端前缀确实守恒)与 provider_reported
(usage 回没回 cache read/creation);没有 usage 就写 unknown,不写 0。

对照设计:
  档 A(Disabled 基线):全部工具 eager,不带 server tool search 声明。
  档 B(NativeReference):延迟工具带 defer_loading:true,tools 里声明
    tool_search_tool_regex_20251119;对话走三拍——R1 用户问,R2 带着服务端
    搜索的 server_tool_use + tool_search_tool_result 原样回传 + 直调真实
    工具的 tool_result。
两档各跑同一条三拍对话,逐请求记:
  input_tokens / cache_read_input_tokens / cache_creation_input_tokens /
  output_tokens / stop_reason / tools 里的 defer_loading 计数。
出账按 §11.3 口径:不只看 cached ratio,完整 input、非缓存 input、额外轮次
一并打印。

用法(需要真钥匙;本机没有,故本脚本在本单内【未验证】):
  export ANTHROPIC_API_KEY=sk-ant-...
  export ANTHROPIC_BASE_URL=https://api.anthropic.com   # 缺省即官方
  python tests/manual/native_tool_search_cache_compare.py --model claude-opus-5

状态:未验(2026-08-31,P3 落地时本机无 Anthropic 钥匙/真机)。结构侧的
三拍前缀合同由 tests/unit/api/test_request_prefix.cpp 的"P3三拍前缀合同"
册与 tests/integration/api/test_wire_replay.cpp 的原生工具搜索回环册钉死;
本脚本补的是 provider_reported 那一栏,等钥匙到位后补测,不冒充已实测。
"""

import argparse
import json
import os
import sys
import urllib.request

API_VERSION_HEADER = "2023-06-01"

# 延迟目录:模拟一揽子 MCP 工具。真机对照要的是"定义很多、只调一枚"的
# 形状,工具正文长一点才看得见 defer_loading 的差别。
DEFERRED_TOOL_COUNT = 12


def build_tools(native: bool) -> list:
    tools = [
        {
            "name": "read_file",
            "description": "读取工作区一个文件的内容。",
            "input_schema": {
                "type": "object",
                "properties": {"path": {"type": "string", "description": "文件路径"}},
                "required": ["path"],
            },
        }
    ]
    for index in range(DEFERRED_TOOL_COUNT):
        tool = {
            "name": f"mcp__demo__tool_{index:02d}",
            "description": f"演示 MCP 工具 {index:02d}:" + "用于撑大工具定义体积的说明文字。" * 8,
            "input_schema": {
                "type": "object",
                "properties": {
                    "query": {"type": "string", "description": "检索词"},
                    "limit": {"type": "integer", "description": "上限"},
                },
                "required": ["query"],
            },
        }
        if native:
            tool["defer_loading"] = True
        tools.append(tool)
    if native:
        tools.append({"type": "tool_search_tool_regex_20251119", "name": "tool_search_tool_regex"})
    return tools


def post(base_url: str, api_key: str, model: str, body: dict) -> dict:
    request = urllib.request.Request(
        base_url.rstrip("/") + "/v1/messages",
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "x-api-key": api_key,
            "anthropic-version": API_VERSION_HEADER,
        },
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.loads(response.read().decode("utf-8"))


def run_track(base_url: str, api_key: str, model: str, native: bool) -> list:
    """跑一条三拍对话,逐请求返回 (usage, stop_reason, assistant_content)。"""
    tools = build_tools(native)
    messages = [{"role": "user", "content": "用工具查一下 demo 07 能干什么,然后直接调用它。"}]
    records = []
    for step in range(3):
        body = {
            "model": model,
            "max_tokens": 2048,
            "stream": False,
            "tools": tools,
            "messages": messages,
        }
        if native and step >= 1:
            # 第二拍起,把第一拍响应里的原生对块原样塞回 assistant content
            # (server_tool_use + tool_search_tool_result 不配 tool_result)。
            pass
        answer = post(base_url, api_key, model, body)
        usage = answer.get("usage", {})
        records.append(
            {
                "step": step,
                "stop_reason": answer.get("stop_reason", ""),
                "usage": usage,
                "cache_reported": "cache_read_input_tokens" in usage
                or "cache_creation_input_tokens" in usage,
                "content_types": [block.get("type", "") for block in answer.get("content", [])],
            }
        )
        messages.append({"role": "assistant", "content": answer.get("content", [])})
        # 停止即收:没有 tool_use 就不必再发。
        if answer.get("stop_reason") != "tool_use":
            break
        # 给每枚本地 tool_use 补 tool_result(server 块不配)。
        results = []
        for block in answer.get("content", []):
            if block.get("type") == "tool_use":
                results.append(
                    {"type": "tool_result", "tool_use_id": block["id"], "content": "演示结果 ok"}
                )
        if results:
            messages.append({"role": "user", "content": results})
    return records


def report(track_name: str, records: list) -> None:
    print(f"\n== {track_name} ==")
    for record in records:
        usage = record["usage"]
        cache_read = usage.get("cache_read_input_tokens")
        cache_write = usage.get("cache_creation_input_tokens")
        print(
            f"  step {record['step']} stop={record['stop_reason']:<10} "
            f"input={usage.get('input_tokens', 'unknown')} "
            f"cache_read={cache_read if cache_read is not None else 'unknown'} "
            f"cache_write={cache_write if cache_write is not None else 'unknown'} "
            f"output={usage.get('output_tokens', 'unknown')} "
            f"blocks={','.join(record['content_types'])}"
        )
    if not any(record["cache_reported"] for record in records):
        print("  provider_reported: unknown(整条对话没有回任何 cache 字段,不写 0)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default="claude-opus-5", help="目录里声明了 deferred_tools 的模型")
    args = parser.parse_args()

    api_key = os.environ.get("ANTHROPIC_API_KEY", "")
    base_url = os.environ.get("ANTHROPIC_BASE_URL", "https://api.anthropic.com")
    if not api_key:
        print(
            "没有 ANTHROPIC_API_KEY:本脚本只在有真钥匙的环境跑,"
            "P3 落地时本机无钥匙,该栏如实留空(单子 §十三 P3 第 5 条标注未验)。",
            file=sys.stderr,
        )
        return 2

    report("档 A:Disabled(全量 eager 基线)", run_track(base_url, api_key, args.model, native=False))
    report("档 B:NativeReference(defer_loading + 服务端搜索)", run_track(base_url, api_key, args.model, native=True))
    print(
        "\n口径:structural_expected 由单测钉死(provider 不参与);本脚本补 "
        "provider_reported 一栏。cached ratio 上升不等于总成本下降(§11.3),"
        "额外轮次与非缓存 input 一并看了再下结论。"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
