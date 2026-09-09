#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""src/cli/grapheme.cpp 两张区间表的机器生成器(conhost 原生几何分叉与宽表机器生成单)。

表一 kExtendRanges:auxiliary/GraphemeBreakProperty.txt 的 GCB=Extend 全量
  区间(UAX#29 分段用;含变体选择符、ZWNJ 与 emoji 肤色修饰。注意不是
  DerivedCoreProperties 的 Grapheme_Extend——那张少了肤色修饰与乐谱附标,
  分段口径以 GraphemeBreakProperty 为准)。
表二 kWideRanges:EastAsianWidth.txt 的 W/F 区段 ∪ emoji-data.txt 的
  Emoji_Presentation 区段;East_Asian_Width 对未赋值码位的默认值(见该文件
  头注释:CJK 统一表意三段与平面 2/3 整面默认 W)按规范一并折入。

数据来源(Unicode 15.1,UCD 15.1.0,写死不改;换版本须连表头注释一起重生成):
  https://www.unicode.org/Public/15.1.0/ucd/auxiliary/GraphemeBreakProperty.txt
  https://www.unicode.org/Public/15.1.0/ucd/EastAsianWidth.txt
  https://www.unicode.org/Public/15.1.0/ucd/emoji/emoji-data.txt

用法:
  python scripts/gen_unicode_tables.py            # 解析数据,打印与 grapheme.cpp
                                                  #   现表的差异清单(只读,不改文件)
  python scripts/gen_unicode_tables.py --write    # 重写 grapheme.cpp 两张表
                                                  #   (表头注明 UCD 版本与生成日期)
  python scripts/gen_unicode_tables.py --data-dir DIR   # 用本地缓存的三份 UCD 文件,
                                                  #   缺哪份下哪份(离线重跑用)

生成的是"数据",不许手改:表要动,动这份脚本再重跑。static_assert 的严格
递增把关在 C++ 侧沿用,本脚本保证输出区间升序、互不重叠、相邻已合并。
"""

from __future__ import annotations

import argparse
import datetime
import re
import sys
import tempfile
import urllib.request
from pathlib import Path

UCD_VERSION = "15.1.0"
SOURCES = {
    "GraphemeBreakProperty.txt":
        "https://www.unicode.org/Public/15.1.0/ucd/auxiliary/GraphemeBreakProperty.txt",
    "EastAsianWidth.txt": "https://www.unicode.org/Public/15.1.0/ucd/EastAsianWidth.txt",
    "emoji-data.txt": "https://www.unicode.org/Public/15.1.0/ucd/emoji/emoji-data.txt",
}
GRAPHEME_CPP = Path(__file__).resolve().parent.parent / "src" / "cli" / "grapheme.cpp"

# EastAsianWidth.txt 头注释规定的"未赋值码位默认 W"段(规范的一部分,不是
# 本脚本的私货;平面 2/3 整面与 CJK 三段)。
EAW_DEFAULT_WIDE = [(0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF),
                    (0x20000, 0x2FFFD), (0x30000, 0x3FFFD)]

RANGE_RE = re.compile(r"^([0-9A-Fa-f]{4,6})(?:\.\.([0-9A-Fa-f]{4,6}))?\s*;\s*([A-Za-z_]+)")


def fetch(name: str, data_dir: Path) -> str:
    path = data_dir / name
    if path.exists():
        return path.read_text(encoding="utf-8")
    text = urllib.request.urlopen(SOURCES[name], timeout=60).read().decode("utf-8")
    path.write_text(text, encoding="utf-8")
    return text


def parse_property_ranges(text: str, wanted: str) -> list[tuple[int, int]]:
    out = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].rstrip()
        if not line:
            continue
        m = RANGE_RE.match(line.strip())
        if not m or m.group(3) != wanted:
            continue
        lo = int(m.group(1), 16)
        hi = int(m.group(2), 16) if m.group(2) else lo
        out.append((lo, hi))
    return out


def merge(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[tuple[int, int]] = []
    for lo, hi in sorted(ranges):
        if merged and lo <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
        else:
            merged.append((lo, hi))
    return merged


def parse_cpp_table(source: str, name: str) -> list[tuple[int, int]]:
    body = re.search(r"k%s\[\]\s*=\s*\{(.*?)\};" % name, source, re.S)
    if body is None:
        return []
    return [(int(a, 16), int(b, 16)) for a, b in
            re.findall(r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\}", body.group(1))]


def format_cpp(ranges: list[tuple[int, int]]) -> list[str]:
    return ["    {0x%04X, 0x%04X}," % (lo, hi) for lo, hi in ranges]


def build_tables(data_dir: Path) -> tuple[list[tuple[int, int]], list[tuple[int, int]]]:
    extend = parse_property_ranges(fetch("GraphemeBreakProperty.txt", data_dir), "Extend")
    wide = parse_property_ranges(fetch("EastAsianWidth.txt", data_dir), "W")
    wide += parse_property_ranges(fetch("EastAsianWidth.txt", data_dir), "F")
    wide += EAW_DEFAULT_WIDE
    wide += parse_property_ranges(fetch("emoji-data.txt", data_dir), "Emoji_Presentation")
    return merge(extend), merge(wide)


def subtract(ranges: list[tuple[int, int]], minus: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """码位集合差:ranges 覆盖的码位去掉 minus 覆盖的码位,剩段合并输出。"""
    out = []
    for lo, hi in sorted(ranges):
        cur = lo
        for mlo, mhi in minus:
            if mhi < cur or mlo > hi:
                continue
            if mlo > cur:
                out.append((cur, mlo - 1))
            cur = max(cur, mhi + 1)
        if cur <= hi:
            out.append((cur, hi))
    return merge(out)


def diff_tables(old: list[tuple[int, int]], new: list[tuple[int, int]]) -> list[str]:
    lines = []
    for lo, hi in subtract(old, new):
        lines.append("  人工表独有(生成表无): U+%04X..U+%04X" % (lo, hi))
    for lo, hi in subtract(new, old):
        lines.append("  生成表新增(人工表无): U+%04X..U+%04X" % (lo, hi))
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="重写 grapheme.cpp 的两张表")
    parser.add_argument("--data-dir", default=None,
                        help="UCD 文件本地缓存目录(默认用系统临时目录,不落仓库)")
    args = parser.parse_args()

    data_dir = Path(args.data_dir) if args.data_dir else Path(tempfile.gettempdir()) / "lubancode-ucd-15.1.0"
    data_dir.mkdir(parents=True, exist_ok=True)

    extend, wide = build_tables(data_dir)
    source = GRAPHEME_CPP.read_text(encoding="utf-8")

    print("生成表: kExtendRanges %d 条, kWideRanges %d 条 (UCD %s)" % (len(extend), len(wide), UCD_VERSION))
    old_extend = parse_cpp_table(source, "ExtendRanges")
    old_wide = parse_cpp_table(source, "WideRanges")
    print("现  表: kExtendRanges %d 条, kWideRanges %d 条" % (len(old_extend), len(old_wide)))

    print("\n[表一 GCB=Extend 差异]")
    for line in diff_tables(old_extend, extend):
        print(line)
    print("\n[表二 宽字差异]")
    for line in diff_tables(old_wide, wide):
        print(line)

    if not args.write:
        print("\n(只读对账;加 --write 重写 grapheme.cpp)")
        return 0

    today = datetime.date.today().isoformat()
    extend_header = (
        "// ---- 表一:UAX#29 Grapheme_Cluster_Break=Extend 全量区间 -------------------\n"
        "// 机器生成,勿手改:scripts/gen_unicode_tables.py --write 重生成。来源\n"
        "// UCD %s auxiliary/GraphemeBreakProperty.txt 的 Extend(含变体选择符、\n"
        "// ZWNJ 与 emoji 肤色修饰),生成日期 %s;区间升序、互不重叠、相邻已\n"
        "// 合并;static_assert 把关沿用。\n"
        % (UCD_VERSION, today))
    wide_header = (
        "// ---- 表二:终端宽字(East_Asian_Width W/F ∪ Emoji_Presentation) ----------\n"
        "// 机器生成,勿手改:scripts/gen_unicode_tables.py --write 重生成。来源\n"
        "// UCD %s EastAsianWidth.txt(W/F,含头注释规定的未赋值码位默认 W 段)\n"
        "// 与 emoji-data.txt(Emoji_Presentation),生成日期 %s;区间升序、互不\n"
        "// 重叠、相邻已合并;static_assert 把关沿用。\n"
        % (UCD_VERSION, today))

    def replace_section(source: str, marker: str, array_name: str, header: str,
                        ranges: list[tuple[int, int]]) -> str:
        pattern = re.compile(
            r"(// ---- 表%s:.*?)(constexpr std::pair<char32_t, char32_t> k%s\[\] = \{).*?(\};)" % (marker, array_name),
            re.S)
        m = pattern.search(source)
        if m is None:
            raise SystemExit("grapheme.cpp 里找不到表%s(k%s)的表段" % (marker, array_name))
        body = "\n".join(format_cpp(ranges))
        replacement = header + m.group(2) + "\n" + body + "\n" + m.group(3)
        return source[:m.start()] + replacement + source[m.end():]

    source = replace_section(source, "一", "ExtendRanges", extend_header, extend)
    source = replace_section(source, "二", "WideRanges", wide_header, wide)
    GRAPHEME_CPP.write_text(source, encoding="utf-8")
    print("\n已重写 %s(表头注明 UCD %s 与生成日期 %s)" % (GRAPHEME_CPP, UCD_VERSION, today))
    return 0


if __name__ == "__main__":
    sys.exit(main())
