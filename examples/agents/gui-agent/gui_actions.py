# -*- coding: utf-8 -*-
"""GUI 工具合同层:十件工具的动作纪律。

这层不管 Win32 花活(那是 gui_backend),也不管协议帧(那是 runner)。
它守的是工单里的几条铁律:

  - 坐标口径:virtual_screen(物理像素,多屏联合,可含负原点)与
    window_client(目标窗口客户区,左上为原点)两种,动作前换算,换算
    完先查界内再注入。
  - stale observation:截图时的窗口矩形进了 observation;动作若带
    expected_window_rect,执行前重查,不合即拒——不拿旧图坐标硬点。
    observation 不是权限凭据,它只防坐标过期。
  - 上限:文本 4096 字符、间隔 0-200ms、滚轮 1-50 格、连点 1-3 次、
    图片 8MB/8000x4000。越帽在注入前拒。
  - 危险组合:Win+X 与 Alt+F4 默认禁,环境变量显式开禁才放。
  - dry-run:动作类工具只走校验、只报计划,不注入一枚事件。
  - 动作只报事实:"点击已发送",不猜界面结果;下一步必须重新观察。
"""
from __future__ import annotations

import hashlib
import json
import os
import random
import time
from pathlib import Path
from typing import Optional

import gui_uia
import png as png_codec

PLUGIN_VERSION = "1.2.0"

# 协议 v2 起截图随结果回喂模型(image 块),宿主验身落账后上 wire;
# 证据文件照旧落盘(路径作附账,artifact 可追)。
MODEL_FEED_NOTE = {
    "rich_result": True,
    "protocol": 2,
    "note": "截图经协议 v2 image 块回喂模型,模型已看见;证据文件照旧落盘。",
}

TEXT_MAX_CHARS = 4096
INTERVAL_MS_RANGE = (0, 200)
SCROLL_TICKS_RANGE = (1, 50)
CLICKS_RANGE = (1, 3)
MOVE_DURATION_MS_RANGE = (0, 1000)
IMAGE_MAX_BYTES = 8 * 1024 * 1024
IMAGE_MAX_DIMENSION = 8000
# 回喂图的长边帽(px):超过就整数步长采样缩进帽内再编码回喂(落盘同
# 一份)。各家视觉 token 都按分辨率计(anthropic ≈ 宽×高/750,建议长边
# ≤1568;gpt/gemini 按 512/768 像素块),大原图原样上 wire 两头吃亏。
# 1568 是各家建议值里最紧的一档。证据目录另存原图走 keep_original,
# 默认不双份。
IMAGE_FEED_LONG_EDGE = 1568
TITLE_MAX_CHARS = 200
SNAPSHOT_NAME_MAX_CHARS = 200
# 正文行帽:与 gui_list_windows 的 20 窗帽同理——有 text 就不投影
# structured,清单必须进正文;但 400 行铺开谁也读不动,正文给前 60 行,
# 全量在 structured,超宽教模型用 depth 收窄。
SNAPSHOT_TEXT_LINES = 60

# 动作类工具(dry-run 拦这些);观察类照常执行。
ACTION_TOOLS = {"gui_focus_window", "gui_move_mouse", "gui_click", "gui_scroll",
                "gui_type_text", "gui_key"}


class ToolError(Exception):
    """工具失败:code 是稳定错误码,message 是人话。runner 包成 ok=false 帧。"""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


class Settings:
    """环境带来的三枚开关。宿主只递 allowlist 点名的变量。"""

    def __init__(self, env: Optional[dict] = None) -> None:
        env = env if env is not None else os.environ
        self.dry_run = env.get("LUBANCODE_GUI_DRY_RUN", "") == "1"
        self.allow_dangerous_keys = env.get("LUBANCODE_GUI_ALLOW_DANGEROUS_KEYS", "") == "1"
        self.evidence_dir = env.get("LUBANCODE_GUI_EVIDENCE_DIR", "")


def _sanitize_text(value: str) -> str:
    """外来文本过 UTF-8 清洗:控制字符(除换行)替成 U+FFFD 之外的问号。"""
    cleaned = []
    for char in value:
        if char == "\n" or ord(char) >= 32:
            cleaned.append(char)
        else:
            cleaned.append("?")
    return "".join(cleaned)


def _parse_window_id(arguments: dict) -> Optional[str]:
    raw = arguments.get("window_id")
    if raw is None or raw == "":
        return None
    if not isinstance(raw, str):
        raise ToolError("invalid_arguments", "window_id 须是 0x 十六进制字符串")
    try:
        int(raw, 16)
    except ValueError:
        raise ToolError("invalid_arguments", f"window_id 不是十六进制: {raw}") from None
    return raw


def _require_window_state(backend, window_id: str) -> dict:
    """动作前的现场重查:窗口在不在、矩形多大、前台是谁、DPI 多少。

    process 插件没有内存状态,每次调用都重新问桌面——这正是本示例的
    教学点:状态在桌面,不在插件。
    """
    state = backend.window_state(window_id)
    if state is None:
        raise ToolError("window_not_found",
                        f"窗口 {window_id} 不在了。窗口 id 只在当前桌面现场有效,"
                        "窗口可能已关闭;请重新 gui_list_windows。")
    return state


def _check_stale(state: dict, arguments: dict) -> None:
    """expected_window_rect 与现场不合即拒。防的是"截图时窗口在这儿,
    现在挪走了"的过期坐标,不是权限。"""
    expected = arguments.get("expected_window_rect")
    if expected is None:
        return
    if (not isinstance(expected, list) or len(expected) != 4
            or not all(isinstance(v, int) for v in expected)):
        raise ToolError("invalid_arguments", "expected_window_rect 须是 [left,top,right,bottom] 整数数组")
    if expected != state["rect"]:
        raise ToolError(
            "stale_observation",
            f"观察已过期:截图时窗口矩形 {expected},现在是 {state['rect']}。"
            "窗口挪过或改过尺寸;请重新 gui_screenshot 再动作。")


def _to_virtual(x: int, y: int, space: str, state: Optional[dict]) -> tuple[int, int]:
    """window_client → 全桌面物理像素;virtual_screen 原样。"""
    if space == "virtual_screen":
        return x, y
    if space == "window_client":
        if state is None:
            raise ToolError("invalid_arguments", "coordinate_space=window_client 须带 window_id")
        origin = state["client_origin"]
        return origin[0] + x, origin[1] + y
    raise ToolError("invalid_arguments", f"coordinate_space 不认得: {space}")


def _check_in_bounds(backend, x: int, y: int) -> None:
    left, top, right, bottom = backend.virtual_screen()
    if not (left <= x < right and top <= y < bottom):
        raise ToolError("coordinate_out_of_range",
                        f"({x},{y}) 不在虚拟屏 {left},{top} - {right},{bottom} 内")


def _ensure_foreground(backend, state: dict) -> bool:
    """目标窗口不在前台就聚焦;失败如实报,不冒充已就位。"""
    if state.get("foreground"):
        return False
    if not backend.focus_window(state["id"]):
        raise ToolError("focus_failed",
                        f"系统拒绝把 {state['title']!r} 切到前台。"
                        "Windows 限制后台进程抢焦点;先人工点一下该窗口再试。")
    return True


def _coordinate_fields(backend, arguments: dict) -> tuple[int, int, Optional[dict]]:
    """公共前置:坐标参数校验 + 换算 + 界内检查。回 (x, y, state)。"""
    x = arguments.get("x")
    y = arguments.get("y")
    if not isinstance(x, int) or not isinstance(y, int):
        raise ToolError("invalid_arguments", "x/y 须是整数像素(物理像素)")
    space = arguments.get("coordinate_space", "virtual_screen")
    window_id = _parse_window_id(arguments)
    state = _require_window_state(backend, window_id) if window_id else None
    if state is not None:
        _check_stale(state, arguments)
        if state["minimized"] and space == "window_client":
            raise ToolError("window_minimized", "窗口最小化了,客户区坐标无从换算;先恢复窗口")
    virtual_x, virtual_y = _to_virtual(x, y, space, state)
    _check_in_bounds(backend, virtual_x, virtual_y)
    return virtual_x, virtual_y, state


# ---------------------------------------------------------------------------
# 观察类工具
# ---------------------------------------------------------------------------

def gui_status(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    import platform
    import sys

    state = {
        "platform": sys.platform,
        "platform_promise": "windows-10-11" if sys.platform == "win32" else "unsupported",
        "python_version": platform.python_version(),
        "backend": type(backend).__name__,
        "dpi_awareness": getattr(backend, "dpi_awareness", "unknown"),
        "virtual_screen": backend.virtual_screen(),
        "monitors": backend.monitor_count(),
        "dry_run": settings.dry_run,
        "dangerous_keys_allowed": settings.allow_dangerous_keys,
        "plugin_version": PLUGIN_VERSION,
        "rich_result": MODEL_FEED_NOTE,
    }
    text = (f"平台 {state['platform']}(承诺范围 Windows 10/11),Python {state['python_version']},"
            f"DPI 感知 {state['dpi_awareness']},显示器 {state['monitors']} 台,"
            f"虚拟屏 {state['virtual_screen']},dry-run {'开' if state['dry_run'] else '关'},"
            "截图经协议 v2 回喂模型。")
    return text, state


def gui_list_windows(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    windows = backend.list_windows()
    title_filter = arguments.get("title_filter", "")
    if title_filter:
        needle = _sanitize_text(str(title_filter)).lower()
        windows = [w for w in windows if needle in w["title"].lower()]
    for window in windows:
        window["title"] = _sanitize_text(window["title"])[:TITLE_MAX_CHARS]
    # 正文自带每窗明细:宿主把工具结果喂模型时,有 text 就不投影 structured,
    # 明细只放 structured 等于不给。逐窗一行(id/标题/进程/状态/矩形),
    # 前 20 个进正文,余下靠 title_filter 收窄——32 窗全量铺开也读不动。
    # 枚举是文本的事(模型看得见 window_id 才截得了图),截图才是图的事。
    lines = [f"共 {len(windows)} 个可见顶层窗口。窗口 id 只在当前桌面现场有效。"]
    for w in windows[:20]:
        state = "/".join(s for s, on in (("前台", w.get("foreground")),
                                         ("最小化", w.get("minimized")),
                                         ("可见", w.get("visible"))) if on)
        lines.append(f"- {w['id']} | {w['title']} | {w.get('process_name', '?')}"
                     f" | {state or '?'} | rect={w.get('rect')}")
    if len(windows) > 20:
        lines.append(f"(其余 {len(windows) - 20} 个略,用 title_filter 收窄)")
    text = "\n".join(lines)
    return text, {"count": len(windows), "windows": windows,
                  "note": "window_id 仅本次桌面现场有效,不跨会话"}


def gui_focus_window(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    window_id = arguments.get("window_id")
    if not isinstance(window_id, str) or not window_id:
        raise ToolError("invalid_arguments", "须带 window_id(gui_list_windows 拿)")
    state = _require_window_state(backend, window_id)
    if settings.dry_run:
        return (f"DRY-RUN:将聚焦窗口 {state['id']}({state['title']!r}),未真切前台。",
                {"dry_run": True, "planned": ["focus"], "window_id": state["id"]})
    if state.get("foreground"):
        return f"窗口 {state['id']} 已在前台。", {"window_id": state["id"], "focused": True}
    if not backend.focus_window(state["id"]):
        raise ToolError("focus_failed",
                        f"系统拒绝把 {state['title']!r} 切到前台(Windows 前台锁定)。")
    verify = _require_window_state(backend, window_id)
    if not verify.get("foreground"):
        raise ToolError("focus_failed", f"已调用切换,复查发现 {state['id']} 仍不在前台。")
    return f"窗口 {verify['id']}({verify['title']!r})已在前台。", {
        "window_id": verify["id"], "focused": True, "rect": verify["rect"]}


def gui_screenshot(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    target = arguments.get("target", "window")
    if target not in ("window", "screen"):
        raise ToolError("invalid_arguments", "target 只认 window|screen;默认 window,少拍无关隐私")
    if target == "screen" and arguments.get("window_id"):
        raise ToolError("invalid_arguments", "target=screen 不收 window_id,两者择一")
    keep_original = arguments.get("keep_original", False)
    if not isinstance(keep_original, bool):
        raise ToolError("invalid_arguments", "keep_original 只认布尔(默认 false,不另存原图)")
    if target == "window":
        window_id = _parse_window_id(arguments)
        if window_id is None:
            raise ToolError("invalid_arguments", "target=window 须带 window_id")
        state = _require_window_state(backend, window_id)
        if state["minimized"]:
            raise ToolError("window_minimized",
                            "窗口最小化了,没有稳定画面(不冒充旧帧);先恢复再拍")
        width, height, rows = backend.screenshot_window(window_id)
        observation_window = {
            "window_id": state["id"], "window_rect": state["rect"],
            "client_size": [state["client_rect_local"][2], state["client_rect_local"][3]],
            "dpi_scale": state["dpi_scale"], "title": state["title"],
        }
        what = f"窗口 {state['title']!r}"
    else:
        virtual = backend.virtual_screen()
        width, height, rows = backend.screenshot_screen(virtual)
        observation_window = {"window_rect": virtual, "client_size": [width, height],
                              "dpi_scale": None, "title": None,
                              "note": "整屏拍摄会拍下所有显示器,含虚拟屏负坐标区"}
        what = f"整屏(虚拟屏 {virtual})"

    # 大图降采样:长边超帽先整数步长采样缩进帽内,回喂与落盘都用缩后图
    # (一份,不默认双份)。observation 里的窗口矩形/客户区仍是拍摄时的
    # 原坐标——动作坐标从矩形来,不经图像,缩放不挪坐标。
    capture_size = [width, height]
    capture_rows = rows  # 原始行位图留个引用:keep_original 另存时才编码
    width, height, rows = png_codec.downscale_to_long_edge(
        width, height, rows, IMAGE_FEED_LONG_EDGE)
    downscaled = [width, height] != capture_size

    encoded = png_codec.encode_png(width, height, rows)
    if len(encoded) > IMAGE_MAX_BYTES:
        raise ToolError("encoding_failed",
                        f"截图编码后 {len(encoded)} 字节,超 {IMAGE_MAX_BYTES} 帽;"
                        "缩小目标范围或降低分辨率")
    if not png_codec.is_png(encoded):
        raise ToolError("encoding_failed", "PNG 魔数自检失败,不出这份图")

    digest = hashlib.sha256(encoded).hexdigest()
    evidence_root = _evidence_root(arguments, settings)
    evidence_root.mkdir(parents=True, exist_ok=True)
    path = evidence_root / f"gui-obs-{time.strftime('%Y%m%d-%H%M%S')}-{digest[:8]}.png"
    path.write_bytes(encoded)

    # 原图另存是可选附账:默认不落,免得证据目录双份翻倍。
    original_path = None
    if downscaled and keep_original:
        original = png_codec.encode_png(capture_size[0], capture_size[1], capture_rows)
        original_digest = hashlib.sha256(original).hexdigest()
        original_path = evidence_root / (
            f"gui-obs-{time.strftime('%Y%m%d-%H%M%S')}-{original_digest[:8]}-orig.png")
        original_path.write_bytes(original)

    image_meta = {"artifact_id": f"sha256:{digest}", "path": str(path),
                  "width": width, "height": height, "mime_type": "image/png",
                  "bytes": len(encoded)}
    if downscaled:
        image_meta["capture_size"] = capture_size
        image_meta["downsample"] = {
            "long_edge_cap": IMAGE_FEED_LONG_EDGE,
            "note": "长边超帽,整数步长采样缩进帽内回喂;原图可传 keep_original 另存",
        }
        if original_path is not None:
            image_meta["downsample"]["original_path"] = str(original_path)
    observation = {
        "observation_id": f"obs-{random.randbytes(6).hex()}",
        "target": target,
        **observation_window,
        "virtual_origin": [backend.virtual_screen()[0], backend.virtual_screen()[1]],
        "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "image": image_meta,
        "model_visibility": MODEL_FEED_NOTE,
    }
    size_note = (f"{capture_size[0]}x{capture_size[1]},降采样 {width}x{height} 回喂"
                 if downscaled else f"{width}x{height}")
    text = (f"已截图 {what},{size_note},图已随结果回喂(直接描述画面即可),"
            f"证据文件落 {path}(sha256 前 8 位 {digest[:8]})。"
            "动作时请带 expected_window_rect 防坐标过期。")
    # 协议 v2:图随结果回喂(path 模式——宿主读文件、验魔数、落会话
    # artifact 后上 wire;这里不塞 base64,响应帧保持轻)。
    return text, observation, [{"mime_type": "image/png", "path": str(path)}]


def _evidence_root(arguments: dict, settings: Settings) -> Path:
    explicit = arguments.get("artifact_dir")
    if explicit:
        if not isinstance(explicit, str) or not explicit:
            raise ToolError("invalid_arguments", "artifact_dir 须是非空字符串")
        return Path(explicit)
    if settings.evidence_dir:
        return Path(settings.evidence_dir)
    return Path(os.environ.get("TEMP", ".")) / "lubancode-gui-agent"


def gui_snapshot(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    """UIA 控件树快照:桌面版 browser_snapshot。

    结构路找控件的正着:每行 `ref | 类型 | Name | rect`,模型按名定位、
    按 rect 中心点击,不用烧 token 看截图。ref 是本份快照内的短码(eN,
    browser 侧同款做法);不做窗口代次失效保护——每次快照重发新 ref,
    窗口没了靠 window_not_found 兜底。自绘控件 UIA 看不见是已知盲区,
    收 0 项时如实说,教模型回视觉路(截图)。
    """
    window_id = arguments.get("window_id")
    if not isinstance(window_id, str) or not window_id:
        raise ToolError("invalid_arguments", "须带 window_id(gui_list_windows 拿)")
    try:
        int(window_id, 16)
    except ValueError:
        raise ToolError("invalid_arguments", f"window_id 不是十六进制: {window_id}") from None
    depth = arguments.get("depth", gui_uia.DEFAULT_DEPTH)
    if (not isinstance(depth, int) or isinstance(depth, bool)
            or not 1 <= depth <= gui_uia.MAX_DEPTH):
        raise ToolError("invalid_arguments",
                        f"depth 须是 1-{gui_uia.MAX_DEPTH} 内整数(默认 {gui_uia.DEFAULT_DEPTH})")
    state = _require_window_state(backend, window_id)
    if state["minimized"]:
        raise ToolError("window_minimized", "窗口最小化了,控件树无从枚举;先恢复再拍")
    try:
        result = backend.snapshot_tree(window_id, depth)
    except OSError as error:
        raise ToolError("snapshot_failed", f"UIA 走树失败:{error}") from None

    elements = result["elements"]
    for index, element in enumerate(elements, 1):
        element["ref"] = f"e{index}"
        element["name"] = _sanitize_text(element.get("name", ""))[:SNAPSHOT_NAME_MAX_CHARS]

    lines = [f"窗口 {state['title']!r}({window_id})UIA 快照:depth={depth},"
             f"收 {len(elements)} 项,走访 {result['visited']} 节点,"
             f"{result['elapsed_ms']}ms。ref 只在本份快照内有效;"
             "rect 是全桌面物理像素(virtual_screen 口径,与截图同源),动作取矩形中心。"]
    for element in elements[:SNAPSHOT_TEXT_LINES]:
        lines.append(f"- {element['ref']} | {element['control_type']} | {element['name']}"
                     f" | rect={element['rect']}")
    if len(elements) > SNAPSHOT_TEXT_LINES:
        lines.append(f"(其余 {len(elements) - SNAPSHOT_TEXT_LINES} 项只进 structured;"
                     "树太宽就用 depth 收窄重拍)")
    if result["truncated"]:
        lines.append(f"树被截断:{result['reason']}。用更小的 depth 收窄,或分区块重拍。")
    if not elements:
        lines.append("一枚控件也没收到。多半是自绘界面(游戏/Electron 部分/老 Win32 自绘)"
                     "没给 UIA 暴露控件树——这是结构路的盲区,回 gui_screenshot 视觉路。")
    text = "\n".join(lines)
    structured = {
        "window_id": state["id"], "title": state["title"], "window_rect": state["rect"],
        "depth": depth, "count": len(elements),
        "truncated": result["truncated"], "truncated_reason": result["reason"],
        "visited": result["visited"], "elapsed_ms": result["elapsed_ms"],
        "read_failures": result.get("read_failures", []),
        "elements": elements,
        "note": "ref 仅本份快照内有效;rect 为 virtual_screen 物理像素;"
                "自绘控件 UIA 看不见,收 0 项时改走截图视觉路",
    }
    return text, structured


# ---------------------------------------------------------------------------
# 动作类工具(dry-run 拦)
# ---------------------------------------------------------------------------

def gui_move_mouse(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    x, y, state = _coordinate_fields(backend, arguments)
    duration = arguments.get("duration_ms", 0)
    if not isinstance(duration, int) or not MOVE_DURATION_MS_RANGE[0] <= duration <= MOVE_DURATION_MS_RANGE[1]:
        raise ToolError("invalid_arguments",
                        f"duration_ms 须是 {MOVE_DURATION_MS_RANGE} 内的整数")
    if settings.dry_run:
        return _dry_run_report("gui_move_mouse", [f"移到 ({x},{y})(全桌面物理像素)"], state)
    if duration <= 0:
        backend.mouse_move(x, y)
    else:
        current = _cursor_position(backend)
        steps = max(2, min(60, duration // 16 + 1))
        for step in range(1, steps + 1):
            ratio = step / steps
            backend.mouse_move(round(current[0] + (x - current[0]) * ratio),
                               round(current[1] + (y - current[1]) * ratio))
            time.sleep(duration / steps / 1000.0)
        backend.mouse_move(x, y)
    return (f"鼠标已移到 ({x},{y})。只移动,未点击。", {
        "x": x, "y": y, "coordinate_space_resolved": "virtual_screen",
        "window_id": state["id"] if state else None})


def _cursor_position(backend) -> tuple[int, int]:
    import ctypes
    class _POINT(ctypes.Structure):
        _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]
    point = _POINT()
    ctypes.windll.user32.GetCursorPos(ctypes.byref(point))  # 仅 Windows 真后端走到这
    return point.x, point.y


def _click_common(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    x, y, state = _coordinate_fields(backend, arguments)
    button = arguments.get("button", "left")
    if button not in ("left", "right", "middle"):
        raise ToolError("invalid_arguments", "button 只认 left|right|middle;默认 left")
    clicks = arguments.get("clicks", 1)
    if not isinstance(clicks, int) or not CLICKS_RANGE[0] <= clicks <= CLICKS_RANGE[1]:
        raise ToolError("invalid_arguments", f"clicks 须是 {CLICKS_RANGE} 内整数;双击显式写 2")
    needs_focus = state is not None and not state.get("foreground")
    if settings.dry_run:
        # dry-run 连聚焦也不发:聚焦本身就是动作(改前台)。
        plan = ([f"先把窗口切到前台({state['title']!r})"] if needs_focus else []) + [
            f"移到 ({x},{y})", f"{button} 键单击 x{clicks}"]
        return _dry_run_report("gui_click", plan, state)
    focused = _ensure_foreground(backend, state) if state else False
    backend.mouse_move(x, y)
    time.sleep(0.02)
    backend.mouse_click(button, clicks)
    return (f"已发送 {button} 点击 x{clicks} 到 ({x},{y})。"
            "这只是动作事实,不代表界面已变;下一步须 gui_screenshot 复验。", {
                "x": x, "y": y, "button": button, "clicks": clicks,
                "ensured_foreground": focused,
                "window_id": state["id"] if state else None,
                "verify_next": "gui_screenshot"})


def gui_click(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    return _click_common(backend, arguments, settings)


def gui_scroll(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    direction = arguments.get("direction")
    if direction not in ("up", "down"):
        raise ToolError("invalid_arguments", "direction 只认 up|down")
    ticks = arguments.get("ticks", 3)
    if not isinstance(ticks, int) or not SCROLL_TICKS_RANGE[0] <= ticks <= SCROLL_TICKS_RANGE[1]:
        raise ToolError("invalid_arguments",
                        f"ticks 须是 {SCROLL_TICKS_RANGE} 内整数,一次滚不出几万格")
    if "x" in arguments or "y" in arguments:
        x, y, state = _coordinate_fields(backend, arguments)
    else:
        x, y, state = None, None, None
    if settings.dry_run:
        return _dry_run_report("gui_scroll", [f"在 {'光标处' if x is None else f'({x},{y})'} "
                                              f"{direction} 滚 {ticks} 格"], state)
    if x is not None and y is not None:
        backend.mouse_move(x, y)
        time.sleep(0.02)
    backend.mouse_scroll(ticks if direction == "up" else -ticks)
    return (f"已发送滚轮 {direction} {ticks} 格。动作事实,界面是否滚动须截图复验。", {
        "direction": direction, "ticks": ticks, "x": x, "y": y,
        "window_id": state["id"] if state else None})


def gui_type_text(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    text = arguments.get("text")
    if not isinstance(text, str) or not text:
        raise ToolError("invalid_arguments", "text 须是非空字符串")
    if len(text) > TEXT_MAX_CHARS:
        raise ToolError("text_too_long",
                        f"text {len(text)} 字符,超 {TEXT_MAX_CHARS} 帽;分段输入")
    interval = arguments.get("interval_ms", 0)
    if not isinstance(interval, int) or not INTERVAL_MS_RANGE[0] <= interval <= INTERVAL_MS_RANGE[1]:
        raise ToolError("invalid_arguments", f"interval_ms 须是 {INTERVAL_MS_RANGE} 内整数")
    window_id = _parse_window_id(arguments)
    state = _require_window_state(backend, window_id) if window_id else None
    if state is not None:
        _check_stale(state, arguments)
    if settings.dry_run:
        # dry-run 也不回显全文,口径与真跑一致:只报长度与去向。
        target = state["title"] if state else "当前前台窗口"
        plan = [f"向 {target!r} 注入 {len(text)} 字符(内容不回显)"]
        if state is not None and not state.get("foreground"):
            plan.insert(0, f"先把窗口切到前台({target!r})")
        return _dry_run_report("gui_type_text", plan, state)
    focused = _ensure_foreground(backend, state) if state else False
    backend.type_unicode(text, interval)
    target = state["title"] if state else "当前前台窗口"
    return (f"已向 {target!r} 注入 {len(text)} 字符(内容不回显,防日志泄密)。"
            "中文经 Unicode 事件直注,不碰剪贴板、不走输入法。", {
                "chars": len(text), "interval_ms": interval,
                "window_id": state["id"] if state else None,
                "ensured_foreground": focused,
                "verify_next": "gui_screenshot"})


def gui_key(backend, arguments: dict, settings: Settings) -> tuple[str, dict]:
    keys = arguments.get("keys")
    if (not isinstance(keys, list) or not 1 <= len(keys) <= 4
            or not all(isinstance(k, str) for k in keys)):
        raise ToolError("invalid_arguments", "keys 须是 1-4 个键名的数组,如 [\"ctrl\",\"s\"]")
    lowered = [k.lower() for k in keys]
    for name in lowered:
        from gui_backend import key_name_to_vk
        if key_name_to_vk(name) is None:
            raise ToolError("unknown_key",
                            f"键名 {name!r} 不在枚举表里。只收枚举键,不收任意脚本")
    if _is_dangerous(lowered) and not settings.allow_dangerous_keys:
        raise ToolError("dangerous_key_blocked",
                        f"组合 {'+'.join(lowered)} 属高风险(能关窗口/唤系统壳),默认禁;"
                        "确要放行,设 LUBANCODE_GUI_ALLOW_DANGEROUS_KEYS=1 再跑")
    window_id = _parse_window_id(arguments)
    state = _require_window_state(backend, window_id) if window_id else None
    if state is not None:
        _check_stale(state, arguments)
    if settings.dry_run:
        plan = [f"发送组合键 {'+'.join(lowered)}"]
        if state is not None and not state.get("foreground"):
            plan.insert(0, f"先把窗口切到前台({state['title']!r})")
        return _dry_run_report("gui_key", plan, state)
    focused = _ensure_foreground(backend, state) if state else False
    backend.key_combo(lowered)
    target = state["title"] if state else "当前前台窗口"
    return (f"已向 {target!r} 发送 {'+'.join(lowered)}。注意:LubanCode 自己的取消键"
            "与发给目标窗口的键是两回事。", {
                "keys": lowered, "window_id": state["id"] if state else None,
                "ensured_foreground": focused, "verify_next": "gui_screenshot"})


def _is_dangerous(keys: list[str]) -> bool:
    """Win 组合与 Alt+F4 列高风险档:前者唤系统壳,后者直接关窗口。"""
    if "win" in keys:
        return True
    if "alt" in keys and "f4" in keys:
        return True
    return False


def _dry_run_report(tool: str, plan: list[str], state: Optional[dict]) -> tuple[str, dict]:
    summary = ";".join(plan)
    text = f"DRY-RUN:{tool} 只校验未注入。计划:{summary}。"
    return text, {"dry_run": True, "planned": plan,
                  "window_id": state["id"] if state else None,
                  "injected": False}


HANDLERS = {
    "gui_status": gui_status,
    "gui_list_windows": gui_list_windows,
    "gui_focus_window": gui_focus_window,
    "gui_screenshot": gui_screenshot,
    "gui_snapshot": gui_snapshot,
    "gui_move_mouse": gui_move_mouse,
    "gui_click": gui_click,
    "gui_scroll": gui_scroll,
    "gui_type_text": gui_type_text,
    "gui_key": gui_key,
}
