#!/usr/bin/env python3
"""LSP 测试夹具:一个最小的 LSP stdio 服务器,给单测/集成测试用——照
mcp_test_server.py 的路数,但协议分帧换成 LSP 的 Content-Length 头:

    Content-Length: N\r\n
    \r\n
    <N 字节 JSON 正文>

应答这些方法:
  initialize                  -> 最小 capabilities + serverInfo
  initialized                 -> 通知,忽略
  textDocument/didOpen        -> 通知;顺手对这个 uri 推一条假诊断
                                 (publishDiagnostics,警告,第 1 行)
  textDocument/definition     -> 同文件 0 基第 2 行第 4 列的一个 Location
  textDocument/references     -> 同文件两处 Location(0 基第 0 行、第 4 行)
  textDocument/documentSymbol -> 层级式 DocumentSymbol:top_func(函数,
                                 0 基第 2 行)带一个子符号 inner(变量,
                                 0 基第 3 行)
  shutdown                    -> result: null
  exit                        -> 退出进程

不认得的方法:有 id 就回 JSON-RPC 错误(-32601),没有 id(通知)静默
忽略。单测和集成拿它当靶,不依赖机器装没装 clangd。
"""
import json
import sys


def read_message(stream):
    """按 Content-Length 头读一条完整消息,流断了返回 None。"""
    content_length = None
    while True:
        line = stream.readline()
        if not line:
            return None
        line = line.decode("utf-8", errors="replace").strip()
        if not line:
            break  # 头部块结束
        if ":" in line:
            name, _, value = line.partition(":")
            if name.strip().lower() == "content-length":
                try:
                    content_length = int(value.strip())
                except ValueError:
                    content_length = None
    if content_length is None:
        return None
    body = stream.read(content_length)
    if body is None or len(body) < content_length:
        return None
    try:
        return json.loads(body.decode("utf-8", errors="replace"))
    except json.JSONDecodeError:
        return {}


def send(msg):
    body = json.dumps(msg).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    sys.stdout.buffer.write(header + body)
    sys.stdout.buffer.flush()


def send_result(msg_id, result):
    send({"jsonrpc": "2.0", "id": msg_id, "result": result})


def send_error(msg_id, code, message):
    send({"jsonrpc": "2.0", "id": msg_id, "error": {"code": code, "message": message}})


def location(uri, line, character, end_character):
    return {
        "uri": uri,
        "range": {
            "start": {"line": line, "character": character},
            "end": {"line": line, "character": end_character},
        },
    }


def handle_did_open(params):
    uri = params.get("textDocument", {}).get("uri", "")
    if not uri:
        return
    # didOpen 之后主动推一条假诊断——lubancode 那边的诊断缓存就靠这个喂。
    send(
        {
            "jsonrpc": "2.0",
            "method": "textDocument/publishDiagnostics",
            "params": {
                "uri": uri,
                "diagnostics": [
                    {
                        "range": {
                            "start": {"line": 0, "character": 0},
                            "end": {"line": 0, "character": 5},
                        },
                        "severity": 2,
                        "message": "fixture-warning: 测试诊断",
                    }
                ],
            },
        }
    )


def main():
    stdin = sys.stdin.buffer
    while True:
        msg = read_message(stdin)
        if msg is None:
            break

        method = msg.get("method", "")
        msg_id = msg.get("id")
        params = msg.get("params", {}) or {}

        if method == "initialize":
            send_result(
                msg_id,
                {
                    "capabilities": {
                        "textDocumentSync": 1,
                        "definitionProvider": True,
                        "referencesProvider": True,
                        "documentSymbolProvider": True,
                    },
                    "serverInfo": {"name": "lsp_test_server", "version": "0.1.0"},
                },
            )
        elif method == "initialized":
            pass  # 通知,不用回应
        elif method == "textDocument/didOpen":
            handle_did_open(params)
        elif method == "textDocument/definition":
            uri = params.get("textDocument", {}).get("uri", "")
            send_result(msg_id, [location(uri, 2, 4, 12)])
        elif method == "textDocument/references":
            uri = params.get("textDocument", {}).get("uri", "")
            send_result(msg_id, [location(uri, 0, 0, 8), location(uri, 4, 4, 12)])
        elif method == "textDocument/documentSymbol":
            send_result(
                msg_id,
                [
                    {
                        "name": "top_func",
                        "kind": 12,
                        "range": {
                            "start": {"line": 2, "character": 0},
                            "end": {"line": 4, "character": 0},
                        },
                        "selectionRange": {
                            "start": {"line": 2, "character": 4},
                            "end": {"line": 2, "character": 12},
                        },
                        "children": [
                            {
                                "name": "inner",
                                "kind": 13,
                                "range": {
                                    "start": {"line": 3, "character": 4},
                                    "end": {"line": 3, "character": 12},
                                },
                                "selectionRange": {
                                    "start": {"line": 3, "character": 4},
                                    "end": {"line": 3, "character": 9},
                                },
                            }
                        ],
                    }
                ],
            )
        elif method == "shutdown":
            send_result(msg_id, None)
        elif method == "exit":
            break
        else:
            if msg_id is not None:
                send_error(msg_id, -32601, "unknown method: " + method)
            # 没有 id 的未知通知,静默忽略


if __name__ == "__main__":
    main()
