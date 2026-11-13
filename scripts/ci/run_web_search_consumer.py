#!/usr/bin/env python3
"""Remote-CI acceptance: installed public caller against an owned HTTP fixture."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading


def validate_wire(method: str, target: str, headers, body: bytes) -> tuple[str, bool, bool]:
    path = target.split("?", 1)[0]
    if path == "/provider/1":
        return path, (method == "GET" and target == "/provider/1?q=a%26b%20%E4%B8%AD%E6%96%87&count=2"
                      and headers.get("X-Subscription-Token") == "fixture-search-key" and not body), False
    try:
        payload = json.loads(body)
    except (ValueError, UnicodeError):
        return path, False, False
    if not isinstance(payload, dict):
        return path, False, False
    if path == "/provider/2":
        return path, (method == "POST" and payload == {"q": "a&b 中文", "num": 2}
                      and headers.get("X-API-KEY") == "fixture-search-key"), False
    key = headers.get("Authorization")
    resumed = path == "/resume" and key == "Bearer resumed-search-key"
    count = 1 if path == "/isolation" and key == "Bearer fixture-search-key" else 2
    allowed = {"Bearer fixture-search-key"}
    if path == "/resume":
        allowed.add("Bearer resumed-search-key")
    if path == "/isolation":
        allowed.add("Bearer isolated-search-key")
    return path, (method == "POST" and payload == {"query": "a&b 中文", "max_results": count}
                  and key in allowed), resumed


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--consumer", required=True, type=Path)
    parser.add_argument("--state", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    args = parser.parse_args()
    args.state.mkdir(parents=True, exist_ok=False)
    args.evidence.mkdir(parents=True, exist_ok=False)
    original = Path(__file__).resolve().parents[2] / "examples/sdk-consumer/web_search.cpp"
    copied = args.source / "web_search.cpp"
    if original.read_bytes() != copied.read_bytes():
        raise RuntimeError("installed consumer helper source differs from native reference")
    ledger = args.state / "requests.tsv"
    ledger.write_text("", encoding="utf-8")
    stop = threading.Event()
    lock = threading.Lock()
    receipts = []
    items = [{"title": "搜索结果", "url": "https://example.com", "content": "中文摘要"},
             {"title": "second"}, {"title": "third"}]

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *unused):
            pass  # Credentials and raw provider bodies never enter diagnostics.

        def do_GET(self):
            self.respond()

        def do_POST(self):
            self.respond()

        def respond(self):
            size = int(self.headers.get("Content-Length", "0"))
            if size < 0 or size > 16384:
                self.send_error(400)
                return
            body = self.rfile.read(size)
            path, valid, resumed = validate_wire(self.command, self.path, self.headers, body)
            with lock:
                receipts.append({"method": self.command, "target": self.path, "wire_valid": valid,
                                 "resumed": resumed})
                with ledger.open("a", encoding="utf-8", newline="\n") as output:
                    output.write(path + "\tentered\n")
                    if valid:
                        output.write(path + "\twire\n")
                    if resumed:
                        output.write(path + "\tresumed\n")
            status, payload, extra = 200, {"results": items}, {}
            if path == "/provider/1":
                payload = {"web": {"results": items}}
            elif path == "/provider/2":
                payload = {"organic": items}
            elif path == "/body":
                payload = b"B" * 2048
            elif path == "/invalid":
                payload = b"fixture-search-key:invalid-json"
            elif path == "/status":
                status, payload = 500, b"fixture-search-key:http-failure"
            elif path == "/redirect":
                status, payload, extra = 302, b"", {"Location": "/unvisited"}
            elif path == "/header":
                extra = {"X-Large": "H" * 4096}
            elif path == "/output":
                payload = {"results": [{"title": "U" * 2000 + "中文"}]}
            if path in {"/timeout", "/close"}:
                stop.wait(15)
            encoded = payload if isinstance(payload, bytes) else json.dumps(payload, ensure_ascii=False).encode("utf-8")
            try:
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                for name, value in extra.items():
                    self.send_header(name, value)
                self.end_headers()
                self.wfile.write(encoded)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass  # The actual client cancels a pending request.

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = False
    server.block_on_close = True
    serving = threading.Thread(target=server.serve_forever, name="web-search-fixture")
    serving.start()
    command = [str(args.consumer.resolve()), "web-search", str(args.state / "consumer"),
               f"http://127.0.0.1:{server.server_port}", str(ledger)]
    env = {key: value for key, value in os.environ.items() if key.lower() not in
           {"http_proxy", "https_proxy", "all_proxy", "no_proxy"}}
    env.update(NO_PROXY="127.0.0.1", no_proxy="127.0.0.1")
    exit_code, status = None, "failed"
    try:
        result = subprocess.run(command, env=env, capture_output=True, text=True, encoding="utf-8", timeout=120)
        exit_code = result.returncode
        (args.evidence / "consumer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode:
            raise RuntimeError("installed web-search caller failed; see consumer.log")
        for name in ("admission", "providers", "lifecycle"):
            if f"[sdk-web-search] {name} passed" not in result.stdout:
                raise RuntimeError("installed web-search caller omitted acceptance case")
        expected = Counter({f"/provider/{index}": 1 for index in range(3)})
        expected.update({f"/{name}": 1 for name in
                         ("body", "invalid", "status", "redirect", "header", "timeout", "output", "close")})
        expected.update({"/resume": 2, "/isolation": 2})
        actual = Counter(row["target"].split("?", 1)[0] for row in receipts)
        if actual != expected or not all(row["wire_valid"] for row in receipts):
            raise RuntimeError("actual installed HTTP requests differ from provider/limit/isolation contract")
        if sum(row["resumed"] for row in receipts) != 1:
            raise RuntimeError("same-ID reopening did not use exactly one fresh credential")
        status = "passed"
    finally:
        stop.set()
        server.shutdown()
        serving.join()
        server.server_close()  # Joins every owned handler, including cancelled requests.
        (args.evidence / "requests.json").write_text(json.dumps(receipts, indent=2) + "\n", encoding="utf-8")
        (args.evidence / "invocation.json").write_text(json.dumps({"command": command, "exit_code": exit_code,
            "status": status,
            "source_sha256": hashlib.sha256(copied.read_bytes()).hexdigest(),
            "owned_threads_joined": not serving.is_alive()}, indent=2) + "\n", encoding="utf-8")
    print("installed SDK web-search actual HTTP acceptance passed (15 requests, owned fixture joined)")


if __name__ == "__main__":
    main()
