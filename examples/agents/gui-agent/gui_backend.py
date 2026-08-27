# -*- coding: utf-8 -*-
"""GUI 后端:Win32 ctypes 一层 + 测试用假后端。

只认标准库。为什么不用 pyautogui/mss:
  - pyautogui 在 DPI unaware 进程里拿的是虚拟化坐标,多屏负原点会错位;
    自己先 SetProcessDpiAwarenessContext,全链路物理像素,口径唯一。
  - mss/Pillow 各带一串依赖;截图 BitBlt + 手写 PNG(png.py)一共百余行,
    权限、坐标合同、字节帽全在本仓,不交第三方猜。

接口按"动作"切,不按 Win32 API 切:上层 gui_actions 只说"移到哪、点几
下、敲什么",Windows 花活(归一化坐标、scan code、DIB 行序)全埋在这层。
FakeBackend 同一份接口,test_runner.py 注入它,单测不碰真鼠标。
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import sys
import time
from typing import Callable, Optional

# ---------------------------------------------------------------------------
# SendInput 结构(x64 下 INPUT 须正好 40 字节;ULONG_PTR 用 c_size_t 对齐)。
# ---------------------------------------------------------------------------
ULONG_PTR = ctypes.c_size_t
LONG = ctypes.c_long
DWORD = ctypes.c_ulong

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000

KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1

PW_RENDERFULLCONTENT = 0x00000002  # PrintWindow 抓 GPU 合成内容(Chrome 一类)
SRCCOPY = 0x00CC0020
CAPTUREBLT = 0x40000000  # 层叠窗口也拍
BI_RGB = 0
DIB_RGB_COLORS = 0
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# 高风险键:能关窗口、能开系统执行的组合,默认禁,须环境变量显式开禁
# (合同层判,这里只列 VK 码)。
_VK_MODIFIERS = {"ctrl": 0x11, "shift": 0x10, "alt": 0x12, "win": 0x5B}
_VK_SINGLES = {
    "enter": 0x0D, "return": 0x0D, "tab": 0x09, "escape": 0x1B, "esc": 0x1B,
    "space": 0x20, "backspace": 0x08, "delete": 0x2E, "home": 0x24, "end": 0x23,
    "pageup": 0x21, "pagedown": 0x22, "up": 0x26, "down": 0x28,
    "left": 0x25, "right": 0x27, "printscreen": 0x2C,
}
for _i in range(10):
    _VK_SINGLES[str(_i)] = 0x30 + _i
for _c in "abcdefghijklmnopqrstuvwxyz":
    _VK_SINGLES[_c] = ord(_c) - 32
for _n in range(1, 13):
    _VK_SINGLES[f"f{_n}"] = 0x6F + _n


def key_name_to_vk(name: str) -> Optional[int]:
    """键名转 VK 码;不认得的名字回 None(合同层拒绝,不猜相近键)。"""
    lowered = name.lower()
    if lowered in _VK_MODIFIERS:
        return _VK_MODIFIERS[lowered]
    return _VK_SINGLES.get(lowered)


def is_modifier(name: str) -> bool:
    return name.lower() in _VK_MODIFIERS


class _MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", LONG), ("dy", LONG), ("mouseData", DWORD), ("dwFlags", DWORD),
                ("time", DWORD), ("dwExtraInfo", ULONG_PTR)]


class _KEYBDINPUT(ctypes.Structure):
    _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", DWORD),
                ("time", DWORD), ("dwExtraInfo", ULONG_PTR)]


class _HARDWAREINPUT(ctypes.Structure):
    _fields_ = [("uMsg", DWORD), ("wParamL", wt.WORD), ("wParamH", wt.WORD)]


class _InputUnion(ctypes.Union):
    _fields_ = [("mi", _MOUSEINPUT), ("ki", _KEYBDINPUT), ("hi", _HARDWAREINPUT)]


class _INPUT(ctypes.Structure):
    _anonymous_ = ("union",)
    _fields_ = [("type", DWORD), ("union", _InputUnion)]


assert ctypes.sizeof(_INPUT) == 40, f"INPUT 布局错:x64 须 40 字节,实得 {ctypes.sizeof(_INPUT)}"


def _input_mouse(flags: int, dx: int = 0, dy: int = 0, data: int = 0) -> _INPUT:
    item = _INPUT()
    item.type = INPUT_MOUSE
    item.mi = _MOUSEINPUT(dx, dy, data & 0xFFFFFFFF, flags, 0, 0)
    return item


def _input_key(vk: int = 0, scan: int = 0, flags: int = 0) -> _INPUT:
    item = _INPUT()
    item.type = INPUT_KEYBOARD
    item.ki = _KEYBDINPUT(vk, scan, flags, 0, 0)
    return item


class _BITMAPINFOHEADER(ctypes.Structure):
    """ctypes.wintypes 没带这枚,自己摆:biHeight 负值 = top-down 行序。"""
    _fields_ = [("biSize", DWORD), ("biWidth", LONG), ("biHeight", LONG),
                ("biPlanes", wt.WORD), ("biBitCount", wt.WORD),
                ("biCompression", DWORD), ("biSizeImage", DWORD),
                ("biXPelsPerMeter", LONG), ("biYPelsPerMeter", LONG),
                ("biClrUsed", DWORD), ("biClrImportant", DWORD)]


class Win32Backend:
    """真桌面后端。Windows 10/11 首版承诺;别的平台构造即抛,不冒充可用。"""

    def __init__(self) -> None:
        if sys.platform != "win32":
            raise RuntimeError(f"unsupported_platform: 首版只承诺 Windows 10/11,当前 {sys.platform}")
        self.user32 = ctypes.windll.user32
        self.kernel32 = ctypes.windll.kernel32
        self.gdi32 = ctypes.windll.gdi32
        self.dpi_awareness = self._make_dpi_aware()

    # -- DPI ---------------------------------------------------------------
    def _make_dpi_aware(self) -> str:
        """先 Per-Monitor v2,再系统级,再裸奔并如实报告。

        不打这枚开关,user32 给的窗口矩形是虚拟化逻辑像素,SendInput 坐标
        与截图像素对不上——DPI 排错第一条就查它(gui_status 会报)。
        """
        try:
            context = ctypes.c_void_p(-4)  # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            if self.user32.SetProcessDpiAwarenessContext(context):
                return "per_monitor_v2"
        except AttributeError:
            pass
        try:
            if self.user32.SetProcessDPIAware():
                return "system"
        except AttributeError:
            pass
        return "unaware"

    # -- 窗口 --------------------------------------------------------------
    def _window_title(self, hwnd: int) -> str:
        length = self.user32.GetWindowTextLengthW(hwnd)
        if length <= 0:
            return ""
        buffer = ctypes.create_unicode_buffer(length + 1)
        self.user32.GetWindowTextW(hwnd, buffer, length + 1)
        return buffer.value

    def _process_name(self, hwnd: int) -> str:
        pid = DWORD()
        self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        handle = self.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid.value)
        if not handle:
            return ""
        try:
            size = DWORD(1024)
            buffer = ctypes.create_unicode_buffer(size.value)
            if self.kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
                # 只留文件名,进程全路径含用户名,不外送。
                return buffer.value.rsplit("\\", 1)[-1].rsplit("/", 1)[-1]
            return ""
        finally:
            self.kernel32.CloseHandle(handle)

    def list_windows(self) -> list[dict]:
        """枚举可见顶层窗口。只报几何与进程名,不读窗口正文。"""
        results: list[dict] = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def on_window(hwnd, _lparam):
            if not self.user32.IsWindowVisible(hwnd):
                return True
            title = self._window_title(hwnd)
            if not title:
                return True  # 无标题的辅助窗口,对模型是噪音
            rect = wt.RECT()
            self.user32.GetWindowRect(hwnd, ctypes.byref(rect))
            results.append({
                "id": f"0x{hwnd & 0xFFFFFFFF:08X}",
                "title": title[:200],
                "process_name": self._process_name(hwnd),
                "rect": [rect.left, rect.top, rect.right, rect.bottom],
                "visible": True,
                "minimized": bool(self.user32.IsIconic(hwnd)),
                "foreground": hwnd == self.user32.GetForegroundWindow(),
            })
            return True

        self.user32.EnumWindows(on_window, 0)
        return results

    def window_exists(self, window_id: str) -> bool:
        hwnd = int(window_id, 16)
        return bool(self.user32.IsWindow(hwnd))

    def window_state(self, window_id: str) -> Optional[dict]:
        """重查一枚窗口的现场:矩形 + 客户区 + 前台 + DPI + 最小化。"""
        hwnd = int(window_id, 16)
        if not self.user32.IsWindow(hwnd):
            return None
        rect = wt.RECT()
        self.user32.GetWindowRect(hwnd, ctypes.byref(rect))
        client = wt.RECT()
        self.user32.GetClientRect(hwnd, ctypes.byref(client))
        point = wt.POINT(0, 0)
        self.user32.ClientToScreen(hwnd, ctypes.byref(point))
        return {
            "id": f"0x{hwnd & 0xFFFFFFFF:08X}",
            "title": self._window_title(hwnd)[:200],
            "rect": [rect.left, rect.top, rect.right, rect.bottom],
            "client_rect_local": [client.left, client.top, client.right, client.bottom],
            "client_origin": [point.x, point.y],
            "dpi_scale": self.dpi_scale(window_id),
            "minimized": bool(self.user32.IsIconic(hwnd)),
            "foreground": hwnd == self.user32.GetForegroundWindow(),
        }

    def dpi_scale(self, window_id: str) -> float:
        hwnd = int(window_id, 16)
        try:
            return round(self.user32.GetDpiForWindow(hwnd) / 96.0, 3)
        except AttributeError:
            return 1.0  # Win10 1607 前无此 API:只报 1.0,不猜

    def virtual_screen(self) -> list[int]:
        metrics = self.user32.GetSystemMetrics
        left = metrics(76)      # SM_XVIRTUALSCREEN
        top = metrics(77)       # SM_YVIRTUALSCREEN
        width = metrics(78)     # SM_CXVIRTUALSCREEN
        height = metrics(79)    # SM_CYVIRTUALSCREEN
        return [left, top, left + width, top + height]

    def monitor_count(self) -> int:
        return self.user32.GetSystemMetrics(80)  # SM_CMONITORS

    def focus_window(self, window_id: str) -> bool:
        """切前台 + 复查。系统拒绝时如实回 False。

        Windows 前台锁定:只有拿到过输入的进程才许 SetForegroundWindow。
        脚本进程多半没这资格,先敲一枚空 Alt 键"领输入权"(pywinauto 同款
        前摇),再切。仍被拒就如实报,不冒充成功。
        """
        hwnd = int(window_id, 16)
        if self.user32.IsIconic(hwnd):
            self.user32.ShowWindow(hwnd, 9)  # SW_RESTORE
            time.sleep(0.05)
        self.user32.keybd_event(0x12, 0, 0, 0)   # VK_MENU down(空 Alt,不产生字符)
        self.user32.keybd_event(0x12, 0, 2, 0)   # KEYEVENTF_KEYUP
        for _ in range(2):
            self.user32.SetForegroundWindow(hwnd)
            time.sleep(0.05)
            if self.user32.GetForegroundWindow() == hwnd:
                return True
        return False

    # -- 输入注入 -----------------------------------------------------------
    def _send(self, *items: _INPUT) -> None:
        count = len(items)
        array = (_INPUT * count)(*items)
        sent = self.user32.SendInput(count, array, ctypes.sizeof(_INPUT))
        if sent != count:
            raise OSError(f"SendInput 只注入了 {sent}/{count} 项")

    def _to_absolute(self, x: int, y: int) -> tuple[int, int]:
        """物理像素 → MOUSEEVENTF_VIRTUALDESK 的 0..65535 归一坐标。"""
        left, top, right, bottom = self.virtual_screen()
        width = max(right - left, 1)
        height = max(bottom - top, 1)
        abs_x = round((x - left) * 65535 / (width - 1)) if width > 1 else 0
        abs_y = round((y - top) * 65535 / (height - 1)) if height > 1 else 0
        return max(0, min(65535, abs_x)), max(0, min(65535, abs_y))

    def mouse_move(self, x: int, y: int) -> None:
        abs_x, abs_y = self._to_absolute(x, y)
        self._send(_input_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
                                abs_x, abs_y))

    def mouse_click(self, button: str, clicks: int, interval_ms: int = 40) -> None:
        down, up = {
            "left": (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP),
            "right": (MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP),
            "middle": (MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP),
        }[button]
        for index in range(clicks):
            self._send(_input_mouse(down))
            self._send(_input_mouse(up))
            if index + 1 < clicks:
                time.sleep(interval_ms / 1000.0)

    def mouse_scroll(self, ticks: int) -> None:
        """正数向上,负数向下;一格 120(Windows 轮定义)。"""
        self._send(_input_mouse(MOUSEEVENTF_WHEEL, data=ticks * 120))

    def key_combo(self, vk_names: list[str]) -> None:
        """依次按下(修饰键在前),反序抬起。调用方管危险组合的闸。"""
        vks = []
        for name in vk_names:
            vk = key_name_to_vk(name)
            if vk is None:
                raise ValueError(f"unknown_key: {name}")
            vks.append(vk)
        for vk in vks:
            self._send(_input_key(vk=vk))
        for vk in reversed(vks):
            self._send(_input_key(vk=vk, flags=KEYEVENTF_KEYUP))

    def type_unicode(self, text: str, interval_ms: int) -> None:
        """逐字符 KEYEVENTF_UNICODE。中文不走 IME——直接发 Unicode 事件,
        目标程序按键盘输入收到,不碰剪贴板,不依赖输入法状态。"""
        delay = interval_ms / 1000.0
        for char in text:
            code = ord(char)
            if code > 0xFFFF:
                raise ValueError(f"unsupported_char: {char!r} 超出 BMP,首版不注代理对")
            self._send(_input_key(scan=code, flags=KEYEVENTF_UNICODE))
            self._send(_input_key(scan=code, flags=KEYEVENTF_UNICODE | KEYEVENTF_KEYUP))
            if delay > 0:
                time.sleep(delay)

    # -- 截图 ---------------------------------------------------------------
    def _bitmap_rows(self, hdc_memory, bitmap, width: int, height: int) -> list[bytes]:
        """GetDIBits 拿 BGR 行;DIB 自下而上,翻成自上而下。"""
        header = _BITMAPINFOHEADER()
        header.biSize = ctypes.sizeof(header)
        header.biWidth = width
        header.biHeight = -height  # 负高 = top-down,免翻转
        header.biPlanes = 1
        header.biBitCount = 24
        header.biCompression = BI_RGB
        stride = ((width * 3 + 3) // 4) * 4  # 4 字节对齐
        buffer = ctypes.create_string_buffer(stride * height)
        fetched = self.gdi32.GetDIBits(hdc_memory, bitmap, 0, height, buffer,
                                       ctypes.byref(header), DIB_RGB_COLORS)
        if fetched != height:
            raise OSError(f"GetDIBits 只取回 {fetched}/{height} 行")
        rows = []
        for index in range(height):
            row = buffer.raw[index * stride:(index + 1) * stride]
            rows.append(row[:width * 3])  # 去掉对齐垫字节
        return rows

    def screenshot_window(self, window_id: str) -> tuple[int, int, list[bytes]]:
        """离屏抓窗:PrintWindow 连遮挡窗也能抓;最小化窗拒绝,不冒充画面。"""
        hwnd = int(window_id, 16)
        if self.user32.IsIconic(hwnd):
            raise OSError("window_minimized: 最小化窗口没有稳定画面,先恢复再拍")
        client = wt.RECT()
        self.user32.GetClientRect(hwnd, ctypes.byref(client))
        width, height = client.right - client.left, client.bottom - client.top
        if width <= 0 or height <= 0:
            raise OSError(f"screenshot_failed: 客户区 {width}x{height} 无从拍起")

        hdc_window = self.user32.GetDC(hwnd)
        hdc_memory = self.gdi32.CreateCompatibleDC(hdc_window)
        bitmap = self.gdi32.CreateCompatibleBitmap(hdc_window, width, height)
        old = self.gdi32.SelectObject(hdc_memory, bitmap)
        try:
            painted = self.user32.PrintWindow(hwnd, hdc_memory, PW_RENDERFULLCONTENT)
            if not painted:
                # 退路:直接从屏幕 DC 按窗口矩形抄(窗口须前台可见)。
                rect = wt.RECT()
                self.user32.GetWindowRect(hwnd, ctypes.byref(rect))
                self.gdi32.BitBlt(hdc_memory, 0, 0, width, height,
                                  self.user32.GetDC(0), rect.left, rect.top,
                                  SRCCOPY | CAPTUREBLT)
            rows = self._bitmap_rows(hdc_memory, bitmap, width, height)
        finally:
            self.gdi32.SelectObject(hdc_memory, old)
            self.gdi32.DeleteObject(bitmap)
            self.gdi32.DeleteDC(hdc_memory)
            self.user32.ReleaseDC(hwnd, hdc_window)
        return width, height, rows

    def screenshot_screen(self, rect: list[int]) -> tuple[int, int, list[bytes]]:
        """整屏(或指定区域)拍摄。隐私口径:调用方默认只让拍窗口,整屏走显式 target。"""
        left, top, right, bottom = rect
        width, height = right - left, bottom - top
        if width <= 0 or height <= 0:
            raise OSError(f"screenshot_failed: 区域 {rect} 尺寸非正")
        hdc_screen = self.user32.GetDC(0)
        hdc_memory = self.gdi32.CreateCompatibleDC(hdc_screen)
        bitmap = self.gdi32.CreateCompatibleBitmap(hdc_screen, width, height)
        old = self.gdi32.SelectObject(hdc_memory, bitmap)
        try:
            self.gdi32.BitBlt(hdc_memory, 0, 0, width, height, hdc_screen, left, top,
                              SRCCOPY | CAPTUREBLT)
            rows = self._bitmap_rows(hdc_memory, bitmap, width, height)
        finally:
            self.gdi32.SelectObject(hdc_memory, old)
            self.gdi32.DeleteObject(bitmap)
            self.gdi32.DeleteDC(hdc_memory)
            self.user32.ReleaseDC(0, hdc_screen)
        return width, height, rows


class FakeBackend:
    """测试后端:窗口现场可预置,注入动作只记账不落桌面。

    test_runner.py 用它断言坐标换算、stale 拦截、危险键闸、dry-run——
    一只鼠标都不动。
    """

    def __init__(self) -> None:
        self.dpi_awareness = "fake_per_monitor_v2"
        self.windows: dict[str, dict] = {}
        self.calls: list[str] = []
        self.focus_succeeds = True
        self.virtual = [-1920, 0, 2560, 1440]
        self.monitors = 2
        self.screenshot_pixels = (3, 2, [b"\x10\x20\x30" * 3, b"\x40\x50\x60" * 3])

    def add_window(self, window_id: str, title: str, rect: list[int], *,
                   process_name: str = "fake.exe", minimized: bool = False,
                   foreground: bool = False, client_origin: Optional[list[int]] = None,
                   dpi_scale: float = 1.0) -> None:
        width, height = rect[2] - rect[0], rect[3] - rect[1]
        self.windows[window_id] = {
            "id": window_id, "title": title, "process_name": process_name, "rect": rect,
            "visible": True, "minimized": minimized, "foreground": foreground,
            "client_origin": client_origin or [rect[0] + 10, rect[1] + 32],
            "client_size": [max(width - 20, 1), max(height - 52, 1)],
            "dpi_scale": dpi_scale,
        }

    # -- 同 Win32Backend 的接口面 -------------------------------------------
    def list_windows(self) -> list[dict]:
        return [dict(info) for info in self.windows.values()]

    def window_exists(self, window_id: str) -> bool:
        return window_id in self.windows

    def window_state(self, window_id: str) -> Optional[dict]:
        info = self.windows.get(window_id)
        if info is None:
            return None
        origin = info["client_origin"]
        size = info["client_size"]
        return {
            "id": info["id"], "title": info["title"], "rect": list(info["rect"]),
            "client_rect_local": [0, 0, size[0], size[1]],
            "client_origin": list(origin), "dpi_scale": info["dpi_scale"],
            "minimized": info["minimized"], "foreground": info["foreground"],
        }

    def dpi_scale(self, window_id: str) -> float:
        info = self.windows.get(window_id)
        return info["dpi_scale"] if info else 1.0

    def virtual_screen(self) -> list[int]:
        return list(self.virtual)

    def monitor_count(self) -> int:
        return self.monitors

    def focus_window(self, window_id: str) -> bool:
        self.calls.append(f"focus({window_id})")
        if not self.focus_succeeds or window_id not in self.windows:
            return False
        for info in self.windows.values():
            info["foreground"] = False
        self.windows[window_id]["foreground"] = True
        return True

    def mouse_move(self, x: int, y: int) -> None:
        self.calls.append(f"move({x},{y})")

    def mouse_click(self, button: str, clicks: int, interval_ms: int = 40) -> None:
        self.calls.append(f"click({button}x{clicks})")

    def mouse_scroll(self, ticks: int) -> None:
        self.calls.append(f"scroll({ticks})")

    def key_combo(self, vk_names: list[str]) -> None:
        self.calls.append("key(" + "+".join(vk_names) + ")")

    def type_unicode(self, text: str, interval_ms: int) -> None:
        self.calls.append(f"unicode({len(text)}ch:{text!r})")

    def screenshot_window(self, window_id: str) -> tuple[int, int, list[bytes]]:
        info = self.windows.get(window_id)
        if info is None:
            raise OSError("window_not_found")
        if info["minimized"]:
            raise OSError("window_minimized")
        self.calls.append(f"screenshot_window({window_id})")
        width, height, rows = self.screenshot_pixels
        return width, height, rows

    def screenshot_screen(self, rect: list[int]) -> tuple[int, int, list[bytes]]:
        self.calls.append(f"screenshot_screen({rect})")
        width, height, rows = self.screenshot_pixels
        return width, height, rows


def make_backend(fake: Optional[FakeBackend] = None) -> Win32Backend | FakeBackend:
    """生产走 Win32Backend;测试塞 FakeBackend 进来。"""
    return fake if fake is not None else Win32Backend()
