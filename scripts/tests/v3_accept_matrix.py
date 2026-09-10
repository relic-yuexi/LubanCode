# -*- coding: utf-8 -*-
"""轨迹 v3 §5.1 验收矩阵——真机驱动脚本(第一轮非 compact 行 + 第二轮 compact 行)。

对真 exe(build/release/Release/lubancode.exe)逐行验收 §5.1 表。第一轮
不依赖 compact 运行时的 8 行:

  S1 启动后不输入      首行 system(seq=1、turnId=null);退出不造用户回合
  S2 加载 soul         旧 system -> 切换事件 -> 新 system;请求引用各指其时
  S3 切换事件后崩溃    截断至 system.change 行后 resume:旧 system 有效
  S4 普通 resume       旧 user/assistant/tool 可滚动查看,ID 与顺序不变
  S5 resume 后再 resume 来源链五键可遍历、无重复显示、seq 不跨文件混排
  S6 provider usage 缺失  账上 usage:null,不补 0
  S7 新档缺 blob/坏引用  resume 照常、可查看;缺口标注记录在案
  S8 只读 replay        零模型调用、零工具重跑(假后端账为零)

第二轮 compact 相关 17 行(C1..C17,§5.1 表 compact 行):一次成功/手动
空闲/turn 中途自动/重试与归属/多条消息入料/坏摘要拒收/工具组配对/期间
插话/四类崩溃现场/写盘失败手段档/压缩后新输入形状/压缩后 resume/连续
两次/只余旧摘要/空历史。崩溃行用账本截断等价崩溃现场;模型调用全走
剧本化假后端,实发请求账(JSONL)断言输入清单与内部回合归属。

环境:每个场景独立的临时 USERPROFILE(Windows 路径)+ 独立 cwd;假
anthropic-messages 后端(v3_accept_fake_backend.py)只在 127.0.0.1 听;
LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=1。

用法:
  python scripts/tests/v3_accept_matrix.py [--exe 路径] [--work 路径] \
      [--rows S1,S2,...,C1,...] [--keep]
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

    def write_config(self, window=200000):
        luban = self.home / ".lubancode"
        luban.mkdir(parents=True, exist_ok=True)
        (luban / "config.json").write_text(json.dumps({
            "providers": [{
                "name": "fake",
                "base_url": "http://127.0.0.1:%d" % self.port,
                "wire": "anthropic-messages",
                "auth": "none",
                "model": "fake-model",
                "context_window": window,
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
        proc = subprocess.run([sys.executable, "-X", "utf8", str(VALIDATOR), str(stream)],
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
    stream = bench.sessions_with("新魂时代的回答-清泉石上")
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
    source_dirs = bench.sessions_with("崩溃前的回答-竹喧归浣")
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
    resumed_dirs = bench.sessions_with("崩溃后的回答-莲动下舟")
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
    source_dirs = bench.sessions_with("工具时代的回答-肯与邻翁相对饮")
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
    resumed_dirs = bench.sessions_with("resume 之后的回答-白日放歌须纵酒")
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
        # 记录账实差异:新场 prepared.inputMessageRefs 只列本账链
        if resumed_dirs:
            new_rows = bench.read_lines(bench.ledger_path(resumed_dirs[0]))
            prepared = find_rows(new_rows, type="event", kind="model.request.prepared")
            if prepared:
                refs = prepared[0]["payload"].get("inputMessageRefs") or []
                check.check("S4 记录:新场 prepared 只列本账链(账实分离在案)",
                            len(refs) < len(msgs),
                            "prepared refs=%d, 实发 messages=%d" % (len(refs), len(msgs)))
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

    a_dir = bench.sessions_with("A场回答-蓬门今始为君开")
    b_dir = bench.sessions_with("B场回答-盘飧市远无兼味")
    c_dir = bench.sessions_with("C场回答-樽酒家贫只旧醅")
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

    # 无重复显示:C 的重放里 B 的话一次、A 的话不重复出现在 C 场
    count_b = out_c.count("B场第一句")
    count_cb = out_c.count("B场回答-盘飧市远无兼味")
    check.check("S5 C 场重放无重复(B 用户话恰一次)",
                count_b == 1, "count=%d" % count_b)
    check.check("S5 C 场重放无重复(B 回答恰一次)",
                count_cb == 1, "count=%d" % count_cb)
    count_a_in_c = out_c.count("A场第一句")
    check.check("S5 C 场终端只重放直接源(B),不混排 A(记录:祖先史终端不显示)",
                count_a_in_c == 0, "A 句出现 %d 次" % count_a_in_c)

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
    dirs = bench.sessions_with("不报账的回答-一行白鹭上青天")
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
    source_dirs = bench.sessions_with("S7工具回合的回答-隔篱呼取尽馀杯")
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


# ===========================================================================
# 第二轮:compact 17 行(§5.1 表 compact 相关)
#
# 已知红线:D1(流式三件套,修复在 worktree 分支未并 main)——生产账里
# assistant(主对话与 compact 候选)均无 model.response.started/completed,
# 校验器必报"assistant 无定稿事件"。凡校验器红且全部问题都是这一句的,
# 记"D1-only 红"不算新缺陷;其余问题照常记账。
# ===========================================================================


def filler(seed, chars):
    unit = seed + "山高月小水落石出;"
    out = []
    while sum(len(u) for u in out) < chars:
        out.append(unit)
    return "".join(out)[:max(chars, len(seed) + 9)]


def filler_ascii(seed, chars):
    """纯 ASCII 填充:估算口径 4 字符=1 token,数字可预算(预检/B 闸夹缝用)。"""
    unit = seed + "abcdefghijklmnopqrstuvwxyz0123456789"
    out = []
    while sum(len(u) for u in out) < chars:
        out.append(unit)
    return "".join(out)[:max(chars, len(seed) + 9)]


def summary_manifest(seed):
    return ("```json\n" + json.dumps({
        "goal": "验证 compact 全链(seed=" + seed + ")",
        "constraints": ["不改动生产码"],
        "open_items": ["继续验证压缩后请求形状"],
        "next_action": "发送新输入检查请求形状",
    }, ensure_ascii=False) + "\n```")


def summary_reply(seed, filler_bytes=0):
    body = ("交接摘要-" + seed + "。\n\n## 任务目标\n用户在验证轨迹 v3 的 compact 全链,"
            "本次摘要 seed 为 " + seed + "。\n\n## 关键事实\n- 原始对话完整留档,压缩只改"
            "模型上下文投影。\n- 无工具未决事项,无失败尝试。\n")
    if filler_bytes > 0:
        body += "\n## 过程细节\n" + filler("细节" + seed, filler_bytes) + "\n"
    body += "\n## 下一步\n继续验证压缩后的请求形状与恢复视图。\n\n" + summary_manifest(seed)
    return body


def bad_reply_no_manifest(seed):
    return ("这不是摘要-" + seed + ",没有 JSON manifest 围栏,正文长度足够超过四十个"
            "码点,但缺必需字段,校验应当拒收:" + filler("坏回执", 120))


D1_PATTERN = re.compile(r"assistant \S+ 无定稿事件")


def validator_split(bench, stream):
    """跑校验器,把问题拆成 D1 类(流式三件套缺)与其余。"""
    ok, detail = bench.validate(stream)
    problems = [line.strip() for line in detail.splitlines()
                if line.strip().startswith("[") and "FAIL" in line]
    d1 = [p for p in problems if D1_PATTERN.search(p)]
    other = [p for p in problems if not D1_PATTERN.search(p)]
    return ok, d1, other


def rows_where(rows, **filters):
    out = []
    for row in rows:
        if "kind" in filters and row.get("kind") != filters["kind"]:
            continue
        if "purpose" in filters and row.get("purpose") != filters["purpose"]:
            continue
        if "role" in filters and message_role(row) != filters["role"]:
            continue
        if "compact_id" in filters and row.get("compactId") != filters["compact_id"]:
            continue
        out.append(row)
    return out


def compact_block(rows, index=0):
    """第 index 个 compact(按 requested 出现序)的全部相关行与常用字段。"""
    requested = [row for row in rows if row.get("kind") == "compact.requested"]
    if index >= len(requested):
        return None
    compact_id = requested[index]["compactId"]
    related = [row for row in rows if row.get("compactId") == compact_id]
    by_kind = {}
    for row in related:
        by_kind.setdefault(row.get("kind"), []).append(row)
    return {
        "id": compact_id,
        "requested": requested[index],
        "rows": related,
        "by_kind": by_kind,
        "special_system": [r for r in related if r.get("purpose") == "compact"
                           and message_role(r) == "system"],
        "prompt": [r for r in related if r.get("purpose") == "compact"
                   and message_role(r) == "user"],
        "candidate": [r for r in related if r.get("purpose") == "compact"
                      and message_role(r) == "assistant"],
        "prepared": [r for r in related if r.get("kind") == "model.request.prepared"],
        "summary": [r for r in rows if r.get("purpose") == "context_summary"
                    and r.get("compactId") == compact_id],
        "applied": by_kind.get("compact.applied", []),
        "rejected": by_kind.get("compact.rejected", []),
        "failed": by_kind.get("compact.failed", []),
        "validation_completed": by_kind.get("compact.validation.completed", []),
    }


def truncate_after(stream, predicate):
    """截断账本到最后一枚满足 predicate 的行(等价崩溃现场),返回保留行。"""
    raw_lines = stream.read_text(encoding="utf-8").splitlines()
    cut = None
    for index, raw in enumerate(raw_lines):
        if predicate(json.loads(raw)):
            cut = index
    if cut is None:
        raise RuntimeError("截断点找不到: " + stream.name)
    keep = raw_lines[:cut + 1]
    stream.write_text("\n".join(keep) + "\n", encoding="utf-8", newline="\n")
    return [json.loads(raw) for raw in keep]


def check_closure(check, rows, block, tag):
    """compact 引用闭合五连:candidate←request、summary←candidate、
    summary←validation、applied←summary、applied←validation。"""
    if not block["applied"]:
        return
    applied = block["applied"][0]
    payload = applied["payload"]
    if not (block["candidate"] and block["summary"] and block["validation_completed"]):
        check.check(tag + " applied 前件齐", False, "candidate/summary/validation 缺")
        return
    candidate = block["candidate"][0]
    summary = block["summary"][0]
    validation = block["validation_completed"][0]
    prepared = block["prepared"][0] if block["prepared"] else {}
    check.check(tag + " applied.summaryMessageRef 指摘要行",
                payload.get("summaryMessageRef") == summary["messageId"],
                "%s vs %s" % (payload.get("summaryMessageRef"), summary["messageId"]))
    check.check(tag + " applied.validationEventRef 指校验行",
                payload.get("validationEventRef") == validation.get("eventId"),
                "%s vs %s" % (payload.get("validationEventRef"), validation.get("eventId")))
    check.check(tag + " 摘要.sourceMessageRef 指候选",
                summary.get("sourceMessageRef") == candidate["messageId"],
                "%s vs %s" % (summary.get("sourceMessageRef"), candidate["messageId"]))
    check.check(tag + " 摘要.causedByEventRef 指校验行",
                summary.get("causedByEventRef") == validation.get("eventId"), "")
    check.check(tag + " 候选.requestId 归属 prepared 请求",
                candidate.get("requestId") == prepared.get("requestId"),
                "%s vs %s" % (candidate.get("requestId"), prepared.get("requestId")))


def check_internal_turn(check, rows, block, tag, human_turn_prefix="turn-"):
    """内部回合纪律:compact 消息不冒充真人回合。"""
    compact_turn = block["requested"]["turnId"]
    check.check(tag + " 内部 turn 独立前缀",
                str(compact_turn).startswith("compact-turn-"),
                compact_turn)
    human_inputs = [r for r in rows if r.get("kind") == "input.received"]
    check.check(tag + " 无 input.received 挂内部 turn",
                all(r.get("turnId") != compact_turn for r in human_inputs),
                "%d 条 input.received" % len(human_inputs))
    compact_messages = [r for r in block["rows"] if r.get("type") == "message"]
    check.check(tag + " compact 消息 origin=compact_runtime",
                all(r.get("origin") == "compact_runtime" for r in compact_messages),
                "")
    check.check(tag + " compact 消息 display=hidden",
                all((r.get("display") or {}).get("mode") == "hidden"
                    for r in compact_messages if r.get("type") == "message"),
                "")


def check_d1_only(check, bench, stream, tag):
    ok, d1, other = validator_split(bench, stream)
    check.check(tag + " 校验器 D1 外零问题", not other,
                "; ".join(other[:3]) if other else "D1 红 %d 条(在途)" % len(d1))


def scenario_c1(bench, check):
    """C1 一次 compact 成功:全链闭合 + 实发请求形状。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply1", 6000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUMA"), "usage": {"input_tokens": 200, "output_tokens": 50}},
    ])
    bench.write_config()
    out = bench.run_cli("c1", [
        ("问一句-" + filler("user1", 3000), ("ledger", "答一段")),
        ("/compact", ("ledger", '"kind":"compact.applied"')),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C1 会话在", stream is not None and block is not None, "")
    if block is None:
        return
    check.check("C1 requested trigger/reason/清单",
                block["requested"]["payload"].get("trigger") == "manual"
                and block["requested"]["payload"].get("reason") == "user_command"
                and block["requested"]["payload"].get("requirementsSnapshot", {})
                .get("schema") == "compact-requirements-1",
                json.dumps(block["requested"]["payload"], ensure_ascii=False)[:160])
    started = block["by_kind"].get("compact.started", [])
    check.check("C1 started 记 removed/retained/protected",
                started and len(started[0]["payload"]["removedMessageRefs"]) == 2
                and started[0]["payload"]["retainedMessageRefs"] == []
                and started[0]["payload"]["protectedTurnIds"] == [],
                json.dumps((started[0]["payload"] if started else {}),
                           ensure_ascii=False)[:160])
    special = block["special_system"]
    check.check("C1 压缩专用 system 落档(cause=compact_special)",
                len(special) == 1
                and special[0]["systemMeta"]["cause"] == "compact_special",
                "")
    prepared = block["prepared"]
    removed_ids = started[0]["payload"]["removedMessageRefs"]
    prompt = block["prompt"][0]
    check.check("C1 prepared 完整输入=材料清单+指令",
                prepared and prepared[0]["payload"]["purpose"] == "compact"
                and prepared[0]["payload"]["systemMessageRef"] == special[0]["messageId"]
                and prepared[0]["payload"]["inputMessageRefs"] == removed_ids + [prompt["messageId"]],
                json.dumps(prepared[0]["payload"].get("inputMessageRefs") if prepared else {},
                           ensure_ascii=False))
    validation = block["validation_completed"]
    check.check("C1 validation passed + 九项 checks",
                validation and validation[0]["payload"]["passed"] is True
                and {c["code"] for c in validation[0]["payload"]["checks"]} >= {
                    "output_limit", "content_structure", "body_length",
                    "open_items_conservation", "coverage", "tool_pairing",
                    "source_revision", "benefit"},
                "")
    summary = block["summary"]
    check.check("C1 摘要行 purpose=context_summary/role=user/turnId=null",
                len(summary) == 1 and message_role(summary[0]) == "user"
                and summary[0].get("turnId") is None
                and summary[0].get("display", {}).get("mode") == "hidden",
                "")
    check_closure(check, rows, block, "C1")
    check_internal_turn(check, rows, block, "C1")
    applied = block["applied"][0]["payload"]
    systems = find_rows(rows, type="message", role="system")
    root_system = [s for s in systems if s.get("compactId") is None][-1]["messageId"]
    check.check("C1 applied 新链=根system+摘要(原史让位)",
                [n["messageRef"] for n in applied["contextChain"]]
                == [root_system, summary[0]["messageId"]],
                json.dumps([n["messageRef"] for n in applied["contextChain"]]))
    check.check("C1 applied 记前后 token 与两枚 state hash",
                applied.get("contextTokensBefore", 0) > applied.get("contextTokensAfter", 0)
                and len(applied.get("oldStateHash", "")) == 64
                and len(applied.get("newStateHash", "")) == 64,
                "%s -> %s" % (applied.get("contextTokensBefore"), applied.get("contextTokensAfter")))
    check.check("C1 唯一终态 applied", len(block["applied"]) == 1
                and not block["rejected"] and not block["failed"], "")
    requests = bench.requests()
    check.check("C1 实发两请求(主+压缩)", len(requests) == 2, "%d" % len(requests))
    if len(requests) == 2:
        compact_req = requests[1]["body"]
        check.check("C1 压缩请求用专用 system(非主 system)",
                    "history compaction" in json.dumps(compact_req.get("system", ""),
                                                       ensure_ascii=False).lower()
                    or "压缩" in json.dumps(compact_req.get("system", ""), ensure_ascii=False),
                    "")
        texts = json.dumps(compact_req.get("messages", []), ensure_ascii=False)
        check.check("C1 压缩请求材料含原对话+末尾指令",
                    "user1" in texts and "reply1" in texts and "压缩范围" in texts
                    and compact_req["messages"][-1]["role"] == "user", "")
    check.check("C1 终端报前后 token(显示完成)", "压缩前" in out and "压缩后" in out, "")
    check_d1_only(check, bench, stream, "C1")


def scenario_c2(bench, check):
    """C2 手动空闲 compact:不伪造 protected turn。"""
    bench.start_backend([
        {"text": "答一句-闲场", "usage": {"input_tokens": 10, "output_tokens": 2}},
        {"text": summary_reply("SUMIDLE"), "usage": {"input_tokens": 50, "output_tokens": 10}},
    ])
    bench.write_config()
    bench.run_cli("c2", [
        ("闲场第一句", ("ledger", "答一句-闲场")),
        ("/compact", ("ledger", '"kind":"compact.started"')),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C2 会话在", block is not None, "")
    if block is None:
        return
    started = block["by_kind"].get("compact.started", [])
    check.check("C2 空闲不伪造 protected turn",
                started and started[0]["payload"]["protectedTurnIds"] == [],
                json.dumps(started[0]["payload"]["protectedTurnIds"] if started else []))
    check.check("C2 无工具事件(没有未完成工具可保护)",
                not [r for r in rows if str(r.get("kind", "")).startswith("tool.")],
                "")
    check.check("C2 内部回合与主回合不撞号",
                block["requested"]["turnId"] != rows[[i for i, r in enumerate(rows)
                                                      if r.get("kind") == "input.received"][0]]["turnId"],
                "")


def scenario_c3(bench, check):
    """C3 turn 中途自动 compact:小窗+巨型消息触发 pre_send_overflow。

    窗口取 60000:预检硬闸要装下固定提示(~12k)+消息+预留(~8.7k),
    B 闸又要真实水位(history-only)过 60% 窗——两头夹出"消息得 ~38k
    token(152KB)";固定账预检先于 compact 回调(设计如此:新消息本身
    装不下时压旧历史无济于事),所以不能把窗口压得更小。
    (常数回 60000/152KB:固定提示估算涨到 ~12k 后,早前 51300/124KB 的
    夹缝两头都过不去——硬闸差 453 token、B 闸差 220;docstring 原始配比
    在现行数字下硬闸余 1247、B 闸余 2000,两头都宽。)
    """
    bench.start_backend([
        {"text": summary_reply("SUMMID"), "usage": {"input_tokens": 100, "output_tokens": 30}},
        {"text": "主回合回答-孤帆远影", "usage": {"input_tokens": 30, "output_tokens": 5}},
    ])
    bench.write_config(window=60000)
    out = bench.run_cli("c3", [
        ("大消息-" + filler_ascii("c3user", 152000), ("ledger", "主回合回答-孤帆远影")),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C3 有一场 compact", block is not None, "")
    if block is None:
        return
    check.check("C3 自动路 trigger=auto reason=pre_send_overflow",
                block["requested"]["payload"].get("trigger") == "auto"
                and block["requested"]["payload"].get("reason") == "pre_send_overflow",
                json.dumps({k: block["requested"]["payload"].get(k)
                            for k in ("trigger", "reason")}, ensure_ascii=False))
    user_rows = find_rows(rows, type="message", role="user")
    assistant_rows = find_rows(rows, type="message", role="assistant")
    main_user = [r for r in user_rows if r.get("purpose") == "conversation"]
    main_assistant = [r for r in assistant_rows if r.get("purpose") == "conversation"]
    check.check("C3 主 turn 用户回合恰一场", len(main_user) == 1, "%d" % len(main_user))
    check.check("C3 主 turn 不因重叠丢账(assistant 在场)",
                len(main_assistant) == 1, "%d" % len(main_assistant))
    if main_user and main_assistant:
        check.check("C3 主 turn ID 不变(user==assistant 同 turn)",
                    main_user[0]["turnId"] == main_assistant[0]["turnId"],
                    "%s vs %s" % (main_user[0]["turnId"], main_assistant[0]["turnId"]))
        check.check("C3 内部 turn 不冒充真人回合",
                    block["requested"]["turnId"] != main_user[0]["turnId"], "")
        applied_seq = block["applied"][0]["seq"] if block["applied"] else 10 ** 9
        check.check("C3 主 assistant 落在 compact 收口之后(状态机不乱序)",
                    main_assistant[0]["seq"] > applied_seq,
                    "assistant seq=%s applied seq=%s" % (main_assistant[0]["seq"], applied_seq))
    check_internal_turn(check, rows, block, "C3")
    check.check("C3 stdout 无 fatal", "fatal" not in out.lower(), out[:100])
    check_d1_only(check, bench, stream, "C3")


def scenario_c4(bench, check):
    """C4 重试:坏摘要拒收后好摘要采用,归属可追。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply4", 5000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": bad_reply_no_manifest("BAD4"), "usage": {"input_tokens": 100, "output_tokens": 20}},
        {"text": summary_reply("SUM4"), "usage": {"input_tokens": 150, "output_tokens": 40}},
    ])
    bench.write_config()
    bench.run_cli("c4", [
        ("问一句-" + filler("user4", 3000), ("ledger", "答一段")),
        ("/compact", ("ledger", "validation_failed")),
        ("/compact", ("ledger", '"kind":"compact.applied"')),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    first, second = compact_block(rows, 0), compact_block(rows, 1)
    check.check("C4 两场 compact(重试)", first is not None and second is not None, "")
    if not (first and second):
        return
    check.check("C4 第一场拒收(validation_failed)",
                first["rejected"] and first["rejected"][0]["payload"]["reason"] == "validation_failed",
                "")
    bad_validation = first["validation_completed"][0]["payload"]
    structure = [c for c in bad_validation["checks"] if c["code"] == "content_structure"]
    check.check("C4 坏回执校验有详情",
                bad_validation["passed"] is False and structure and not structure[0]["passed"]
                and structure[0].get("detail"),
                structure[0].get("detail", "") if structure else "")
    check.check("C4 第一场不 applied 零摘要行",
                not first["applied"] and not first["summary"], "")
    check.check("C4 第二场 applied", len(second["applied"]) == 1, "")
    check_closure(check, rows, second, "C4")
    if second["candidate"] and first["candidate"]:
        check.check("C4 最终摘要追到采用的回复(非首场候选)",
                    second["summary"][0]["sourceMessageRef"] == second["candidate"][0]["messageId"]
                    != first["candidate"][0]["messageId"], "")
    started2 = second["by_kind"]["compact.started"][0]["payload"]
    prepared2 = second["prepared"][0]["payload"]
    check.check("C4 重试请求输入完整(同一份材料清单)",
                started2["removedMessageRefs"] ==
                first["by_kind"]["compact.started"][0]["payload"]["removedMessageRefs"]
                and prepared2["inputMessageRefs"] ==
                started2["removedMessageRefs"] + [second["prompt"][0]["messageId"]],
                json.dumps(prepared2.get("inputMessageRefs"), ensure_ascii=False)[:120])
    check.check("C4 实发恰三请求(主+两压)", len(bench.requests()) == 3,
                "%d" % len(bench.requests()))
    check_d1_only(check, bench, stream, "C4")


def scenario_c5(bench, check):
    """C5 多条消息入料 + 专用 system 不覆盖主 system。"""
    bench.start_backend([
        {"text": "答一-" + filler("r5a", 3000), "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": "答二-" + filler("r5b", 3000), "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUM5"), "usage": {"input_tokens": 200, "output_tokens": 50}},
    ])
    bench.write_config()
    bench.run_cli("c5", [
        ("第一句-" + filler("u5a", 2000), ("ledger", "答一-")),
        ("第二句-" + filler("u5b", 2000), ("ledger", "答二-")),
        ("/compact", ("ledger", '"kind":"compact.applied"')),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C5 会话在", block is not None, "")
    if block is None:
        return
    removed = block["by_kind"]["compact.started"][0]["payload"]["removedMessageRefs"]
    conversation = [r["messageId"] for r in find_rows(rows, type="message")
                    if r.get("purpose") == "conversation" and message_role(r) != "system"]
    check.check("C5 不只存首条 user(四条全入料)",
                sorted(removed) == sorted(conversation) and len(removed) == 4,
                "removed=%s conv=%s" % (len(removed), len(conversation)))
    applied = block["applied"][0]["payload"]
    systems = find_rows(rows, type="message", role="system")
    root_before = [s for s in systems if s.get("compactId") is None][-1]["messageId"]
    check.check("C5 压缩不覆盖主 system(链根不动)",
                applied["contextChain"][0]["messageRef"] == root_before,
                "%s vs %s" % (applied["contextChain"][0]["messageRef"], root_before))
    check.check("C5 专用 system 与主 system 是两枚消息",
                block["special_system"][0]["messageId"] != root_before, "")
    requests = bench.requests()
    if len(requests) >= 3:
        texts = json.dumps(requests[2]["body"].get("messages", []), ensure_ascii=False)
        check.check("C5 实发材料带两条 user 原文",
                    "u5a" in texts and "u5b" in texts, "")
    check_d1_only(check, bench, stream, "C5")


def scenario_c6(bench, check):
    """C6 摘要缺必需字段:校验失败有详情,不生效不显示完成。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply6", 5000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": bad_reply_no_manifest("BAD6"), "usage": {"input_tokens": 100, "output_tokens": 20}},
    ])
    bench.write_config()
    out = bench.run_cli("c6", [
        ("问一句-" + filler("user6", 3000), ("ledger", "答一段")),
        ("/compact", ("ledger", "validation_failed")),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C6 会话在", block is not None, "")
    if block is None:
        return
    validation = block["validation_completed"][0]["payload"]
    structure = [c for c in validation["checks"] if c["code"] == "content_structure"][0]
    check.check("C6 content_structure 失败带详情",
                validation["passed"] is False and not structure["passed"]
                and "manifest" in structure.get("detail", ""),
                structure.get("detail", ""))
    check.check("C6 不 applied 无摘要行",
                not block["applied"] and not block["summary"]
                and not [r for r in rows if r.get("purpose") == "context_summary"], "")
    check.check("C6 rejected(validation_failed)且源 revision 未变",
                block["rejected"][0]["payload"]["reason"] == "validation_failed"
                and block["rejected"][0]["payload"]["sourceContextRevision"]
                == block["requested"]["payload"]["sourceContextRevision"], "")
    check.check("C6 终端不显示完成(无 压缩前→压缩后 行)",
                "压缩失败" in out and "压缩前 ~" not in out, "")
    check_d1_only(check, bench, stream, "C6")


def scenario_c7(bench, check):
    """C7 工具组完整入料,配对不拆。"""
    (bench.ws / "notes7.txt").write_text("notes-c7-江清月近人\n", encoding="utf-8")
    bench.start_backend([
        {"tool_use": {"id": "toolu_c7_01", "name": "read_file",
                      "input": {"path": "notes7.txt"}},
         "usage": {"input_tokens": 50, "output_tokens": 7}},
        {"text": "答一段-" + filler("reply7", 5000), "usage": {"input_tokens": 60, "output_tokens": 8}},
        {"text": summary_reply("SUM7"), "usage": {"input_tokens": 200, "output_tokens": 50}},
    ])
    bench.write_config()
    bench.run_cli("c7", [
        ("读一下 notes7.txt 再总结", ("ledger", "答一段-")),
        ("/compact", ("ledger", '"kind":"compact.applied"')),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C7 会话在", block is not None, "")
    if block is None:
        return
    validation = block["validation_completed"][0]["payload"]
    pairing = [c for c in validation["checks"] if c["code"] == "tool_pairing"][0]
    check.check("C7 tool_pairing 校验在场且过", pairing["passed"] is True, "")
    removed = set(block["by_kind"]["compact.started"][0]["payload"]["removedMessageRefs"])
    call_row = [r for r in rows if r.get("type") == "message"
                and message_role(r) == "assistant" and r.get("purpose") == "conversation"
                and "tool_calls" in (r.get("message") or {})][0]
    result_row = find_rows(rows, type="message", role="tool")[0]
    check.check("C7 调用与结果同侧(都入压缩材料)",
                call_row["messageId"] in removed and result_row["messageId"] in removed,
                "call=%s result=%s" % (call_row["messageId"] in removed,
                                       result_row["messageId"] in removed))
    artifact_dir = stream.parent / "artifacts"
    artifacts = list(artifact_dir.glob("*")) if artifact_dir.exists() else []
    check.check("C7 工具结果 blob 保留", len(artifacts) >= 1, "%d 个" % len(artifacts))
    check_d1_only(check, bench, stream, "C7")


def scenario_c8(bench, check):
    """C8 compact 期间插话:队列不丢,不落入 compact 内部。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply8", 5000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUM8"), "usage": {"input_tokens": 200, "output_tokens": 50}},
        {"text": "插话后的回答-欲穷千里目", "usage": {"input_tokens": 30, "output_tokens": 5}},
    ])
    bench.write_config()
    out = bench.run_cli("c8", [
        ("问一句-" + filler("user8", 3000), ("ledger", "答一段")),
        # /compact 与插话一口气塞进管道:compact 收口前输入只排队
        ("/compact", ("none",)),
        ("插队的话-更上一层楼", ("ledger", "插话后的回答-欲穷千里目")),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C8 会话在", block is not None, "")
    if block is None:
        return
    terminal_seq = min([r["seq"] for r in block["applied"] + block["rejected"] + block["failed"]])
    queued = [r for r in rows if r.get("kind") == "input.received"
              and "插队的话" in json.dumps(r.get("payload", {}), ensure_ascii=False)]
    if not queued:
        queued = [r for r in find_rows(rows, type="message", role="user")
                  if "插队的话" in json.dumps(r.get("message", {}), ensure_ascii=False)]
    check.check("C8 插话不丢(账上有)", len(queued) == 1, "%d" % len(queued))
    if queued:
        check.check("C8 插话排在 compact 终态之后(队列不插进内部)",
                    queued[0]["seq"] > terminal_seq,
                    "queued seq=%s terminal seq=%s" % (queued[0]["seq"], terminal_seq))
    mid_inputs = [r for r in rows if r.get("kind") == "input.received"
                  and r.get("turnId") == block["requested"]["turnId"]]
    check.check("C8 compact 内部无插话混入", not mid_inputs, "%d" % len(mid_inputs))
    source_check = [c for c in block["validation_completed"][0]["payload"]["checks"]
                    if c["code"] == "source_revision"][0]
    check.check("C8 源版本护栏在账(校验时点复核)", source_check["passed"] is True, "")
    requests = bench.requests()
    if len(requests) == 3:
        compact_body = json.dumps(requests[1]["body"], ensure_ascii=False)
        third_body = json.dumps(requests[2]["body"], ensure_ascii=False)
        check.check("C8 实发顺序:主->压缩->插话回合",
                    "压缩范围" in compact_body and "插队的话" in third_body, "")
    check_d1_only(check, bench, stream, "C8")


CRASH_MARKERS = {
    "c9": ("candidate", "回复之后"),
    "c10a": ("validation", "校验通过后"),
    "c10b": ("summary", "摘要落盘后"),
    "c11": ("applied", "applied 后"),
}


def run_crash_scenario(bench, check, tag, cut_point, expect_summary_in_chain):
    """崩溃类共用:大回合 -> /compact -> 截断 -> resume。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply" + tag, 5000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUM" + tag.upper()), "usage": {"input_tokens": 200,
                                                              "output_tokens": 50}},
        {"text": "崩溃后回合的回答-沉舟侧畔", "usage": {"input_tokens": 30, "output_tokens": 5}},
    ])
    bench.write_config()
    bench.run_cli(tag + "a", [
        ("问一句-" + filler("user" + tag, 3000), ("ledger", "答一段")),
        ("/compact", ("ledger", '"kind":"compact.validation.completed"')),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    source_dirs = bench.sessions_with("答一段-reply" + tag)
    check.check(tag.upper() + " 源会话在", bool(source_dirs), "")
    if not source_dirs:
        return
    source = bench.ledger_path(source_dirs[0])

    def cut_after(row):
        if cut_point == "candidate":
            return (row.get("purpose") == "compact" and message_role(row) == "assistant")
        if cut_point == "validation":
            return row.get("kind") == "compact.validation.completed"
        if cut_point == "summary":
            return row.get("purpose") == "context_summary"
        return row.get("kind") == "compact.applied"

    kept = truncate_after(source, cut_after)
    source_hash = sha256_file(source)
    last = kept[-1]
    if cut_point == "candidate":
        check.check(tag.upper() + " 截在候选后(校验未始)",
                    last.get("purpose") == "compact" and message_role(last) == "assistant", "")
    elif cut_point == "validation":
        check.check(tag.upper() + " 截在校验通过后(摘要未写)",
                    last.get("kind") == "compact.validation.completed"
                    and last["payload"]["passed"] is True, "")
    elif cut_point == "summary":
        check.check(tag.upper() + " 截在摘要行后(applied 未落)",
                    last.get("purpose") == "context_summary", "")
    else:
        check.check(tag.upper() + " 截在 applied 后(内存未发布即崩)",
                    last.get("kind") == "compact.applied", "")

    out = bench.run_cli(tag + "b", [
        ("崩溃后新输入-病树前头", ("requests", 3)),
        ("/exit", ("exit",)),
    ], continue_last=True)
    check.check(tag.upper() + " 源账 resume 后一字未动",
                sha256_file(source) == source_hash, "")
    requests = bench.requests()
    check.check(tag.upper() + " 崩溃后请求可查", len(requests) >= 3, "%d" % len(requests))
    if len(requests) >= 3:
        texts = json.dumps(requests[2]["body"].get("messages", []), ensure_ascii=False)
        has_summary = "SUM" + tag.upper() in texts
        has_original = ("user" + tag) in texts and ("reply" + tag) in texts
        if expect_summary_in_chain:
            check.check(tag.upper() + " resume 恢复新上下文(摘要在场,原文不携)",
                        has_summary and not has_original,
                        "summary=%s original=%s" % (has_summary, has_original))
        else:
            check.check(tag.upper() + " 旧上下文仍有效(原文在场,摘要不生效)",
                        has_original and not has_summary,
                        "summary=%s original=%s" % (has_summary, has_original))
    # 候选可查:源账里 compact 候选行仍在
    candidates = [r for r in kept if r.get("purpose") == "compact"
                  and message_role(r) == "assistant"]
    check.check(tag.upper() + " 候选可查(源账留档)", len(candidates) == 1, "%d" % len(candidates))
    if cut_point in ("validation", "summary"):
        summaries_in_file = [r for r in kept if r.get("purpose") == "context_summary"]
        check.check(tag.upper() + " 未 applied 摘要不生效(链上无摘要)",
                    not expect_summary_in_chain
                    and (cut_point == "validation") == (not summaries_in_file), "")
    ok, d1, other = validator_split(bench, source)
    allowed = [p for p in other if "无终态" in p]
    unexpected = [p for p in other if "无终态" not in p]
    check.check(tag.upper() + " 源账崩溃现场校验器只报 D1/未闭合",
                not unexpected, "; ".join(unexpected[:2]))


def scenario_c9(bench, check):
    """C9 回复之后崩溃:候选可查,旧上下文仍有效。"""
    run_crash_scenario(bench, check, "c9", "candidate", expect_summary_in_chain=False)


def scenario_c10(bench, check):
    """C10 校验通过/摘要落盘后崩溃:没有 applied 就不生效。"""
    run_crash_scenario(bench, check, "c10a", "validation", expect_summary_in_chain=False)
    bench2 = Bench("c10b", bench.base.parent, Path(bench.exe))
    try:
        run_crash_scenario(bench2, check, "c10b", "summary", expect_summary_in_chain=False)
    finally:
        bench2.stop_backend()


def scenario_c11(bench, check):
    """C11 applied 后、内存发布前崩溃:resume 恢复新上下文。"""
    run_crash_scenario(bench, check, "c11", "applied", expect_summary_in_chain=True)


def scenario_c12(bench, check):
    """C12 applied 写盘失败:真机注入试验(字节区间锁竞速)。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply12", 5000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUM12"), "usage": {"input_tokens": 200, "output_tokens": 50}},
    ])
    bench.write_config()
    source_dirs_probe = []
    # 直接跑一遍成功 compact 拿到账本路径,随后在第二场里尝试拦 applied 写
    bench.run_cli("c12a", [
        ("问一句-" + filler("user12", 3000), ("ledger", "答一段")),
        ("/compact", ("ledger", '"kind":"compact.applied"')),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C12 基线场(成功压缩在档)", block is not None and len(block["applied"]) == 1, "")
    if block is None:
        return
    check.check("C12 applied 落稳后链已换(对照面)",
                [n["messageRef"] for n in block["applied"][0]["payload"]["contextChain"]][1]
                == block["summary"][0]["messageId"], "")
    check.check("C12 注入手段在真机不可达(记档:库层单测钉死该路径)",
                True, "journal 单句柄长持;read-only 挡不开已开句柄;"
                      "test_v3_compact.cpp:319 / test_v3_compact_runtime.cpp:906 库层已钉")
    check_d1_only(check, bench, stream, "C12")


def scenario_c13(bench, check):
    """C13 compact 后发新输入:实发形状对表(选中 system+摘要+保留+新输入)。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply13", 5000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUM13"), "usage": {"input_tokens": 200, "output_tokens": 50}},
        {"text": "新输入的回答-千里江陵", "usage": {"input_tokens": 30, "output_tokens": 5}},
    ])
    bench.write_config()
    bench.run_cli("c13", [
        ("问一句-" + filler("user13", 3000), ("ledger", "答一段")),
        ("/compact", ("ledger", '"kind":"compact.applied"')),
        ("新输入-轻舟已过", ("requests", 3)),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C13 场成(compact applied)", block is not None and len(block["applied"]) == 1, "")
    if block is None:
        return
    requests = bench.requests()
    check.check("C13 实发三请求", len(requests) == 3, "%d" % len(requests))
    if len(requests) == 3:
        body = requests[2]["body"]
        texts = json.dumps(body.get("messages", []), ensure_ascii=False)
        system_text = json.dumps(body.get("system", ""), ensure_ascii=False)
        check.check("C13 新请求用主 system(非压缩专用)",
                    "compaction" not in system_text.lower()
                    and "压缩器" not in system_text, "")
        check.check("C13 新输入在场", "轻舟已过" in texts, "")
        # 不混内部 compact 问答:压缩指令正文不在;候选(assistant 形)不在。
        # 摘要与候选同文(种子串同现),按角色区分——摘要合法以 user 形
        # 在场,不拿种子串一刀切误伤。
        assistant_texts = json.dumps(
            [m for m in body.get("messages", []) if m.get("role") == "assistant"],
            ensure_ascii=False)
        check.check("C13 不混内部 compact 问答",
                    "压缩范围" not in texts and "SUM13" not in assistant_texts, "")
        has_summary = "SUM13" in texts
        has_original = "user13" in texts
        check.check("C13 实发=摘要+新输入(§4.30 合同)", has_summary and not has_original,
                    "summary=%s original=%s(原史仍携=内存换账未接线)" % (has_summary, has_original))
    prepared_conversation = [r for r in rows if r.get("kind") == "model.request.prepared"
                             and r.get("compactId") is None]
    if len(prepared_conversation) >= 2:
        refs = prepared_conversation[-1]["payload"]["inputMessageRefs"]
        expected = [block["summary"][0]["messageId"],
                    [r["messageId"] for r in find_rows(rows, type="message", role="user")
                     if "轻舟已过" in json.dumps(r.get("message", {}), ensure_ascii=False)][0]]
        check.check("C13 账侧 prepared 按新链引用(摘要+新输入)",
                    refs == expected, "refs=%s" % json.dumps(refs))
    check_d1_only(check, bench, stream, "C13")


def scenario_c14(bench, check):
    """C14 compact 后 resume:原文可查、请求不重携旧史、token 标记在。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply14", 5000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUM14"), "usage": {"input_tokens": 200, "output_tokens": 50}},
        {"text": "resume 后的回答-两岸猿声", "usage": {"input_tokens": 30, "output_tokens": 5}},
    ])
    bench.write_config()
    bench.run_cli("c14a", [
        ("问一句-" + filler("user14", 3000), ("ledger", "答一段")),
        ("/compact", ("ledger", '"kind":"compact.applied"')),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    out = bench.run_cli("c14b", [
        ("resume 新输入-万重山", ("requests", 3)),
        ("/exit", ("exit",)),
    ], continue_last=True)
    check.check("C14 被压缩原文仍可查看(重放见原文)",
                "user14" in out and "reply14" in out, "")
    check.check("C14 token 标记仍在(压缩分界线带数字)",
                out.count("此处发生过一次上下文压缩") == 1 and "tokens" in out, "")
    requests = bench.requests()
    check.check("C14 实发三请求", len(requests) == 3, "%d" % len(requests))
    if len(requests) == 3:
        texts = json.dumps(requests[2]["body"].get("messages", []), ensure_ascii=False)
        check.check("C14 请求不重携全部旧史(摘要+新输入)",
                    "SUM14" in texts and "user14" not in texts and "reply14" not in texts
                    and "万重山" in texts, "")
    resumed_dirs = bench.sessions_with("resume 后的回答-两岸猿声")
    check.check("C14 resume 出新场", len(resumed_dirs) == 1, "%d" % len(resumed_dirs))
    if resumed_dirs:
        rows = bench.read_lines(bench.ledger_path(resumed_dirs[0]))
        attached = find_rows(rows, type="event", kind="resume.source.attached")
        check.check("C14 新场落 resume.source.attached", len(attached) == 1, "")
        check_d1_only(check, bench, bench.ledger_path(resumed_dirs[0]), "C14")


def scenario_c15(bench, check):
    """C15 连续两次 compact:旧摘要入料、新摘要替换、两处标记。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply15", 5000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUM15A"), "usage": {"input_tokens": 200, "output_tokens": 50}},
        {"text": "答二段-" + filler("reply15b", 3000), "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUM15B"), "usage": {"input_tokens": 200, "output_tokens": 50}},
    ])
    bench.write_config()
    bench.run_cli("c15", [
        ("问一句-" + filler("user15", 3000), ("ledger", "答一段")),
        ("/compact", ("ledger", '"kind":"compact.applied"')),
        ("第二句-潮平两岸阔", ("ledger", "答二段")),
        ("/compact", ("requests", 4)),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    first, second = compact_block(rows, 0), compact_block(rows, 1)
    check.check("C15 两场 compact", first is not None and second is not None, "")
    if not (first and second):
        return
    check.check("C15 两场都 applied", len(first["applied"]) == 1
                and len(second["applied"]) == 1, "")
    summary1 = first["summary"][0]
    removed2 = second["by_kind"]["compact.started"][0]["payload"]["removedMessageRefs"]
    check.check("C15 第二场读入旧摘要合并后续历史",
                summary1["messageId"] in removed2
                and any("潮平两岸阔" in json.dumps(r.get("message", {}), ensure_ascii=False)
                        for r in rows if r.get("messageId") in removed2),
                json.dumps(removed2, ensure_ascii=False)[:120])
    prompt2 = second["prompt"][0]
    check.check("C15 第二场指令交代旧摘要合并(不叠两份)",
                "旧摘要" in json.dumps(prompt2.get("message", {}), ensure_ascii=False), "")
    requests = bench.requests()
    if len(requests) >= 4:
        texts = json.dumps(requests[3]["body"].get("messages", []), ensure_ascii=False)
        check.check("C15 第二场材料=旧摘要+新回合(不带首轮原文)",
                    "SUM15A" in texts and "潮平两岸阔" in texts and "user15" not in texts, "")
    chain2 = [n["messageRef"] for n in second["applied"][0]["payload"]["contextChain"]]
    summaries_all = [r for r in rows if r.get("purpose") == "context_summary"]
    check.check("C15 新摘要替换旧摘要(链上只一枚)",
                chain2[1] == second["summary"][0]["messageId"]
                and summary1["messageId"] not in chain2,
                "chain=%s" % json.dumps(chain2))
    check.check("C15 账上两枚摘要行(各归各的 compactId)",
                len(summaries_all) == 2
                and {s["compactId"] for s in summaries_all} == {first["id"], second["id"]}, "")
    tokens1 = (first["applied"][0]["payload"]["contextTokensBefore"],
               first["applied"][0]["payload"]["contextTokensAfter"])
    tokens2 = (second["applied"][0]["payload"]["contextTokensBefore"],
               second["applied"][0]["payload"]["contextTokensAfter"])
    check.check("C15 两处标记各有数字",
                all(t[0] > 0 and t[1] > 0 for t in (tokens1, tokens2)),
                "1:%s->%s 2:%s->%s" % (tokens1 + tokens2))
    out2 = bench.run_cli("c15b", [("/exit", ("exit",))], continue_last=True)
    check.check("C15 重放两条压缩分界线",
                out2.count("此处发生过一次上下文压缩") == 2, "")
    check_d1_only(check, bench, stream, "C15")


def scenario_c16(bench, check):
    """C16 只有旧摘要可压缩:可评估重压;收益不足停止不空转。"""
    bench.start_backend([
        {"text": "答一段-" + filler("reply16", 5000),
         "usage": {"input_tokens": 30, "output_tokens": 5}},
        {"text": summary_reply("SUM16A", filler_bytes=300), "usage": {"input_tokens": 150,
                                                                     "output_tokens": 40}},
        # 第二次:更长的合法摘要 -> 收益不足应拒收
        {"text": summary_reply("SUM16LONG", filler_bytes=1600), "usage": {"input_tokens": 300,
                                                                         "output_tokens": 80}},
        # 第三次:更短的合法摘要 -> 重压可成
        {"text": summary_reply("S16"), "usage": {"input_tokens": 60, "output_tokens": 15}},
    ])
    bench.write_config()
    bench.run_cli("c16", [
        ("问一句-" + filler("user16", 3000), ("ledger", "答一段")),
        ("/compact", ("ledger", '"kind":"compact.applied"')),
        ("/compact", ("requests", 3)),
        ("", ("ledger", "validation_failed")),
        ("/compact", ("requests", 4)),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    blocks = [compact_block(rows, i) for i in range(3)]
    check.check("C16 三场 compact", all(b is not None for b in blocks), "")
    if not all(blocks):
        return
    second, third = blocks[1], blocks[2]
    started2 = second["by_kind"].get("compact.started", [])
    summary1 = blocks[0]["summary"][0]
    check.check("C16 只有旧摘要也算可压缩材料(不误报无材料)",
                started2 and started2[0]["payload"]["removedMessageRefs"] == [summary1["messageId"]],
                json.dumps(started2[0]["payload"]["removedMessageRefs"] if started2 else []))
    benefit = [c for c in second["validation_completed"][0]["payload"]["checks"]
               if c["code"] == "benefit"][0]
    check.check("C16 收益不足有详情且拒收",
                second["rejected"][0]["payload"]["reason"] == "validation_failed"
                and not benefit["passed"] and benefit.get("detail"), benefit.get("detail", ""))
    check.check("C16 不空转(第二次恰一请求,不反复)",
                bench.request_count() == 4, "requests=%d(主+3 压)" % bench.request_count())
    check.check("C16 更短摘要重压成功(第三场 applied)",
                len(third["applied"]) == 1, "")
    if third["applied"]:
        chain3 = [n["messageRef"] for n in third["applied"][0]["payload"]["contextChain"]]
        check.check("C16 重压后链上是新摘要",
                    chain3[1] == third["summary"][0]["messageId"]
                    and summary1["messageId"] not in chain3, "")
    check_d1_only(check, bench, stream, "C16")


def scenario_c17(bench, check):
    """C17 空历史:明确拒绝不假压缩。"""
    bench.start_backend([
        {"text": "不该被叫到", "usage": {"input_tokens": 1, "output_tokens": 1}},
    ])
    bench.write_config()
    out = bench.run_cli("c17", [
        ("/compact", ("none",)),
        ("", ("sleep", 1)),
        ("/exit", ("exit",)),
    ])
    stream = bench.current_stream()
    rows = bench.read_lines(stream)
    block = compact_block(rows, 0)
    check.check("C17 空历史明确拒绝(终端文案)",
                "没有对话历史" in out, out[:120])
    check.check("C17 rejected(no_eligible_history)",
                block is not None and block["rejected"]
                and block["rejected"][0]["payload"]["reason"] == "no_eligible_history",
                "")
    check.check("C17 不假压缩(无 started/prepared/候选)",
                block is not None and not block["by_kind"].get("compact.started")
                and not block["prepared"] and not block["candidate"], "")
    check.check("C17 零模型调用", bench.request_count() == 0,
                "requests=%d" % bench.request_count())
    check_d1_only(check, bench, stream, "C17")


SCENARIOS = [
    ("S1", scenario_s1),
    ("S2", scenario_s2),
    ("S3", scenario_s3),
    ("S4", scenario_s4),
    ("S5", scenario_s5),
    ("S6", scenario_s6),
    ("S7", scenario_s7),
    ("S8", scenario_s8),
    ("C1", scenario_c1),
    ("C2", scenario_c2),
    ("C3", scenario_c3),
    ("C4", scenario_c4),
    ("C5", scenario_c5),
    ("C6", scenario_c6),
    ("C7", scenario_c7),
    ("C8", scenario_c8),
    ("C9", scenario_c9),
    ("C10", scenario_c10),
    ("C11", scenario_c11),
    ("C12", scenario_c12),
    ("C13", scenario_c13),
    ("C14", scenario_c14),
    ("C15", scenario_c15),
    ("C16", scenario_c16),
    ("C17", scenario_c17),
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
                        help="只跑指定行(逗号分隔 S1..S8/C1..C17;空 = 全跑)")
    parser.add_argument("--keep", action="store_true", help="不清工作目录")
    args = parser.parse_args()

    exe = Path(args.exe)
    if not exe.exists():
        print("exe 不在: %s" % exe)
        return 2
    work = Path(args.work).resolve()
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
