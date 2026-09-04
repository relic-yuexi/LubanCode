"""Q2 量化评测 collect 公共件(工具与上下文治理量化评测单 P0)。

四份 collect.py(tests/eval/<实验名>/collect.py)共用的:读原始 JSONL、
分桶、中位数/P95、零分母 unavailable 规则、落件(csv+md,pandas/pyarrow
齐时再出 parquet)。

记账规则(单子 §二 B):
  - 零分母写 "unavailable",不填 0——分母没有样本,商就没有意义,填 0
    是撒谎(safe_ratio/safe_pct 的返回约定:str 或 float);
  - 中位数 + P95 是标准两件;P95 用线性插值(numpy 默认同款),样本数 < 2
    时 P95 与中位数同值;
  - 空样本桶在 md 里照列,值全 unavailable——列出来是"这格没跑",不是
    "这格是 0"。

落件形态(本机 python 3.13 无 pandas/pyarrow,实测 2026-09-05):
  csv + md 双件恒出;parquet 只在 import pandas/pyarrow 双全时出。
  TODO(parquet):本机补 `pip install pandas pyarrow` 后无需改码,collect
  自动落 parquet;勿为出 parquet 硬引第三方依赖(单子 P0 明令)。

用法(各 collect.py 同一形状):
  python collect.py --raw <原始.jsonl> --out-dir <输出目录> [--out-stem 前缀]
"""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import math
import sys
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence

# "unavailable" 的落表字样——与单子 §二 B 的记账规则一字不差。
UNAVAILABLE = "unavailable"


# ---- 原始账读取 --------------------------------------------------------------


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    """读 JSONL;一行一个 JSON 对象,空行跳过,坏行带着行号报错退出。"""
    if not path.exists():
        return []
    records: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as exc:
                sys.exit(f"collect: {path}:{lineno} 不是合法 JSON: {exc}")
    return records


# ---- 统计件 ------------------------------------------------------------------


def median(values: Sequence[float]) -> float | None:
    """中位数;空序列返回 None(由调用方按 unavailable 落表)。"""
    if not values:
        return None
    ordered = sorted(values)
    mid = len(ordered) // 2
    if len(ordered) % 2 == 1:
        return float(ordered[mid])
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def p95(values: Sequence[float]) -> float | None:
    """P95,线性插值法(与 numpy.percentile 默认线性同款);空序列 None。"""
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = 0.95 * (len(ordered) - 1)
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return float(ordered[low])
    return ordered[low] + (ordered[high] - ordered[low]) * (rank - low)


def safe_ratio(numerator: float, denominator: float) -> float | str:
    """商;分母为 0/None 返回 "unavailable",绝不填 0(单子记账规则)。"""
    if denominator is None or denominator == 0:
        return UNAVAILABLE
    return numerator / denominator


def safe_pct(numerator: float, denominator: float) -> float | str:
    """百分比(0-100);零分母 unavailable。"""
    ratio = safe_ratio(numerator, denominator)
    return UNAVAILABLE if isinstance(ratio, str) else ratio * 100.0


def fmt_cell(value: Any) -> str:
    """md/csv 单元格统一格式:None→unavailable,str 原样,float 保 2 位。"""
    if value is None:
        return UNAVAILABLE
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, float):
        return f"{value:.2f}"
    return str(value)


# ---- 分桶 --------------------------------------------------------------------


def bucket(value: float, edges: Sequence[float], labels: Sequence[str] | None = None) -> str:
    """按左闭右开区间分桶:edges=[10,20] 给三桶 "<10"/"[10,20)/">=20"。
    labels 与桶数一致时用它命名;默认自动命名。None 值落 "unavailable"。"""
    if value is None:
        return UNAVAILABLE
    n = len(edges)
    names = list(labels) if labels is not None else []
    if len(names) != n + 1:
        names = [f"<{edges[0]}"] if n >= 1 else []
        for i in range(n - 1):
            names.append(f"[{edges[i]},{edges[i + 1]})")
        if n >= 1:
            names.append(f">={edges[n - 1]}")
    for i, edge in enumerate(edges):
        if value < edge:
            return names[i]
    return names[n]


def group_by(records: Iterable[dict[str, Any]], keys: Sequence[str]) -> dict[tuple, list[dict[str, Any]]]:
    """按多键分组;缺键按 "" 归组。保插入序。"""
    grouped: dict[tuple, list[dict[str, Any]]] = {}
    for record in records:
        key = tuple(str(record.get(k, "")) for k in keys)
        grouped.setdefault(key, []).append(record)
    return grouped


def numeric_column(records: Sequence[dict[str, Any]], key: str) -> list[float]:
    """取一列数值;None/缺键/非数值剔除(它们在另一本"缺数账"里)。"""
    out: list[float] = []
    for record in records:
        value = record.get(key)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            out.append(float(value))
    return out


# ---- 落件 --------------------------------------------------------------------


def _parquet_available() -> bool:
    return importlib.util.find_spec("pandas") is not None and importlib.util.find_spec(
        "pyarrow"
    ) is not None


def write_outputs(
    rows: Sequence[dict[str, Any]],
    columns: Sequence[str],
    out_dir: Path,
    stem: str,
    title: str,
    notes: Sequence[str] = (),
) -> list[Path]:
    """统一落件:csv + md 恒出,pandas/pyarrow 齐时加 parquet。
    rows 为空也落件(md 头注明"暂无原始记录"),collect 测试不因没数据红。
    返回写出的文件清单。"""
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []

    csv_path = out_dir / f"{stem}.csv"
    with csv_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(columns)
        for row in rows:
            writer.writerow([fmt_cell(row.get(col)) for col in columns])
    written.append(csv_path)

    md_path = out_dir / f"{stem}.md"
    with md_path.open("w", encoding="utf-8") as handle:
        handle.write(f"# {title}\n\n")
        for note in notes:
            handle.write(f"- {note}\n")
        if notes:
            handle.write("\n")
        if not rows:
            handle.write(
                "暂无原始记录——实验未跑或原始件缺位。此文件是装置自检产物,"
                "证明 collect 管道本身转得动(零分母规则:空桶记 unavailable 不填 0)。\n"
            )
        else:
            handle.write(f"| {' | '.join(columns)} |\n")
            handle.write(f"|{' | '.join(['---'] * len(columns))}|\n")
            for row in rows:
                handle.write(f"| {' | '.join(fmt_cell(row.get(col)) for col in columns)} |\n")
    written.append(md_path)

    if _parquet_available():
        import pandas as pd  # 延迟 import:没有 pandas 的机器不碰它

        parquet_path = out_dir / f"{stem}.parquet"
        frame = pd.DataFrame(
            [{col: row.get(col) for col in columns} for row in rows], columns=list(columns)
        )
        frame.to_parquet(parquet_path, index=False)
        written.append(parquet_path)

    return written


# ---- 脚手架 ------------------------------------------------------------------


def base_parser(description: str) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--raw", type=Path, required=True, help="原始 JSONL 账")
    parser.add_argument("--out-dir", type=Path, required=True, help="csv/md/parquet 落件目录")
    parser.add_argument("--out-stem", default=None, help="落件名前缀(缺省按实验定)")
    return parser


def report_written(written: Sequence[Path], raw: Path, record_count: int) -> int:
    for path in written:
        print(f"collect: {record_count} 条原始账({raw.name}) -> {path}")
    if record_count == 0:
        print(f"collect: 注意 {raw} 无记录,输出为空表装置自检件")
    return 0
