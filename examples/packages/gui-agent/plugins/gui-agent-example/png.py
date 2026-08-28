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
    # 换序走步长切片(0::3 取 R、2::3 取 B),整行在 C 里倒——
    # 1568x980 的图逐像素循环要好几秒,切片法几十毫秒。
    scanlines = bytearray()
    for row in bgr_rows:
        rgb = bytearray(expected)
        rgb[0::3] = row[2::3]  # R <- BGR 的第三字节
        rgb[1::3] = row[1::3]  # G
        rgb[2::3] = row[0::3]  # B <- BGR 的第一字节
        scanlines += b"\x00"
        scanlines += rgb

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


def downscale_to_long_edge(width: int, height: int, bgr_rows: list[bytes],
                           max_edge: int) -> tuple[int, int, list[bytes]]:
    """整数步长采样降采样:BGR 逐行位图长边缩进 max_edge 以内。

    为什么缩:各家视觉 token 都按分辨率计——anthropic 口径 tokens ≈
    (宽×高)/750 且建议长边 ≤1568(超了服务端也会先缩再计),gpt/gemini
    按 512/768 像素块计片。3072x1918 的整窗截图原样回喂,token 与边长帽
    两头吃亏;缩到 1568 长边内,四家上限都在安全侧。取 1568 是各家上限
    里最紧的那档(anthropic 的有效上限,gpt 的 2048、gemini 的 4096 都
    在它之上)。

    怎么缩:整数步长采样(每 step 个像素取一个),不做插值——纯切片
    (bytes 切片与 bytearray 步长赋值都在 C 层跑),零依赖、毫秒级,大图
    也不卡工具超时。代价是极端细线可能被步长跳过;UI 截图的字号都在
    数像素以上,step ≤ 2 的场景里肉眼与模型都读得清。
    """
    if width <= 0 or height <= 0:
        raise ValueError("宽高必须是正整数")
    if max_edge <= 0:
        raise ValueError("max_edge 必须是正整数")
    long_edge = max(width, height)
    if long_edge <= max_edge:
        return width, height, bgr_rows  # 帽内不动:不放大、不重排

    step = -(-long_edge // max_edge)  # ceil:保证缩后长边 ≤ max_edge
    dst_width = -(-width // step)
    dst_height = -(-height // step)
    # 每行三通道各按步长切片(B、G、R 起点错 1),再交错写回 bytearray
    # 的 0/1/2 步长位——三个切片等长(尾像素三通道同进同出),纯 C 层。
    stride = 3 * step
    out_rows: list[bytes] = []
    for y in range(0, height, step):
        row = bgr_rows[y]
        out = bytearray(dst_width * 3)
        out[0::3] = row[0::stride]
        out[1::3] = row[1::stride]
        out[2::3] = row[2::stride]
        out_rows.append(bytes(out))
    return dst_width, dst_height, out_rows
