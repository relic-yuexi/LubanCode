#!/usr/bin/env python3
"""记忆写入调度单 P0:现行 every-turn 抽取触发率与成本账的离线测量。

只读本地 trajectory 流(<workspace>/sessions/<session>/main.jsonl),不发
一个网络请求、不碰任何钥匙。口径(单子 §2.5/§10.3):

  outer_user_turns        分母:turn.started 且 payload.trigger=external_user
                           (slash 命令在这层之上已分流,不进回合账)
  turns_with_output       现行门重放的分子:该回合内 >=1 枚
                           model.output.completed(history 增长的保守代理)
  turns_with_request      该回合内 >=1 枚 model.request.sent(更宽的代理,
                           含失败回合)
  extraction_calls        真实 memory_extract 请求数(model.request.prepared
                           的 payload.purpose == "memory_extract")
  extraction_tokens       上述请求的 usage 账(v2 model.usage.recorded 按
                           request_id 关联;v1 completed.payload.usage 兜底)
  write_requested         memory.save.requested 事件数(写路因果边)

产出三个数:
  calls_per_100_user_turns(实测)      = 100 * extraction_calls / outer_user_turns
  calls_per_100_user_turns(门重放)    = 100 * turns_with_output / outer_user_turns
                                        ——现行唯一判据是"history 有增长",
                                          门若开着会叫多少;这是投影,不是实测
  tokens_per_accepted_memory          ——分子 = extraction_tokens,分母 = 真正
                                        被接受入库的记忆。两者任一为零即"尚未
                                        测得",不许拿离线评测的数顶上(§2.5)。

顺带一份 §7.3 初门槛的 shadow 投影(cjk>=8 或 latin 词>=3),只作 P1 的
参考量,不当验收数。

用法:
  python eval/memory_gate/measure_current_rate.py <workspaces 根> [workspace 名 ...]
  不点名 workspace 就全量扫。
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class SessionStats:
    session_id: str = ""
    outer_user_turns: int = 0
    turns_with_output: int = 0
    turns_with_request: int = 0
    extraction_calls: int = 0
    extraction_input_tokens: int = 0
    extraction_output_tokens: int = 0
    extraction_cached_tokens: int = 0
    write_requested: int = 0
    gate_pass_turns: int = 0  # §7.3 初门槛 shadow 投影


@dataclass
class WorkspaceStats:
    sessions: int = 0
    first_session: str = ""
    last_session: str = ""
    totals: SessionStats = field(default_factory=SessionStats)


def is_cjk(cp: int) -> bool:
    return (
        0x4E00 <= cp <= 0x9FFF
        or 0x3400 <= cp <= 0x4DBF
        or 0xF900 <= cp <= 0xFAFF
    )


def text_stats(text: str):
    cjk = sum(1 for ch in text if is_cjk(ord(ch)))
    latin_words = 0
    in_word = False
    for ch in text:
        if ch.isascii() and (ch.isalnum()):
            if not in_word:
                in_word = True
                latin_words += 1
        else:
            in_word = False
    return cjk, latin_words


def scan_stream(path: Path) -> SessionStats:
    stats = SessionStats(session_id=path.parent.name)
    outer_turns: set[str] = set()
    turns_with_output: set[str] = set()
    turns_with_request: set[str] = set()
    turn_user_text: dict[str, str] = {}
    extract_requests: dict[str, bool] = {}  # request_id -> is memory_extract
    extract_request_turn: dict[str, str] = {}
    usage_seen: set[str] = set()

    with path.open("r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            kind = event.get("kind", "")
            payload = event.get("payload", {})
            turn_id = event.get("turn_id")

            if kind == "turn.started" and payload.get("trigger") == "external_user":
                outer_turns.add(turn_id)
                continue
            if turn_id is not None and turn_id in outer_turns:
                if kind == "model.output.completed":
                    turns_with_output.add(turn_id)
                elif kind == "model.request.sent":
                    turns_with_request.add(turn_id)
                elif kind == "input.received":
                    parts = [
                        block.get("text", "")
                        for block in payload.get("content", [])
                        if isinstance(block, dict)
                    ]
                    turn_user_text[turn_id] = turn_user_text.get(turn_id, "") + "".join(parts)

            if kind == "model.request.prepared":
                request_id = event.get("request_id")
                if payload.get("purpose") == "memory_extract" and request_id:
                    extract_requests[request_id] = True
                    if turn_id:
                        extract_request_turn[request_id] = turn_id
                continue
            if kind == "model.usage.recorded":
                request_id = event.get("request_id")
                if request_id in extract_requests and request_id not in usage_seen:
                    usage_seen.add(request_id)
                    if payload.get("reported_by_provider"):
                        stats.extraction_input_tokens += (
                            payload.get("input_tokens", 0)
                            + payload.get("cache_read_tokens", 0)
                            + payload.get("cache_creation_tokens", 0)
                        )
                        stats.extraction_output_tokens += payload.get("output_tokens", 0)
                        stats.extraction_cached_tokens += (
                            payload.get("cache_read_tokens", 0)
                            + payload.get("cache_creation_tokens", 0)
                        )
                continue
            if kind == "model.output.completed":
                # v1 legacy:usage 挂在 completed 上;没有 v2 usage 事件时兜底。
                request_id = event.get("request_id")
                usage = payload.get("usage")
                if (
                    request_id in extract_requests
                    and request_id not in usage_seen
                    and isinstance(usage, dict)
                ):
                    usage_seen.add(request_id)
                    stats.extraction_input_tokens += (
                        usage.get("input_tokens", 0)
                        + usage.get("cache_read_input_tokens", 0)
                        + usage.get("cache_creation_input_tokens", 0)
                    )
                    stats.extraction_output_tokens += usage.get("output_tokens", 0)
                    stats.extraction_cached_tokens += (
                        usage.get("cache_read_input_tokens", 0)
                        + usage.get("cache_creation_input_tokens", 0)
                    )
                continue
            if kind == "memory.save.requested":
                stats.write_requested += 1

    stats.outer_user_turns = len(outer_turns)
    stats.turns_with_output = len(turns_with_output)
    stats.turns_with_request = len(turns_with_request)
    stats.extraction_calls = len(extract_requests)
    for turn in outer_turns:
        cjk, latin = text_stats(turn_user_text.get(turn, ""))
        if cjk >= 8 or latin >= 3:
            stats.gate_pass_turns += 1
    return stats


def add_into(target: SessionStats, source: SessionStats) -> None:
    target.outer_user_turns += source.outer_user_turns
    target.turns_with_output += source.turns_with_output
    target.turns_with_request += source.turns_with_request
    target.extraction_calls += source.extraction_calls
    target.extraction_input_tokens += source.extraction_input_tokens
    target.extraction_output_tokens += source.extraction_output_tokens
    target.extraction_cached_tokens += source.extraction_cached_tokens
    target.write_requested += source.write_requested
    target.gate_pass_turns += source.gate_pass_turns


def per_100(calls: int, turns: int) -> str:
    if turns == 0:
        return "n/a(0 回合)"
    return f"{100.0 * calls / turns:.1f}"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = Path(sys.argv[1])
    wanted = set(sys.argv[2:])

    workspaces: dict[str, WorkspaceStats] = {}
    streams = sorted(root.glob("*/sessions/*/main.jsonl"))
    for stream in streams:
        workspace = stream.relative_to(root).parts[0]
        if wanted and workspace not in wanted:
            continue
        stats = scan_stream(stream)
        book = workspaces.setdefault(workspace, WorkspaceStats())
        book.sessions += 1
        name = stream.parent.name
        if not book.first_session or name < book.first_session:
            book.first_session = name
        if name > book.last_session:
            book.last_session = name
        add_into(book.totals, stats)

    if not workspaces:
        print("没有可读的 main.jsonl")
        return 1

    grand = SessionStats(session_id="(all)")
    for name in sorted(workspaces):
        book = workspaces[name]
        add_into(grand, book.totals)
        print(f"== {name}: {book.sessions} 场 ({book.first_session} .. {book.last_session})")
        t = book.totals
        print(
            f"   外层用户回合 {t.outer_user_turns} | 有产出回合 {t.turns_with_output} |"
            f" 发过请求回合 {t.turns_with_request} | 真抽取调用 {t.extraction_calls} |"
            f" 写路事件 {t.write_requested}"
        )

    print("\n---- 汇总(点名 workspace 的就是点名部分;否则全量) ----")
    print(f"外层用户回合            {grand.outer_user_turns}")
    print(f"有产出回合(门重放分子){grand.turns_with_output}")
    print(f"真抽取调用              {grand.extraction_calls}")
    print(
        f"真抽取 Token            input={grand.extraction_input_tokens}"
        f" output={grand.extraction_output_tokens} cached={grand.extraction_cached_tokens}"
    )
    print(f"写路事件                {grand.write_requested}")
    print()
    print(f"calls_per_100_user_turns(实测)   {per_100(grand.extraction_calls, grand.outer_user_turns)}")
    print(
        f"calls_per_100_user_turns(门重放) {per_100(grand.turns_with_output, grand.outer_user_turns)}"
        "  [现行门=history 增长,若 learn 开着;有产出代理]"
    )
    print(
        f"  (请求代理上界)                 {per_100(grand.turns_with_request, grand.outer_user_turns)}"
        "  [发过请求的回合;现行门连本轮回合输入也计增长,真值贴近此数]"
    )
    print(
        f"shadow 初门槛(§7.3)通过回合     {grand.gate_pass_turns}"
        f" ({per_100(grand.gate_pass_turns, grand.outer_user_turns)}/100)  [P1 参考投影]"
    )
    if grand.extraction_calls == 0 or grand.write_requested == 0:
        print("tokens_per_accepted_memory        尚未测得")
        why = []
        if grand.extraction_calls == 0:
            why.append("本地轨迹零笔 memory_extract 请求(现行默认 memory.enabled=false,真实会话没开过)")
        if grand.write_requested == 0:
            why.append("零笔 memory.save.requested(没有可作分母的接受记忆)")
        print("  缺什么: " + ";".join(why))
        print("  补法: 开着 memory(learn 非 off)与 trajectory 跑一场真实会话,")
        print("        P0 的 memory.extraction.assessed / memory.write.receipted 会把两头的账记齐。")
    else:
        accepted = grand.write_requested  # 上界代理:全部写路事件当作接受
        total = grand.extraction_input_tokens + grand.extraction_output_tokens
        print(f"tokens_per_accepted_memory(上界代理,写路事件当分母) {total / accepted:.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
