#!/usr/bin/env python3
# P0-6 真机与发布门探针:Windows 真 lubancode.exe + 假 Responses 服务。
#
# 用法:
#   python tests/manual/p06_release_gate_probe.py <lubancode.exe> <证据输出目录>
#
# 场景(每场独立临时 USERPROFILE + 临时工作目录 + 独立假服务端口):
#   oneshot_tool   单发一轮带只读工具调用(read_file)→ verify/replay 全过、
#                  悬空工具 0
#   subagent_run   单发派一只真子代理(agent 工具;子代理先 read_file 再交
#                  差)→ 父子账分离:子账 subagents/*.jsonl、父账只留边,
#                  verify 边核全过、悬空工具 0
#   crash_midturn  第 2 个请求挂闸,等账上出现模型边界后杀进程 → 前缀链
#                  完好、run 未收口,verify/replay 如实报(崩溃恢复账)
#   reader_gate    拿 oneshot_tool 的真账改出"未来账"(min_reader_version=3
#                  与 schema_version=3 两档,哈希链重建,链完整)→ 旧读者
#                  明拒读(replay.unsupported / verify.unsupported_reader_
#                  version / schema.unsupported_version),Journal 一枚字节
#                  不被改写(回滚纪律 §十七)
#
# 每场证据落 <输出目录>/<场景>/:command.txt / exit_code.txt / stdout.txt /
# session_id.txt / files.txt / verify.txt / replay.txt(+ 各场专档)。
# 证据目录不进 git;路径记在 todos 单子里。
import hashlib
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


def tool_round(call_id, name, args_json):
    return [added(0, call_id, name), args_delta(0, args_json),
            done(0, call_id, name, args_json), completed()]


def text_round(text):
    return [text_added(0), text_delta(text),
            {"type": "response.output_item.done", "output_index": 0,
             "item": {"type": "message", "id": "msg_0", "role": "assistant",
                      "content": [], "status": "completed"}},
            completed()]


class FakeServer:
    """假 Responses SSE 服务:按场景脚本逐请求回放;gate 非空时第 2 个
    请求等闸——崩溃场用它把进程卡在"工具已交、下一请求未回"的边界。"""

    def __init__(self, rounds, gate=None, gate_request=2):
        self.rounds = list(rounds)
        self.gate = gate
        self.gate_request = gate_request
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
                if server.gate is not None and idx == server.gate_request:
                    server.gate.wait(timeout=300)
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


def direct_env(home):
    env = dict(os.environ)
    env["USERPROFILE"] = str(home)
    for key in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                "ALL_PROXY"):
        env.pop(key, None)
    env["NO_PROXY"] = "*"
    env["no_proxy"] = "*"
    return env


def newest_session(home):
    sessions_root = home / ".lubancode" / "workspaces"
    newest = None
    if sessions_root.exists():
        for sessions in sessions_root.glob("*/sessions"):
            for session_dir in sessions.iterdir():
                if session_dir.is_dir() and (session_dir / "session.json").exists():
                    if newest is None or session_dir.stat().st_mtime > newest.stat().st_mtime:
                        newest = session_dir
    return newest


def write_files_listing(session_dir, out_dir):
    lines = []
    for root, _dirs, files in os.walk(session_dir):
        for f in sorted(files):
            p = Path(root) / f
            lines.append("%8d  %s" % (p.stat().st_size, p.relative_to(session_dir)))
    (out_dir / "files.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_verify_replay(exe, work, env, session_id, out_dir):
    for label, subcmd in (("verify.txt", "verify"), ("replay.txt", "replay")):
        probe = subprocess.run([str(exe), "trajectory", subcmd, session_id], cwd=str(work),
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace", timeout=120)
        (out_dir / label).write_text("[exit %d]\n%s" % (probe.returncode, probe.stdout or ""),
                                     encoding="utf-8")


def setup_workspace(home, work, port):
    config = {"active_provider": "probe",
              "providers": [{"name": "probe", "base_url": "http://127.0.0.1:%d" % port,
                             "wire": "responses", "api_key": "probe-key",
                             "model": "probe-model"}]}
    (work / ".lubancode").mkdir()
    (work / ".lubancode" / "config.json").write_text(json.dumps(config), encoding="utf-8")
    (work / "probe.txt").write_text("第一行\n第二行\n", encoding="utf-8")


def run_oneshot(exe, out_dir, name, rounds, gate=None, kill_after_events=None):
    """跑一场真 one_shot。kill_after_events 非空时:等 main.jsonl 出现至少
    N 行后 kill 进程(崩溃注入),返回 (proc_exit, session_dir, home, work)。"""
    home = Path(tempfile.mkdtemp(prefix="lb-p06-home-"))
    work = Path(tempfile.mkdtemp(prefix="lb-p06-work-"))
    server = FakeServer(rounds, gate=gate)
    setup_workspace(home, work, server.port)
    env = direct_env(home)
    cmd = [str(exe), "读 probe.txt 并回答"]
    (out_dir / "command.txt").write_text(
        " ".join(cmd) + "\n工作目录: " + str(work) + "\nUSERPROFILE: " + str(home) +
        "\n假服务: 127.0.0.1:%d" % server.port, encoding="utf-8")
    try:
        proc = subprocess.Popen(cmd, cwd=str(work), env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                errors="replace")
        if kill_after_events is not None:
            deadline = time.time() + 60
            target = None
            while time.time() < deadline and proc.poll() is None:
                session = newest_session(home)
                if session is not None:
                    main = session / "main.jsonl"
                    if main.exists():
                        with open(main, "rb") as fh:
                            n = sum(1 for _ in fh)
                        if n >= kill_after_events:
                            target = session
                            break
                time.sleep(0.05)
            (out_dir / "kill_point.txt").write_text(
                ("等到 %d 行,已杀" % kill_after_events) if target is not None
                else "60 秒没等到事件行数,仍杀", encoding="utf-8")
            proc.kill()
            stdout, _ = proc.communicate(timeout=30)
            (out_dir / "exit_code.txt").write_text("%d (killed)\n" % proc.returncode,
                                                   encoding="utf-8")
            (out_dir / "stdout.txt").write_text(stdout or "", encoding="utf-8")
            return proc.returncode, newest_session(home), home, work, env, server
        try:
            stdout, _ = proc.communicate(timeout=180)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, _ = proc.communicate()
            (out_dir / "stdout.txt").write_text("[TIMEOUT 180s,已杀进程]\n" + (stdout or ""),
                                                encoding="utf-8")
            return None, newest_session(home), home, work, env, server
        (out_dir / "exit_code.txt").write_text(str(proc.returncode) + "\n", encoding="utf-8")
        (out_dir / "stdout.txt").write_text(stdout or "", encoding="utf-8")
        (out_dir / "server_requests.txt").write_text(
            "请求数: %d" % len(server.requests), encoding="utf-8")
        return proc.returncode, newest_session(home), home, work, env, server
    except Exception:
        server.stop()
        raise


def scenario_oneshot_tool(exe, out_root):
    out_dir = out_root / "oneshot_tool"
    out_dir.mkdir(parents=True, exist_ok=True)
    rounds = [tool_round("call_p6a", "read_file", json.dumps({"path": "probe.txt"})),
              text_round("收工:文件两行,读完了。")]
    code, session, home, work, env, server = run_oneshot(exe, out_dir, "oneshot_tool", rounds)
    try:
        if session is None:
            (out_dir / "verify.txt").write_text("无 session\n", encoding="utf-8")
            return {"scenario": "oneshot_tool", "exit_code": code, "session_id": ""}
        (out_dir / "session_id.txt").write_text(session.name + "\n", encoding="utf-8")
        write_files_listing(session, out_dir)
        run_verify_replay(exe, work, env, session.name, out_dir)
        return {"scenario": "oneshot_tool", "exit_code": code, "session_id": session.name,
                "session_dir": str(session)}
    finally:
        server.stop()


def scenario_subagent_run(exe, out_root):
    out_dir = out_root / "subagent_run"
    out_dir.mkdir(parents=True, exist_ok=True)
    subagent_answer = "子代理交差:probe.txt 共两行。"
    rounds = [tool_round("call_p6s", "agent",
                         json.dumps({"title": "数文件行数",
                                     "prompt": "读 probe.txt,数出行数并报告"})),
              # 子代理自己的回合:它先调 read_file,再交一段文本差。
              tool_round("call_sub1", "read_file", json.dumps({"path": "probe.txt"})),
              text_round(subagent_answer),
              # 父代理收到子代理回话后收尾。
              text_round("回禀:子代理报 probe.txt 共两行。")]
    code, session, home, work, env, server = run_oneshot(exe, out_dir, "subagent_run", rounds)
    try:
        if session is None:
            (out_dir / "verify.txt").write_text("无 session\n", encoding="utf-8")
            return {"scenario": "subagent_run", "exit_code": code, "session_id": ""}
        (out_dir / "session_id.txt").write_text(session.name + "\n", encoding="utf-8")
        write_files_listing(session, out_dir)
        run_verify_replay(exe, work, env, session.name, out_dir)

        checks = []
        subagents = sorted((session / "subagents").glob("*.jsonl")) \
            if (session / "subagents").exists() else []
        checks.append("子账文件 %d 份: %s" %
                      (len(subagents), ", ".join(p.name for p in subagents)))
        main_text = (session / "main.jsonl").read_text(encoding="utf-8")
        main_lines = main_text.splitlines()
        # 分账判据(§3.5):父账只留边——工具调用生命周期 + 子代理的最终
        # 回话(那是 agent 工具的 result,父模型要吃,是边界事实);子账的
        # 内部细账(子代理自己的模型请求、它调的工具 call id)一枚都不进
        # 父账。
        main_request_count = sum(1 for line in main_lines if '"model.request.sent"' in line)
        child_internal_in_main = 0
        child_kinds = []
        for p in subagents:
            child_lines = p.read_text(encoding="utf-8").splitlines()
            for line in child_lines:
                event = json.loads(line)
                if event.get("kind") == "run.started":
                    child_kinds.append(event.get("run_kind"))
                if event.get("kind") in ("model.request.prepared", "model.request.sent",
                                         "tool.execution.planned", "tool.execution.started"):
                    call = event.get("call_id") or ""
                    if call and ('"%s"' % call) in main_text:
                        child_internal_in_main += 1
        checks.append("父账模型请求数: %d(要 2,只算父自己的)" % main_request_count)
        checks.append("子账内部事件(call id)漏进父账: %d 枚(要 0)" % child_internal_in_main)
        checks.append("子账 run_kind: %s" % child_kinds)
        edges = [json.loads(line) for line in main_lines if '"child_run_id"' in line]
        checks.append("父账带 child_run_id 的事件 %d 枚" % len(edges))
        (out_dir / "separation.txt").write_text("\n".join(checks) + "\n", encoding="utf-8")
        return {"scenario": "subagent_run", "exit_code": code, "session_id": session.name,
                "session_dir": str(session), "child_internal_in_main": child_internal_in_main,
                "subagent_files": len(subagents)}
    finally:
        server.stop()


def scenario_crash_midturn(exe, out_root):
    out_dir = out_root / "crash_midturn"
    out_dir.mkdir(parents=True, exist_ok=True)
    gate = threading.Event()
    rounds = [tool_round("call_p6c", "read_file", json.dumps({"path": "probe.txt"})),
              text_round("这一句永远到不了:请求挂在闸上,进程先死。")]
    code, session, home, work, env, server = run_oneshot(
        exe, out_dir, "crash_midturn", rounds, gate=gate,
        kill_after_events=12)
    try:
        if session is None:
            (out_dir / "verify.txt").write_text("无 session\n", encoding="utf-8")
            return {"scenario": "crash_midturn", "exit_code": code, "session_id": ""}
        (out_dir / "session_id.txt").write_text(session.name + "\n", encoding="utf-8")
        write_files_listing(session, out_dir)
        run_verify_replay(exe, work, env, session.name, out_dir)
        verify = (out_dir / "verify.txt").read_text(encoding="utf-8")
        replay = (out_dir / "replay.txt").read_text(encoding="utf-8")
        return {"scenario": "crash_midturn", "exit_code": code, "session_id": session.name,
                "session_dir": str(session),
                "verify_exit0": verify.startswith("[exit 0]"),
                "replay_exit0": replay.startswith("[exit 0]")}
    finally:
        gate.set()
        server.stop()


# ---- reader_gate:未来账与旧读者(回滚纪律)--------------------------------

def rechain_lines(lines):
    """同长替换法重建哈希链:只动 64 位 hex 串与同长 payload 数字,其余
    字节逐字不动——canonical 形状天然保持。注意 hash 材料是"新行去掉
    event_hash"的正文:prev_hash 也在材料里,得先换 prev 再算 hash。"""
    prev = "0" * 64
    out = []
    for line in lines:
        obj = json.loads(line)
        old_hash = obj["event_hash"]
        stage = line.replace('"prev_hash":"%s"' % obj["prev_hash"],
                             '"prev_hash":"%s"' % prev, 1)
        material = stage.replace('"event_hash":"%s",' % old_hash, "", 1)
        new_hash = hashlib.sha256((prev + material).encode("utf-8")).hexdigest()
        new_line = stage.replace('"event_hash":"%s"' % old_hash,
                                 '"event_hash":"%s"' % new_hash, 1)
        out.append(new_line)
        prev = new_hash
    return out


def tamper_session(source_session, home, tag, edit):
    """复制真 session 到新目录名,对 main.jsonl 施 edit(行编辑函数),
    重建哈希链。返回 (新 session 目录, 新 session id, bytes)。写盘必须
    LF-only 二进制——真账是 LF,CRLF 会让验链的规范字节比对整本红。"""
    sessions_root = source_session.parent
    new_id = source_session.name + "-" + tag
    target = sessions_root / new_id
    if target.exists():
        import shutil
        shutil.rmtree(target)
    import shutil
    shutil.copytree(source_session, target)
    main = target / "main.jsonl"
    lines = main.read_text(encoding="utf-8").splitlines()
    lines = edit(lines)
    rebuilt = rechain_lines(lines)
    with open(main, "wb") as fh:
        fh.write(("\n".join(rebuilt) + "\n").encode("utf-8"))
    with open(main, "rb") as fh:
        return target, new_id, fh.read()


def scenario_reader_gate(exe, out_root, oneshot_result):
    out_dir = out_root / "reader_gate"
    out_dir.mkdir(parents=True, exist_ok=True)
    source = Path(oneshot_result["session_dir"])
    # session 路径形如 <home>/.lubancode/workspaces/<key>/sessions/<id>:
    # parents[0]=sessions, [1]=<key>, [2]=workspaces, [3]=.lubancode。
    home = source.parents[3].parent
    env = direct_env(home)
    work = Path(tempfile.mkdtemp(prefix="lb-p06-gate-work-"))

    # 对照组:重链但不编辑——逐字节一致才证明链重建可信(二进制比,LF/CRLF
    # 差一拍都算不一致)。
    control_target, control_id, _ = tamper_session(source, home, "rechained",
                                                   lambda lines: lines)
    with open(control_target / "main.jsonl", "rb") as fh:
        control_bytes = fh.read()
    with open(source / "main.jsonl", "rb") as fh:
        source_bytes = fh.read()
    identical = control_bytes == source_bytes
    (out_dir / "rechain_control.txt").write_text(
        "无编辑重链与原文逐字节一致: %s\n源: %s\n重链: %s" %
        (identical, source, control_target), encoding="utf-8")

    results = {"scenario": "reader_gate", "rechain_control_identical": identical}

    # 档一:min_reader_version 2 -> 3(链完整,读者最高认 2)。
    def bump_min_reader(lines):
        edited = list(lines)
        for old in ('"min_reader_version":2', '"min_reader_version":1'):
            if old in edited[0]:
                edited[0] = edited[0].replace(old, '"min_reader_version":3', 1)
                break
        assert '"min_reader_version":3' in edited[0], "run.started 没带 min_reader_version"
        return edited

    t1, id1, bytes1 = tamper_session(source, home, "minreader3", bump_min_reader)
    (out_dir / "minreader3.txt").write_text(
        "session: %s\nmain.jsonl sha256: %s\n" %
        (id1, hashlib.sha256(bytes1).hexdigest()), encoding="utf-8")
    for label, subcmd in (("minreader3_verify.txt", "verify"),
                          ("minreader3_replay.txt", "replay")):
        probe = subprocess.run([str(exe), "trajectory", subcmd, id1], cwd=str(work),
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace", timeout=120)
        (out_dir / label).write_text("[exit %d]\n%s" % (probe.returncode, probe.stdout or ""),
                                     encoding="utf-8")
    after = (t1 / "main.jsonl").read_bytes()
    (out_dir / "minreader3_unchanged.txt").write_text(
        "读后 Journal 字节未变: %s" % (after == bytes1), encoding="utf-8")
    results["minreader3_verify_refused"] = "unsupported" in (
        out_dir / "minreader3_verify.txt").read_text(encoding="utf-8")
    results["minreader3_replay_refused"] = "unsupported" in (
        out_dir / "minreader3_replay.txt").read_text(encoding="utf-8")
    results["minreader3_bytes_unchanged"] = after == bytes1

    # 档二:整本 schema_version 2 -> 3(未来 envelope)。
    def bump_schema(lines):
        edited = [line.replace('"schema_version":2', '"schema_version":3', 1)
                  for line in lines]
        assert any('"schema_version":3' in line for line in edited), "没找到 schema_version=2 可升"
        return edited

    t2, id2, bytes2 = tamper_session(source, home, "schema3", bump_schema)
    (out_dir / "schema3.txt").write_text(
        "session: %s\nmain.jsonl sha256: %s\n" %
        (id2, hashlib.sha256(bytes2).hexdigest()), encoding="utf-8")
    for label, subcmd in (("schema3_verify.txt", "verify"),
                          ("schema3_replay.txt", "replay")):
        probe = subprocess.run([str(exe), "trajectory", subcmd, id2], cwd=str(work),
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace", timeout=120)
        (out_dir / label).write_text("[exit %d]\n%s" % (probe.returncode, probe.stdout or ""),
                                     encoding="utf-8")
    after2 = (t2 / "main.jsonl").read_bytes()
    (out_dir / "schema3_unchanged.txt").write_text(
        "读后 Journal 字节未变: %s" % (after2 == bytes2), encoding="utf-8")
    schema_verify = (out_dir / "schema3_verify.txt").read_text(encoding="utf-8")
    results["schema3_verify_refused"] = ("schema.unsupported_version" in schema_verify)
    results["schema3_bytes_unchanged"] = after2 == bytes2
    return results


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
    oneshot = scenario_oneshot_tool(exe, out_root)
    summary.append(oneshot)
    print(json.dumps(oneshot, ensure_ascii=False), flush=True)
    summary.append(scenario_subagent_run(exe, out_root))
    summary.append(scenario_crash_midturn(exe, out_root))
    if oneshot.get("session_dir"):
        summary.append(scenario_reader_gate(exe, out_root, oneshot))
    (out_root / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1),
                                           encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
