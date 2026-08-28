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

    def test_manifest_is_valid_and_ten_tools(self):
        with open(os.path.join(HERE, "plugin.json"), encoding="utf-8") as handle:
            manifest = json.load(handle)
        self.assertEqual(manifest["manifest_version"], 1)
        self.assertEqual(manifest["id"], "gui-agent-example")
        self.assertEqual(manifest["runtime"]["kind"], "process")
        self.assertFalse(manifest["permissions"]["network"])
        self.assertIn("LUBANCODE_GUI_DRY_RUN", manifest["permissions"]["env"])
        self.assertEqual(len(manifest["tools"]), 10)
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
            self.assertEqual(observation["image"]["width"], 1568)
            self.assertEqual(observation["image"]["height"], 719)
            scale = observation["image_pixel_scale"]
            self.assertAlmostEqual(scale[0], 1980 / 1568, places=3)
            self.assertAlmostEqual(scale[1], 908 / 719, places=3)
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
            self.assertEqual(observation["image"]["width"], 1568)
            self.assertEqual(observation["image"]["height"], 784)


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
                 {"control_type": "button", "name": "提交", "rect": [164, 172, 240, 200]},
                 {"control_type": "button", "name": "重置", "rect": [244, 172, 320, 200]},
                 {"control_type": "edit", "name": "", "rect": [164, 220, 360, 244],
                  "value": "阿明"},
                 {"control_type": "text", "name": "", "rect": [116, 260, 160, 280]},
             ]},
            {"control_type": "titlebar", "name": "", "rect": [100, 80, 580, 116],
             "children": [
                 {"control_type": "button", "name": "关闭", "rect": [540, 84, 576, 112]},
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
