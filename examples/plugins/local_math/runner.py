# -*- coding: utf-8 -*-
"""local-math process 插件 runner(plugins 单第 9 步的 Python 示例)。

协议 v1:stdin 恰好一份 JSON 请求,stdout 恰好一份 JSON 响应。
日志只写 stderr;stdout 前后混任何字节都会被判协议错。
不加锁、不起线程、不自建 server——进程退出即调用结束。
"""
import json
import sys


def add(arguments):
    return arguments["a"] + arguments["b"]


HANDLERS = {
    "add": add,
}


def main():
    # Windows 管道下 Python 默认按本地代码页:中文先钉死 UTF-8。
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, OSError):
            pass
    request = json.load(sys.stdin)
    handler = HANDLERS.get(request.get("tool", ""))
    if handler is None:
        json.dump({
            "protocol": 1,
            "call_id": request.get("call_id", ""),
            "ok": False,
            "error": {"code": "unknown_tool", "message": "不认得的工具"},
        }, sys.stdout, ensure_ascii=False)
        return
    try:
        value = handler(request.get("arguments", {}))
    except Exception as error:  # noqa: BLE001 - 协议要求任何业务失败都回失败帧
        json.dump({
            "protocol": 1,
            "call_id": request.get("call_id", ""),
            "ok": False,
            "error": {"code": "execution_failed", "message": str(error)},
        }, sys.stdout, ensure_ascii=False)
        return
    json.dump({
        "protocol": 1,
        "call_id": request.get("call_id", ""),
        "ok": True,
        "content": [{"type": "text", "text": str(value)}],
        "structured": value,
    }, sys.stdout, ensure_ascii=False)


if __name__ == "__main__":
    main()
