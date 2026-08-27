# -*- coding: utf-8 -*-
"""零依赖 PNG 编码(标准库 zlib + struct,CRC 用 zlib.crc32)。

只做一件事:把 BGR 逐行位图(每行 3*width 字节,BitBlt 抓屏的原始形态)
编成真 PNG。写它不为省一枚 Pillow——截图链路少一个第三方依赖,权限、
字节帽、魔数校验便全在本仓代码里,不交外人猜。

教学校验:encode_png 回的字节以 \\x89PNG\\r\\n\\x1a\\n 开头,IHDR 宽高与
入参一致,IDAT 可被 zlib 解回,四段 CRC 全对——test_runner.py 逐项断言。
"""
from __future__ import annotations

import struct
import zlib

# 8 字节签名:所有 PNG 的第一句话,魔数校验认它。
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# IHDR 的 color type 2 = 真彩色 RGB,bit depth 8。
_COLOR_TYPE_RGB = 2
_BIT_DEPTH_8 = 8


def _chunk(kind: bytes, payload: bytes) -> bytes:
    """拼一段 PNG chunk:长度 + 类型 + 正文 + CRC32(类型与正文一起算)。"""
    crc = zlib.crc32(kind + payload) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)


def encode_png(width: int, height: int, bgr_rows: list[bytes]) -> bytes:
    """BGR 逐行位图编 PNG。

    bgr_rows:height 个元素,每个是 3*width 字节,序 B,G,R(Windows
    BitBlt 的原生序)。行内翻转成 PNG 要的 R,G,B;行首加 filter 字节 0
    (None 滤波)。
    """
    if width <= 0 or height <= 0:
        raise ValueError("宽高必须是正整数")
    if len(bgr_rows) != height:
        raise ValueError(f"行数 {len(bgr_rows)} 与高 {height} 不合")
    expected = width * 3
    for index, row in enumerate(bgr_rows):
        if len(row) != expected:
            raise ValueError(f"第 {index} 行 {len(row)} 字节,该是 {expected}")

    # RGB 换序 + filter 0 前缀,一口气压成一段 IDAT。
    scanlines = bytearray()
    for row in bgr_rows:
        scanlines.append(0)
        for offset in range(0, expected, 3):
            scanlines += bytes((row[offset + 2], row[offset + 1], row[offset]))

    header = struct.pack(">IIBBBBB", width, height, _BIT_DEPTH_8, _COLOR_TYPE_RGB, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + _chunk(b"IHDR", header)
        + _chunk(b"IDAT", zlib.compress(bytes(scanlines), 6))
        + _chunk(b"IEND", b"")
    )


def is_png(data: bytes) -> bool:
    """魔数 + IHDR 尺寸自检:交出去之前先自己认一遍,不把坏图冒充证据。"""
    if len(data) < 8 or not data.startswith(PNG_SIGNATURE):
        return False
    return data[12:16] == b"IHDR" if len(data) >= 16 else False
