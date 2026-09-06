#!/usr/bin/env python3
# 子代理空轨迹单 5.3 真机验收:Windows 真 lubancode.exe + 假 Responses 服务。
#
# 用法:
#   python tests/manual/subagent_trajectory_probe.py <lubancode.exe> <证据输出目录>
#
# 场景(每场独立临时 USERPROFILE + 临时工作目录 + 独立假服务端口):
#   normal_call   正常 function_call(早帧即带 call_id)→ 工具轮 → 收尾
#   late_id       早帧缺 call_id、done 帧补 id → assembler 并 id,工具照常执行
#   never_id      added/done 都缺 call_id → 不产可执行 ToolUseBlock,回合明败
#   spawn_fail    预占 subagents/agent-1-main-0001.jsonl(目录)→ agent 工具
#                 fail closed,父账落 subagent.run.start_failed
#
# 每场证据落 <输出目录>/<场景>/:command.txt / exit_code / stdout.txt /
# session_id / files.txt / verify.txt / replay.txt。ESC 场景需真终端按键,
# 管道模式发不了 ESC——如实不跑(见单子回报的留白清单)。
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def sse(events):
    out = []
    for payload in events:
        out.append("data: " + json.dumps(payload, ensure_ascii=False) + "\n\n")
    return "".join(out).encode("utf-8")


def added(index, call_id, name):
    return {"type": "response.output_item.added", "output_index": index,
            "item": {"type": "function_call", "id": "fc_%d" % index, "call_id": call_id,
                     "name": name, "arguments": ""}}


def args_delta(index, args_json):
    return {"type": "response.function_call_arguments.delta", "output_index": index,
            "delta": args_json}


def done(index, call_id, name, args_json):
    return {"type": "response.output_item.done", "output_index": index,
            "item": {"type": "function_call", "id": "fc_%d" % index, "call_id": call_id,
                     "name": name, "arguments": args_json}}


def text_added(index):
    return {"type": "response.output_item.added", "output_index": index,
            "item": {"type": "message", "id": "msg_%d" % index, "role": "assistant",
                     "content": [], "status": "in_progress"}}


def text_delta(text):
    return {"type": "response.output_text.delta", "delta": text}


def completed():
    return {"type": "response.completed",
            "response": {"id": "resp_probe", "status": "completed", "output": [],
                         "usage": {"input_tokens": 12, "output_tokens": 7}}}


def tool_round(call_id_early, call_id_done, name, args_json):
    """一发 function_call:早帧 id 用 call_id_early,终帧 id 用 call_id_done。"""
    return [added(0, call_id_early, name), args_delta(0, args_json),
            done(0, call_id_done, name, args_json), completed()]


def text_round(text):
    return [text_added(0), text_delta(text),
            {"type": "response.output_item.done", "output_index": 0,
             "item": {"type": "message", "id": "msg_0", "role": "assistant",
                      "content": [], "status": "completed"}},
            completed()]


class FakeServer:
    """假 Responses SSE 服务:按场景脚本逐请求回放;支持"等闸再回第 1 帧"。"""

    def __init__(self, rounds, gate=None):
        self.rounds = list(rounds)
        self.gate = gate          # threading.Event;非空时第 1 个请求等它
        self.requests = []
        lock = threading.Lock()
        server = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                with lock:
                    server.requests.append(body)
                    idx = len(server.requests)
                if server.gate is not None and idx == 1:
                    server.gate.wait(timeout=60)
                events = server.rounds[idx - 1] if idx <= len(server.rounds) else []
                payload = sse(events)
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            def log_message(self, *args):
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()


def find_free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


SCENARIOS = ["normal_call", "late_id", "never_id", "spawn_fail"]


def run_scenario(exe, out_root, name):
    out_dir = out_root / name
    out_dir.mkdir(parents=True, exist_ok=True)
    home = Path(tempfile.mkdtemp(prefix="lb53-home-"))
    work = Path(tempfile.mkdtemp(prefix="lb53-work-"))
    (work / "probe.txt").write_text("第一行\n第二行\n", encoding="utf-8")

    gate = threading.Event() if name == "spawn_fail" else None
    if name == "normal_call":
        rounds = [tool_round("call_n1", "call_n1", "read_file", json.dumps({"path": "probe.txt"})),
                  text_round("收工:文件读完了。")]
    elif name == "late_id":
        rounds = [tool_round("", "call_late", "read_file", json.dumps({"path": "probe.txt"})),
                  text_round("收工:晚到的 id 也对上了。")]
    elif name == "never_id":
        rounds = [tool_round("", "", "read_file", json.dumps({"path": "probe.txt"}))]
    elif name == "spawn_fail":
        rounds = [tool_round("call_a1", "call_a1", "agent",
                             json.dumps({"title": "数仓库文件", "prompt": "数一下仓库里几个文件"})),
                  text_round("知道了,子账开不了就不派了。")]
    else:
        raise ValueError(name)

    server = FakeServer(rounds, gate)
    try:
        config = {"active_provider": "probe",
                  "providers": [{"name": "probe", "base_url": "http://127.0.0.1:%d" % server.port,
                                 "wire": "responses", "api_key": "probe-key",
                                 "model": "probe-model"}]}
        (work / ".lubancode").mkdir()
        (work / ".lubancode" / "config.json").write_text(json.dumps(config), encoding="utf-8")

        env = dict(os.environ)
        env["USERPROFILE"] = str(home)
        # 本机代理不许劫持回环假服务:全部直连。
        for key in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                    "ALL_PROXY"):
            env.pop(key, None)
        env["NO_PROXY"] = "*"
        env["no_proxy"] = "*"
        cmd = [str(exe), "读 probe.txt 并回答"]
        (out_dir / "command.txt").write_text(" ".join(cmd) + "\n工作目录: " + str(work) +
                                             "\nUSERPROFILE: " + str(home) +
                                             "\n假服务: 127.0.0.1:%d" % server.port, encoding="utf-8")
        proc = subprocess.Popen(cmd, cwd=str(work), env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                errors="replace")

        # spawn_fail:等 session 目录落盘,预占子 stream 目标名(目录),
        # 再放第 1 帧过去——真进程里注入子账开张失败,不靠测试钩子。
        if name == "spawn_fail":
            jam = None
            deadline = time.time() + 60
            while time.time() < deadline and proc.poll() is None:
                sessions_root = home / ".lubancode" / "workspaces"
                if sessions_root.exists():
                    for sessions in sessions_root.glob("*/sessions"):
                        for session_dir in sessions.iterdir():
                            if session_dir.is_dir() and (session_dir / "main.jsonl").exists():
                                subagents = session_dir / "subagents"
                                if subagents.exists():
                                    jam = subagents / "agent-1-main-0001.jsonl"
                                    if not jam.exists():
                                        jam.mkdir()
                                        break
                        if jam is not None:
                            break
                if jam is not None:
                    break
                time.sleep(0.05)
            if jam is None:
                (out_dir / "jam.txt").write_text("预占失败:没等到 session 目录", encoding="utf-8")
            else:
                (out_dir / "jam.txt").write_text("已预占: " + str(jam), encoding="utf-8")
            gate.set()

        try:
            stdout, _ = proc.communicate(timeout=180)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, _ = proc.communicate()
            (out_dir / "stdout.txt").write_text("[TIMEOUT 180s,已杀进程]\n" + (stdout or ""),
                                                encoding="utf-8")
            (out_dir / "server_requests.txt").write_text(
                "请求数: %d\n%s" % (len(server.requests),
                                    "\n".join(r[:300].decode("utf-8", "replace") for r in server.requests)),
                encoding="utf-8")
            return {"scenario": name, "exit_code": "timeout", "session_id": ""}
        (out_dir / "exit_code.txt").write_text(str(proc.returncode) + "\n", encoding="utf-8")
        (out_dir / "stdout.txt").write_text(stdout or "", encoding="utf-8")
        (out_dir / "server_requests.txt").write_text(
            "请求数: %d\n%s" % (len(server.requests),
                                "\n".join(r[:300].decode("utf-8", "replace") for r in server.requests)),
            encoding="utf-8")

        # 定位本场 session(新的那场)。
        session_id = ""
        sessions_root = home / ".lubancode" / "workspaces"
        newest = None
        if sessions_root.exists():
            for sessions in sessions_root.glob("*/sessions"):
                for session_dir in sessions.iterdir():
                    if session_dir.is_dir() and (session_dir / "session.json").exists():
                        if newest is None or session_dir.stat().st_mtime > newest.stat().st_mtime:
                            newest = session_dir
        if newest is not None:
            session_id = newest.name
            session_dir = newest
            (out_dir / "session_id.txt").write_text(session_id + "\n", encoding="utf-8")
            lines = []
            for root, _dirs, files in os.walk(session_dir):
                for f in sorted(files):
                    p = Path(root) / f
                    lines.append("%8d  %s" % (p.stat().st_size, p.relative_to(session_dir)))
            (out_dir / "files.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

        # verify 与 replay(同环境,真 exe)。
        for label, subcmd in (("verify.txt", "verify"), ("replay.txt", "replay")):
            if not session_id:
                (out_dir / label).write_text("无 session 可验\n", encoding="utf-8")
                continue
            probe = subprocess.run([str(exe), "trajectory", subcmd, session_id], cwd=str(work),
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, encoding="utf-8", errors="replace", timeout=120)
            (out_dir / label).write_text("[exit %d]\n%s" % (probe.returncode, probe.stdout or ""),
                                         encoding="utf-8")
        return {"scenario": name, "exit_code": proc.returncode, "session_id": session_id}
    finally:
        server.stop()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    exe = Path(sys.argv[1]).resolve()
    out_root = Path(sys.argv[2]).resolve()
    if not exe.exists():
        print("找不到 lubancode.exe:", exe)
        return 2
    out_root.mkdir(parents=True, exist_ok=True)
    summary = []
    for name in SCENARIOS:
        print("== 场景:", name, flush=True)
        summary.append(run_scenario(exe, out_root, name))
    (out_root / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1),
                                           encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
