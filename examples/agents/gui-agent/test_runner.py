# -*- coding: utf-8 -*-
"""离线自测:python test_runner.py(只依赖标准库,从哪个目录跑都行)。

全部用 FakeBackend,一只真鼠标都不动:坐标换算、stale 拦截、危险键闸、
dry-run、各类上限、PNG 编码、协议帧,逐项断言。真桌面 E2E 另走
scripts/manual_e2e.py(默认 SKIP,须显式 --run)。
"""
from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import png  # noqa: E402
import runner  # noqa: E402
from gui_backend import FakeBackend, key_name_to_vk  # noqa: E402
from gui_actions import Settings  # noqa: E402

WINDOW = "0x001A0B0C"


def make_backend() -> FakeBackend:
    """预置一枚假窗口:虚拟屏 [-1920,0,2560,1440](双屏负原点,教学用)。"""
    backend = FakeBackend()
    backend.add_window(WINDOW, "LubanCode GUI Fixture", [100, 80, 580, 440],
                       client_origin=[108, 112], dpi_scale=1.5)
    return backend


def call(backend: FakeBackend, tool: str, arguments: dict, settings=None) -> dict:
    """直灌协议请求字典,不经管道。settings 缺省纯开关(dry-run 等)。"""
    if settings is not None:
        original = runner.Settings
        runner.Settings = lambda: settings
        try:
            return runner.build_response(
                {"protocol": 1, "call_id": "t1", "plugin": "gui-agent-example",
                 "tool": tool, "arguments": arguments, "context": {}}, backend)
        finally:
            runner.Settings = original
    return runner.build_response(
        {"protocol": 1, "call_id": "t1", "plugin": "gui-agent-example",
         "tool": tool, "arguments": arguments, "context": {}}, backend)


def error_code(response: dict) -> str:
    assert response["ok"] is False, f"期望失败帧,却成功了: {response}"
    return response["error"]["code"]


class ProtocolTest(unittest.TestCase):
    """协议帧形状:call_id 回显、ok 真、content 是 text,错误码稳定。"""

    def test_manifest_is_valid_and_nine_tools(self):
        with open(os.path.join(HERE, "plugin.json"), encoding="utf-8") as handle:
            manifest = json.load(handle)
        self.assertEqual(manifest["manifest_version"], 1)
        self.assertEqual(manifest["id"], "gui-agent-example")
        self.assertEqual(manifest["runtime"]["kind"], "process")
        self.assertFalse(manifest["permissions"]["network"])
        self.assertIn("LUBANCODE_GUI_DRY_RUN", manifest["permissions"]["env"])
        self.assertEqual(len(manifest["tools"]), 9)
        for tool in manifest["tools"]:
            self.assertEqual(tool["input_schema"]["type"], "object")
            self.assertFalse(tool["input_schema"].get("additionalProperties", True),
                             f"{tool['name']} 的 schema 没关 additionalProperties")

    def test_ok_frame_shape(self):
        response = call(make_backend(), "gui_status", {})
        self.assertTrue(response["ok"])
        self.assertEqual(response["call_id"], "t1")
        self.assertEqual(response["protocol"], 2)
        self.assertEqual(response["content"][0]["type"], "text")
        self.assertIn("structured", response)

    def test_screenshot_frame_carries_image_block(self):
        """协议 v2:截图响应的 content 里带 type=image 块(path 模式)。"""
        import tempfile
        with tempfile.TemporaryDirectory() as temp:
            response = call(make_backend(), "gui_screenshot",
                            {"target": "window", "window_id": WINDOW,
                             "artifact_dir": temp})
            self.assertTrue(response["ok"])
            blocks = response["content"]
            self.assertEqual(blocks[0]["type"], "text")
            images = [block for block in blocks if block["type"] == "image"]
            self.assertEqual(len(images), 1)
            self.assertEqual(images[0]["mime_type"], "image/png")
            self.assertTrue(os.path.isfile(images[0]["path"]))
            # data/path 恰给其一(path 模式不塞 base64)
            self.assertNotIn("data", images[0])

    def test_unknown_tool(self):
        self.assertEqual(error_code(call(make_backend(), "gui_nope", {})), "unknown_tool")

    def test_error_frame_shape(self):
        response = call(make_backend(), "gui_click", {"x": "abc", "y": 5})
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "invalid_arguments")
        self.assertTrue(response["error"]["message"])


class CoordinateTest(unittest.TestCase):
    """坐标口径:window_client 换算成全桌面物理像素;越界拒绝。"""

    def test_client_space_translates_to_virtual(self):
        backend = make_backend()
        response = call(backend, "gui_move_mouse",
                        {"x": 10, "y": 20, "coordinate_space": "window_client",
                         "window_id": WINDOW})
        self.assertTrue(response["ok"])
        # client_origin=[108,112] + (10,20)
        self.assertIn("move(118,132)", backend.calls)

    def test_virtual_space_passes_through(self):
        backend = make_backend()
        call(backend, "gui_move_mouse", {"x": -1900, "y": 5})
        self.assertIn("move(-1900,5)", backend.calls)  # 多屏负原点是合法坐标

    def test_out_of_virtual_screen_rejected(self):
        backend = make_backend()
        self.assertEqual(error_code(call(backend, "gui_move_mouse", {"x": 99999, "y": 0})),
                         "coordinate_out_of_range")
        self.assertEqual(backend.calls, [])  # 拒在注入前

    def test_client_space_requires_window(self):
        self.assertEqual(error_code(call(make_backend(), "gui_move_mouse",
                                         {"x": 5, "y": 5, "coordinate_space": "window_client"})),
                         "invalid_arguments")


class StaleObservationTest(unittest.TestCase):
    """截图到动作之间窗口挪了:带旧矩形动作,必须在注入前拒。"""

    def test_moved_window_rejected_before_click(self):
        backend = make_backend()
        response = call(backend, "gui_click", {
            "x": 50, "y": 50, "coordinate_space": "window_client", "window_id": WINDOW,
            "expected_window_rect": [100, 80, 580, 440]})
        self.assertTrue(response["ok"])  # 矩形没变,放行
        # 现在窗口"挪走"了(改预置矩形),同一份旧坐标必须被拒
        backend.windows[WINDOW]["rect"] = [240, 200, 720, 560]
        self.assertEqual(error_code(call(backend, "gui_click", {
            "x": 50, "y": 50, "coordinate_space": "window_client", "window_id": WINDOW,
            "expected_window_rect": [100, 80, 580, 440]})), "stale_observation")

    def test_window_gone_rejected(self):
        backend = make_backend()
        del backend.windows[WINDOW]
        self.assertEqual(error_code(call(backend, "gui_focus_window",
                                         {"window_id": WINDOW})), "window_not_found")

    def test_minimized_window_rejects_client_actions(self):
        backend = make_backend()
        backend.windows[WINDOW]["minimized"] = True
        self.assertEqual(error_code(call(backend, "gui_move_mouse", {
            "x": 5, "y": 5, "coordinate_space": "window_client", "window_id": WINDOW})),
            "window_minimized")


class DryRunTest(unittest.TestCase):
    """dry-run:校验全走,注入零发。默认关;开只认 1。"""

    def setUp(self):
        self.settings = Settings(env={})

    def test_click_injects_nothing(self):
        self.settings.dry_run = True
        backend = make_backend()
        response = call(backend, "gui_click", {
            "x": 50, "y": 50, "coordinate_space": "window_client", "window_id": WINDOW,
            "expected_window_rect": [100, 80, 580, 440]}, self.settings)
        self.assertTrue(response["ok"])
        self.assertTrue(response["structured"]["dry_run"])
        self.assertEqual(backend.calls, [])  # 一枚事件都没注入

    def test_dry_run_still_validates_stale(self):
        self.settings.dry_run = True
        backend = make_backend()
        self.assertEqual(error_code(call(backend, "gui_click", {
            "x": 50, "y": 50, "window_id": WINDOW,
            "expected_window_rect": [0, 0, 1, 1]}, self.settings)), "stale_observation")

    def test_dry_run_off_by_default(self):
        self.assertFalse(self.settings.dry_run)


class CapsAndSafetyTest(unittest.TestCase):
    """上限与危险闸:全在注入前收口。"""

    def test_text_cap(self):
        self.assertEqual(error_code(call(make_backend(), "gui_type_text",
                                         {"text": "长" * 4097, "window_id": WINDOW})),
                         "text_too_long")

    def test_scroll_cap(self):
        self.assertEqual(error_code(call(make_backend(), "gui_scroll",
                                         {"direction": "down", "ticks": 51})),
                         "invalid_arguments")

    def test_clicks_cap(self):
        self.assertEqual(error_code(call(make_backend(), "gui_click",
                                         {"x": 100, "y": 100, "clicks": 4})),
                         "invalid_arguments")

    def test_dangerous_combo_blocked_by_default(self):
        backend = make_backend()
        self.assertEqual(error_code(call(backend, "gui_key", {"keys": ["alt", "f4"]})),
                         "dangerous_key_blocked")
        self.assertEqual(error_code(call(backend, "gui_key", {"keys": ["win", "r"]})),
                         "dangerous_key_blocked")
        self.assertEqual(backend.calls, [])

    def test_dangerous_combo_needs_explicit_opt_in(self):
        settings = Settings(env={})
        settings.allow_dangerous_keys = True
        backend = make_backend()
        response = call(backend, "gui_key", {"keys": ["alt", "f4"]}, settings)
        self.assertTrue(response["ok"])
        self.assertIn("key(alt+f4)", backend.calls)

    def test_unknown_key_name_rejected(self):
        self.assertEqual(error_code(call(make_backend(), "gui_key", {"keys": ["ctl"]})),
                         "unknown_key")

    def test_vk_table_covers_teaching_keys(self):
        self.assertEqual(key_name_to_vk("enter"), 0x0D)
        self.assertEqual(key_name_to_vk("ctrl"), 0x11)
        self.assertEqual(key_name_to_vk("f4"), 0x73)
        self.assertIsNone(key_name_to_vk("capslock"))

    def test_type_text_result_does_not_echo_content(self):
        response = call(make_backend(), "gui_type_text",
                        {"text": "阿明", "window_id": WINDOW})
        self.assertTrue(response["ok"])
        dumped = json.dumps(response, ensure_ascii=False)
        self.assertNotIn("阿明", dumped)  # 正文不进结果帧,只报字符数
        self.assertEqual(response["structured"]["chars"], 2)

    def test_chinese_goes_unicode_path(self):
        backend = make_backend()
        call(backend, "gui_type_text", {"text": "阿明七号", "window_id": WINDOW})
        self.assertTrue(any("unicode(4ch" in entry for entry in backend.calls))

    def test_focus_failure_reported(self):
        backend = make_backend()
        backend.focus_succeeds = False
        self.assertEqual(error_code(call(backend, "gui_focus_window",
                                         {"window_id": WINDOW})), "focus_failed")


class ScreenshotTest(unittest.TestCase):
    """截图:真编码、真落盘、observation 元数据齐;最小化拒。"""

    def test_window_screenshot_writes_png_artifact(self):
        with tempfile.TemporaryDirectory() as temp:
            backend = make_backend()
            response = call(backend, "gui_screenshot",
                            {"target": "window", "window_id": WINDOW,
                             "artifact_dir": temp})
            self.assertTrue(response["ok"])
            observation = response["structured"]
            self.assertEqual(observation["window_id"], WINDOW)
            self.assertEqual(observation["window_rect"], [100, 80, 580, 440])
            self.assertEqual(observation["virtual_origin"], [-1920, 0])
            self.assertEqual(observation["dpi_scale"], 1.5)
            image = observation["image"]
            self.assertEqual(image["mime_type"], "image/png")
            self.assertEqual(image["width"], 3)  # FakeBackend 固定 3x2
            self.assertEqual(image["height"], 2)
            self.assertTrue(image["path"].startswith(temp))
            with open(image["path"], "rb") as handle:
                data = handle.read()
            self.assertTrue(png.is_png(data))
            self.assertEqual(len(data), image["bytes"])
            # 协议 v2:图随结果回喂,observation 的可见性标记翻真
            visibility = observation["model_visibility"]
            self.assertTrue(visibility["rich_result"])
            self.assertEqual(visibility["protocol"], 2)

    def test_screen_target_rejects_window_id(self):
        self.assertEqual(error_code(call(make_backend(), "gui_screenshot",
                                         {"target": "screen", "window_id": WINDOW})),
                         "invalid_arguments")

    def test_window_target_requires_window_id(self):
        self.assertEqual(error_code(call(make_backend(), "gui_screenshot",
                                         {"target": "window"})),
                         "invalid_arguments")

    def test_minimized_window_rejected(self):
        backend = make_backend()
        backend.windows[WINDOW]["minimized"] = True
        self.assertEqual(error_code(call(backend, "gui_screenshot",
                                         {"target": "window", "window_id": WINDOW})),
                         "window_minimized")


class PngCodecTest(unittest.TestCase):
    """PNG 编码:魔数、IHDR、可解压、尺寸一致——自己写的,自己先验。"""

    def test_encode_decode_roundtrip(self):
        # 2x2 像素,BGR 每行 6 字节;R 与 B 换序后才算对得上。
        data = png.encode_png(2, 2, [b"\x01\x02\xc8\x03\x04\xc8", b"\x05\x06\xc8\x07\x08\xc8"])
        self.assertTrue(data.startswith(png.PNG_SIGNATURE))
        width, height = struct.unpack(">II", data[16:24])
        self.assertEqual((width, height), (2, 2))
        # IDAT 解回来:每行 filter 0 + RGB(BGR 已按序换过:BGR(01,02,c8)→RGB(c8,02,01))
        idat_start = data.find(b"IDAT") + 4
        idat_end = idat_start + struct.unpack(">I", data[idat_start - 4:idat_start])[0]
        raw = zlib.decompress(data[idat_start:idat_end])
        self.assertEqual(raw, b"\x00\xc8\x02\x01\xc8\x04\x03\x00\xc8\x06\x05\xc8\x08\x07")

    def test_rejects_ragged_rows(self):
        with self.assertRaises(ValueError):
            png.encode_png(2, 2, [b"\x00" * 6, b"\x00" * 5])

    def test_all_zero_pixel_image_still_valid(self):
        data = png.encode_png(1, 1, [b"\x00\x00\x00"])
        self.assertTrue(png.is_png(data))


class WindowsOnlyTest(unittest.TestCase):
    """非 Windows:整插件如实报 unsupported_platform,不装能跑。"""

    def test_unsupported_platform_reported_when_backend_factory_fails(self):
        import gui_backend
        original = gui_backend.make_backend

        def broken():
            raise RuntimeError("unsupported_platform: 首版只承诺 Windows 10/11,当前 linux")

        gui_backend.make_backend = broken
        try:
            response = runner.build_response(
                {"protocol": 1, "call_id": "t9", "plugin": "gui-agent-example",
                 "tool": "gui_status", "arguments": {}, "context": {}})
        finally:
            gui_backend.make_backend = original
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "unsupported_platform")


if __name__ == "__main__":
    unittest.main()
