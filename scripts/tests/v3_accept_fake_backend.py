# -*- coding: utf-8 -*-
"""轨迹 v3 §5.1 验收用假 Anthropic Messages 后端。

一只极小的 http.server:只认本机回环,POST /v1/messages 回一段最小合法
SSE(anthropic-messages wire)。剧本(--script)是 JSON 数组,逐请求按序
消费,每项:

  {"text": "答话正文", "usage": {"input_tokens": 10, "output_tokens": 3}}
  {"tool_use": {"id": "toolu_01", "name": "read_file",
                "input": {"path": "notes.txt"}},
   "usage": null}          # usage 缺省/为 null = message_delta 不带 usage 对象
                           # (provider 缺报场景:账上须落 null 不补 0)

剧本耗尽后回 500 + error 事件并记一笔 exhausted(测试据此抓超计划请求)。
每个请求(含超计划的)原样落 --log 指定的 JSONL:n / path / body 全文,
验收脚本靠它断言"零模型调用"与请求体形状。

用法:
  python v3_accept_fake_backend.py --port 8791 \
      --script script.json --log requests.jsonl
"""
import argparse
import json
import socket
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def sse(frames):
    out = []
    for event_type, payload in frames:
        out.append("event: " + event_type)
        out.append("data: " + json.dumps(payload, ensure_ascii=False))
        out.append("")
    return ("\n".join(out) + "\n").encode("utf-8")


def build_frames(script_item, request_number):
    """一个剧本项 -> anthropic-messages SSE 帧序列。"""
    message_id = "fake_msg_%03d" % request_number
    usage = script_item.get("usage")
    tool_use = script_item.get("tool_use")
    text = script_item.get("text")
    stop_reason = script_item.get("stop_reason", "end_turn")

    frames = [
        ("message_start", {"type": "message_start",
                           "message": {"id": message_id, "model": "fake-model",
                                       "role": "assistant"}}),
        ("ping", {"type": "ping"}),
    ]
    index = 0
    if text:
        frames.append(("content_block_start",
                       {"type": "content_block_start", "index": index,
                        "content_block": {"type": "text", "text": ""}}))
        frames.append(("content_block_delta",
                       {"type": "content_block_delta", "index": index,
                        "delta": {"type": "text_delta", "text": text}}))
        frames.append(("content_block_stop",
                       {"type": "content_block_stop", "index": index}))
        index += 1
    if tool_use:
        frames.append(("content_block_start",
                       {"type": "content_block_start", "index": index,
                        "content_block": {"type": "tool_use",
                                          "id": tool_use["id"],
                                          "name": tool_use["name"],
                                          "input": {}}}))
        frames.append(("content_block_delta",
                       {"type": "content_block_delta", "index": index,
                        "delta": {"type": "input_json_delta",
                                  "partial_json": json.dumps(
                                      tool_util_input(tool_use),
                                      ensure_ascii=False)}}))
        frames.append(("content_block_stop",
                       {"type": "content_block_stop", "index": index}))
        index += 1
        stop_reason = "tool_use"
    delta = {"type": "message_delta", "delta": {"stop_reason": stop_reason}}
    if usage is not None:
        delta["usage"] = {
            "input_tokens": usage.get("input_tokens", 0),
            "output_tokens": usage.get("output_tokens", 0),
        }
    frames.append(("message_delta", delta))
    frames.append(("message_stop", {"type": "message_stop"}))
    return frames


def tool_util_input(tool_use):
    """剧本里 input 缺省给 {"path": "notes.txt"}(验收主用 read_file)。"""
    return tool_use.get("input", {"path": "notes.txt"})


class State:
    def __init__(self, script, log_path):
        self.lock = threading.Lock()
        self.script = script
        self.log_path = log_path
        self.count = 0

    def next_item(self, body, path):
        with self.lock:
            self.count += 1
            exhausted = self.count > len(self.script)
            item = None if exhausted else self.script[self.count - 1]
            with open(self.log_path, "a", encoding="utf-8") as handle:
                handle.write(json.dumps(
                    {"n": self.count, "path": path, "exhausted": exhausted,
                     "body": body}, ensure_ascii=False) + "\n")
            return item, self.count


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass  # 静音默认访问日志(验收日志走 --log)

        def do_GET(self):
            if self.path == "/__count":
                body = json.dumps({"count": state.count}).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if length else b""
            try:
                body = json.loads(raw.decode("utf-8"))
            except Exception:
                body = {"_unparsable": raw.decode("utf-8", "replace")}
            if self.path.rstrip("/") != "/v1/messages":
                self._json_error(404, "unexpected path: " + self.path)
                return
            item, number = state.next_item(body, self.path)
            if item is None:
                frames = [("error", {"type": "error",
                                     "error": {"type": "api_error",
                                               "message": "script exhausted"}})]
                payload = sse(frames)
                self.send_response(200)  # SSE 通路的错也走流
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            payload = sse(build_frames(item, number))
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def _json_error(self, code, message):
            body = json.dumps({"error": message}).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=0,
                        help="监听端口(0 = 现挑空闲口,挑中后打印到 stdout)")
    parser.add_argument("--script", required=True, help="剧本 JSON 文件")
    parser.add_argument("--log", required=True, help="请求账 JSONL 落点")
    args = parser.parse_args()

    with open(args.script, "r", encoding="utf-8") as handle:
        script = json.load(handle)
    open(args.log, "w", encoding="utf-8").close()

    port = args.port
    if port == 0:
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]

    state = State(script, args.log)
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(state))
    print("LISTEN %d" % port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
