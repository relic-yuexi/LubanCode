#!/usr/bin/env python3
"""code-stack 夹具的 MCP stdio 服务器:换行分隔的 JSON-RPC 2.0。

支持 initialize / notifications/initialized / tools/list / tools/call,
两个工具:ping -> 应声,note{text} -> 记一笔并回执。不认得的方法回
-32601。stdout 只写协议帧,日志全走 stderr。

环境变量 LUBANCODE_TEST_MCP_PID_FILE 设了的话,启动时把自己的 PID 写进
那个文件——供"整包回滚后进程零残留"的断言用。不设不写,行为不变。
"""
import json
import os
import sys

if hasattr(sys.stdin, "reconfigure"):
    sys.stdin.reconfigure(encoding="utf-8")
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

PID_FILE = os.environ.get("LUBANCODE_TEST_MCP_PID_FILE", "")

TOOLS = [
    {
        "name": "ping",
        "description": "应一声,回 generation。",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "note",
        "description": "记一笔文本,回执带序号。",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
]

generation = 0


def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def handle(request):
    method = request.get("method", "")
    msg_id = request.get("id")
    params = request.get("params", {}) or {}

    if method == "initialize":
        send({
            "jsonrpc": "2.0",
            "id": msg_id,
            "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "ledger-fixture", "version": "0.1.0"},
            },
        })
        return
    if method == "notifications/initialized":
        return
    if method == "tools/list":
        send({"jsonrpc": "2.0", "id": msg_id, "result": {"tools": TOOLS}})
        return
    if method == "tools/call":
        global generation
        generation += 1
        name = params.get("name", "")
        arguments = params.get("arguments", {}) or {}
        if name == "ping":
            text = "pong %d" % generation
        elif name == "note":
            text = "记下 #%d: %s" % (generation, arguments.get("text", ""))
        else:
            send({"jsonrpc": "2.0", "id": msg_id,
                  "error": {"code": -32601, "message": "unknown tool: " + name}})
            return
        send({"jsonrpc": "2.0", "id": msg_id,
              "result": {"content": [{"type": "text", "text": text}], "isError": False}})
        return
    if msg_id is not None:
        send({"jsonrpc": "2.0", "id": msg_id,
              "error": {"code": -32601, "message": "unknown method: " + method}})


def main():
    if PID_FILE:
        try:
            with open(PID_FILE, "w", encoding="utf-8") as pid_out:
                pid_out.write(str(os.getpid()))
        except OSError:
            pass  # 写不动就算了,断言侧自会说话
    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue
        try:
            handle(json.loads(line))
        except json.JSONDecodeError:
            continue


if __name__ == "__main__":
    main()
