# -*- coding: utf-8 -*-
"""轨迹 v3 §5.1 验收矩阵第一轮(非 compact 行)——真机驱动脚本。

对真 exe(build/release/Release/lubancode.exe)逐行验收 §5.1 表里不依赖
compact 运行时(PR #32)的 8 行:

  S1 启动后不输入      首行 system(seq=1、turnId=null);退出不造用户回合
  S2 加载 soul         旧 system -> 切换事件 -> 新 system;请求引用各指其时
  S3 切换事件后崩溃    截断至 system.change 行后 resume:旧 system 有效
  S4 普通 resume       旧 user/assistant/tool 可滚动查看,ID 与顺序不变
  S5 resume 后再 resume 来源链五键可遍历、无重复显示、seq 不跨文件混排
  S6 provider usage 缺失  账上 usage:null,不补 0
  S7 新档缺 blob/坏引用  resume 照常、可查看;缺口标注记录在案
  S8 只读 replay        零模型调用、零工具重跑(假后端账为零)

环境:每个场景独立的临时 USERPROFILE(Windows 路径)+ 独立 cwd;假
anthropic-messages 后端(v3_accept_fake_backend.py)只在 127.0.0.1 听;
LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=1。

用法:
  python scripts/tests/v3_accept_matrix.py [--exe 路径] [--work 路径] \
      [--rows S1,S2,...] [--keep]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VALIDATOR = REPO / "scripts" / "validate_trajectory_v3.py"
BACKEND = REPO / "scripts" / "tests" / "v3_accept_fake_backend.py"

SOUL_MARKER = "松间照·评审魂标记"


class Check:
    def __init__(self):
        self.items = []  # (name, ok, evidence)

    def check(self, name, ok, evidence=""):
        self.items.append((name, bool(ok), str(evidence)))
        print(("  [PASS] " if ok else "  [FAIL] ") + name +
              (" | " + str(evidence)[:220] if evidence else ""))
        return bool(ok)

    @property
    def failed(self):
        return [item for item in self.items if not item[1]]


class Bench:
    """一个场景:独立 home(临时 USERPROFILE)+ 独立 ws(cwd)+ 假后端。"""

    def __init__(self, name, work_root, exe):
        self.name = name
        self.exe = str(exe)
        self.base = work_root / name
        self.home = self.base / "home"
        self.ws = self.base / "ws"
        self.outdir = self.base / "proc"
        self.home.mkdir(parents=True, exist_ok=True)
        self.ws.mkdir(parents=True, exist_ok=True)
        self.outdir.mkdir(parents=True, exist_ok=True)
        self.backend_proc = None
        self.port = 0
        self.request_log = self.base / "backend_requests.jsonl"

    # ---- 基础设施 -----------------------------------------------------

    def start_backend(self, script):
        script_path = self.base / "backend_script.json"
        script_path.write_text(json.dumps(script, ensure_ascii=False, indent=1),
                               encoding="utf-8")
        self.backend_proc = subprocess.Popen(
            [sys.executable, str(BACKEND), "--port", "0",
             "--script", str(script_path), "--log", str(self.request_log)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=str(self.base))
        line = self.backend_proc.stdout.readline().decode("utf-8", "replace").strip()
        match = re.match(r"LISTEN (\d+)", line)
        if not match:
            raise RuntimeError("假后端没起来: " + line)
        self.port = int(match.group(1))

    def stop_backend(self):
        if self.backend_proc is not None:
            self.backend_proc.kill()
            self.backend_proc.wait(timeout=10)
            self.backend_proc = None

    def write_config(self):
        luban = self.home / ".lubancode"
        luban.mkdir(parents=True, exist_ok=True)
        (luban / "config.json").write_text(json.dumps({
            "providers": [{
                "name": "fake",
                "base_url": "http://127.0.0.1:%d" % self.port,
                "wire": "anthropic-messages",
                "auth": "none",
                "model": "fake-model",
                "context_window": 200000,
            }],
            "active_provider": "fake",
        }, ensure_ascii=False, indent=1), encoding="utf-8")

    def write_soul(self, name="reviewer"):
        souls = self.home / ".lubancode" / "souls"
        souls.mkdir(parents=True, exist_ok=True)
        (souls / (name + ".md")).write_text(
            "你是评审之魂,遇事先挑刺再点头。魂标记:" + SOUL_MARKER + "\n",
            encoding="utf-8")

    def env(self):
        env = dict(os.environ)
        for key in list(env):
            if key.startswith(("ANTHROPIC_", "LUBAN_", "LUBANCODE_")):
                del env[key]
        home_win = str(self.home.resolve())
        env["USERPROFILE"] = home_win
        env["HOME"] = home_win
        env["LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS"] = "1"
        env["LUBANCODE_LANG"] = "zh-CN"
        return env

    # ---- 进程驱动 -----------------------------------------------------

    def run_cli(self, tag, steps, continue_last=False, extra_args=()):
        """steps: [(line, wait)] 序列。wait:
        ("none",)          发完即走
        ("ledger", substr)  等当前账出现 substr
        ("requests", n)     等假后端请求数达到 n
        ("exit",)           发完等进程退出
        """
        out_path = self.outdir / (tag + ".out")
        err_path = self.outdir / (tag + ".err")
        args = [self.exe] + list(extra_args)
        if continue_last:
            args.append("--continue")
        args.append("--yes")
        with open(out_path, "wb") as out_file, open(err_path, "wb") as err_file:
            proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=out_file,
                                    stderr=err_file, cwd=str(self.ws),
                                    env=self.env())
            try:
                for step in steps:
                    line, wait = step
                    kind, arg = ((wait + (None,))[:2] if isinstance(wait, tuple)
                                 else (wait, None))
                    proc.stdin.write((line + "\n").encode("utf-8"))
                    proc.stdin.flush()
                    if kind == "exit":
                        break
                    if kind == "ledger":
                        if not self._wait_ledger(arg, proc):
                            raise RuntimeError("等账超时: " + str(arg))
                    elif kind == "requests":
                        if not self._wait_requests(arg):
                            raise RuntimeError("等请求超时: n=%d" % arg)
                    elif kind == "sleep":
                        time.sleep(arg)
                proc.stdin.close()
                try:
                    proc.wait(timeout=40)
                except subprocess.TimeoutExpired:
                    raise RuntimeError("进程不退: " + tag)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=10)
        return out_path.read_text(encoding="utf-8", errors="replace")

    def _wait_ledger(self, substr, proc, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if proc.poll() is not None:
                return False
            stream = self.current_stream()
            if stream is not None:
                try:
                    if substr in stream.read_text(encoding="utf-8", errors="replace"):
                        return True
                except OSError:
                    pass
            time.sleep(0.25)
        return False

    def _wait_requests(self, count, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.request_count() >= count:
                return True
            time.sleep(0.25)
        return False

    # ---- 会话/账本寻位 -------------------------------------------------

    def workspaces_root(self):
        return self.home / ".lubancode" / "workspaces"

    def session_dirs(self):
        result = []
        sessions_root = self.workspaces_root()
        if not sessions_root.exists():
            return result
        for room in sessions_root.iterdir():
            sessions = room / "sessions"
            if not sessions.is_dir():
                continue
            for entry in sorted(sessions.iterdir()):
                if entry.is_dir():
                    result.append(entry)
        return result

    def ledger_path(self, session_dir):
        return session_dir / (session_dir.name + ".jsonl")

    def current_stream(self):
        """最新的 v3 主账(按首行 timestamp)。"""
        best = None
        best_ts = ""
        for session_dir in self.session_dirs():
            stream = self.ledger_path(session_dir)
            if not stream.exists():
                continue
            try:
                first = json.loads(
                    stream.read_text(encoding="utf-8", errors="replace").splitlines()[0])
                ts = first.get("timestamp", "")
            except Exception:
                ts = ""
            if ts >= best_ts:
                best_ts, best = ts, stream
        return best

    def sessions_with(self, substr):
        """账内包含 substr 的会话目录列表(按首行时间升序)。"""
        found = []
        for session_dir in self.session_dirs():
            stream = self.ledger_path(session_dir)
            if stream.exists() and substr in stream.read_text(
                    encoding="utf-8", errors="replace"):
                found.append(session_dir)
        return found

    def sessions_owning(self, substr):
        """substr 出现在"在场消息"(messageId 不带来源键 "/",非链史抄本)
        的会话目录列表。D2 后新场账里有祖先史抄本,按纯文本找会把整条
        链都找出来;判归属要认自己写的行。"""
        found = []
        for session_dir in self.session_dirs():
            stream = self.ledger_path(session_dir)
            if not stream.exists():
                continue
            owns = False
            for row in self.read_lines(stream):
                if row.get("type") != "message":
                    continue
                if "/" in row.get("messageId", ""):
                    continue  # 链史抄本不算本场亲笔
                if substr in json.dumps(row.get("message", {}), ensure_ascii=False):
                    owns = True
                    break
            if owns:
                found.append(session_dir)
        return found

    def read_lines(self, stream):
        rows = []
        for raw in Path(stream).read_text(encoding="utf-8", errors="replace").splitlines():
            raw = raw.strip()
            if raw:
                rows.append(json.loads(raw))
        return rows

    def request_count(self):
        if not self.request_log.exists():
            return 0
        return len(self.request_log.read_text(encoding="utf-8",
                                              errors="replace").splitlines())

    def requests(self):
        if not self.request_log.exists():
            return []
        result = []
        for raw in self.request_log.read_text(encoding="utf-8",
                                              errors="replace").splitlines():
            raw = raw.strip()
            if raw:
                result.append(json.loads(raw))
        return result

    # ---- 校验器 -------------------------------------------------------

    def validate(self, stream):
        proc = subprocess.run([sys.executable, str(VALIDATOR), str(stream)],
                              capture_output=True, cwd=str(REPO), timeout=60)
        return proc.returncode == 0, proc.stdout.decode("utf-8", "replace").strip()


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def message_role(row):
    return ((row.get("message") or {}).get("role"))


def find_rows(rows, **filters):
    out = []
    for row in rows:
        if "type" in filters and row.get("type") != filters["type"]:
            continue
        if "kind" in filters and row.get("kind") != filters["kind"]:
            continue
        if "role" in filters and message_role(row) != filters["role"]:
            continue
        out.append(row)
    return out


# ===========================================================================
# 场景
# ===========================================================================

def scenario_s1(bench, check):
    """S1 启动后不输入:开一场直接 /exit。"""
    bench.start_backend([{"text": "不该被叫到", "usage": {"input_tokens": 1, "output_tokens": 1}}])
    bench.write_config()
    out = bench.run_cli("s1", [("/exit", ("exit",))])
    stream = bench.current_stream()
    check.check("S1 开出了 v3 主账", stream is not None,
                str(stream) if stream else "无账")
    if stream is None:
        return
    rows = bench.read_lines(stream)
    first = rows[0] if rows else {}
    check.check("S1 首行 system/seq=1/turnId=null",
                first.get("type") == "message" and message_role(first) == "system"
                and first.get("seq") == 1 and first.get("turnId") is None,
                "seq=%s turnId=%s role=%s" % (first.get("seq"), first.get("turnId"),
                                              message_role(first)))
    check.check("S1 第二行 session.started",
                rows[1].get("kind") == "session.started" if len(rows) > 1 else False,
                rows[1].get("kind") if len(rows) > 1 else "无第二行")
    human = [row for row in rows if row.get("type") == "message"
             and message_role(row) == "user"]
    check.check("S1 退出不造用户回合(无任何 user 消息)", not human,
                "user 消息 %d 条" % len(human))
    turn_rows = [row for row in rows if row.get("type") == "message"
                 and row.get("turnId") is not None]
    check.check("S1 无带 turnId 的消息(没编造回合)", not turn_rows,
                "turnId 消息 %d 条" % len(turn_rows))
    input_events = find_rows(rows, type="event", kind="input.received")
    check.check("S1 无 input.received", not input_events,
                "%d 条" % len(input_events))
    ok, detail = bench.validate(stream)
    check.check("S1 校验器 PASS", ok, detail)
    check.check("S1 零模型调用", bench.request_count() == 0,
                "requests=%d" % bench.request_count())
    ended = [row for row in rows if row.get("kind") == "session.ended"]
    check.check("S1 干净收场 session.ended", len(ended) == 1,
                "%d 条" % len(ended))
    check.check("S1 stdout 无报错栈", "fatal" not in out.lower(), out[:120])


def scenario_s2(bench, check):
    """S2 加载 soul:turn1(旧魂默认)-> /soul reviewer -> turn2(新魂)。"""
    bench.start_backend([
        {"text": "旧魂时代的回答-空山新雨", "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": "新魂时代的回答-清泉石上", "usage": {"input_tokens": 40, "output_tokens": 6}},
    ])
    bench.write_config()
    bench.write_soul()
    bench.run_cli("s2", [
        ("第一句话-明月松间照", ("ledger", "旧魂时代的回答-空山新雨")),
        ("/soul reviewer", ("none",)),
        ("n", ("none",)),
        ("第二句话-王孙自可留", ("requests", 2)),
        ("", ("ledger", "新魂时代的回答-清泉石上")),
        ("/exit", ("exit",)),
    ])
    stream = bench.sessions_owning("新魂时代的回答-清泉石上")
    check.check("S2 会话落账", bool(stream), "")
    if not stream:
        return
    rows = bench.read_lines(bench.ledger_path(stream[0]))

    systems = find_rows(rows, type="message", role="system")
    check.check("S2 system 递进两跳以上(基础版->完整版->含魂版)",
                len(systems) >= 3, "system 消息 %d 条" % len(systems))
    changes = find_rows(rows, type="event", kind="system.change")
    applied = find_rows(rows, type="event", kind="context.system.applied")
    check.check("S2 切换事件数 == 版本数-1",
                len(changes) == len(systems) - 1 and len(applied) == len(systems) - 1,
                "changes=%d applied=%d systems=%d" % (len(changes), len(applied),
                                                      len(systems)))
    prepared = find_rows(rows, type="event", kind="model.request.prepared")
    check.check("S2 两枚 prepared", len(prepared) == 2, "%d 枚" % len(prepared))
    if len(systems) >= 3 and len(changes) >= 2 and len(applied) >= 2 \
            and len(prepared) == 2:
        old_sys, new_sys = systems[-2], systems[-1]
        last_change, last_applied = changes[-1], applied[-1]
        seq_old = old_sys["seq"]
        seq_change = last_change["seq"]
        seq_new = new_sys["seq"]
        seq_applied = last_applied["seq"]
        check.check("S2 顺序 旧system<切换事件<新system<applied",
                    seq_old < seq_change < seq_new < seq_applied,
                    "seq %s<%s<%s<%s" % (seq_old, seq_change, seq_new, seq_applied))
        meta = new_sys.get("systemMeta") or {}
        check.check("S2 新 system 挂 changeEventRef",
                    meta.get("changeEventRef") == last_change["eventId"],
                    "%s vs %s" % (meta.get("changeEventRef"), last_change["eventId"]))
        check.check("S2 新 system 正文含魂标记",
                    SOUL_MARKER in (new_sys["message"].get("content") or ""),
                    "")
        check.check("S2 旧 system 正文不含魂标记",
                    SOUL_MARKER not in (old_sys["message"].get("content") or ""),
                    "")
        check.check("S2 applied 链根 = 新 system",
                    (last_applied["payload"].get("contextChain") or [{}])[0]
                    .get("messageRef") == new_sys["messageId"],
                    json.dumps(last_applied["payload"].get("contextChain"),
                               ensure_ascii=False))
        check.check("S2 请求1 指魂前 system(旧请求仍指旧版)",
                    prepared[0]["payload"].get("systemMessageRef") == old_sys["messageId"],
                    "%s vs %s" % (prepared[0]["payload"].get("systemMessageRef"),
                                  old_sys["messageId"]))
        check.check("S2 请求2 指含魂 system",
                    prepared[1]["payload"].get("systemMessageRef") == new_sys["messageId"],
                    "%s vs %s" % (prepared[1]["payload"].get("systemMessageRef"),
                                  new_sys["messageId"]))
    requests = bench.requests()
    if len(requests) >= 2:
        sys1 = json.dumps(requests[0]["body"].get("system", ""), ensure_ascii=False)
        sys2 = json.dumps(requests[1]["body"].get("system", ""), ensure_ascii=False)
        check.check("S2 实发请求1 system 无魂标记", SOUL_MARKER not in sys1, "")
        check.check("S2 实发请求2 system 带魂标记(内存只选新版)",
                    SOUL_MARKER in sys2, "")
    else:
        check.check("S2 实发请求两枚可查", False,
                    "requests=%d" % len(requests))
    ok, detail = bench.validate(bench.ledger_path(stream[0]))
    check.check("S2 校验器 PASS", ok, detail)


def scenario_s3(bench, check):
    """S3 切换事件后崩溃:截断到 system.change 行,--continue resume。"""
    bench.start_backend([
        {"text": "崩溃前的回答-竹喧归浣", "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": "第二句的回答-不必到远方", "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": "崩溃后的回答-莲动下舟", "usage": {"input_tokens": 30, "output_tokens": 5}},
    ])
    bench.write_config()
    bench.write_soul()
    bench.run_cli("s3a", [
        ("第一句话-随意春芳歇", ("ledger", "崩溃前的回答-竹喧归浣")),
        ("/soul reviewer", ("none",)),
        ("n", ("none",)),
        ("第二句话-王孙自可留", ("requests", 2)),
        ("", ("ledger", "system.change")),
        ("/exit", ("exit",)),
    ])
    source_dirs = bench.sessions_owning("崩溃前的回答-竹喧归浣")
    check.check("S3 源会话在", bool(source_dirs), "")
    if not source_dirs:
        return
    source = bench.ledger_path(source_dirs[0])
    raw_lines = source.read_text(encoding="utf-8").splitlines()
    cut = None
    for index, raw in enumerate(raw_lines):
        row = json.loads(raw)
        if row.get("kind") == "system.change":
            cut = index  # 最后一枚:基础版->完整版在前,魂切换在后
    check.check("S3 账里有 system.change 可截", cut is not None, "")
    if cut is None:
        return
    truncated = raw_lines[:cut + 1]
    source.write_text("\n".join(truncated) + "\n", encoding="utf-8", newline="\n")
    after = [json.loads(raw) for raw in truncated]
    has_new_system = any(row.get("type") == "message"
                         and message_role(row) == "system"
                         and SOUL_MARKER in json.dumps(row.get("message", {}),
                                                       ensure_ascii=False)
                         for row in after)
    check.check("S3 截断后新 system 未落盘(崩溃现场)",
                not has_new_system and after[-1].get("kind") == "system.change",
                "末行 kind=%s" % after[-1].get("kind"))
    source_hash = sha256_file(source)

    out = bench.run_cli("s3b", [
        ("现在用什么身份回答", ("ledger", "崩溃后的回答-莲动下舟")),
        ("/exit", ("exit",)),
    ], continue_last=True)
    resumed_dirs = bench.sessions_owning("崩溃后的回答-莲动下舟")
    check.check("S3 resume 成出新场", len(resumed_dirs) == 1, "%d 场" % len(resumed_dirs))
    check.check("S3 源账 resume 后一字未动",
                sha256_file(source) == source_hash, "")
    check.check("S3 旧史照常滚动(重放含崩溃前回合)",
                "崩溃前的回答-竹喧归浣" in out and "随意春芳歇" in out, "")
    if not resumed_dirs:
        return
    rows = bench.read_lines(bench.ledger_path(resumed_dirs[0]))
    attached = find_rows(rows, type="event", kind="resume.source.attached")
    check.check("S3 新场落 resume.source.attached", len(attached) == 1, "")
    if attached:
        ref = attached[0]["payload"].get("sourceRef") or {}
        check.check("S3 五键指源末行(=system.change 行)",
                    ref.get("sessionId") == source_dirs[0].name
                    and ref.get("id") == after[-1].get("eventId")
                    and ref.get("seq") == len(after)
                    and ref.get("hash") == after[-1].get("lineHash"),
                    json.dumps(ref, ensure_ascii=False))
    requests = bench.requests()
    if len(requests) >= 3:
        # 请求3 = resume 后那句;system 应仍是旧版(无魂标记)
        sys3 = json.dumps(requests[2]["body"].get("system", ""), ensure_ascii=False)
        check.check("S3 resume 后内存仍用旧 system(实发无魂标记)",
                    SOUL_MARKER not in sys3, "")
        msgs3 = requests[2]["body"].get("messages", [])
        roles3 = [m.get("role") for m in msgs3]
        texts3 = json.dumps(msgs3, ensure_ascii=False)
        check.check("S3 resume 后请求带旧史+新输入",
                    "随意春芳歇" in texts3 and "现在用什么身份回答" in texts3
                    and roles3.count("user") >= 2,
                    "roles=%s" % roles3)
    else:
        check.check("S3 resume 后请求可查", False,
                    "requests=%d" % len(requests))
    ok, detail = bench.validate(bench.ledger_path(resumed_dirs[0]))
    check.check("S3 新场校验器 PASS", ok, detail)
    switch_shown = "system" in out.lower() and ("切换" in out or "变更" in out)
    check.check("S3 终端重放不含切换标记(显示层未画,记录在案)",
                not switch_shown, "终端无 system 切换标记渲染")


def scenario_s4(bench, check):
    """S4 普通 resume:带工具回合的会话 -> --continue。"""
    (bench.ws / "notes.txt").write_text("notes 正文-隔篱呼取尽馀杯\n", encoding="utf-8")
    bench.start_backend([
        {"tool_use": {"id": "toolu_s4_01", "name": "read_file",
                      "input": {"path": "notes.txt"}},
         "usage": {"input_tokens": 50, "output_tokens": 7}},
        {"text": "工具时代的回答-肯与邻翁相对饮", "usage": {"input_tokens": 60, "output_tokens": 8}},
        {"text": "resume 之后的回答-白日放歌须纵酒", "usage": {"input_tokens": 70, "output_tokens": 9}},
    ])
    bench.write_config()
    bench.run_cli("s4a", [
        ("帮我读一下 notes.txt", ("ledger", "工具时代的回答-肯与邻翁相对饮")),
        ("/exit", ("exit",)),
    ])
    source_dirs = bench.sessions_owning("工具时代的回答-肯与邻翁相对饮")
    check.check("S4 源会话在", bool(source_dirs), "")
    if not source_dirs:
        return
    source = bench.ledger_path(source_dirs[0])
    source_hash = sha256_file(source)
    source_rows = bench.read_lines(source)
    tool_msgs = find_rows(source_rows, type="message", role="tool")
    check.check("S4 源账有 tool 消息(工具真跑过)",
                len(tool_msgs) == 1, "%d 条" % len(tool_msgs))
    artifact_dir = source_dirs[0] / "artifacts"
    artifacts = list(artifact_dir.glob("*")) if artifact_dir.exists() else []
    check.check("S4 工具结果落了 blob", len(artifacts) >= 1,
                "%d 个文件" % len(artifacts))

    out = bench.run_cli("s4b", [
        ("继续说说", ("ledger", "resume 之后的回答-白日放歌须纵酒")),
        ("/exit", ("exit",)),
    ], continue_last=True)
    check.check("S4 源账 resume 后一字未动(ID 与顺序原样)",
                sha256_file(source) == source_hash, "")
    order = [
        out.find("帮我读一下 notes.txt"),
        out.find("notes 正文-隔篱呼取尽馀杯"),
        out.find("工具时代的回答-肯与邻翁相对饮"),
    ]
    check.check("S4 重放按原序滚动 user->tool预览->assistant(时间线原序)",
                all(pos >= 0 for pos in order) and order == sorted(order),
                "find=%s" % order)
    resumed_dirs = bench.sessions_owning("resume 之后的回答-白日放歌须纵酒")
    check.check("S4 resume 出新场", len(resumed_dirs) == 1, "")
    if resumed_dirs:
        rows = bench.read_lines(bench.ledger_path(resumed_dirs[0]))
        attached = find_rows(rows, type="event", kind="resume.source.attached")
        check.check("S4 新场落 resume.source.attached", len(attached) == 1, "")
        ok, detail = bench.validate(bench.ledger_path(resumed_dirs[0]))
        check.check("S4 新场校验器 PASS", ok, detail)
    ok, detail = bench.validate(source)
    check.check("S4 源账校验器 PASS", ok, detail)
    requests = bench.requests()
    if len(requests) >= 3:
        msgs = requests[2]["body"].get("messages", [])
        roles = [m.get("role") for m in msgs]
        texts = json.dumps(msgs, ensure_ascii=False)
        # anthropic 线:tool_result 装 user 角色,序列=user/assistant(tool_use)/
        # user(tool_result)/assistant(终答)/user(新输入)
        check.check("S4 resume 后请求按原序带全旧史+新输入",
                    roles == ["user", "assistant", "user", "assistant", "user"]
                    and "帮我读一下 notes.txt" in texts
                    and "notes 正文-隔篱呼取尽馀杯" in texts
                    and "继续说说" in texts,
                    "roles=%s" % roles)
        # D2 修复:源场史以来源键抄进新账并接纳进链——新场
        # prepared.inputMessageRefs 与实发消息一致(修前 1 比 5)。
        if resumed_dirs:
            new_rows = bench.read_lines(bench.ledger_path(resumed_dirs[0]))
            prepared = find_rows(new_rows, type="event", kind="model.request.prepared")
            if prepared:
                refs = prepared[0]["payload"].get("inputMessageRefs") or []
                check.check("S4 新场 prepared.inputMessageRefs 与实发消息一致",
                            len(refs) == len(msgs),
                            "prepared refs=%d, 实发 messages=%d" % (len(refs), len(msgs)))
            imported = [row for row in new_rows
                        if row.get("type") == "message"
                        and "/" in row.get("messageId", "")]
            check.check("S4 新场落链史抄本(来源键 messageId)",
                        len(imported) == 4, "%d 条" % len(imported))
            check.check("S4 抄本来源键指源场",
                        all(row.get("sourceMessageRef", "").startswith(
                                source_dirs[0].name + "/") for row in imported),
                        "source=%s" % source_dirs[0].name)
    else:
        check.check("S4 请求可查", False, "requests=%d" % len(requests))


def scenario_s5(bench, check):
    """S5 resume 后再 resume:A -> B -> C,来源链与重复显示。"""
    (bench.ws / "notes.txt").write_text("notes5-花径不曾缘客扫\n", encoding="utf-8")
    bench.start_backend([
        {"text": "A场回答-蓬门今始为君开", "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": "B场回答-盘飧市远无兼味", "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": "C场回答-樽酒家贫只旧醅", "usage": {"input_tokens": 30, "output_tokens": 5}},
    ])
    bench.write_config()
    bench.run_cli("s5a", [
        ("A场第一句", ("ledger", "A场回答-蓬门今始为君开")),
        ("/exit", ("exit",)),
    ])
    bench.run_cli("s5b", [
        ("B场第一句", ("ledger", "B场回答-盘飧市远无兼味")),
        ("/exit", ("exit",)),
    ], continue_last=True)
    out_c = bench.run_cli("s5c", [
        ("C场第一句", ("ledger", "C场回答-樽酒家贫只旧醅")),
        ("/exit", ("exit",)),
    ], continue_last=True)

    a_dir = bench.sessions_owning("A场回答-蓬门今始为君开")
    b_dir = bench.sessions_owning("B场回答-盘飧市远无兼味")
    c_dir = bench.sessions_owning("C场回答-樽酒家贫只旧醅")
    check.check("S5 三场齐(A/B/C)", a_dir and b_dir and c_dir,
                "%d/%d/%d" % (len(a_dir), len(b_dir), len(c_dir)))
    if not (a_dir and b_dir and c_dir):
        return

    def attached_ref(session_dir):
        rows = bench.read_lines(bench.ledger_path(session_dir))
        for row in rows:
            if row.get("kind") == "resume.source.attached":
                return row["payload"].get("sourceRef") or {}
        return {}

    def last_line(session_dir):
        rows = bench.read_lines(bench.ledger_path(session_dir))
        return rows[-1] if rows else {}

    ref_b = attached_ref(b_dir[0])
    ref_c = attached_ref(c_dir[0])
    a_last, b_last = last_line(a_dir[0]), last_line(b_dir[0])
    check.check("S5 B 的来源五键指 A 末行",
                ref_b.get("sessionId") == a_dir[0].name
                and ref_b.get("seq") == a_last.get("seq")
                and ref_b.get("hash") == a_last.get("lineHash"),
                json.dumps(ref_b, ensure_ascii=False))
    check.check("S5 C 的来源五键指 B 末行",
                ref_c.get("sessionId") == b_dir[0].name
                and ref_c.get("seq") == b_last.get("seq")
                and ref_c.get("hash") == b_last.get("lineHash"),
                json.dumps(ref_c, ensure_ascii=False))
    check.check("S5 来源链可遍历(B->A、C->B 都验得上 hash)",
                ref_b.get("hash") == a_last.get("lineHash")
                and ref_c.get("hash") == b_last.get("lineHash")
                and ref_b.get("sessionId") != ref_c.get("sessionId"), "")

    # 无重复显示:C 的重放里 B 的话一次;祖先 A 沿链也画,恰一次(D2
    # 显示侧:RestoredHistoryView 画链上祖先,时间线原序,不重复)。
    count_b = out_c.count("B场第一句")
    count_cb = out_c.count("B场回答-盘飧市远无兼味")
    check.check("S5 C 场重放无重复(B 用户话恰一次)",
                count_b == 1, "count=%d" % count_b)
    check.check("S5 C 场重放无重复(B 回答恰一次)",
                count_cb == 1, "count=%d" % count_cb)
    count_a_in_c = out_c.count("A场第一句")
    count_a_ans_in_c = out_c.count("A场回答-蓬门今始为君开")
    check.check("S5 C 场终端重放画链上祖先(A 句恰一次,不重复)",
                count_a_in_c == 1, "A 句出现 %d 次" % count_a_in_c)
    check.check("S5 C 场终端重放画链上祖先(A 回答恰一次)",
                count_a_ans_in_c == 1, "A 回答出现 %d 次" % count_a_ans_in_c)

    # D2:祖先史进新场模型上下文——C 场实发请求带 A、B 两场历史;
    # C 场 prepared.inputMessageRefs 与实发一致,refs 含祖先来源键。
    requests = bench.requests()
    if len(requests) >= 3:
        msgs = requests[2]["body"].get("messages", [])
        texts = json.dumps(msgs, ensure_ascii=False)
        check.check("S5 C 场请求含 A 场历史(D2:祖先史不退出上下文)",
                    "A场第一句" in texts and "A场回答-蓬门今始为君开" in texts
                    and "B场第一句" in texts and "C场第一句" in texts,
                    "roles=%s" % [m.get("role") for m in msgs])
        c_rows = bench.read_lines(bench.ledger_path(c_dir[0]))
        prepared = find_rows(c_rows, type="event", kind="model.request.prepared")
        if prepared:
            refs = prepared[0]["payload"].get("inputMessageRefs") or []
            check.check("S5 C 场 prepared.inputMessageRefs 与实发一致",
                        len(refs) == len(msgs),
                        "prepared refs=%d, 实发=%d" % (len(refs), len(msgs)))
            with_source = [r for r in refs if isinstance(r, str) and "/" in r]
            check.check("S5 C 场 prepared refs 含祖先来源键",
                        len(with_source) >= 2, "%d 枚" % len(with_source))
        # B 场账含 A 场链史抄本(来源键指 A)。
        b_rows = bench.read_lines(bench.ledger_path(b_dir[0]))
        b_imported = [row for row in b_rows
                      if row.get("type") == "message" and "/" in row.get("messageId", "")]
        check.check("S5 B 场落 A 场链史抄本",
                    len(b_imported) == 2 and all(
                        row.get("sourceMessageRef", "").startswith(a_dir[0].name + "/")
                        for row in b_imported),
                    "%d 条" % len(b_imported))
    else:
        check.check("S5 请求可查", False, "requests=%d" % len(requests))

    # seq 不跨文件混排:三本账各自从 1 连续(校验器钉);末行 seq == 行数
    for tag, directory in (("A", a_dir[0]), ("B", b_dir[0]), ("C", c_dir[0])):
        rows = bench.read_lines(bench.ledger_path(directory))
        seqs = [row.get("seq") for row in rows]
        ok = seqs == list(range(1, len(rows) + 1))
        check.check("S5 %s 场 seq 自 1 连续不混排" % tag, ok,
                    "seqs=%s..%s" % (seqs[:3], seqs[-3:]))
    ok, detail = bench.validate(bench.ledger_path(c_dir[0]))
    check.check("S5 C 场校验器 PASS", ok, detail)


def scenario_s6(bench, check):
    """S6 provider usage 缺失:回 1 报 usage、回 2 不报。"""
    bench.start_backend([
        {"text": "报账的回答-两个黄鹂鸣翠柳", "usage": {"input_tokens": 111, "output_tokens": 22}},
        {"text": "不报账的回答-一行白鹭上青天", "usage": None},
    ])
    bench.write_config()
    bench.run_cli("s6", [
        ("第一句问账", ("ledger", "报账的回答-两个黄鹂鸣翠柳")),
        ("第二句问账", ("ledger", "不报账的回答-一行白鹭上青天")),
        ("/exit", ("exit",)),
    ])
    dirs = bench.sessions_owning("不报账的回答-一行白鹭上青天")
    check.check("S6 会话落账", bool(dirs), "")
    if not dirs:
        return
    rows = bench.read_lines(bench.ledger_path(dirs[0]))
    assistants = find_rows(rows, type="message", role="assistant")
    check.check("S6 两条 assistant", len(assistants) == 2, "%d 条" % len(assistants))
    if len(assistants) == 2:
        usage1, usage2 = assistants[0].get("usage"), assistants[1].get("usage")
        check.check("S6 明报 usage 落对象(inputTokens=111)",
                    isinstance(usage1, dict) and usage1.get("inputTokens") == 111
                    and usage1.get("outputTokens") == 22,
                    json.dumps(usage1, ensure_ascii=False))
        check.check("S6 缺报 usage 落 null(不补 0)", usage2 is None,
                    "usage2=%r" % usage2)
        check.check("S6 usage 键必现", all("usage" in a for a in assistants), "")
    ok, detail = bench.validate(bench.ledger_path(dirs[0]))
    check.check("S6 校验器 PASS", ok, detail)


def scenario_s7(bench, check):
    """S7 新档缺 blob/坏引用:删一枚 artifact、篡改一枚,再 resume。"""
    (bench.ws / "notes.txt").write_text("notes7-肯与邻翁相对饮\n", encoding="utf-8")
    bench.start_backend([
        {"tool_use": {"id": "toolu_s7_01", "name": "read_file",
                      "input": {"path": "notes.txt"}},
         "usage": {"input_tokens": 50, "output_tokens": 7}},
        {"text": "S7工具回合的回答-隔篱呼取尽馀杯", "usage": {"input_tokens": 60, "output_tokens": 8}},
        {"text": "S7缺件后的回答-举杯消愁愁更愁", "usage": {"input_tokens": 60, "output_tokens": 8}},
    ])
    bench.write_config()
    bench.run_cli("s7a", [
        ("读一下 notes.txt", ("ledger", "S7工具回合的回答-隔篱呼取尽馀杯")),
        ("/exit", ("exit",)),
    ])
    source_dirs = bench.sessions_owning("S7工具回合的回答-隔篱呼取尽馀杯")
    check.check("S7 源会话在", bool(source_dirs), "")
    if not source_dirs:
        return
    source = bench.ledger_path(source_dirs[0])
    artifact_dir = source_dirs[0] / "artifacts"
    artifacts = sorted(p for p in artifact_dir.glob("*") if p.is_file()) \
        if artifact_dir.exists() else []
    check.check("S7 工具结果 blob 在场", len(artifacts) >= 2,
                "%d 个" % len(artifacts))
    if len(artifacts) < 2:
        return
    deleted, tampered = artifacts[0], artifacts[-1]
    deleted.unlink()
    data = tampered.read_bytes()
    tampered.write_bytes((bytes([data[0] ^ 0xFF]) + data[1:]) if data else b"x")
    ledger_hash = sha256_file(source)

    out = bench.run_cli("s7b", [
        ("缺了零件也照聊", ("requests", 3)),
        ("/exit", ("exit",)),
    ], continue_last=True)
    check.check("S7 缺 blob/坏 blob 不阻 resume(照常出话)",
                "S7缺件后的回答-举杯消愁愁更愁" in out
                and "S7工具回合的回答-隔篱呼取尽馀杯" in out, "")
    check.check("S7 账本不受 blob 损毁影响(哈希未变)",
                sha256_file(source) == ledger_hash, "")
    ok, detail = bench.validate(source)
    check.check("S7 校验器仍 PASS", ok, detail)
    check.check("S7 重放仍显示工具预览(可查看部分)",
                "notes7-肯与邻翁相对饮" in out, "")
    check.check("S7 终端不宣称『完整恢复』字样", "完整恢复" not in out, "")
    check.check("S7 缺口标注在账外(blob 缺失无终端提示,记录在案)",
                "missing_blob" not in out and "缺口" not in out, "")


def scenario_s8(bench, check):
    """S8 只读 replay:--continue 进场只看不发,零调用零重跑。"""
    (bench.ws / "notes.txt").write_text("notes8-山重水复疑无路\n", encoding="utf-8")
    bench.start_backend([
        {"tool_use": {"id": "toolu_s8_01", "name": "read_file",
                      "input": {"path": "notes.txt"}},
         "usage": {"input_tokens": 50, "output_tokens": 7}},
        {"text": "S8旧场的回答-柳暗花明又一村", "usage": {"input_tokens": 60, "output_tokens": 8}},
    ])
    bench.write_config()
    bench.run_cli("s8a", [
        ("读一下 notes.txt 再总结", ("ledger", "S8旧场的回答-柳暗花明又一村")),
        ("/exit", ("exit",)),
    ])
    out = bench.run_cli("s8b", [("/exit", ("exit",))], continue_last=True)
    check.check("S8 重放开场(旧史可见)",
                "S8旧场的回答-柳暗花明又一村" in out
                and "读一下 notes.txt 再总结" in out, "")
    check.check("S8 零模型调用(假后端请求账止于 2)",
                bench.request_count() == 2,
                "requests=%d" % bench.request_count())
    newest = bench.current_stream()
    dirs = bench.session_dirs()
    newest_dir = newest.parent if newest else None
    check.check("S8 resume 出了新场", newest_dir is not None
                and newest_dir in dirs, str(newest_dir))
    if newest_dir is None:
        return
    rows = bench.read_lines(newest)
    model_events = [row for row in rows if str(row.get("kind", "")).startswith(
        ("model.request", "model.response"))]
    check.check("S8 新场零模型事件", not model_events,
                "%d 条" % len(model_events))
    tool_events = [row for row in rows if str(row.get("kind", "")).startswith(
        ("tool.execution", "tool.result"))]
    check.check("S8 新场零工具事件(零重跑)", not tool_events,
                "%d 条" % len(tool_events))
    input_events = find_rows(rows, type="event", kind="input.received")
    check.check("S8 新场零用户输入", not input_events, "%d 条" % len(input_events))
    assistants = find_rows(rows, type="message", role="assistant")
    check.check("S8 新场零 assistant 消息", not assistants, "%d 条" % len(assistants))
    new_artifacts = newest_dir / "artifacts"
    files = ([p for p in new_artifacts.glob("*") if p.is_file()]
             if new_artifacts.exists() else [])
    check.check("S8 新场零新 blob(工具没重跑)", not files, "%d 个" % len(files))
    ok, detail = bench.validate(newest)
    check.check("S8 新场校验器 PASS", ok, detail)


SCENARIOS = [
    ("S1", scenario_s1),
    ("S2", scenario_s2),
    ("S3", scenario_s3),
    ("S4", scenario_s4),
    ("S5", scenario_s5),
    ("S6", scenario_s6),
    ("S7", scenario_s7),
    ("S8", scenario_s8),
]


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", default=str(REPO / "build" / "release" / "Release"
                                             / "lubancode.exe"))
    parser.add_argument("--work", default=str(REPO / "build" / "v3_accept"))
    parser.add_argument("--rows", default="",
                        help="只跑指定行(逗号分隔 S1..S8;空 = 全跑)")
    parser.add_argument("--keep", action="store_true", help="不清工作目录")
    args = parser.parse_args()

    exe = Path(args.exe)
    if not exe.exists():
        print("exe 不在: %s" % exe)
        return 2
    work = Path(args.work)
    if not args.keep and work.exists():
        shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True, exist_ok=True)

    wanted = set(args.rows.split(",")) if args.rows else None
    report = {}
    failures = 0
    for name, scenario in SCENARIOS:
        if wanted is not None and name not in wanted:
            continue
        print("== %s ==" % name)
        bench = Bench(name, work, exe)
        check = Check()
        try:
            scenario(bench, check)
        except Exception as error:  # noqa: BLE001 - 验收脚本要兜住一切场面
            check.check(name + " 场景执行完成", False, "%s: %s" % (
                type(error).__name__, error))
        finally:
            bench.stop_backend()
        report[name] = [{"name": n, "ok": ok, "evidence": e}
                        for n, ok, e in check.items]
        failures += len(check.failed)

    print("\n===== 汇总 =====")
    for name, items in report.items():
        passed = sum(1 for item in items if item["ok"])
        print("%s: %d/%d" % (name, passed, len(items)))
        for item in items:
            if not item["ok"]:
                print("   FAIL %s | %s" % (item["name"], item["evidence"][:200]))
    report_path = work / "report.json"
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=1),
                           encoding="utf-8")
    print("明细: %s" % report_path)
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
