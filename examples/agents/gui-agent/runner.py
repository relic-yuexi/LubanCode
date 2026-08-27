# -*- coding: utf-8 -*-
"""gui-agent process 插件 runner(协议 v2)。

协议铁律与 examples/plugins/local_math 同源:
  stdin 恰好一份 JSON 请求,写完即关;stdout 恰好一份 JSON 响应,前后混
  任何字节都判协议错;日志只写 stderr。

v2 新事:响应 content 里可以带 type=image 块(截图回喂模型)。宿主(支持
协议 v2)验身落账后把图直接上 wire;旧宿主只认 v1,收到 protocol=2 的
响应按 UnknownContent 拒——所以本插件要求宿主 >= v2(请求帧说 2 才是真
支持)。

本插件的教学主张在这份文件里看得分明:九件工具,件件短命——进程起来,
干一件事,退场。窗口、光标、前台、输入焦点,全保存在 Windows 桌面手里;
插件自己不留一枚会话文件、不起一只后台 daemon。状态在桌面,不在插件,
所以不需要 MCP。

测试:test_runner.py 直接 import 本模块,注入 FakeBackend 离线自测,
不碰真鼠标。
"""
from __future__ import annotations

import json
import sys

from gui_actions import HANDLERS, Settings, ToolError


def build_response(request: dict, backend=None, settings=None) -> dict:
    """处理一份请求字典,回一份响应字典。协议层与传输层在这拆开:
    test_runner.py / manual_e2e.py 直接调它灌假请求,不经管道。
    backend 与 settings 都是注入口:不传则生产口径(真桌面 + 环境开关)。"""
    call_id = request.get("call_id", "")
    tool = request.get("tool", "")
    arguments = request.get("arguments", {})
    if not isinstance(arguments, dict):
        return _error_frame(call_id, "invalid_arguments", "arguments 必须是 object")
    handler = HANDLERS.get(tool)
    if handler is None:
        return _error_frame(call_id, "unknown_tool",
                            f"gui-agent 不认得工具 {tool!r};清单见 plugin.json")

    # 生产走真桌面;测试注入假后端。Win32Backend 构造失败(非 Windows)
    # 如实回 unsupported_platform,不装能跑。
    if backend is None:
        try:
            from gui_backend import make_backend
            backend = make_backend()
        except RuntimeError as error:
            return _error_frame(call_id, "unsupported_platform", str(error))

    try:
        outcome = handler(backend, arguments, settings if settings is not None else Settings())
        # 协议 v2:handler 可以回 (text, structured) 或 (text, structured, images)。
        # images 是要回喂模型的图:[{"mime_type": ..., "path": ...}],宿主验
        # 身(魔数/大小帽)后落会话 artifact 再上 wire。
        if len(outcome) == 3:
            text, structured, images = outcome
        else:
            text, structured = outcome
            images = []
    except ToolError as error:
        return _error_frame(call_id, error.code, error.message)
    except Exception as error:  # noqa: BLE001 - 协议要求任何业务失败都回失败帧
        print(f"[gui-agent] 未预期异常 tool={tool}: {error!r}", file=sys.stderr)
        return _error_frame(call_id, "execution_failed",
                            f"{type(error).__name__}: {error}")
    content = [{"type": "text", "text": text}]
    for image in images:
        content.append({"type": "image", "mime_type": image["mime_type"],
                        "path": image["path"]})
    return {
        "protocol": 2,
        "call_id": call_id,
        "ok": True,
        "content": content,
        "structured": structured,
    }


def _error_frame(call_id: str, code: str, message: str) -> dict:
    return {
        "protocol": 2,
        "call_id": call_id,
        "ok": False,
        "error": {"code": code, "message": message},
    }


def main() -> None:
    # Windows 管道下 Python 默认按本地代码页:中文先钉死 UTF-8。
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, OSError):
            pass
    request = json.load(sys.stdin)
    response = build_response(request)
    json.dump(response, sys.stdout, ensure_ascii=False)


if __name__ == "__main__":
    main()
