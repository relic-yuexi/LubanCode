#!/usr/bin/env python3
# P0-6 workflow 场真机探针:真 lubancode.exe 交互 TUI + 真控制台键入。
#
# 用法:
#   python tests/manual/p06_workflow_console_probe.py <lubancode.exe> <证据输出目录>
#
# 为什么不用管道:交互输入走 ReadConsoleW/ReadConsoleInputW,管道喂不进
# (单子"真 TUI 交互留白"说的是 ESC 这类半路按键;本场只在空闲提示符敲
# 一行命令,用 Windows 控制台输入 API(WriteConsoleInputW)驱动,是真
# TUI 真进程真键入,不是管道伪造)。
#
# 场景:
#   workflow_run   工作目录里自造最小 workflow(.lubancode/workflows/
#                 p06-min-wf,纯 transform 节点,零 LLM),真 TUI 里键入
#                 "/workflow run p06-min-wf 验收差事"跑完,再键入 "/exit"
#                 干净封口。证据:编排账(workflow-runs/<run>/{definition,
#                 manifest,events}.json*)、session 轨迹 verify/replay、
#                 main.jsonl 的 control.command 生命周期。
#
# 证据落 <输出目录>/workflow_run/。证据目录不进 git;路径记单子。
import ctypes
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes
from pathlib import Path

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
CREATE_NEW_CONSOLE = 0x00000010
KEY_EVENT = 0x0001
VK_RETURN = 0x0D


class KEY_EVENT_RECORD(ctypes.Structure):
    _fields_ = [("bKeyDown", wintypes.BOOL),
                ("wRepeatCount", wintypes.WORD),
                ("wVirtualKeyCode", wintypes.WORD),
                ("wVirtualScanCode", wintypes.WORD),
                ("uChar", wintypes.WCHAR),
                ("dwControlKeyState", wintypes.DWORD)]


class INPUT_RECORD(ctypes.Structure):
    _fields_ = [("EventType", wintypes.WORD),
                ("Event", KEY_EVENT_RECORD)]


def type_line(pid, text, char_delay=0.06, pre_enter_delay=0.2):
    """把一行文本逐键敲进 pid 的控制台输入队列,带回车。逐键带 60ms 间隔:
    一整把灌进去会被 TUI 的"快打连击=粘贴"启发式当成一次多行粘贴,回车
    被吞成换行(实测:整把灌入时两行并作一条提交,参数里夹 \\n)。"""
    kernel32.FreeConsole()
    if not kernel32.AttachConsole(pid):
        raise OSError("AttachConsole(%d) 失败: %d" % (pid, ctypes.get_last_error()))
    try:
        handle = kernel32.CreateFileW("CONIN$", GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                      OPEN_EXISTING, 0, None)
        if handle in (0, -1) or handle == 0xFFFFFFFFFFFFFFFF:
            raise OSError("CONIN$ 打不开: %d" % ctypes.get_last_error())

        def send(ch, vk, delay):
            record = INPUT_RECORD()
            record.EventType = KEY_EVENT
            record.Event.bKeyDown = True
            record.Event.wRepeatCount = 1
            record.Event.wVirtualKeyCode = vk
            record.Event.uChar = ch
            written = wintypes.DWORD(0)
            if not kernel32.WriteConsoleInputW(handle, ctypes.byref(record), 1,
                                               ctypes.byref(written)):
                raise OSError("WriteConsoleInputW 失败: %d" % ctypes.get_last_error())
            time.sleep(delay)

        try:
            for ch in text:
                send(ch, user32.VkKeyScanW(ord(ch)) & 0xFF, char_delay)
            time.sleep(pre_enter_delay)
            send("\r", VK_RETURN, 0.1)
        finally:
            kernel32.CloseHandle(handle)
    finally:
        kernel32.FreeConsole()
        kernel32.AttachConsole(-1)  # ATTACH_PARENT_PROCESS:回自己原先的控制台


def start_in_new_console(exe, work, env):
    # CREATE_NEW_PROCESS_GROUP 与 CREATE_NEW_CONSOLE 是非法组合(后者会被
    # 吃掉,AttachConsole 随即 5 拒);也不许重定向 std 句柄——TUI 的
    # RawInputScope/ReadConsoleW 认的就是新控制台的本征句柄,接 NUL 会让
    # 会话当 EOF 直接退。
    creationflags = CREATE_NEW_CONSOLE
    proc = subprocess.Popen([str(exe)], cwd=str(work), env=env,
                            creationflags=creationflags)
    return proc


MINIMAL_WORKFLOW = """schema_version: 1
id: p06-min-wf
version: 1.0.0
name: P06 最小编排
description: P0-6 真机验收最小 workflow:纯 transform 节点,零 LLM。
alias: p06-min-wf
enabled: true
entry: merge
inputs:
  type: object
  required: [task]
  properties:
    task: { type: string }
nodes:
  merge:
    type: transform
    operation: json_merge
    input:
      task: "${inputs.task}"
      gate: "p06"
  fin:
    type: end
edges:
  - { from: merge, on: success, to: fin }
result:
  out: "${nodes.merge.output}"
"""


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


def wait_for(predicate, timeout_s, poll_s=0.1):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(poll_s)
    return None


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    exe = Path(sys.argv[1]).resolve()
    out_root = Path(sys.argv[2]).resolve()
    if not exe.exists():
        print("找不到 lubancode.exe:", exe)
        return 2
    out_dir = out_root / "workflow_run"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    home = Path(tempfile.mkdtemp(prefix="lb-p06wf-home-"))
    work = Path(tempfile.mkdtemp(prefix="lb-p06wf-work-"))
    # 配置给个回环假 provider(本场零 LLM,配置只求"已配置"过欢迎页)。
    config = {"active_provider": "probe",
              "providers": [{"name": "probe", "base_url": "http://127.0.0.1:9",
                             "wire": "responses", "api_key": "probe-key",
                             "model": "probe-model"}]}
    (work / ".lubancode").mkdir()
    (work / ".lubancode" / "config.json").write_text(json.dumps(config), encoding="utf-8")
    wf_dir = work / ".lubancode" / "workflows" / "p06-min-wf"
    wf_dir.mkdir(parents=True)
    (wf_dir / "workflow.yaml").write_text(MINIMAL_WORKFLOW, encoding="utf-8")
    (work / "probe.txt").write_text("工作目录占位\n", encoding="utf-8")

    env = dict(os.environ)
    env["USERPROFILE"] = str(home)
    for key in ("http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                "ALL_PROXY"):
        env.pop(key, None)
    env["NO_PROXY"] = "*"
    env["no_proxy"] = "*"

    (out_dir / "command.txt").write_text(
        "lubancode(交互 TUI,新控制台,WriteConsoleInputW 逐键 60ms)\n"
        "键入: /workflow run p06-min-wf p06-gate-task\n"
        "键入: /exit\n工作目录: %s\nUSERPROFILE: %s\n" % (work, home), encoding="utf-8")

    # 交互 TUI 进新控制台跑;探针自己的控制台在 type_line 里 FreeConsole
    # 后用 ATTACH_PARENT_PROCESS(-1) 接回——探针的现场输出全走文件,控制台
    # 只服务键入。
    proc = start_in_new_console(exe, work, env)
    (out_dir / "pid.txt").write_text(str(proc.pid) + "\n", encoding="utf-8")

    # 等 session 开张(main.jsonl 出现)再敲命令——别把键敲进欢迎页。
    session = wait_for(lambda: newest_session(home), 60)
    if session is None:
        proc.kill()
        (out_dir / "result.txt").write_text("60 秒没等到 session 开张", encoding="utf-8")
        return 1

    runs_root = home / ".lubancode" / "workflow-runs"
    typed = []
    try:
        time.sleep(1.0)  # 提示符就位
        type_line(proc.pid, "/workflow run p06-min-wf p06-gate-task")
        typed.append("/workflow run p06-min-wf p06-gate-task")
        # 等编排账终态(manifest.json 带 final_state)。
        def run_finished():
            if not runs_root.exists():
                return None
            for run_dir in runs_root.iterdir():
                manifest = run_dir / "manifest.json"
                if manifest.exists():
                    try:
                        data = json.loads(manifest.read_text(encoding="utf-8"))
                    except (ValueError, OSError):
                        continue
                    if data.get("final_state"):
                        return run_dir
            return None
        run_dir = wait_for(run_finished, 120)
        (out_dir / "workflow_run_dir.txt").write_text(
            (str(run_dir) + "\n") if run_dir else "120 秒没等到 workflow 终态\n",
            encoding="utf-8")
        time.sleep(1.0)
        type_line(proc.pid, "/exit")
        typed.append("/exit")
        proc.wait(timeout=60)
    except Exception as exc:  # noqa: BLE001 - 探针要把失败现场留全
        (out_dir / "driver_error.txt").write_text(repr(exc) + "\n", encoding="utf-8")
        proc.kill()
        proc.wait(timeout=30)
        return 1
    finally:
        # 探针自己的控制台在 type_line 里 FreeConsole 后没接回来:输出都走
        # 文件,stdout 打印放最后没关系;attach 回去以防万一。
        kernel32.FreeConsole()
        kernel32.AttachConsole(-1)  # ATTACH_PARENT_PROCESS

    (out_dir / "exit_code.txt").write_text(str(proc.returncode) + "\n", encoding="utf-8")
    (out_dir / "typed.txt").write_text("\n".join(typed) + "\n", encoding="utf-8")

    session = newest_session(home)
    if session is None:
        (out_dir / "result.txt").write_text("没有 session", encoding="utf-8")
        return 1
    (out_dir / "session_id.txt").write_text(session.name + "\n", encoding="utf-8")
    lines = []
    for root, _dirs, files in os.walk(session):
        for f in sorted(files):
            p = Path(root) / f
            lines.append("%8d  %s" % (p.stat().st_size, p.relative_to(session)))
    (out_dir / "session_files.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    # 编排账清单 + 事件尾。
    if runs_root.exists():
        wf_lines = []
        for run_dir in sorted(runs_root.iterdir()):
            for root, _dirs, files in os.walk(run_dir):
                for f in sorted(files):
                    p = Path(root) / f
                    wf_lines.append("%8d  %s" % (p.stat().st_size,
                                                 p.relative_to(run_dir)))
        (out_dir / "workflow_files.txt").write_text("\n".join(wf_lines) + "\n",
                                                    encoding="utf-8")
        for run_dir in sorted(runs_root.iterdir()):
            events = run_dir / "events.jsonl"
            if events.exists():
                text = events.read_text(encoding="utf-8").splitlines()
                (out_dir / ("events_tail_%s.txt" % run_dir.name)).write_text(
                    "\n".join(text[-6:]) + "\n", encoding="utf-8")

    # main.jsonl 里的命令生命周期与 workflow 边界事件。
    main_text = (session / "main.jsonl").read_text(encoding="utf-8")
    cmd_events = [json.loads(line) for line in main_text.splitlines()
                  if '"control.command' in line or '"workflow' in line]
    (out_dir / "command_events.txt").write_text(
        "\n".join("%s %s" % (e.get("kind"), json.dumps(e.get("payload", {}),
                                                       ensure_ascii=False)[:200])
                  for e in cmd_events) + "\n", encoding="utf-8")

    for label, subcmd in (("verify.txt", "verify"), ("replay.txt", "replay")):
        probe = subprocess.run([str(exe), "trajectory", subcmd, session.name],
                               cwd=str(work), env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                               errors="replace", timeout=120)
        (out_dir / label).write_text("[exit %d]\n%s" % (probe.returncode,
                                                        probe.stdout or ""),
                                     encoding="utf-8")

    # workflow 会话归属统一单:session 内的编排账(workflows/<run>/
    # workflow.jsonl + nodes/*.jsonl)必须在场、收口齐全——这是本探针的
    # 硬门槛,不只取证。
    verdict = []
    ok = True
    verify_text = (out_dir / "verify.txt").read_text(encoding="utf-8")
    if "[exit 0]" not in verify_text:
        ok = False
        verdict.append("trajectory verify 未过")
    workflows_root = session / "workflows"
    wf_runs = sorted(p for p in workflows_root.iterdir()) if workflows_root.exists() else []
    if not wf_runs:
        ok = False
        verdict.append("session 里没有 workflows/<run>/ 编排账")
    for run_dir in wf_runs:
        orchestration = run_dir / "workflow.jsonl"
        if not orchestration.exists():
            ok = False
            verdict.append("%s 缺 workflow.jsonl" % run_dir.name)
            continue
        kinds = []
        for line in orchestration.read_text(encoding="utf-8").splitlines():
            try:
                kinds.append(json.loads(line).get("kind", ""))
            except ValueError:
                kinds.append("<bad>")
        (out_dir / ("orchestration_%s.txt" % run_dir.name)).write_text(
            "\n".join(kinds) + "\n", encoding="utf-8")
        if not kinds or kinds[0] != "run.started":
            ok = False
            verdict.append("%s 编排账首行不是 run.started" % run_dir.name)
        if not kinds or kinds[-1] not in ("run.completed", "run.failed", "run.cancelled"):
            ok = False
            verdict.append("%s 编排账未收口" % run_dir.name)
        if "workflow.node.dispatched" not in kinds:
            ok = False
            verdict.append("%s 编排账没有 node 派发事实" % run_dir.name)
        nodes_dir = run_dir / "nodes"
        node_files = sorted(nodes_dir.glob("*.jsonl")) if nodes_dir.exists() else []
        if not node_files:
            ok = False
            verdict.append("%s 没有 node attempt 账" % run_dir.name)
        for node_file in node_files:
            node_kinds = []
            for line in node_file.read_text(encoding="utf-8").splitlines():
                try:
                    node_kinds.append(json.loads(line).get("kind", ""))
                except ValueError:
                    node_kinds.append("<bad>")
            if not node_kinds or node_kinds[0] != "run.started" or \
                    node_kinds[-1] not in ("run.completed", "run.failed", "run.cancelled"):
                ok = False
                verdict.append("%s node 账 %s 未收口" % (run_dir.name, node_file.name))
    (out_dir / "result.txt").write_text(
        ("通过\n" if ok else "未过\n") + "\n".join(verdict) + "\n", encoding="utf-8")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
