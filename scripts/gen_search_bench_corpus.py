#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SearchTool 内置 ripgrep 后端迁移单 P0-7:大树基准语料生成器。

产两样东西(确定性:固定种子,同参数必产同树):
  large  10 万文件的目录树(默认 40 顶目录 x 25 子目录 x 95 文件 + 5000 文件
         的 ignored 生成树),文件内容混 C++ 风格代码与英文散文,埋四枚探针:
           bench_needle_common        约 30% 文件各 1~3 处(纯字面量查询)
           std::<名><                 约半数文件(普通正则查询)
           the                        几乎每行(高频命中,触发 100 条截断)
           definitely_absent_zzz_token_12345  全树不存在(无命中查询)
         ignored 生成树 gen_ignored/ 写进 .ignore 与 .gitignore(rg 的 ignore
         引擎照吃;不带 .git 仓库上下文也生效,.gitignore 是补充),树内文件
         埋 needle_only_in_ignored——它只存在于被忽略的树里,从语料根搜它
         必须零命中:这是"walker 真跳过 ignored 树"的活探针。
  small_file  单个 ~4000 行文件(单文件档,看进程启动固定成本)。

语料必须放在任何 git 仓库之外:rg 尊重父仓的 .gitignore,放仓库 ignored
路径下(比如 build/)会被整树忽略,基准就成了空转。manifest 一并落盘,
驱动与报告按 manifest 对账文件数。

幂等:目标已有 corpus_manifest.json 且参数一致即跳过;--force 重造。

用法:
  python scripts/gen_search_bench_corpus.py --target D:/lubancode_bench_p07/corpus [--force]
"""

import argparse
import json
import random
import sys
from pathlib import Path

SEED = 20260901

# 布局常量(P0-7 大树档:95k 正常 + 5k ignored = 100k)
TOP_DIRS = 40
SUB_DIRS = 25
FILES_PER_SUBDIR = 95
IGNORED_FILES = 5000

WORDS = (
    "the of and to in is it that this with for as was on are by he she they "
    "we you not but all can will one there their which from have has had "
    "search walker stream pattern policy request runner backend engine limit "
    "window linux macos platform process thread memory handle cancel timeout"
).split()

CPP_TYPES = (
    "vector", "string", "optional", "expected", "unique_ptr", "shared_ptr",
    "map", "unordered_map", "function", "atomic", "string_view", "span",
    "chrono", "filesystem", "thread", "mutex", "variant", "pair", "array",
)


def make_file_content(rng: random.Random, idx: int, *, put_common: bool,
                      put_regex: bool) -> str:
    """确定性伪随机内容:散文行为主,按探针概率埋字面量与模板记号。"""
    lines = [f"// bench corpus file {idx:07d}"]
    n_lines = rng.randint(30, 60)
    for _ in range(n_lines):
        n_words = rng.randint(8, 16)
        words = [rng.choice(WORDS) for _ in range(n_words)]
        line = " ".join(words)
        # 高频命中探针:绝大多数行都含 "the"(选词表里它出现概率本就最高,
        # 再显式保证一句),100 条截断在头几个文件就该触发。
        if rng.random() < 0.85:
            line += " the end"
        lines.append(line + ".")
    if put_regex:
        for _ in range(rng.randint(1, 3)):
            lines.append(
                f"    std::{rng.choice(CPP_TYPES)}<{rng.choice(CPP_TYPES)}> v{rng.randint(0, 999)};")
    if put_common:
        for _ in range(rng.randint(1, 3)):
            lines.append(f"    // bench_needle_common occurrence {rng.randint(0, 9999)}")
    lines.append("")  # 尾换行
    return "\n".join(lines)


def write_file(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def gen_large(root: Path, rng: random.Random) -> dict:
    counts = {"tree_files": 0, "ignored_files": 0, "with_common": 0,
              "with_regex": 0, "bytes": 0}
    exts = [".cpp"] * 4 + [".hpp"] * 2 + [".txt"] * 2 + [".md"] * 2
    idx = 0
    for d in range(TOP_DIRS):
        for s in range(SUB_DIRS):
            subdir = root / "tree" / f"d{d:02d}" / f"s{s:02d}"
            for f in range(FILES_PER_SUBDIR):
                put_common = rng.random() < 0.30
                put_regex = rng.random() < 0.50
                content = make_file_content(rng, idx, put_common=put_common,
                                            put_regex=put_regex)
                write_file(subdir / f"f_{f:03d}{rng.choice(exts)}", content)
                counts["tree_files"] += 1
                counts["bytes"] += len(content.encode("utf-8"))
                if put_common:
                    counts["with_common"] += 1
                if put_regex:
                    counts["with_regex"] += 1
                idx += 1
                if idx % 10000 == 0:
                    print(f"  ... {idx} files", flush=True)
    # ignored 生成树:文件名/正文带 needle_only_in_ignored,从根搜必须零命中。
    per_dir = IGNORED_FILES // TOP_DIRS
    for d in range(TOP_DIRS):
        subdir = root / "gen_ignored" / f"g{d:02d}"
        for f in range(per_dir):
            content = (
                f"// generated artifact {d:02d}/{f:03d}\n"
                "needle_only_in_ignored machine output line the the the\n" * 20
            )
            write_file(subdir / f"g_{f:03d}.gen", content)
            counts["ignored_files"] += 1
            counts["bytes"] += len(content.encode("utf-8"))
    # ignore 规则:.ignore 无条件生效(不依赖 git 仓库上下文),.gitignore 补一份。
    write_file(root / ".ignore", "gen_ignored/\n")
    write_file(root / ".gitignore", "gen_ignored/\n")
    return counts


def gen_small_file(root: Path, rng: random.Random) -> dict:
    lines = ["// bench corpus small single file"]
    for i in range(4000):
        words = [rng.choice(WORDS) for _ in range(12)]
        line = " ".join(words)
        if rng.random() < 0.85:
            line += " the end"
        lines.append(f"{i:04d}: {line}.")
        if rng.random() < 0.05:
            lines.append(f"    std::{rng.choice(CPP_TYPES)}<{rng.choice(CPP_TYPES)}> s{i};")
        if rng.random() < 0.03:
            lines.append(f"    // bench_needle_common occurrence {i}")
    content = "\n".join(lines) + "\n"
    path = root / "small_single_file.cpp"
    write_file(path, content)
    return {"path": str(path), "lines": len(lines),
            "bytes": len(content.encode("utf-8"))}


def main() -> int:
    ap = argparse.ArgumentParser(description="P0-7 基准语料生成器(确定性)")
    ap.add_argument("--target", required=True, help="语料根目录(须在 git 仓库外)")
    ap.add_argument("--force", action="store_true", help="已有 manifest 也重造")
    args = ap.parse_args()

    root = Path(args.target)
    manifest_path = root / "corpus_manifest.json"
    manifest = {
        "seed": SEED,
        "layout": {"top_dirs": TOP_DIRS, "sub_dirs": SUB_DIRS,
                   "files_per_subdir": FILES_PER_SUBDIR,
                   "ignored_files": IGNORED_FILES},
        "needles": {
            "literal_common": "bench_needle_common",
            "regex_moderate": "std::[A-Za-z_]+<",
            "no_match": "definitely_absent_zzz_token_12345",
            "high_frequency": "the",
            "glob_enum": "*.cpp",
            "only_in_ignored": "needle_only_in_ignored",
        },
    }
    if manifest_path.exists() and not args.force:
        old = json.loads(manifest_path.read_text(encoding="utf-8"))
        if old.get("seed") == SEED and old.get("layout") == manifest["layout"]:
            print(f"语料已在且参数一致,跳过: {root}")
            return 0
        print("参数变了,重造。", file=sys.stderr)

    root.mkdir(parents=True, exist_ok=True)
    print(f"生成 large 大树(约 {TOP_DIRS * SUB_DIRS * FILES_PER_SUBDIR + IGNORED_FILES} 文件)...")
    rng = random.Random(SEED)
    counts = gen_large(root / "large", rng)
    print("生成 small 单文件...")
    small = gen_small_file(root, rng)

    manifest["large"] = counts
    manifest["total_files"] = counts["tree_files"] + counts["ignored_files"]
    manifest["small_file"] = small
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"完:large {counts['tree_files']}+{counts['ignored_files']}(ignored) 文件,"
          f"共 {counts['bytes'] / 1e6:.0f} MB;manifest 落 {manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
