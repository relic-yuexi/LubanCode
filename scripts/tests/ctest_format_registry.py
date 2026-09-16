#!/usr/bin/env python3
"""CTest 册 × 会话格式环境 分账表生成器(T16/V3-ADD-02)。

合同:todos/SessionV3旧设计清理_消费迁移缺口补齐与旧接口退役.todo §九
T16 勾一/勾三;配套 CMake 侧 tests/CMakeLists.txt 的
LUBANCODE_TESTS_V3_DEFAULT_BOOKS 分账清单。分类四档与
docs/development/v3-consumer-inventory.md §二 同口径:

  default-v3     ctest 不注入格式变量(在 CMake 撤 0 清单内),环境=生产默认
  explicit-v3    册内 EnvGuard("...","1")/EnvUnset 显式自证 v3
  legacy-only    吃 ctest 注入 0 的 v2 行为断言(含册内 EnvGuard("...","0"))
  format-neutral 不经会话建场口,格式开关与本册无关

判定不以目录名代替真实所走路径:每册按三件事归证据——
  1. ctest 注入:解析 tests/CMakeLists.txt 的注册循环(撤 0 清单 membership);
  2. 册内覆盖:册源码引用 LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS 的形态
     (guard 1/guard 0/unset);
  3. 是否经建场口:TrajectorySessionLedger::Open / LaunchSession /
     SpawnSubagent(开关只在这几处被读,绕开它们的册与格式无关)。

用法:
  python scripts/tests/ctest_format_registry.py            # 打印分账表
  python scripts/tests/ctest_format_registry.py --out f.md # 表落文件
  python scripts/tests/ctest_format_registry.py --check    # CI 门:
      #  - CMake 撤 0 清单仍在、机制标记未被回退;
      #  - 清单内每册真实存在(册文件在,防笔误);
      #  - 清单内每册"册内自证格式 或 完全不经建场口"——
      #    建场却不自证的册撤 0 后会被父环境漂移,就地红。
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TESTS_DIR = REPO_ROOT / "tests"
CMAKELISTS = TESTS_DIR / "CMakeLists.txt"

FORMAT_VAR = "LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS"
V3_DEFAULT_LIST = "LUBANCODE_TESTS_V3_DEFAULT_BOOKS"

# 建场口:NewSessionV3WriteEnabled 只在 SessionManager 建场/resume/clear
# 分派处被读;这些调用点决定一册是否真的经过格式开关。
SESSION_OPENERS = (
    "TrajectorySessionLedger::Open",
    ".LaunchSession(",
    "->LaunchSession(",
    ".SpawnSubagent(",
    "->SpawnSubagent(",
)

ENV_GUARD_V3_RE = re.compile(
    r'\b\w*EnvGuard\b\s*(?:\w+\s*)?\(\s*"' + re.escape(FORMAT_VAR)
    + r'"\s*,\s*"(1|true)"')
ENV_GUARD_V2_RE = re.compile(
    r'\b\w*EnvGuard\b\s*(?:\w+\s*)?\(\s*"' + re.escape(FORMAT_VAR)
    + r'"\s*,\s*"(0|false)"')
# 册内摘变量(default_smoke 的 EnvUnset 形态;也认裸 unsetenv/_putenv 清值)。
ENV_UNSET_RE = re.compile(
    r'\b\w*EnvUnset\b\s*(?:\w+\s*)?\(\s*"' + re.escape(FORMAT_VAR) + r'"')
VAR_REF_RE = re.compile(re.escape(FORMAT_VAR))
SWITCH_PROBE_RE = re.compile(r"NewSessionV3WriteEnabled")


class Book:
    """一册测试的格式路径证据。"""

    def __init__(self, name: str, rel_path: str) -> None:
        self.name = name
        self.rel_path = rel_path
        self.guard_v3 = 0
        self.guard_v2 = 0
        self.unset = 0
        self.env_refs = 0
        self.opens_session = 0
        self.switch_probe = 0

    def classify(self, in_v3_default_list: bool) -> list[str]:
        """归分类标签(可多标签:如 explicit-v3 + legacy 对照)。"""
        tags: list[str] = []
        if in_v3_default_list:
            tags.append("default-v3")
        if self.guard_v3 > 0 or self.unset > 0:
            tags.append("explicit-v3")
        if self.guard_v2 > 0:
            tags.append("legacy-only")
        # 册内无任何 v3 自证、但经建场口 → 吃注入 0 走 v2 形状。
        if not tags and self.opens_session > 0:
            tags.append("legacy-only")
        if not tags:
            tags.append("format-neutral")
        return tags

    def evidence(self) -> str:
        parts = []
        if self.guard_v3:
            parts.append(f'guard1×{self.guard_v3}')
        if self.guard_v2:
            parts.append(f'guard0×{self.guard_v2}')
        if self.unset:
            parts.append(f'unset×{self.unset}')
        if self.opens_session:
            parts.append(f'建场×{self.opens_session}')
        if self.switch_probe:
            parts.append(f'开关直检×{self.switch_probe}')
        if self.env_refs and not (self.guard_v3 or self.guard_v2 or self.unset):
            parts.append(f'变量引用×{self.env_refs}(未归档形态)')
        return ",".join(parts) if parts else "无格式证据"


def scan_cmake() -> tuple[set[str], str]:
    """读 tests/CMakeLists.txt:返回(撤 0 清单册名集, 原文)。

    逐行状态机解析,不用跨行正则——清单注释里的半角括号会截断非贪婪
    匹配(与 CMake 分号坑同教训:含标点正文的 CMake 块按行处理)。
    """
    text = CMAKELISTS.read_text(encoding="utf-8")
    books: set[str] = set()
    collecting = False
    for raw in text.splitlines():
        if not collecting:
            if raw.strip().startswith(f"set({V3_DEFAULT_LIST}"):
                collecting = True
            continue
        stripped = raw.split("#", 1)[0].strip()
        # 收尾括号可能贴在最后一个词上(如 "...v3_default_smoke)"):
        # 先摘尾括号收词,再结束收集。
        done = stripped == ")" or stripped.endswith(")")
        if done:
            stripped = stripped.rstrip(")").strip()
        for word in stripped.split():
            if re.fullmatch(r"(unit|integration)\.[A-Za-z0-9_]+\.[A-Za-z0-9_]+", word):
                books.add(word)
        if done:
            collecting = False
    return books, text


def scan_books() -> list[Book]:
    """扫全部册源码,归每册证据。"""
    books: list[Book] = []
    pattern = re.compile(r"^(unit|integration)/([^/]+)/test_([A-Za-z0-9_]+)\.cpp$")
    sources = sorted(TESTS_DIR.rglob("test_*.cpp"))
    if not sources:
        raise SystemExit(f"未找到册源码: {TESTS_DIR}")
    for source in sources:
        rel = source.relative_to(TESTS_DIR).as_posix()
        match = pattern.match(rel)
        if not match:
            continue  # manual/ 等不入 CTest 注册循环的源码
        book = Book(f"{match.group(1)}.{match.group(2)}.{match.group(3)}", rel)
        text = source.read_text(encoding="utf-8", errors="replace")
        book.guard_v3 = len(ENV_GUARD_V3_RE.findall(text))
        book.guard_v2 = len(ENV_GUARD_V2_RE.findall(text))
        book.unset = len(ENV_UNSET_RE.findall(text))
        book.env_refs = len(VAR_REF_RE.findall(text))
        book.switch_probe = len(SWITCH_PROBE_RE.findall(text))
        for opener in SESSION_OPENERS:
            book.opens_session += text.count(opener)
        books.append(book)
    return books


def render_table(books: list[Book], v3_default: set[str]) -> str:
    lines = [
        "# CTest 册 × 会话格式环境 分账表(T16 机器产出)",
        "",
        f"- 册总数: {len(books)};ctest 撤 0 清单内(default-v3 环境): "
        f"{len(v3_default)} 册。",
        "- 生成: scripts/tests/ctest_format_registry.py(只读源码,不 build)。",
        "- 判定法与四档口径见 docs/development/v3-consumer-inventory.md §二。",
        "",
        "| 册 | ctest 环境 | 册内证据 | 分类 |",
        "| --- | --- | --- | --- |",
    ]
    for book in sorted(books, key=lambda b: b.name):
        env = "未注入(生产默认)" if book.name in v3_default else "钉 0"
        lines.append(
            f"| `{book.name}` | {env} | {book.evidence()} | "
            f"{' + '.join(book.classify(book.name in v3_default))} |")
    # 反向对账:清单里指名却不存在册文件的条目。
    known = {book.name for book in books}
    ghosts = sorted(v3_default - known)
    if ghosts:
        lines.append("")
        lines.append("**清单笔误(册不存在)**: " + ", ".join(f"`{g}`" for g in ghosts))
    return "\n".join(lines) + "\n"


def run_check(books: list[Book], v3_default: set[str], cmake_text: str) -> int:
    """CI 门:见模块 docstring 的三条断言。"""
    errors: list[str] = []
    if not v3_default:
        errors.append(
            f"tests/CMakeLists.txt 里 {V3_DEFAULT_LIST} 为空——分账机制被回退"
            "(撤 0 进度至少保留已迁域清单;B4 退役时应连机制与脚本一并删)")
    if "LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS=0" not in cmake_text:
        errors.append("注册循环的全局 0 注入语句不见了——分账分支结构被改坏")
    known = {book.name for book in books}
    for name in sorted(v3_default - known):
        errors.append(f"撤 0 清单里的册不存在: {name}")
    for book in books:
        if book.name not in v3_default:
            continue
        # 撤 0 门:册内自证(guard/unset/开关直检)或完全不经建场口。
        self_pinned = (book.guard_v3 > 0 or book.unset > 0
                       or book.switch_probe > 0)
        if book.opens_session > 0 and not self_pinned:
            errors.append(
                f"{book.name}: 经建场口(×{book.opens_session})但册内无 v3 自证"
                f"({book.evidence()})——撤 0 后该册会被父环境漂移,"
                "先在册内归档格式或移出清单")
    if errors:
        for error in errors:
            print(f"ctest_format_registry: {error}", file=sys.stderr)
        return 1
    pinned = sum(1 for b in books if b.name in v3_default)
    print(f"ctest_format_registry: OK——{pinned} 册撤 0 均自证格式或不经建场口")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="CTest 册 × 会话格式环境分账表(T16)")
    parser.add_argument("--out", type=Path, help="分账表落此文件(markdown)")
    parser.add_argument("--check", action="store_true",
                        help="CI 门:清单存在性 + 撤 0 册自证核验")
    args = parser.parse_args()

    v3_default, cmake_text = scan_cmake()
    books = scan_books()

    if args.check:
        return run_check(books, v3_default, cmake_text)

    table = render_table(books, v3_default)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(table, encoding="utf-8")
        print(f"分账表已写 {args.out}({len(books)} 册)")
    else:
        sys.stdout.write(table)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
