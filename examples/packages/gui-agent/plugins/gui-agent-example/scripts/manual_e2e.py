# -*- coding: utf-8 -*-
"""GUI Agent 真桌面 E2E:起夹具→列窗口→聚焦→截图→填名→选色→提交→复验。

为什么默认 SKIP:它会真的移动鼠标、点击、敲键盘,还会把窗口切到前台——
在有人用着的开发机上跑等于抢鼠标。这是工单的停手线:普通开发机默认
skip,真 E2E 须专用桌面或得到用户点头。

  跑法(同机自跑,先确认接下来几秒鼠标不归你):
    python scripts/manual_e2e.py --run
  只走 dry-run(校验全链路,零输入注入):
    python scripts/manual_e2e.py --run --dry-run

证据(截图、state)落 build/test-evidence/agent-examples/,不进仓。
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN_DIR = os.path.dirname(HERE)
sys.path.insert(0, PLUGIN_DIR)

import runner  # noqa: E402
from gui_actions import Settings  # noqa: E402
from gui_backend import Win32Backend  # noqa: E402

FIXTURE_TITLE = "LubanCode GUI Fixture"


def evidence_root() -> str:
    root = os.path.join(os.getcwd(), "build", "test-evidence", "agent-examples")
    os.makedirs(root, exist_ok=True)
    return root


def call(backend, tool: str, arguments: dict, settings: Settings, step: str) -> dict:
    print(f"--- {step}: {tool} {json.dumps(arguments, ensure_ascii=False)}")
    response = runner.build_response(
        {"protocol": 1, "call_id": f"e2e-{step}", "plugin": "gui-agent-example",
         "tool": tool, "arguments": arguments, "context": {}}, backend, settings)
    if response["ok"]:
        print(f"    ok: {response['content'][0]['text'][:160]}")
    else:
        print(f"    error {response['error']['code']}: {response['error']['message'][:160]}")
    return response


def must(response: dict, step: str) -> dict:
    if not response["ok"]:
        print(f"E2E 失败在 {step}:{response['error']['code']} {response['error']['message']}")
        raise SystemExit(1)
    return response["structured"]


def wait_for_fixture(backend, timeout_s: float = 15.0) -> dict:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for window in backend.list_windows():
            if window["title"] == FIXTURE_TITLE:
                return window
        time.sleep(0.4)
    raise SystemExit(f"夹具窗口 {FIXTURE_TITLE!r} {timeout_s}s 内没出现")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true",
                        help="真跑(会动鼠标键盘);不给就只打印 SKIP 说明")
    parser.add_argument("--dry-run", action="store_true",
                        help="动作全走 dry-run:校验链路,不注入一枚事件")
    args = parser.parse_args()
    if not args.run:
        print("SKIP:本脚本会真实移动鼠标、点击与输入,须专用桌面或用户点头。")
        print("  真跑:   python scripts/manual_e2e.py --run")
        print("  干跑:   python scripts/manual_e2e.py --run --dry-run")
        print("  CI/日常 ctest 不跑本脚本;离线单测见 test_runner.py(零真输入)。")
        return 0

    switches = {"LUBANCODE_GUI_EVIDENCE_DIR": evidence_root()}
    if args.dry_run:
        switches["LUBANCODE_GUI_DRY_RUN"] = "1"
    settings = Settings(env=switches)
    backend = Win32Backend()
    evidence = evidence_root()
    state_file = os.path.join(tempfile.mkdtemp(prefix="gui-e2e-"), "state.json")
    events_file = os.path.join(evidence, "fixture-events.log")
    if os.path.exists(events_file):
        os.remove(events_file)

    fixture = subprocess.Popen(
        [sys.executable, os.path.join(PLUGIN_DIR, "fixtures", "fixture_app.py"),
         "--state-file", state_file, "--events-file", events_file,
         "--reset-on-start"])
    try:
        # 1-2. 列窗口,找到夹具。
        window = wait_for_fixture(backend)
        window_id = window["id"]
        print(f"找到夹具窗口 {window_id} rect={window['rect']}")

        # 3. 聚焦后截图,拿 observation(含 dpi_scale——夹具按它放大布局,
        #    点击坐标同乘,两边同口径)。
        must(call(backend, "gui_focus_window", {"window_id": window_id},
                  settings, "focus"), "focus")
        observation = must(call(backend, "gui_screenshot",
                                {"target": "window", "window_id": window_id},
                                settings, "shot-1"), "shot-1")
        rect = observation["window_rect"]
        scale = observation.get("dpi_scale") or 1.0

        common = {"window_id": window_id, "coordinate_space": "window_client",
                  "expected_window_rect": rect}

        # 4. 点名字框,输入中文。坐标按夹具的 1x 设计值乘 DPI。
        must(call(backend, "gui_click",
                  {**common, "x": int(104 * scale), "y": int(20 * scale)},
                  settings, "click-name"), "click-name")
        must(call(backend, "gui_type_text", {"text": "阿明", "window_id": window_id,
                                             "interval_ms": 20}, settings, "type-name"),
             "type-name")

        # 5. 选颜色:点开下拉,Down→Up→Return——键盘选值,回车确认收起。
        #    (教训:点开下拉后不收起,后续点击会落在浮层上——GUI 自动化
        #    的常见坑,事件账里 widget=str 就是它。)
        must(call(backend, "gui_click",
                  {**common, "x": int(100 * scale), "y": int(60 * scale)},
                  settings, "click-color"), "click-color")
        must(call(backend, "gui_key", {"keys": ["down"], "window_id": window_id},
                  settings, "color-down"), "color-down")
        must(call(backend, "gui_key", {"keys": ["up"], "window_id": window_id},
                  settings, "color-up"), "color-up")
        must(call(backend, "gui_key", {"keys": ["return"], "window_id": window_id},
                  settings, "color-confirm"), "color-confirm")

        # 6. 提交前再截一张,确认现场没漂。
        must(call(backend, "gui_screenshot", {"target": "window", "window_id": window_id},
                  settings, "shot-2"), "shot-2")
        must(call(backend, "gui_click",
                  {**common, "x": int(89 * scale), "y": int(104 * scale)},
                  settings, "click-submit"), "click-submit")

        # 7. 复验截图 + 断言状态文件。
        must(call(backend, "gui_screenshot", {"target": "window", "window_id": window_id},
                  settings, "shot-3"), "shot-3")

        # 8. 挪窗口,拿旧 expected_rect 再点——必须吃 stale_observation。
        import ctypes
        hwnd = int(window_id, 16)
        ctypes.windll.user32.MoveWindow(hwnd, rect[0] + 120, rect[1] + 80,
                                        rect[2] - rect[0], rect[3] - rect[1], True)
        stale = call(backend, "gui_click",
                     {**common, "x": int(89 * scale), "y": int(104 * scale)},
                     settings, "stale-click")
        if stale.get("ok") or stale.get("error", {}).get("code") != "stale_observation":
            print("E2E 失败:窗口挪了,旧坐标点击未被 stale_observation 拦住。")
            return 1
        print("stale 拦截验证通过:旧 observation 的点击被拒。")

        if args.dry_run:
            print("DRY-RUN 走完:全链校验绿,未注入一枚输入事件,state 不应有提交。")
            with open(state_file, encoding="utf-8") as handle:
                final_state = json.load(handle)
            if final_state.get("submitted"):
                print("E2E 失败:dry-run 不该产生提交。")
                return 1
        else:
            for _ in range(20):
                time.sleep(0.25)
                with open(state_file, encoding="utf-8") as handle:
                    final_state = json.load(handle)
                if final_state.get("submitted"):
                    break
            if final_state.get("name") != "阿明" or final_state.get("color") != "green":
                print(f"E2E 失败:状态 {final_state},期望 name=阿明 color=green submitted=true")
                return 1
            print(f"提交核验通过:{final_state['name']} / {final_state['color']}")
        print(f"E2E 全绿。证据目录:{evidence_root()}")
        return 0
    finally:
        if os.path.exists(events_file):
            print("--- 夹具事件账(逐笔对账):")
            with open(events_file, encoding="utf-8") as handle:
                print(handle.read())
        fixture.terminate()
        try:
            fixture.wait(timeout=5)
        except subprocess.TimeoutExpired:
            fixture.kill()


if __name__ == "__main__":
    raise SystemExit(main())
