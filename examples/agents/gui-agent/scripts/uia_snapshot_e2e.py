# -*- coding: utf-8 -*-
"""GUI Agent 结构路 E2E:起夹具→list→snapshot→按 ref 的 rect 中心点提交。

与 manual_e2e.py(视觉路:截图→看→点)对照,这条验的是结构路:UIA 快照
给控件名+类型+矩形,模型不烧一枚截图 token 就能找到按钮。全程对账靠
夹具事件账(BTN-1 + SUBMIT)与状态文件。

会真动鼠标,默认 SKIP,须专用桌面或用户点头:
    python scripts/uia_snapshot_e2e.py --run          # 真点
    python scripts/uia_snapshot_e2e.py --run --dry-run  # 只验快照,不注入点击
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


def call(backend, tool: str, arguments: dict, settings: Settings, step: str) -> dict:
    print(f"--- {step}: {tool} {json.dumps(arguments, ensure_ascii=False)}")
    response = runner.build_response(
        {"protocol": 1, "call_id": f"uia-{step}", "plugin": "gui-agent-example",
         "tool": tool, "arguments": arguments, "context": {}}, backend, settings)
    if response["ok"]:
        print(f"    ok: {response['content'][0]['text'][:200]}")
    else:
        print(f"    error {response['error']['code']}: {response['error']['message'][:200]}")
    return response


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", action="store_true",
                        help="真跑(会动鼠标);不给就只打印 SKIP 说明")
    parser.add_argument("--dry-run", action="store_true",
                        help="只验快照链路,点击走 dry-run(零注入)")
    args = parser.parse_args()
    if not args.run:
        print("SKIP:本脚本会真实移动鼠标点击,须专用桌面或用户点头。")
        print("  真跑:   python scripts/uia_snapshot_e2e.py --run")
        print("  干跑:   python scripts/uia_snapshot_e2e.py --run --dry-run")
        print("  离线单测(零真输入)见 test_runner.py 的 SnapshotTest。")
        return 0

    switches = {}
    if args.dry_run:
        switches["LUBANCODE_GUI_DRY_RUN"] = "1"
    settings = Settings(env=switches)
    backend = Win32Backend()
    state_file = os.path.join(tempfile.mkdtemp(prefix="gui-uia-"), "state.json")
    events_file = os.path.join(tempfile.gettempdir(), "gui-uia-e2e-events.log")
    if os.path.exists(events_file):
        os.remove(events_file)

    fixture = subprocess.Popen(
        [sys.executable, os.path.join(PLUGIN_DIR, "fixtures", "fixture_app.py"),
         "--state-file", state_file, "--events-file", events_file,
         "--reset-on-start"])
    try:
        window_id = None
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline and window_id is None:
            for window in backend.list_windows():
                if window["title"] == FIXTURE_TITLE:
                    window_id = window["id"]
            if window_id is None:
                time.sleep(0.4)
        if window_id is None:
            print(f"E2E 失败:夹具窗口 {FIXTURE_TITLE!r} 15s 内没出现。")
            return 1
        time.sleep(1.2)  # 等夹具 after(300) 的无障碍装饰第二枪落定

        # 1. 结构快照——不发一枚截图。
        response = call(backend, "gui_snapshot", {"window_id": window_id}, settings, "snapshot")
        if not response["ok"]:
            return 1
        structured = response["structured"]
        elements = structured["elements"]

        def find(control_type: str, name: str) -> dict | None:
            for element in elements:
                if element["control_type"] == control_type and element["name"] == name:
                    return element
            return None

        # 2. 夹具的按钮/下拉/输入框该都能枚举到(tkinter 的 UIA 树,夹具
        #    自补的无障碍名字)。
        expected = [("button", "提交"), ("button", "重置"),
                    ("pane", "名字输入框"), ("pane", "颜色下拉")]
        missing = [f"{t}:{n}" for t, n in expected if find(t, n) is None]
        if missing:
            print(f"E2E 失败:快照里找不到 {missing};快照共 {len(elements)} 项:")
            for element in elements:
                print("   ", element["ref"], element["control_type"], element["name"])
            return 1
        print(f"快照核验通过:{len(elements)} 项,按钮/下拉/输入框全枚举到。")

        # 3. 按提交按钮 rect 中心点击——纯坐标,无视觉。
        submit = find("button", "提交")
        left, top, right, bottom = submit["rect"]
        click_x, click_y = (left + right) // 2, (top + bottom) // 2
        response = call(backend, "gui_click", {
            "x": click_x, "y": click_y,
            "expected_window_rect": structured["window_rect"]}, settings, "click-submit")
        if not response["ok"]:
            return 1

        if args.dry_run:
            print("DRY-RUN 走完:快照与点击校验链路绿,未注入一枚事件。")
            return 0

        # 4. 事件账对账:BTN-1 与 SUBMIT 都要到,state 文件 submitted=true。
        deadline = time.monotonic() + 5.0
        events = ""
        while time.monotonic() < deadline:
            if os.path.exists(events_file):
                with open(events_file, encoding="utf-8") as handle:
                    events = handle.read()
                if "SUBMIT" in events:
                    break
            time.sleep(0.2)
        with open(state_file, encoding="utf-8") as handle:
            state = json.load(handle)
        if "BTN-1" not in events or "SUBMIT" not in events:
            print(f"E2E 失败:事件账缺 BTN-1/SUBMIT。账:\n{events}")
            return 1
        if not state.get("submitted"):
            print(f"E2E 失败:状态文件无提交:{state}")
            return 1
        print(f"提交核验通过:事件账有 BTN-1+SUBMIT,状态 {state['name']!r}/{state['color']!r}。")
        print("--- 夹具事件账(逐笔对账):")
        print(events)
        print("结构路 E2E 全绿:零截图,快照定位,点击到账。")
        return 0
    finally:
        fixture.terminate()
        try:
            fixture.wait(timeout=5)
        except subprocess.TimeoutExpired:
            fixture.kill()


if __name__ == "__main__":
    raise SystemExit(main())
