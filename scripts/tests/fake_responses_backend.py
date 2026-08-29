# -*- coding: utf-8 -*-
"""问题 9 冒烟用的假 Responses 后端。

一只极小的 http.server:接 POST .../responses,回一段最小合法 SSE。
usage 里的 cached_tokens 按"剧本"逐请求给——烧瓶脚本用 --script 选:

  all-hit      每笔都报大额 cached_tokens(全命中)
  all-miss     每笔 cached_tokens = 0(全 miss,本地前缀稳定 -> 面板应分型
               "上游未命中")
  alternate    命中/未命中交替(间歇抖动,复刻问题单里 0%/98% 的形状)
  mixed-silent 奇数笔报命中、偶数笔整个不发 usage(缺测行与命中行并见,
               面板应写"未回报",不冒充 0%)
  probe        供 /doctor cache probe:探针请求(正文含固定填充段)按
               探针剧本回:首轮 miss、后续轮全额命中(稳定命中形状)

日常请求与探针请求按正文里有没有 "prefix-cache probe filler" 分流,
互不干扰。只认本机回环,不落任何请求正文到磁盘。
"""
import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FILLER_MARK = "prefix-cache probe filler"
PROBE_PREFIX_TOKENS = 2048  # 探针固定前缀段的 token 预算(与 C++ 侧设计值同量级)


def sse_events(events):
    out = []
    for event in events:
        out.append("data: " + json.dumps(event, ensure_ascii=False))
    return ("\n\n".join(out) + "\n\n").encode("utf-8")


def message_stream(request_id, text, usage):
    """一段最小合法的 assistant 消息 SSE(含 usage 的 response.completed)。"""
    events = [
        {"type": "response.created", "response": {"id": request_id, "status": "queued"}},
        {"type": "response.in_progress", "response": {"id": request_id, "status": "in_progress"}},
        {
            "type": "response.output_item.added",
            "output_index": 0,
            "item": {"id": "msg_1", "type": "message", "role": "assistant", "status": "in_progress",
                     "content": []},
        },
        {"type": "response.output_text.delta", "item_id": "msg_1", "output_index": 0,
         "content_index": 0, "delta": text},
        {
            "type": "response.output_item.done",
            "output_index": 0,
            "item": {"id": "msg_1", "type": "message", "role": "assistant", "status": "completed",
                     "content": [{"type": "output_text", "text": text}]},
        },
        {
            "type": "response.completed",
            "response": {
                "id": request_id,
                "status": "completed",
                "output": [],
                "usage": usage,
            },
        },
    ]
    return sse_events(events)


def usage_with(total_input, cached, output_tokens=3):
    return {
        "input_tokens": total_input,
        "output_tokens": output_tokens,
        "total_tokens": total_input + output_tokens,
        "input_tokens_details": {"cached_tokens": cached},
    }


class Handler(BaseHTTPRequestHandler):
    script = "all-hit"
    counter = {"chat": 0, "probe": 0}

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", errors="replace")
        try:
            payload = json.loads(body)
        except ValueError:
            payload = {}
        raw = json.dumps(payload, ensure_ascii=False)
        is_probe = FILLER_MARK in raw
        bucket = "probe" if is_probe else "chat"
        self.counter[bucket] += 1
        nth = self.counter[bucket]
        request_id = "resp_fake_%s_%d" % (bucket, nth)

        if is_probe:
            usage = self.probe_usage(nth)
        else:
            usage = self.chat_usage(nth)
        if usage is None:  # unreported:completed 里不带 usage
            stream = self.stream_without_usage(request_id)
        else:
            stream = message_stream(request_id, "ok", usage)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(stream)))
        self.end_headers()
        self.wfile.write(stream)

    def stream_without_usage(self, request_id):
        events = [
            {"type": "response.created", "response": {"id": request_id, "status": "queued"}},
            {
                "type": "response.output_item.added",
                "output_index": 0,
                "item": {"id": "msg_1", "type": "message", "role": "assistant",
                         "status": "in_progress", "content": []},
            },
            {"type": "response.output_text.delta", "item_id": "msg_1", "output_index": 0,
             "content_index": 0, "delta": "ok"},
            {
                "type": "response.output_item.done",
                "output_index": 0,
                "item": {"id": "msg_1", "type": "message", "role": "assistant",
                         "status": "completed",
                         "content": [{"type": "output_text", "text": "ok"}]},
            },
            {"type": "response.completed",
             "response": {"id": request_id, "status": "completed", "output": []}},
        ]
        return sse_events(events)

    def chat_usage(self, nth):
        script = self.script
        if script == "mixed-silent" and nth % 2 == 0:
            return None  # 偶数笔缺测:completed 不带 usage
        total = 6000 + nth * 100  # 逐请求略涨:追加律下的自然形状
        if script == "all-hit":
            return usage_with(total, max(0, total - 400))
        if script == "all-miss":
            return usage_with(total, 0)
        if script == "alternate":
            cached = total - 400 if nth % 2 == 1 else 0
            return usage_with(total, cached)
        return usage_with(total, 0)

    def probe_usage(self, nth):
        # 探针剧本:首轮 miss(写缓存),后续轮全额命中 -> 稳定命中形状。
        if nth == 1:
            return usage_with(PROBE_PREFIX_TOKENS + 40, 0, output_tokens=1)
        return usage_with(PROBE_PREFIX_TOKENS + 40, PROBE_PREFIX_TOKENS, output_tokens=1)

    def log_message(self, fmt, *args):
        sys.stderr.write("[fake-backend] " + (fmt % args) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--script", default="all-hit",
                        choices=["all-hit", "all-miss", "alternate", "mixed-silent"])
    args = parser.parse_args()
    Handler.script = args.script
    Handler.counter = {"chat": 0, "probe": 0}
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("FAKE_BACKEND_READY %d" % server.server_address[1], flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
