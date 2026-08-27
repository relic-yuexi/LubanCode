#!/usr/bin/env python3
"""M8 测试夹具:一个最小的 MCP stdio 服务器,给单测/集成测试用。

协议:换行分隔的 JSON-RPC 2.0,一行一条完整消息。支持
initialize / notifications/initialized / tools/list / tools/call,
两个工具:
  echo{text: string}  -> 原样返回 text
  add{a: number, b: number} -> 返回 a+b 的字符串

不认得的方法:有 id 就回一条 JSON-RPC 错误(-32601),没有 id(通知)就
静默忽略——跟真实 MCP 服务器该有的行为一致。
"""
import json
import sys

# Windows 下 Python 的 stdin/stdout 默认编码跟着系统代码页走(中文 Windows
# 常是 GBK/cp936),不是 UTF-8——lubancode 那边发的是 UTF-8 字节,不重新绑定
# 编码的话,中文参数读出来就是乱码(先按错的编码解码,再按 ensure_ascii
# 转义,一步錯全錯)。这里强制两边都用 UTF-8,跟协议本身"stdio 是 UTF-8
# 文本"的预期对上。
if hasattr(sys.stdin, "reconfigure"):
    sys.stdin.reconfigure(encoding="utf-8")
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

BAD_TOOLS_CALL = "--bad-tools-call" in sys.argv[1:]

# 1x1 像素 PNG(MCP 富结果单 P0.7 夹具):67 字节,魔数/尺寸客户端可验。
TINY_PNG_B64 = (
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQ"
    "AAAABJRU5ErkJggg=="
)
# 44 字节静音 WAV:RIFF/WAVE 魔数齐全。
TINY_WAV_B64 = (
    "UklGRiQAAABXQVZFZm10IBAAAAABAAEAQB8AAEAfAAABAAgAZGF0YQAAAAA="
)

TOOLS = [
    {
        "name": "echo",
        "description": "原样返回传入的文本",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string"}},
            "required": ["text"],
        },
    },
    {
        "name": "add",
        "description": "返回两个数字的和",
        "inputSchema": {
            "type": "object",
            "properties": {
                "a": {"type": "number"},
                "b": {"type": "number"},
            },
            "required": ["a", "b"],
        },
    },
    {
        "name": "rich",
        "description": "按 kind 返回富结果内容块(text/image/audio/resource_link/resource_text/resource_blob/mixed/unknown)",
        "title": "富结果样例工具",
        "inputSchema": {
            "type": "object",
            "properties": {"kind": {"type": "string"}},
            "required": ["kind"],
        },
        "annotations": {"title": "富结果样例", "readOnlyHint": True},
    },
    {
        "name": "structured",
        "description": "返回 structuredContent(声明了 outputSchema)",
        "inputSchema": {"type": "object", "properties": {}},
        "outputSchema": {
            "type": "object",
            "properties": {"answer": {"type": "integer"}, "label": {"type": "string"}},
            "required": ["answer"],
        },
    },
    {
        "name": "bad_structured",
        "description": "返回不合自己 outputSchema 的 structuredContent(客户端须报 schema 不合)",
        "inputSchema": {"type": "object", "properties": {}},
        "outputSchema": {
            "type": "object",
            "properties": {"answer": {"type": "integer"}},
            "required": ["answer"],
        },
    },
    {
        "name": "bad_image",
        "description": "返回伪 MIME 图片(image/png 声明、字节不是 PNG)",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def send(msg):
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def send_result(msg_id, result):
    send({"jsonrpc": "2.0", "id": msg_id, "result": result})


def send_error(msg_id, code, message):
    send({"jsonrpc": "2.0", "id": msg_id, "error": {"code": code, "message": message}})


def handle_initialize(msg_id, _params):
    send_result(
        msg_id,
        {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "serverInfo": {"name": "mcp_test_server", "version": "0.1.0"},
        },
    )


def handle_tools_list(msg_id, _params):
    send_result(msg_id, {"tools": TOOLS})


def handle_tools_call(msg_id, params):
    name = params.get("name", "")
    arguments = params.get("arguments", {}) or {}

    # 0.13.1 加固验证:--bad-tools-call 启动的"坏响应模式"——tools/call 的
    # 响应字段类型全是错的(type/text 是数字、isError 是字符串),客户端
    # 必须不崩、翻译成可读的 is_error 结果。initialize/tools/list 不受影响。
    if BAD_TOOLS_CALL:
        send_result(msg_id, {"content": [{"type": 123, "text": 456}], "isError": "yes"})
        return

    # 0.13.1 加固验证:die 工具(不在 tools/list 里公开)——收到调用后一声
    # 不吭直接退出进程,模拟"服务器在 tools/call 等待期间死掉",客户端要
    # 靠 IsAlive 轮询快速失败,不许傻等满 120s 超时。
    if name == "die":
        sys.stdout.flush()
        sys.exit(0)

    if name == "echo":
        text = arguments.get("text", "")
        send_result(msg_id, {"content": [{"type": "text", "text": text}], "isError": False})
        return

    if name == "add":
        a = arguments.get("a", 0)
        b = arguments.get("b", 0)
        send_result(msg_id, {"content": [{"type": "text", "text": str(a + b)}], "isError": False})
        return

    if name == "rich":
        kind = arguments.get("kind", "")
        blocks = []
        if kind == "text":
            blocks = [{"type": "text", "text": "只有文本"}]
        elif kind == "image":
            blocks = [
                {"type": "text", "text": "截图如下"},
                {"type": "image", "data": TINY_PNG_B64, "mimeType": "image/png"},
            ]
        elif kind == "audio":
            blocks = [{"type": "audio", "data": TINY_WAV_B64, "mimeType": "audio/wav"}]
        elif kind == "resource_link":
            blocks = [
                {
                    "type": "resource_link",
                    "uri": "file:///reports/q3.md",
                    "name": "q3.md",
                    "title": "三季度报告",
                    "mimeType": "text/markdown",
                    "size": 4096,
                }
            ]
        elif kind == "resource_text":
            blocks = [
                {
                    "type": "resource",
                    "resource": {
                        "uri": "file:///notes/a.txt",
                        "mimeType": "text/plain",
                        "text": "内嵌文本资源的正文",
                    },
                }
            ]
        elif kind == "resource_blob":
            blocks = [
                {
                    "type": "resource",
                    "resource": {
                        "uri": "file:///blob/x.bin",
                        "mimeType": "application/zip",
                        "blob": TINY_WAV_B64,
                    },
                }
            ]
        elif kind == "mixed":
            blocks = [
                {"type": "text", "text": "开头"},
                {"type": "image", "data": TINY_PNG_B64, "mimeType": "image/png"},
                {"type": "resource_link", "uri": "file:///m.md", "name": "m.md"},
                {"type": "text", "text": "结尾"},
            ]
        elif kind == "unknown":
            blocks = [{"type": "video", "data": "AAAA"}]
        else:
            send_error(msg_id, -32602, "rich: unknown kind " + kind)
            return
        send_result(msg_id, {"content": blocks, "isError": False})
        return

    if name == "structured":
        send_result(
            msg_id,
            {
                "content": [{"type": "text", "text": "结构化结果见 structuredContent"}],
                "structuredContent": {"answer": 42, "label": "四十二"},
                "isError": False,
            },
        )
        return

    if name == "bad_structured":
        # answer 给字符串,不合声明的 integer——客户端须按 outputSchema 拦。
        send_result(
            msg_id,
            {
                "content": [{"type": "text", "text": "形状不对"}],
                "structuredContent": {"answer": "不是整数"},
                "isError": False,
            },
        )
        return

    if name == "bad_image":
        # 声明 image/png,字节是纯文本:伪 MIME,客户端须拒绝。
        import base64

        fake = base64.b64encode(b"this is definitely not a png file").decode("ascii")
        send_result(
            msg_id,
            {"content": [{"type": "image", "data": fake, "mimeType": "image/png"}], "isError": False},
        )
        return

    send_error(msg_id, -32601, "unknown tool: " + name)


def main():
    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        method = msg.get("method", "")
        msg_id = msg.get("id")

        if method == "initialize":
            handle_initialize(msg_id, msg.get("params", {}))
        elif method == "notifications/initialized":
            pass  # 通知,不用回应
        elif method == "tools/list":
            handle_tools_list(msg_id, msg.get("params", {}))
        elif method == "tools/call":
            handle_tools_call(msg_id, msg.get("params", {}))
        else:
            if msg_id is not None:
                send_error(msg_id, -32601, "unknown method: " + method)
            # 没有 id 的未知通知,静默忽略


if __name__ == "__main__":
    main()
