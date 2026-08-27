# -*- coding: utf-8 -*-
"""LubanCode GUI Fixture——GUI Agent 教学夹具。

一只本地 tkinter 小窗:名字输入框、颜色下拉、提交按钮、结果行、事件账。
不联网、不需管理员、不读用户数据;一条命令起,布局钉死,窗口标题固定,
供"列窗口→聚焦→截图→点→输→提交→复验"教学链全程真跑。

参数:
  --state-file PATH   提交后把结果写进这文件,manual_e2e 靠它断言
  --events-file PATH  事件账同步落盘(每行一笔),脚本对账用
  --reset-on-start    启动即重置状态文件(submitted=false)

事件账(bind_all 收到的 Button/Key)显示在窗口下方,人工对账:每次
click/type/key 到没到,一眼看穿。
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import tkinter as tk
from tkinter import ttk

WINDOW_TITLE = "LubanCode GUI Fixture"
COLORS = ["green", "blue", "red"]


class FixtureApp:
    def __init__(self, state_file: str | None, reset_on_start: bool,
                 events_file: str | None = None) -> None:
        self.state_file = state_file
        self.events_file = events_file
        self.started_at = time.monotonic()

        # 与插件 runner 同一枚开关:per-monitor DPI 感知。不然 tk 的 place
        # 坐标是逻辑像素,插件注入的坐标是物理像素,高 DPI 屏上点得偏。
        # 两边同口径后,布局再按 DPI 放大,窗口不缩成邮票。
        scale = 1.0
        if sys.platform == "win32":
            import ctypes
            try:
                ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
                scale = ctypes.windll.user32.GetDpiForSystem() / 96.0
            except Exception:
                try:
                    ctypes.windll.user32.SetProcessDPIAware()
                except Exception:
                    pass
        self.scale = scale
        self._base_font = ("Segoe UI", max(9, int(9 * scale)))

        self.root = tk.Tk()
        self.root.title(WINDOW_TITLE)
        self.root.geometry(f"{int(480 * scale)}x{int(360 * scale)}")
        self.root.resizable(False, False)

        def place(widget, x, y):
            widget.place(x=int(x * scale), y=int(y * scale))

        # 布局用 place 钉死绝对坐标(逻辑 1x 设计,DPI 再整体放大):
        # 教学点击坐标可预测,事件账在下方。
        place(tk.Label(self.root, text="名字:", font=self._base_font), 16, 14)
        self.name_entry = tk.Entry(self.root, width=28, font=self._base_font)
        place(self.name_entry, 64, 10)

        place(tk.Label(self.root, text="颜色:", font=self._base_font), 16, 54)
        self.color_box = ttk.Combobox(self.root, values=COLORS, state="readonly", width=25)
        self.color_box.set("green")
        place(self.color_box, 64, 50)

        self.submit_button = tk.Button(self.root, text="提交", command=self.on_submit)
        place(self.submit_button, 64, 92)

        self.reset_button = tk.Button(self.root, text="重置", command=self.on_reset)
        place(self.reset_button, 132, 92)

        self.result_label = tk.Label(self.root, text="(未提交)", fg="#555", font=self._base_font)
        place(self.result_label, 16, 132)

        place(tk.Label(self.root, text="事件账(最近在前,人工对账用):",
                      font=self._base_font), 16, 160)
        self.event_log = tk.Text(self.root, width=58, height=8, state="disabled",
                                 font=("Consolas", max(8, int(8 * scale))))
        place(self.event_log, 16, 184)

        # 事件账:窗口里任何 Button/Key 都记一笔。SendInput 注入的输入
        # 走同一条路,真到没到瞒不了人。
        self.root.bind_all("<Button-1>", lambda e: self.log_event(
            f"BTN-1 @({e.x_root - self.root.winfo_rootx()},{e.y_root - self.root.winfo_rooty()}) "
            f"widget={type(e.widget).__name__}"))
        self.root.bind_all("<Key>", lambda e: self.log_event(
            f"KEY <{e.keysym}> chars={bool(e.char)} widget={type(e.widget).__name__}"))

        if state_file and reset_on_start:
            self.write_state({"submitted": False, "name": "", "color": ""})

    # -- 事件与状态 -----------------------------------------------------------
    def log_event(self, line: str) -> None:
        elapsed = int((time.monotonic() - self.started_at) * 1000)
        entry = f"[{elapsed:6d}ms] {line}"
        self.event_log.configure(state="normal")
        self.event_log.insert("1.0", entry + "\n")
        self.event_log.configure(state="disabled")
        if self.events_file:
            with open(self.events_file, "a", encoding="utf-8") as handle:
                handle.write(entry + "\n")

    def write_state(self, state: dict) -> None:
        if not self.state_file:
            return
        with open(self.state_file, "w", encoding="utf-8") as handle:
            json.dump(state, handle, ensure_ascii=False)

    def on_submit(self) -> None:
        name = self.name_entry.get().strip() or "(空)"
        color = self.color_box.get()
        self.result_label.configure(text=f"Hello, {name} / {color}", fg="#060")
        self.write_state({"submitted": True, "name": name, "color": color})
        self.log_event(f"SUBMIT name={name!r} color={color}")

    def on_reset(self) -> None:
        self.name_entry.delete(0, "end")
        self.color_box.set("green")
        self.result_label.configure(text="(未提交)", fg="#555")
        self.write_state({"submitted": False, "name": "", "color": ""})
        self.log_event("RESET")

    def run(self) -> None:
        self.root.mainloop()


def main() -> int:
    parser = argparse.ArgumentParser(description="LubanCode GUI 教学夹具")
    parser.add_argument("--state-file", default="")
    parser.add_argument("--events-file", default="")
    parser.add_argument("--reset-on-start", action="store_true")
    args = parser.parse_args()
    FixtureApp(args.state_file or None, args.reset_on_start,
               args.events_file or None).run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
