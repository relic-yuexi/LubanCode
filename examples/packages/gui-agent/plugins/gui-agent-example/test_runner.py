# -*- coding: utf-8 -*-
"""离线自测:python test_runner.py(只依赖标准库,从哪个目录跑都行)。

全部用 FakeBackend,一只真鼠标都不动:坐标换算、stale 拦截、危险键闸、
dry-run、各类上限、PNG 编码、协议帧,逐项断言。真桌面 E2E 另走
scripts/manual_e2e.py(默认 SKIP,须显式 --run)。

例外一册:LiveStructuralTest 在 Windows 上起一只原生 Win32 窗(纯 ctypes
造 EDIT/BUTTON/COMBOBOX),真 UIA 真快照真 set_value/invoke/expand——这些
是 COM pattern 调用,不注入一枚键盘鼠标事件,不抢焦点,与"一只真鼠标
都不动"的承诺不冲突。tkinter 的控件造不了这只夹具:Tk 自绘,UIA 不带
ValuePattern/InvokePattern/ExpandCollapsePattern(探针实测,连 BM_CLICK
都不触发 Tk 命令),原生控件才是结构路的真靶子。
"""
from __future__ import annotations

import ctypes
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
from gui_actions import SNAPSHOT_TEXT_LINES, Settings  # noqa: E402

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


def decode_png_pixels(data: bytes) -> tuple[int, int, list[bytes]]:
    """解本插件自己编码的 PNG(IHDR 定尺寸、单段 IDAT、filter 0),回
    (width, height, rgb_rows)。region/降采样测试靠它逐像素对账。"""
    width, height = struct.unpack(">II", data[16:24])
    idat_start = data.find(b"IDAT") + 4
    idat_end = idat_start + struct.unpack(">I", data[idat_start - 4:idat_start])[0]
    raw = zlib.decompress(data[idat_start:idat_end])
    stride = width * 3 + 1  # 行首 filter 字节
    rows = [raw[y * stride + 1:(y + 1) * stride] for y in range(height)]
    return width, height, rows


class ProtocolTest(unittest.TestCase):
    """协议帧形状:call_id 回显、ok 真、content 是 text,错误码稳定。"""

    def test_manifest_is_valid_and_twelve_tools(self):
        with open(os.path.join(HERE, "plugin.json"), encoding="utf-8") as handle:
            manifest = json.load(handle)
        self.assertEqual(manifest["manifest_version"], 1)
        self.assertEqual(manifest["id"], "gui-agent-example")
        self.assertEqual(manifest["runtime"]["kind"], "process")
        self.assertFalse(manifest["permissions"]["network"])
        self.assertIn("LUBANCODE_GUI_DRY_RUN", manifest["permissions"]["env"])
        self.assertEqual(len(manifest["tools"]), 12)
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

    def test_large_screenshot_downscaled_before_feed(self):
        """大图降采样:3072x1918 进,1536x959 出;落盘即回喂图,默认一份。"""
        with tempfile.TemporaryDirectory() as temp:
            backend = make_backend()
            row = bytes(range(256)) * 36  # 9216 = 3*3072
            backend.screenshot_pixels = (3072, 1918, [row] * 1918)
            response = call(backend, "gui_screenshot",
                            {"target": "window", "window_id": WINDOW,
                             "artifact_dir": temp})
            self.assertTrue(response["ok"])
            observation = response["structured"]
            image = observation["image"]
            # 回喂与落盘都是缩后图;capture_size 留原尺寸,坐标矩形不动。
            self.assertEqual((image["width"], image["height"]), (1536, 959))
            self.assertEqual(image["capture_size"], [3072, 1918])
            self.assertEqual(image["downsample"]["long_edge_cap"], 1568)
            self.assertIn("降采样 1536x959", response["content"][0]["text"])
            with open(image["path"], "rb") as handle:
                data = handle.read()
            self.assertEqual(len(data), image["bytes"])
            self.assertEqual(struct.unpack(">II", data[16:24]), (1536, 959))
            # 默认不双份:证据目录只有这一只 PNG。
            self.assertEqual([name for name in os.listdir(temp) if name.endswith(".png")],
                             [os.path.basename(image["path"])])
            # 窗口矩形仍是原坐标:动作坐标从矩形来,不经图像。
            self.assertEqual(observation["window_rect"], [100, 80, 580, 440])

    def test_keep_original_saves_extra_evidence_copy(self):
        """keep_original=true 且发生降采样:证据目录另存 -orig 原图。"""
        with tempfile.TemporaryDirectory() as temp:
            backend = make_backend()
            row = bytes(range(256)) * 36
            backend.screenshot_pixels = (3072, 1918, [row] * 1918)
            response = call(backend, "gui_screenshot",
                            {"target": "window", "window_id": WINDOW,
                             "artifact_dir": temp, "keep_original": True})
            self.assertTrue(response["ok"])
            downsample = response["structured"]["image"]["downsample"]
            original_path = downsample.get("original_path")
            self.assertTrue(original_path)
            with open(original_path, "rb") as handle:
                original = handle.read()
            self.assertTrue(original_path.endswith("-orig.png"))
            self.assertEqual(struct.unpack(">II", original[16:24]), (3072, 1918))
            # 帽内小图:不发生降采样,keep_original 也不另存。
            small = make_backend()
            response = call(small, "gui_screenshot",
                            {"target": "window", "window_id": WINDOW,
                             "artifact_dir": temp, "keep_original": True})
            self.assertTrue(response["ok"])
            image = response["structured"]["image"]
            self.assertNotIn("downsample", image)
            self.assertNotIn("capture_size", image)
    def test_small_fullshot_not_downscaled(self):
        """小图(长边≤1568)原样回:capture_size 与 image 同尺寸。"""
        with tempfile.TemporaryDirectory() as temp:
            response = call(make_backend(), "gui_screenshot",
                            {"target": "window", "window_id": WINDOW,
                             "artifact_dir": temp})
            observation = response["structured"]
            self.assertFalse(observation["downscaled"])
            self.assertEqual(observation["capture_size"], [3, 2])
            self.assertEqual(observation["image"]["width"], 3)
            self.assertEqual(observation["image"]["height"], 2)


class RegionCropTest(unittest.TestCase):
    """region 裁切:原像素回喂、位置逐像素对账、越界/空区明拒、
    target=window 按客户区原点换算。FakeBackend 开位置编码画布
    (B=0x40,G=vy&0xFF,R=vx&0xFF),解出 PNG 每枚像素都能反查坐标。"""

    def make_positional(self) -> FakeBackend:
        backend = make_backend()
        backend.positional_pixels = True
        return backend

    def test_window_region_original_pixels_and_position(self):
        with tempfile.TemporaryDirectory() as temp:
            backend = self.make_positional()
            # client_origin=[108,112],客户区 460x308:裁图内 (130,88) 起 64x48。
            region = [108 + 130, 112 + 88, 64, 48]
            response = call(backend, "gui_screenshot",
                            {"target": "window", "window_id": WINDOW,
                             "region": region, "artifact_dir": temp})
            self.assertTrue(response["ok"])
            observation = response["structured"]
            self.assertEqual(observation["region"], region)
            self.assertEqual(observation["region_in_image"], [130, 88, 64, 48])
            self.assertEqual(observation["capture_size"], [460, 308])
            self.assertFalse(observation["downscaled"])
            self.assertEqual(observation["image"]["width"], 64)
            self.assertEqual(observation["image"]["height"], 48)
            with open(observation["image"]["path"], "rb") as handle:
                data = handle.read()
            width, height, rows = decode_png_pixels(data)
            self.assertEqual((width, height), (64, 48))
            # 角像素对账:左上 (vx&0xFF, vy&0xFF, 0x40),右下同式——
            # 裁切没挪一像素,这就是"无损放大"。
            self.assertEqual(rows[0][0:3],
                             bytes((region[0] & 0xFF, region[1] & 0xFF, 0x40)))
            self.assertEqual(rows[height - 1][(width - 1) * 3:width * 3],
                             bytes(((region[0] + 63) & 0xFF,
                                    (region[1] + 47) & 0xFF, 0x40)))
            # 回执教模型把局部摆回全图、按原图坐标换算点击位
            text = response["content"][0]["text"]
            self.assertIn("原始分辨率", text)
            self.assertIn(f"({region[0]}+px, {region[1]}+py)", text)

    def test_screen_region_captures_exactly_the_rect(self):
        with tempfile.TemporaryDirectory() as temp:
            backend = self.make_positional()
            region = [-1920 + 40, 100, 32, 24]  # 多屏负原点区照裁
            response = call(backend, "gui_screenshot",
                            {"target": "screen", "region": region,
                             "artifact_dir": temp})
            self.assertTrue(response["ok"])
            observation = response["structured"]
            self.assertEqual(observation["region"], region)
            self.assertEqual(observation["region_in_image"], [40, 100, 32, 24])
            self.assertEqual(observation["image"]["width"], 32)
            self.assertEqual(observation["image"]["height"], 24)
            # 后端按区域直拍(不是全屏拍完再裁)
            self.assertIn("screenshot_screen([-1880, 100, -1848, 124])",
                          backend.calls)
            with open(observation["image"]["path"], "rb") as handle:
                data = handle.read()
            _, _, rows = decode_png_pixels(data)
            self.assertEqual(rows[0][0:3],
                             bytes((region[0] & 0xFF, region[1] & 0xFF, 0x40)))

    def test_window_region_out_of_client_rejected_before_capture(self):
        backend = self.make_positional()
        # 客户区在 virtual [108,112]-[568,420];这两个区都在外头。
        for region in ([0, 0, 50, 50],           # 左上越界(把标题栏当客户区了)
                       [108, 112, 461, 10],      # 右缘探出一像素
                       [560, 400, 20, 30]):      # 右下整个在外
            with self.subTest(region=region):
                response = call(backend, "gui_screenshot",
                                {"target": "window", "window_id": WINDOW,
                                 "region": region})
                self.assertEqual(error_code(response), "region_out_of_range")
                # 拒在拍摄前:一枚 BitBlt 都没发生
                self.assertEqual(backend.calls, [])
                self.assertIn("客户区", response["error"]["message"])

    def test_screen_region_out_of_virtual_rejected(self):
        backend = self.make_positional()
        for region in ([2560 - 10, 0, 20, 5],    # 右缘探出
                       [-1980, 0, 50, 50]):      # 左屏外头
            with self.subTest(region=region):
                self.assertEqual(error_code(call(backend, "gui_screenshot",
                                                 {"target": "screen",
                                                  "region": region})),
                                 "region_out_of_range")

    def test_region_shape_and_emptiness_rejected(self):
        backend = self.make_positional()
        for region in ([108, 112, 0, 10],        # 零宽
                       [108, 112, 10, -5],       # 负高
                       [108, 112, 10],           # 三个数
                       [108, 112, 10, 10, 10],   # 五个数
                       [108, 112, 10.5, 10],     # 浮点
                       [108, 112, True, 10],     # 布尔冒充整数
                       "108,112,10,10"):         # 字符串
            with self.subTest(region=region):
                self.assertEqual(error_code(call(backend, "gui_screenshot",
                                                 {"target": "window",
                                                  "window_id": WINDOW,
                                                  "region": region})),
                                 "invalid_arguments")


class DownscaleTest(unittest.TestCase):
    """1568 长边帽:整幅超帽近邻压到 1568(认布局够用);region 局部
    不走帽(原像素放大看细节)。两条规矩是一对,测就成对测。"""

    BIG = "0x00C0FFEE"

    def make_big_window(self) -> FakeBackend:
        backend = FakeBackend()
        backend.positional_pixels = True
        # rect 2000x960 → 客户区 1980x908(长边 1980 > 1568)
        backend.add_window(self.BIG, "Big Window", [200, 100, 2200, 1060],
                           client_origin=[210, 140])
        return backend

    def test_fullshot_over_cap_downscaled_to_1568(self):
        with tempfile.TemporaryDirectory() as temp:
            backend = self.make_big_window()
            response = call(backend, "gui_screenshot",
                            {"target": "window", "window_id": self.BIG,
                             "artifact_dir": temp})
            self.assertTrue(response["ok"])
            observation = response["structured"]
            self.assertTrue(observation["downscaled"])
            self.assertEqual(observation["capture_size"], [1980, 908])
            # 步长采样:ceil(1980/1568)=2 → 990x454(零插值,C 层切片,快)。
            self.assertEqual(observation["image"]["width"], 990)
            self.assertEqual(observation["image"]["height"], 454)
            scale = observation["image_pixel_scale"]
            self.assertAlmostEqual(scale[0], 2.0, places=3)
            self.assertAlmostEqual(scale[1], 2.0, places=3)
            text = response["content"][0]["text"]
            self.assertIn("1568", text)
            self.assertIn("region", text)  # 文案教模型:看不清就裁局部

    def test_region_bypasses_cap_original_pixels(self):
        with tempfile.TemporaryDirectory() as temp:
            backend = self.make_big_window()
            region = [210 + 100, 140 + 80, 300, 200]
            response = call(backend, "gui_screenshot",
                            {"target": "window", "window_id": self.BIG,
                             "region": region, "artifact_dir": temp})
            self.assertTrue(response["ok"])
            observation = response["structured"]
            self.assertFalse(observation["downscaled"])
            self.assertEqual(observation["image"]["width"], 300)
            self.assertEqual(observation["image"]["height"], 200)
            with open(observation["image"]["path"], "rb") as handle:
                data = handle.read()
            width, height, rows = decode_png_pixels(data)
            self.assertEqual((width, height), (300, 200))
            self.assertEqual(rows[0][0:3],
                             bytes((region[0] & 0xFF, region[1] & 0xFF, 0x40)))

    def test_screen_fullshot_over_cap_downscaled(self):
        with tempfile.TemporaryDirectory() as temp:
            backend = self.make_big_window()
            backend.virtual = [0, 0, 2000, 1000]
            response = call(backend, "gui_screenshot",
                            {"target": "screen", "artifact_dir": temp})
            self.assertTrue(response["ok"])
            observation = response["structured"]
            self.assertTrue(observation["downscaled"])
            self.assertEqual(observation["capture_size"], [2000, 1000])
            self.assertEqual(observation["image"]["width"], 1000)
            self.assertEqual(observation["image"]["height"], 500)


class SnapshotTest(unittest.TestCase):
    """UIA 快照:折叠规则、ref 顺序、深度帽、元素帽、错误码。全走
    FakeBackend 的假 UIA 树,零真 COM。"""

    def make_tree_backend(self) -> FakeBackend:
        """假窗一棵:内容 pane(折叠)里装按钮/输入/文本,titlebar 下还
        挂着系统按钮——夹具真树的形状。"""
        backend = make_backend()
        backend.set_uia_tree(WINDOW, [
            {"control_type": "pane", "name": "", "rect": [110, 120, 570, 400],
             "children": [
                 {"control_type": "text", "name": "名字:", "rect": [116, 124, 160, 144]},
                 {"control_type": "pane", "name": "名字输入框", "rect": [164, 120, 360, 148],
                  "children": []},
                 {"control_type": "button", "name": "提交", "rect": [164, 172, 240, 200],
                  "patterns": ["invoke"]},
                 {"control_type": "button", "name": "重置", "rect": [244, 172, 320, 200],
                  "patterns": ["invoke"]},
                 {"control_type": "edit", "name": "", "rect": [164, 220, 360, 244],
                  "value": "阿明", "patterns": ["value"]},
                 {"control_type": "text", "name": "", "rect": [116, 260, 160, 280]},
             ]},
            {"control_type": "titlebar", "name": "", "rect": [100, 80, 580, 116],
             "children": [
                 {"control_type": "button", "name": "关闭", "rect": [540, 84, 576, 112],
                  "patterns": ["invoke"]},
             ]},
        ])
        return backend

    def test_tree_folded_and_refed(self):
        response = call(self.make_tree_backend(), "gui_snapshot", {"window_id": WINDOW})
        self.assertTrue(response["ok"])
        elements = response["structured"]["elements"]
        # 折叠:无名 pane(内容容器)、无名 text、titlebar 本体都不收;
        # 有名 pane(名字输入框)、titlebar 的子按钮照收。
        got = [(e["ref"], e["control_type"], e["name"]) for e in elements]
        self.assertEqual(got, [
            ("e1", "text", "名字:"), ("e2", "pane", "名字输入框"),
            ("e3", "button", "提交"), ("e4", "button", "重置"),
            ("e5", "edit", ""), ("e6", "button", "关闭")])
        # 正文自带清单(有 text 不投影 structured),行格式 ref | 类型 | Name | rect
        text = response["content"][0]["text"]
        self.assertIn("- e3 | button | 提交 | rect=[164, 172, 240, 200]", text)
        self.assertIn("取矩形中心", text)

    def test_value_pattern_carried_in_structured(self):
        response = call(self.make_tree_backend(), "gui_snapshot", {"window_id": WINDOW})
        edit = [e for e in response["structured"]["elements"] if e["control_type"] == "edit"][0]
        self.assertEqual(edit["value"], "阿明")

    def test_pattern_tags_in_snapshot_lines(self):
        """行尾短标:模型看得见哪枚控件有结构路可选([value]/[invoke]/[expand])。"""
        response = call(self.make_tree_backend(), "gui_snapshot", {"window_id": WINDOW})
        text = response["content"][0]["text"]
        self.assertIn("- e3 | button | 提交 | rect=[164, 172, 240, 200] | [invoke]", text)
        self.assertIn("- e5 | edit |  | rect=[164, 220, 360, 244] | [value]", text)
        # 没探测出 pattern 的控件行尾不带标(与旧格式逐字节一致)。
        self.assertIn("- e1 | text | 名字: | rect=[116, 124, 160, 144]\n", text)
        self.assertIn("[value]/[invoke]/[expand]", text.splitlines()[0])  # 头行教用法
        structured = response["structured"]["elements"]
        self.assertEqual([e for e in structured if e["ref"] == "e3"][0]["patterns"],
                         ["invoke"])

    def test_depth_cut(self):
        backend = self.make_tree_backend()
        # depth=1:根的直接孩子是无名 pane/titlebar,折叠后一枚不剩。
        response = call(backend, "gui_snapshot", {"window_id": WINDOW, "depth": 1})
        self.assertTrue(response["ok"])
        self.assertEqual(response["structured"]["elements"], [])
        self.assertFalse(response["structured"]["truncated"])
        # depth=2 才够到内容 pane 的孩子们。
        response = call(backend, "gui_snapshot", {"window_id": WINDOW, "depth": 2})
        self.assertEqual(response["structured"]["count"], 6)

    def test_depth_validation(self):
        for bad in (0, 25, "3", 2.5, True):
            self.assertEqual(error_code(call(self.make_tree_backend(), "gui_snapshot",
                                             {"window_id": WINDOW, "depth": bad})),
                             "invalid_arguments",
                             f"depth={bad!r} 该拒")

    def test_element_cap_truncates_with_hint(self):
        import gui_uia
        backend = make_backend()
        backend.set_uia_tree(WINDOW, [
            {"control_type": "button", "name": f"按钮{i}", "rect": [0, i, 10, i + 10]}
            for i in range(gui_uia.MAX_ELEMENTS + 50)])
        response = call(backend, "gui_snapshot", {"window_id": WINDOW})
        self.assertTrue(response["ok"])
        structured = response["structured"]
        self.assertEqual(structured["count"], gui_uia.MAX_ELEMENTS)
        self.assertTrue(structured["truncated"])
        self.assertIn("depth 收窄", response["content"][0]["text"])
        # 正文行帽:structured 全量,正文只前 60 行。
        self.assertLessEqual(response["content"][0]["text"].count("\n- "),
                             SNAPSHOT_TEXT_LINES)

    def test_window_errors(self):
        backend = make_backend()
        del backend.windows[WINDOW]
        self.assertEqual(error_code(call(backend, "gui_snapshot", {"window_id": WINDOW})),
                         "window_not_found")
        backend = make_backend()
        backend.set_uia_tree(WINDOW, [])
        self.assertEqual(error_code(call(backend, "gui_snapshot", {})),
                         "invalid_arguments")
        backend = make_backend()
        backend.windows[WINDOW]["minimized"] = True
        self.assertEqual(error_code(call(backend, "gui_snapshot", {"window_id": WINDOW})),
                         "window_minimized")

    def test_empty_tree_advises_visual_path(self):
        backend = make_backend()
        backend.set_uia_tree(WINDOW, [])
        response = call(backend, "gui_snapshot", {"window_id": WINDOW})
        self.assertTrue(response["ok"])
        self.assertEqual(response["structured"]["count"], 0)
        self.assertIn("视觉路", response["content"][0]["text"])  # 盲区如实说

    def test_classify_rules(self):
        import gui_uia
        self.assertTrue(gui_uia.should_emit("button", ""))       # 交互件无名也收
        self.assertTrue(gui_uia.should_emit("edit", ""))
        self.assertFalse(gui_uia.should_emit("pane", ""))        # 无名容器折叠
        self.assertTrue(gui_uia.should_emit("pane", "名字输入框"))  # 有名容器是锚
        self.assertTrue(gui_uia.should_emit("text", "名字:"))
        self.assertFalse(gui_uia.should_emit("text", ""))
        self.assertFalse(gui_uia.should_emit("window", "有名字也不收"))
        self.assertFalse(gui_uia.should_emit("separator", "x"))
        # 控件类型表对头文件(UIAutomationClient.h 1318-1384)。
        self.assertEqual(gui_uia.CONTROL_TYPE_NAMES[50000], "button")
        self.assertEqual(gui_uia.CONTROL_TYPE_NAMES[50004], "edit")
        self.assertEqual(gui_uia.CONTROL_TYPE_NAMES[50032], "window")
        self.assertEqual(gui_uia.CONTROL_TYPE_NAMES[50033], "pane")


class StructuralActionTest(unittest.TestCase):
    """结构路动作合同(离线,FakeBackend):gui_set_value 整替、gui_invoke
    的 invoke/expand/collapse、pattern 闸、ref 解析、dry-run、错误码。"""

    def make_action_backend(self) -> FakeBackend:
        backend = make_backend()
        backend.set_uia_tree(WINDOW, [
            {"control_type": "edit", "name": "名字", "rect": [164, 120, 360, 148],
             "value": "旧值", "patterns": ["value"]},
            {"control_type": "button", "name": "提交", "rect": [164, 172, 240, 200],
             "patterns": ["invoke"]},
            {"control_type": "combobox", "name": "颜色", "rect": [164, 220, 360, 248],
             "value": "green", "patterns": ["value", "expand"]},
            {"control_type": "text", "name": "说明", "rect": [16, 260, 160, 280]},
        ])
        return backend

    def tree_node(self, backend: FakeBackend, name: str) -> dict:
        return [n for n in backend.uia_trees[WINDOW] if n.get("name") == name][0]

    def test_set_value_replaces_whole_value(self):
        backend = self.make_action_backend()
        response = call(backend, "gui_set_value",
                        {"window_id": WINDOW, "ref": "e1", "text": "新值一二三"})
        self.assertTrue(response["ok"])
        structured = response["structured"]
        self.assertEqual(structured["before"], "旧值")
        self.assertEqual(structured["after"], "新值一二三")
        self.assertTrue(structured["verified"])
        # 真替换:假树节点本身换了值,不只是回执嘴上说说。
        self.assertEqual(self.tree_node(backend, "名字")["value"], "新值一二三")
        self.assertIn("set_value(", "".join(backend.calls))

    def test_set_value_empty_string_clears(self):
        backend = self.make_action_backend()
        response = call(backend, "gui_set_value",
                        {"window_id": WINDOW, "ref": "e1", "text": ""})
        self.assertTrue(response["ok"])
        self.assertEqual(response["structured"]["after"], "")
        self.assertEqual(self.tree_node(backend, "名字")["value"], "")

    def test_set_value_without_pattern_points_to_typing(self):
        backend = self.make_action_backend()
        response = call(backend, "gui_set_value",
                        {"window_id": WINDOW, "ref": "e2", "text": "x"})
        self.assertEqual(error_code(response), "pattern_unsupported")
        self.assertIn("gui_type_text", response["error"]["message"])
        self.assertEqual(self.tree_node(backend, "名字")["value"], "旧值")  # 没动

    def test_invoke_fires_and_records(self):
        backend = self.make_action_backend()
        response = call(backend, "gui_invoke",
                        {"window_id": WINDOW, "ref": "e2", "action": "invoke"})
        self.assertTrue(response["ok"])
        self.assertEqual(response["structured"]["action"], "invoke")
        self.assertIn(f"invoke({WINDOW},e2)", backend.calls)

    def test_expand_and_collapse_toggle_state(self):
        backend = self.make_action_backend()
        response = call(backend, "gui_invoke",
                        {"window_id": WINDOW, "ref": "e3", "action": "expand"})
        self.assertTrue(response["ok"])
        self.assertEqual(response["structured"]["expand_state"], "expanded")
        self.assertTrue(self.tree_node(backend, "颜色")["expanded"])
        response = call(backend, "gui_invoke",
                        {"window_id": WINDOW, "ref": "e3", "action": "collapse"})
        self.assertEqual(response["structured"]["expand_state"], "collapsed")
        self.assertFalse(self.tree_node(backend, "颜色")["expanded"])
        self.assertIn(f"expand({WINDOW},e3)", "".join(backend.calls))
        self.assertIn(f"collapse({WINDOW},e3)", "".join(backend.calls))

    def test_expand_without_pattern_rejected(self):
        backend = self.make_action_backend()
        self.assertEqual(error_code(call(backend, "gui_invoke",
                                         {"window_id": WINDOW, "ref": "e1",
                                          "action": "expand"})),
                         "pattern_unsupported")

    def test_unknown_action_rejected(self):
        backend = self.make_action_backend()
        # sky 的 Raise/Scroll 有等价物(activate/滚轮),不进这枚枚举。
        for bad in ("raise", "scroll_up", "Invoke", ""):
            with self.subTest(action=bad):
                self.assertEqual(error_code(call(backend, "gui_invoke",
                                                 {"window_id": WINDOW, "ref": "e2",
                                                  "action": bad})),
                                 "invalid_arguments")

    def test_ref_format_rejected(self):
        for bad in ("3", "e", "e0", "x3", "e1x", 3, None, True):
            with self.subTest(ref=bad):
                self.assertEqual(error_code(call(self.make_action_backend(), "gui_set_value",
                                                 {"window_id": WINDOW, "ref": bad,
                                                  "text": "x"})),
                                 "invalid_arguments")

    def test_ref_beyond_count_reports_stale_ref(self):
        backend = self.make_action_backend()
        response = call(backend, "gui_set_value",
                        {"window_id": WINDOW, "ref": "e9", "text": "x"})
        self.assertEqual(error_code(response), "ref_not_found")
        self.assertIn("重拍", response["error"]["message"])

    def test_window_errors(self):
        backend = self.make_action_backend()
        del backend.windows[WINDOW]
        self.assertEqual(error_code(call(backend, "gui_set_value",
                                         {"window_id": WINDOW, "ref": "e1", "text": "x"})),
                         "window_not_found")
        backend = self.make_action_backend()
        backend.windows[WINDOW]["minimized"] = True
        self.assertEqual(error_code(call(backend, "gui_invoke",
                                         {"window_id": WINDOW, "ref": "e2",
                                          "action": "invoke"})),
                         "window_minimized")

    def test_depth_mismatch_changes_resolution(self):
        """depth 数错位就指错元素:深度帽砍的是 DFS 序中段的深节点,同一枚
        e3 在 depth=2 的快照里是丙,depth=8 重数却指到乙——这就是 schema 里
        'depth 须与快照一致'的原因。"""
        backend = make_backend()
        backend.set_uia_tree(WINDOW, [
            {"control_type": "pane", "name": "表单区", "rect": [10, 10, 90, 120],
             "children": [
                 {"control_type": "edit", "name": "甲输入", "rect": [12, 12, 88, 30],
                  "value": "", "patterns": ["value"],
                  "children": [
                      {"control_type": "edit", "name": "乙输入", "rect": [12, 32, 88, 50],
                       "value": "", "patterns": ["value"]},
                  ]},
                 {"control_type": "edit", "name": "丙输入", "rect": [12, 52, 88, 70],
                  "value": "", "patterns": ["value"]},
             ]},
        ])
        snap = call(backend, "gui_snapshot", {"window_id": WINDOW, "depth": 2})
        self.assertEqual([e["name"] for e in snap["structured"]["elements"]],
                         ["表单区", "甲输入", "丙输入"])  # 乙被深度帽砍了
        response = call(backend, "gui_set_value",
                        {"window_id": WINDOW, "ref": "e3", "text": "x", "depth": 8})
        # 快照里 e3 是丙输入;depth=8 重数把乙算进来,e3 变了乙输入。
        self.assertTrue(response["ok"])
        self.assertEqual(response["structured"]["name"], "乙输入")
        response = call(backend, "gui_set_value",
                        {"window_id": WINDOW, "ref": "e3", "text": "y", "depth": 2})
        self.assertEqual(response["structured"]["name"], "丙输入")  # 一致时才对得上

    def test_dry_run_reports_plan_only(self):
        settings = Settings(env={})
        settings.dry_run = True
        backend = self.make_action_backend()
        response = call(backend, "gui_set_value",
                        {"window_id": WINDOW, "ref": "e1", "text": "计划值"}, settings)
        self.assertTrue(response["ok"])
        self.assertTrue(response["structured"]["dry_run"])
        response = call(backend, "gui_invoke",
                        {"window_id": WINDOW, "ref": "e2", "action": "invoke"}, settings)
        self.assertTrue(response["structured"]["dry_run"])
        self.assertEqual(backend.calls, [])  # 一枚 pattern 调用都没发
        self.assertEqual(self.tree_node(backend, "名字")["value"], "旧值")


@unittest.skipUnless(sys.platform == "win32", "真 UIA 活体册只在 Windows")
class LiveStructuralTest(unittest.TestCase):
    """真 COM、真控件、零输入注入:set_value/invoke/expand 是 UIA pattern
    调用,不是 SendInput——不碰鼠标键盘、不抢焦点(窗口 WS_EX_NOACTIVATE)。

    夹具是纯 ctypes 造的原生 EDIT/BUTTON/COMBOBOX。为什么不用 tkinter:
    Tk 控件自绘,UIA 不带这三只 pattern(实测 Entry/Combobox 一只 pattern
    都探不出;tk.Button 的 Invoke 回 S_OK 但 BM_CLICK 不进 Tk 的事件翻译,
    命令不触发)。原生控件是结构路的真靶子;tkinter 的"半瞎"本身是教学
    点,排错表里另立账。建窗失败(无桌面会话的 CI)整册跳过。
    """

    WINDOW_TEXT = "LubanCode Live Structural Fixture"

    @classmethod
    def setUpClass(cls):
        import ctypes
        import ctypes.wintypes as wt
        cls._ct = ctypes
        cls._wt = wt
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        # 64 位句柄不经 argtypes 钉死会被 c_int 截断——探针上踩实过的坑。
        user32.CreateWindowExW.restype = ctypes.c_void_p
        user32.CreateWindowExW.argtypes = [ctypes.c_ulong, ctypes.c_wchar_p, ctypes.c_wchar_p,
                                           ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                                           ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                                           ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        user32.DefWindowProcW.restype = ctypes.c_ssize_t
        user32.DefWindowProcW.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                          ctypes.c_size_t, ctypes.c_ssize_t]
        user32.SendMessageW.restype = ctypes.c_ssize_t
        user32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                        ctypes.c_size_t, ctypes.c_ssize_t]
        user32.PeekMessageW.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
        user32.TranslateMessage.argtypes = [ctypes.c_void_p]
        user32.DispatchMessageW.argtypes = [ctypes.c_void_p]
        user32.DestroyWindow.argtypes = [ctypes.c_void_p]
        user32.UnregisterClassW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p]
        cls.user32 = user32
        cls.instance = kernel32.GetModuleHandleW(None)
        cls.clicks: list[int] = []
        cls.edit_id, cls.button_id, cls.combo_id = 2001, 2002, 2003

        @ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_void_p, ctypes.c_uint,
                            ctypes.c_size_t, ctypes.c_ssize_t)
        def wnd_proc(hwnd, msg, wparam, lparam):
            if msg == 0x0111 and (wparam >> 16) == 0:  # WM_COMMAND / BN_CLICKED
                cls.clicks.append(wparam & 0xFFFF)
            return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

        cls.wnd_proc = wnd_proc  # 引用攥在类上,窗口活着它就得活着

        class WNDCLASSW(ctypes.Structure):
            _fields_ = [("style", ctypes.c_uint), ("lpfnWndProc", ctypes.c_void_p),
                        ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                        ("hInstance", ctypes.c_void_p), ("hIcon", ctypes.c_void_p),
                        ("hCursor", ctypes.c_void_p), ("hbrBackground", ctypes.c_void_p),
                        ("lpszMenuName", ctypes.c_wchar_p),
                        ("lpszClassName", ctypes.c_wchar_p)]

        wc = WNDCLASSW()
        wc.style = 3
        wc.lpfnWndProc = ctypes.cast(wnd_proc, ctypes.c_void_p)
        wc.hInstance = ctypes.c_void_p(cls.instance)
        wc.lpszClassName = "LubanCodeGuiAgentLive"
        if not user32.RegisterClassW(ctypes.byref(wc)):
            raise unittest.SkipTest("RegisterClassW 失败,活体册跳过")
        # WS_EX_NOACTIVATE(0x08000000):不抢前台;/plugin test 跑这册时
        # 用户桌面不动一分。
        cls.top = user32.CreateWindowExW(
            0x08000000, "LubanCodeGuiAgentLive", cls.WINDOW_TEXT,
            0x00CF0000 | 0x10000000, 60, 60, 420, 240,
            None, None, ctypes.c_void_p(cls.instance), None)
        if not cls.top:
            raise unittest.SkipTest("CreateWindowExW 失败(无桌面会话?),活体册跳过")
        WS_CHILD_VISIBLE = 0x40000000 | 0x10000000
        cls.edit_hwnd = user32.CreateWindowExW(
            0, "EDIT", "活体旧值", WS_CHILD_VISIBLE, 16, 14, 300, 24,
            cls.top, cls.edit_id, ctypes.c_void_p(cls.instance), None)
        cls.combo_hwnd = user32.CreateWindowExW(
            0, "COMBOBOX", None, WS_CHILD_VISIBLE | 0x0003,  # CBS_DROPDOWNLIST
            16, 54, 300, 200, cls.top, cls.combo_id,
            ctypes.c_void_p(cls.instance), None)
        for item in ("green", "blue", "red"):
            user32.SendMessageW(cls.combo_hwnd, 0x0143, 0,
                                ctypes.cast(ctypes.c_wchar_p(item),
                                            ctypes.c_void_p).value or 0)
        user32.SendMessageW(cls.combo_hwnd, 0x014E, 0, 0)  # CB_SETCURSEL 0
        cls.button_hwnd = user32.CreateWindowExW(
            0, "BUTTON", "活体提交", WS_CHILD_VISIBLE, 16, 94, 120, 30,
            cls.top, cls.button_id, ctypes.c_void_p(cls.instance), None)
        user32.UpdateWindow(cls.top)
        cls.window_id = f"0x{cls.top & 0xFFFFFFFF:08X}"

    @classmethod
    def tearDownClass(cls):
        if getattr(cls, "top", None):
            cls.user32.DestroyWindow(cls.top)
            cls.user32.UnregisterClassW("LubanCodeGuiAgentLive",
                                        ctypes.c_void_p(cls.instance))

    @classmethod
    def _pump(cls):
        msg = cls._wt.MSG()
        while cls.user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            cls.user32.TranslateMessage(ctypes.byref(msg))
            cls.user32.DispatchMessageW(ctypes.byref(msg))

    def _live_call(self, tool: str, arguments: dict) -> dict:
        return runner.build_response(
            {"protocol": 1, "call_id": "live", "plugin": "gui-agent-example",
             "tool": tool, "arguments": arguments, "context": {}}, None)

    def _snapshot(self) -> dict:
        response = self._live_call("gui_snapshot", {"window_id": self.window_id})
        self.assertTrue(response["ok"],
                        f"快照失败: {response.get('error', response['structured'])}")
        return response

    def _element(self, snapshot: dict, control_type: str) -> dict:
        for element in snapshot["structured"]["elements"]:
            if element["control_type"] == control_type:
                return element
        self.fail(f"快照里没有 {control_type}:"
                  f"{[e['control_type'] for e in snapshot['structured']['elements']]}")

    def _element_by_name(self, snapshot: dict, name: str) -> dict:
        """按名定位(不是按类型取头一枚:标题栏的最小化/关闭也是 button,
        逮住它们 invoke 会真把窗口最小化/关掉)。"""
        for element in snapshot["structured"]["elements"]:
            if element["name"] == name:
                return element
        self.fail(f"快照里没有名为 {name!r} 的控件:"
                  f"{[(e['ref'], e['control_type'], e['name']) for e in snapshot['structured']['elements']]}")

    def test_snapshot_annotates_native_controls(self):
        snapshot = self._snapshot()
        text = snapshot["content"][0]["text"]
        edit = self._element(snapshot, "edit")
        button = self._element_by_name(snapshot, "活体提交")
        combobox = self._element(snapshot, "combobox")
        # 真 UIA 探出的 pattern 进 structured,也进正文行尾短标。
        self.assertIn("value", edit["patterns"])
        self.assertIn("invoke", button["patterns"])
        self.assertIn("expand", combobox["patterns"])
        self.assertRegex(text, rf"- {edit['ref']} \| edit \|.*\| \[value\]")
        self.assertRegex(text, rf"- {button['ref']} \| button \|.*\| .*invoke")
        self.assertRegex(text, rf"- {combobox['ref']} \| combobox \|.*expand")

    def test_set_value_replaces_edit_content(self):
        snapshot = self._snapshot()
        edit = self._element(snapshot, "edit")
        response = self._live_call("gui_set_value",
                                   {"window_id": self.window_id, "ref": edit["ref"],
                                    "text": "整替后的新值"})
        self.assertTrue(response["ok"], response.get("error"))
        structured = response["structured"]
        self.assertEqual(structured["before"], "活体旧值")
        self.assertEqual(structured["after"], "整替后的新值")
        self.assertTrue(structured["verified"])
        # 不信回执信控件:WM_GETTEXT 直读 EDIT 的真身。
        user32 = self.user32
        length = user32.SendMessageW(self.edit_hwnd, 0x000E, 0, 0)  # WM_GETTEXTLENGTH
        buffer = self._ct.create_unicode_buffer(length + 1)
        user32.SendMessageW(self.edit_hwnd, 0x000D, length + 1,
                            self._ct.addressof(buffer))  # WM_GETTEXT
        self.assertEqual(buffer.value, "整替后的新值")

    def test_invoke_fires_real_button_command(self):
        snapshot = self._snapshot()
        button = self._element_by_name(snapshot, "活体提交")
        clicks_before = len(self.clicks)
        response = self._live_call("gui_invoke",
                                   {"window_id": self.window_id, "ref": button["ref"],
                                    "action": "invoke"})
        self.assertTrue(response["ok"], response.get("error"))
        self._pump()  # BN_CLICKED 走消息队列,泵一把才落地
        self.assertEqual(len(self.clicks), clicks_before + 1, response["structured"])
        self.assertEqual(self.clicks[-1], self.button_id)

    def test_combobox_expand_and_collapse(self):
        snapshot = self._snapshot()
        combobox = self._element(snapshot, "combobox")
        response = self._live_call("gui_invoke",
                                   {"window_id": self.window_id, "ref": combobox["ref"],
                                    "action": "expand"})
        self.assertTrue(response["ok"], response.get("error"))
        self.assertEqual(response["structured"]["expand_state"], "expanded")
        response = self._live_call("gui_invoke",
                                   {"window_id": self.window_id, "ref": combobox["ref"],
                                    "action": "collapse"})
        self.assertTrue(response["ok"], response.get("error"))
        self.assertEqual(response["structured"]["expand_state"], "collapsed")

    def test_set_value_on_button_reports_pattern_fallback(self):
        snapshot = self._snapshot()
        button = self._element(snapshot, "button")
        response = self._live_call("gui_set_value",
                                   {"window_id": self.window_id, "ref": button["ref"],
                                    "text": "x"})
        self.assertFalse(response["ok"])
        self.assertEqual(response["error"]["code"], "pattern_unsupported")
        self.assertIn("gui_type_text", response["error"]["message"])


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

    def test_downscale_caps_long_edge(self):
        """长边超帽缩进帽内:步长采样、比例保住、通道序不乱。"""
        # 3072x1918(用户炸单的那副形状):step=ceil(3072/1568)=2 →
        # 1536x959,长边进帽,比例 3072/1918≈1536/959。
        width, height = 3072, 1918
        row = bytes(range(256)) * 36  # 9216 字节 = 3*3072,内容有起伏
        rows = [row] * height
        new_w, new_h, new_rows = png.downscale_to_long_edge(width, height, rows, 1568)
        self.assertEqual((new_w, new_h), (1536, 959))
        self.assertEqual(len(new_rows), new_h)
        for out in new_rows:
            self.assertEqual(len(out), new_w * 3)
        # 通道序与步长:出图首像素=原图第 0 像素,次像素=原图第 2 像素
        # (步长 2),BGR 三字节原样;行同理,出图第 1 行=原图第 2 行。
        self.assertEqual(new_rows[0][:3], row[0:3])
        self.assertEqual(new_rows[0][3:6], row[6:9])
        self.assertEqual(new_rows[1][:3], row[:3])
        # 缩后图能直接进编码器,IHDR 宽高就是缩后尺寸。
        data = png.encode_png(new_w, new_h, new_rows)
        self.assertEqual(struct.unpack(">II", data[16:24]), (new_w, new_h))

    def test_downscale_noop_within_cap(self):
        """帽内不动:小图原样返回,不放大、不重排。"""
        rows = [b"\x10\x20\x30" * 3, b"\x40\x50\x60" * 3]
        self.assertEqual(png.downscale_to_long_edge(3, 2, rows, 1568), (3, 2, rows))
        # 恰在帽上(长边=帽)也不动。
        self.assertEqual(png.downscale_to_long_edge(1568, 2, [b"\x00" * 1568 * 3] * 2, 1568)[0], 1568)

    def test_downscale_rejects_bad_input(self):
        with self.assertRaises(ValueError):
            png.downscale_to_long_edge(0, 2, [b""], 1568)
        with self.assertRaises(ValueError):
            png.downscale_to_long_edge(2, 2, [b"\x00" * 6] * 2, 0)


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
