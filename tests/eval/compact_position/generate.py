"""实验 B1(compact 位置探针)造稿器:长上下文底稿 + 分层 needle 金账。

Q2 单 §二 B1(2026-09-05 工头令改判卷三铁律):
  1) 问答驱动判卷——needle 答案是短规范值(名字-号码形),判卷只看模型
     答没答对;造稿器把每 needle 的"问题"一并写进金账,判卷口径全在账上;
  2) 触发条件贴生产——热冷区切分与阈值全用产品默认,造稿只管"事实",
     不掺任何处理侧的东西;
  3) needle 分层——金账带 layer 字段:contract(合同类:用户约定/偏好,
     落 user 消息,按产品设计该进 manifest 逐字收编)与 evidence(证据类:
     工具输出里的事实,落 read_file 结果段,本分是被摘要)。两类各半,
     "设计如此"与"意外丢失"不许混锅。

底稿形状(驱动 eval_driver.cpp 按此拼 api::Message 历史):
  - 段(segment)= 一次 read_file 的结果正文;tool_use_id/path 逐段唯一,
    冲突类 needle 的新旧两版刻意共用同一 path(同键不同 hash)——正好踩
    microcompact 折叠路的"文件改版"分支(NewVersion),与产品机制同形。
  - 长度三档:short(<2048B)/ medium(2048-8192B)/ long(>8192B,默认
    分布 30%/45%/25%)。long 段超过产品默认 long_result_bytes=8192,
    microcompact 折叠时会被换成头尾各 256B 的 artifact 预览——needle 放
    段中段(插入带 25%-75%,long 段再保证离头尾各 ≥384B),折掉就是真丢。
  - 探针两类 × 层两类 = 每档四枚 needle:
      evidence/recall     直接召回(段中一句事实,答值即对);
      evidence/conflict   更新冲突(同一配置项旧值在前、新值在后、同 path
                           再读;答新值=hit,答旧值=stale,答不出=lost);
      contract/recall     用户约定(user 消息正文里一句"定死为 X");
      contract/conflict   用户改约(旧约在前 user 消息、新约在后 user 消息)。
  - user 消息(user_turns):开工 1 条 + 每 4 段续读 1 条,正文由造稿器
    出(默认语 + 嵌进去的 contract needle 句);contract needle 的位置档
    按 user 消息在会话里的序位(位置轴与 evidence 同口径)。
  - 中英两形:zh/en 两套底稿,同一 fact_id 两个措辞形,防判卷吃措辞。

金账 needle_gold.jsonl 一行一 needle:fact_id / layer / probe_kind /
carrier(tool_result|user_turn)/ 位置档(designed)/ 实际位置档(actual)/
措辞形 / 问题 question / 期望答案 / 旧值 / source 引用(段:seg_index、
path、tool_use_id、段内偏移;user 消息:slot、old_slot)/ 段长档 / seed。
底稿落 results/drafts/(gitignored,可由 seed 复现)。

生成器自带断言(每次都跑,--self-check 另跑一轮小矩阵):
  1. 期望值唯一性:每个 value 在本底稿全文(段 + user 消息)出现次数恰为
     期望值(recall=1;conflict 新值=1,旧值=2——旧声明一次+新声明里提一次),
     绝不出现在别的段、别的消息或别的 needle 里;
  2. evidence needle 落段中段(插入带 25%-75%),long 段离头尾各 ≥384B;
  3. conflict 旧声明严格在新声明之前(段:old_seg_index < seg_index;
     user 消息:old_slot < slot);
  4. 位置档表全覆盖,一档不缺;
  5. 同 seed 两次生成逐字节一致(确定性,sha256 对账)。

用法:
  python generate.py [--segments 96] [--repeats 5] [--langs zh,en]
      [--positions 0,5,...,95] [--needles-per-position 1]
      [--seed-base 20260904] [--long-result-bytes 8192] [--out-dir results/]
  python generate.py --self-check        # 小矩阵自检 + doctest
"""

from __future__ import annotations

import argparse
import doctest
import hashlib
import json
import random
import sys
from pathlib import Path

DEFAULT_POSITIONS = list(range(0, 100, 5))  # 20 档(单子 §二 B1)
DEFAULT_LANGS = ["zh", "en"]
DEFAULT_SEGMENTS = 96
DEFAULT_REPEATS = 5
DEFAULT_NEEDLES_PER_POSITION = 1
DEFAULT_SEED_BASE = 20260904
DEFAULT_LONG_RESULT_BYTES = 8192  # 与产品 StructuralCompressionOptions 默认同值

# 长度档目标(bytes)与分档边界:short/medium 都在产品折叠闸
# (long_result_bytes)之下——这两档 microcompact 一律全文;long 档必超闸,
# 折叠换成头尾 256B 预览。SHORT_MAX 留了行溢出 + 最多 3 枚 needle 句的
# 余量,保证"目标档 == 插完 needle 后的实际档"两侧判定不打架。
SHORT_MAX = 1280
CLASS_BOUNDS = {"short": (380, 760), "medium": (1400, 2600), "long": (8992, 10792)}
CLASS_DISTRIBUTION = [("short", 0.30), ("medium", 0.45), ("long", 0.25)]

# 事实值前缀与号码:四族号码区间互不重叠(evidence recall 1009-2738 /
# contract recall 4400-5293 / evidence conflict 旧 3100-4107 新 6100-7373 /
# contract conflict 旧 7600-8417 新 8600-9721),填充材料里的数字一律
# ≤999 或带小数点,从根上杜绝串号。
RECALL_PREFIX = {"zh": "青梧", "en": "Qingwu"}
CONFLICT_PREFIX = {"zh": "玄序", "en": "Xuanxu"}
CONTRACT_RECALL_PREFIX = {"zh": "承影", "en": "Chengying"}
CONTRACT_CONFLICT_PREFIX = {"zh": "白鹭", "en": "Egret"}


def recall_num(step_index: int, sub: int) -> int:
    return 1009 + 91 * step_index + sub


def conflict_old_num(step_index: int) -> int:
    return 3100 + 53 * step_index


def conflict_new_num(step_index: int) -> int:
    return 6100 + 67 * step_index


def contract_recall_num(step_index: int) -> int:
    return 4400 + 47 * step_index


def contract_conflict_old_num(step_index: int) -> int:
    return 7600 + 43 * step_index


def contract_conflict_new_num(step_index: int) -> int:
    return 8600 + 59 * step_index


def recall_key(step_index: int) -> str:
    return f"runtime.knob_{step_index:02d}"


def conflict_key(step_index: int) -> str:
    return f"deploy.baseline_{step_index:02d}"


def contract_recall_key(step_index: int) -> str:
    return f"prefs.mode_{step_index:02d}"


def contract_conflict_key(step_index: int) -> str:
    return f"prefs.limit_{step_index:02d}"


# ---- 填充行(中英混排;数字全部 ≤999 或带小数点,不与事实值串号) ----------

ZLINES = [
    "构建日志:模块 {a} 编译完成,警告 {b} 条,耗时 {c} 毫秒。",
    "索引分片 {a} 的合并进度为 {b}%,累计写入 {c} KiB。",
    "依赖扫描:{a} 声明版本 {b}.{c},实际解析为 {d}.{e}。",
    "测试桩 {a} 收到 {b} 次调用,平均返回耗时 {c} 微秒。",
    "缓存层报告:键空间占用 {b}%,驱逐策略保持 LRU 不变。",
    "文档批注:第 {a} 页的图表标题与正文编号需要对齐。",
    "巡检记录:节点 {a} 的磁盘水位 {b}%,仍在安全带内。",
    "评审意见:{a} 的注释里补一句边界条件的说明,别改逻辑。",
    "回放脚本:第 {a} 号用例的期望输出与实际相差 {b} 行。",
    "装配清单:批次 {a} 缺一枚垫圈,库房已补发,编号 {b}。",
]

ELINES = [
    "build log: module {a} compiled with {b} warnings in {c} ms.",
    "index shard {a} merge at {b}%, {c} KiB written so far.",
    "dependency scan: {a} requested {b}.{c}, resolved to {d}.{e}.",
    "test stub {a} served {b} calls, mean latency {c} us.",
    "cache layer: keyspace at {b}%, eviction policy stays LRU.",
    "doc note: align the chart title on page {a} with body numbering.",
    "patrol record: node {a} disk watermark {b}%, inside the safe band.",
    "review comment: add one line about edge cases in {a}, no logic change.",
    "replay script: case {a} expected output differs by {b} lines.",
    "packing list: batch {a} missing one washer, reshipped as {b}.",
]


def _fill(template: str, rng: random.Random) -> str:
    out = template
    for slot in ("a", "b", "c", "d", "e"):
        if "{" + slot + "}" not in out:
            continue
        if slot in ("b",) and "%," in out:
            out = out.replace("{" + slot + "}", f"{rng.randrange(3, 99)}.{rng.randrange(0, 9)}")
        elif slot in ("c", "e"):
            out = out.replace("{" + slot + "}", str(rng.randrange(2, 9)))
        elif slot in ("b",):
            out = out.replace("{" + slot + "}", str(rng.randrange(0, 21)))
        else:
            out = out.replace("{" + slot + "}", f"{rng.randrange(0, 999):03d}")
    return out


def make_line(rng: random.Random) -> str:
    if rng.random() < 0.5:
        return _fill(rng.choice(ZLINES), rng)
    return _fill(rng.choice(ELINES), rng)


def byte_len(text: str) -> int:
    return len(text.encode("utf-8"))


def build_filler(rng: random.Random, target_bytes: int) -> str:
    lines: list[str] = []
    size = 0
    while size < target_bytes:
        line = make_line(rng)
        lines.append(line)
        size += byte_len(line) + 1
    return "\n".join(lines) + "\n"


# ---- needle 造句与问题 --------------------------------------------------------

RECALL_SENTENCE = {
    "zh": "【关键事实】配置项 {key} 的取值核定为 {value},后续工作以此为准。",
    "en": "[KEY FACT] The setting '{key}' has been verified as {value}; later steps must follow it.",
}
CONFLICT_OLD_SENTENCE = {
    "zh": "【配置基线】配置项 {key} 初始设定为 {value}。",
    "en": "[BASELINE] The setting '{key}' is initially set to {value}.",
}
CONFLICT_NEW_SENTENCE = {
    "zh": "【配置变更】配置项 {key} 已由 {old} 变更为 {value},以本条为准,先前取值作废。",
    "en": "[CHANGE] The setting '{key}' has been changed from {old} to {value}; this record supersedes the earlier value.",
}
CONTRACT_RECALL_SENTENCE = {
    "zh": "【用户约定】配置项 {key} 的取值,由用户定死为 {value},全程照此执行,不得偏离。",
    "en": "[USER CONTRACT] The setting '{key}' is fixed by the user at {value}; follow it throughout.",
}
CONTRACT_OLD_SENTENCE = {
    "zh": "【用户约定】配置项 {key} 的取值,用户起初定为 {value}。",
    "en": "[USER CONTRACT] The setting '{key}' was initially agreed by the user as {value}.",
}
CONTRACT_NEW_SENTENCE = {
    "zh": "【用户约定变更】配置项 {key} 的取值,用户已从 {old} 改定为 {value},以本条约定为准,先前约定作废。",
    "en": "[USER CONTRACT CHANGE] The setting '{key}' is re-agreed by the user from {old} to {value}; this supersedes the earlier agreement.",
}

# 问题模板:答案都是短规范值(判卷确定性),conflict 问"最新为准"。
QUESTIONS = {
    ("evidence", "recall"): {
        "zh": "配置项 {key} 的取值是什么?",
        "en": "What is the value of the setting '{key}'?",
    },
    ("evidence", "conflict"): {
        "zh": "配置项 {key} 当前的取值是什么(以最新变更后的为准)?",
        "en": "What is the current value of the setting '{key}' (latest change wins)?",
    },
    ("contract", "recall"): {
        "zh": "用户为配置项 {key} 定下的取值是什么?",
        "en": "What value did the user fix for the setting '{key}'?",
    },
    ("contract", "conflict"): {
        "zh": "用户对配置项 {key} 的最新约定取值是什么(以最后一次约定为准)?",
        "en": "What is the user's latest agreed value for the setting '{key}' (last agreement wins)?",
    },
}

# user 消息默认正文(开工 / 续读;contract needle 句逐行追加在其后)。
DEFAULT_OPEN_TEXT = {
    "zh": "任务:通读 docs/draft/ 下的工程文档,整理其中的关键配置项与变更记录。",
    "en": "Task: read through the engineering docs under docs/draft/ and collect key settings and changes.",
}
DEFAULT_TURN_TEXT = {
    "zh": "继续,留意配置项的变更记录。",
    "en": "Continue; watch for setting changes.",
}

# 同段多枚 needle 的插入带(段内字节百分比),错开防叠句。
INSERT_BANDS = [(0.48, 0.56), (0.30, 0.36), (0.64, 0.70)]


def insert_sentence(text: str, sentence: str, band_index: int, long_segment: bool) -> tuple[str, float]:
    """把句子按第 band_index 条插入带插进 text 的行边界前,返回(新文本,插入偏移%)。"""
    lo, hi = INSERT_BANDS[band_index % len(INSERT_BANDS)]
    total = byte_len(text)
    target = int(total * ((lo + hi) / 2.0))
    margin = 384 if long_segment else 64  # long 段须离头尾预览(256B)更远
    # 行起点(字节口径)从后往前找第一个满足双侧余量的行边界。
    best_char = -1
    best_bytes = -1
    walked = 0
    for char_index in range(len(text) + 1):
        if char_index == len(text) or text[char_index] == "\n":
            if walked >= target - 80 and walked <= target + 400:
                if walked >= margin and total - walked >= margin:
                    if best_char < 0:
                        best_char, best_bytes = char_index, walked
            walked += 1
        else:
            walked += byte_len(text[char_index])
    if best_char < 0:
        raise AssertionError("找不到满足余量的行边界插入 needle(段太短或带太挤)")
    inserted = text[:best_char] + sentence + "\n" + text[best_char:]
    offset_pct = round(best_bytes / max(1, byte_len(text)) * 100.0, 1)
    return inserted, offset_pct


def position_index(step: int, positions: list[int], n_segments: int) -> int:
    """位置档 → 段下标(确定式取整,不进银行家舍入那套)。

    >>> position_index(0, DEFAULT_POSITIONS, 96)
    0
    >>> position_index(95, DEFAULT_POSITIONS, 96)
    90
    >>> position_index(50, DEFAULT_POSITIONS, 96)
    48
    """
    return min(n_segments - 1, int(step / 100.0 * (n_segments - 1) + 0.5))


def user_slot_count(n_segments: int) -> int:
    """user 文本消息数(开工 1 + 每 4 段续读 1):contract needle 的位置轴。

    >>> user_slot_count(96)
    24
    >>> user_slot_count(24)
    6
    """
    return 1 + (n_segments - 1) // 4


def user_slot_for(step_index: int, positions: list[int], n_slots: int) -> int:
    """位置档 → user 消息序位(与 evidence 的位置轴同口径)。

    >>> user_slot_for(0, DEFAULT_POSITIONS, 24)
    0
    >>> user_slot_for(19, DEFAULT_POSITIONS, 24)
    23
    >>> user_slot_for(9, DEFAULT_POSITIONS, 24)
    11
    """
    span = max(1, len(positions) - 1)
    return min(n_slots - 1, int(step_index / span * (n_slots - 1) + 0.5))


def pick_length_class(rng: random.Random) -> str:
    roll = rng.random()
    acc = 0.0
    for name, weight in CLASS_DISTRIBUTION:
        acc += weight
        if roll < acc:
            return name
    return CLASS_DISTRIBUTION[-1][0]


def class_for_size(size: int, long_result_bytes: int) -> str:
    """字节数 → 长度档(与产品折叠闸同口径:超 long_result_bytes 才会被
    换成头尾预览)。

    >>> class_for_size(600, 8192)
    'short'
    >>> class_for_size(3000, 8192)
    'medium'
    >>> class_for_size(9000, 8192)
    'long'
    """
    if size < SHORT_MAX:
        return "short"
    if size <= long_result_bytes:
        return "medium"
    return "long"


# ---- 单份底稿 ---------------------------------------------------------------


# 措辞形 → 种子偏移:不得用 hash()(进程间随机化,破确定性)。
LANG_SEED_OFFSET = {"zh": 0, "en": 51}


def generate_draft(lang: str, repeat: int, seed: int, positions: list[int],
                   n_segments: int, needles_per_position: int,
                   long_result_bytes: int) -> dict:
    rng = random.Random(seed)
    draft_id = f"draft_{lang}_r{repeat}"
    n_slots = user_slot_count(n_segments)

    # 1) 段落骨架:逐段定长度档与填充正文。
    segments: list[dict] = []
    for i in range(n_segments):
        cls = pick_length_class(rng)
        lo, hi = CLASS_BOUNDS[cls]
        text = build_filler(rng, rng.randrange(lo, hi + 1))
        segments.append({
            "index": i,
            "tool_use_id": f"u_{i:03d}",
            "path": f"docs/draft/seg_{i:03d}.md",
            "length_class": cls,
            "text": text,
            "insertions": 0,
            "conflict_claimed": False,
        })

    # user 消息骨架(开工 + 续读;contract needle 落这里)。
    user_turns: list[dict] = []
    for k in range(n_slots):
        default_text = DEFAULT_OPEN_TEXT[lang] if k == 0 else DEFAULT_TURN_TEXT[lang]
        user_turns.append({"slot": k, "text": default_text, "insertions": 0})

    conflict_claimed: set[int] = set()

    def seg_for(idx: int, prefer_forward: bool = True, avoid_claimed: bool = False) -> dict:
        """取第 idx 段;插满(≥3 枚)就向邻段走(±3 段内)。avoid_claimed
        时先挑没被冲突对认领的段,一圈找不到再退回任意可用段——recall 存活
        尽量别跟冲突对的 NewVersion 机制缠在一起,但排不开时偶缠一两根不算错。"""
        base = [idx, idx + 1, idx - 1] if prefer_forward else [idx, idx - 1, idx + 1]
        span = base + [idx + 2, idx - 2, idx + 3, idx - 3]

        def usable(seg: dict, strict: bool) -> bool:
            if seg["insertions"] >= 3:
                return False
            return not (strict and seg["index"] in conflict_claimed)

        for strict in (avoid_claimed, False):
            for candidate in span:
                if 0 <= candidate < n_segments and usable(segments[candidate], strict):
                    return segments[candidate]
        raise AssertionError(f"第 {idx} 段附近全满,needle 没处放")

    needles: list[dict] = []

    def plant(step: int, fact_id: str, layer: str, kind: str, sentence: str, key: str,
              expected: str, old_value: str | None, seg: dict, old_seg: dict | None = None) -> None:
        long_seg = seg["length_class"] == "long"
        seg["text"], offset = insert_sentence(seg["text"], sentence, seg["insertions"], long_seg)
        seg["insertions"] += 1
        needles.append({
            "fact_id": fact_id,
            "layer": layer,
            "probe_kind": kind,
            "carrier": "tool_result",
            "lang": lang,
            "position_pct": step,
            "actual_position_pct": round(seg["index"] / (n_segments - 1) * 100.0, 1),
            "seg_index": seg["index"],
            "offset_pct_in_seg": offset,
            "key": key,
            "question": QUESTIONS[(layer, kind)][lang].format(key=key),
            "expected_value": expected,
            "old_value": old_value,
            "old_seg_index": old_seg["index"] if old_seg is not None else None,
            "slot": None,
            "old_slot": None,
            # path/tool_use_id/seg_length_class 在收尾统一从段上回填,
            # 免得冲突对后改 path 把先记的 recall 行带歪。
        })

    def plant_contract(step: int, fact_id: str, kind: str, sentence: str, key: str,
                       expected: str, old_value: str | None, slot: int,
                       old_slot: int | None = None) -> None:
        """contract needle:句落 user 消息正文末(独立行),判卷单元同 recall/conflict。"""
        turn = user_turns[slot]
        turn["text"] = turn["text"] + "\n" + sentence
        turn["insertions"] += 1
        needles.append({
            "fact_id": fact_id,
            "layer": "contract",
            "probe_kind": kind,
            "carrier": "user_turn",
            "lang": lang,
            "position_pct": step,
            "actual_position_pct": round(slot / max(1, n_slots - 1) * 100.0, 1),
            "seg_index": None,
            "offset_pct_in_seg": None,
            "key": key,
            "question": QUESTIONS[("contract", kind)][lang].format(key=key),
            "expected_value": expected,
            "old_value": old_value,
            "old_seg_index": None,
            "slot": slot,
            "old_slot": old_slot,
            "path": None,
            "tool_use_id": None,
            "seg_length_class": None,
        })

    # 2) evidence/conflict needle 先种(它们认领段、改 path):每位置档一枚;
    #    旧值在约 9 段之前,与新版共用同一 path 再读一次——同键不同 hash,
    #    正好踩 microcompact 折叠路的"文件改版"(NewVersion)分支。档 0 没有
    #    更早的空间:旧值落 0 段、新值挪到第 4 段,实际位置档进金账,
    #    曲线仍按设计档记。
    for step_index, step in enumerate(positions):
        new_idx = position_index(step, positions, n_segments)
        old_idx = max(0, new_idx - 9)
        if old_idx == new_idx:
            new_idx = min(n_segments - 1, old_idx + 4)
        fact_id = f"C{step_index:02d}"
        old_value = f"{CONFLICT_PREFIX[lang]}-{conflict_old_num(step_index)}"
        new_value = f"{CONFLICT_PREFIX[lang]}-{conflict_new_num(step_index)}"
        old_sentence = CONFLICT_OLD_SENTENCE[lang].format(key=conflict_key(step_index), value=old_value)
        new_sentence = CONFLICT_NEW_SENTENCE[lang].format(
            key=conflict_key(step_index), old=old_value, value=new_value)
        old_seg = seg_for(old_idx, prefer_forward=False, avoid_claimed=True)
        if old_seg["index"] >= new_idx:
            new_idx = min(n_segments - 1, old_seg["index"] + 2)
        # 新段:从 new_idx 起正向找第一个未认领、未满、且在旧段之后的段。
        new_seg = None
        for candidate in range(new_idx, n_segments):
            seg = segments[candidate]
            if seg["index"] in conflict_claimed or seg["insertions"] >= 3:
                continue
            if seg["index"] <= old_seg["index"]:
                continue
            new_seg = seg
            break
        if new_seg is None:
            raise AssertionError(f"{draft_id} C{step_index:02d}: 旧段之后找不到可用的同键段")
        pair_path = f"config/baseline_{step_index:02d}.toml"
        old_seg["path"] = pair_path
        new_seg["path"] = pair_path
        conflict_claimed.add(old_seg["index"])
        conflict_claimed.add(new_seg["index"])
        # 新版声明 needle 化(判卷单元);旧版声明只种进旧段,金账 old_seg_index 指路。
        plant(step, fact_id, "evidence", "conflict", new_sentence, conflict_key(step_index),
              new_value, old_value, new_seg, old_seg)
        old_long = old_seg["length_class"] == "long"
        old_seg["text"], old_offset = insert_sentence(
            old_seg["text"], old_sentence, old_seg["insertions"], old_long)
        old_seg["insertions"] += 1
        needles[-1]["old_offset_pct_in_seg"] = old_offset

    # 3) evidence/recall needle:每位置档 needles_per_position 枚,避开冲突对认领的段。
    for step_index, step in enumerate(positions):
        for sub in range(needles_per_position):
            fact_id = f"R{step_index:02d}" + (chr(ord("a") + sub) if needles_per_position > 1 else "")
            value = f"{RECALL_PREFIX[lang]}-{recall_num(step_index, sub)}"
            sentence = RECALL_SENTENCE[lang].format(key=recall_key(step_index), value=value)
            seg = seg_for(position_index(step, positions, n_segments), avoid_claimed=True)
            plant(step, fact_id, "evidence", "recall", sentence, recall_key(step_index),
                  value, None, seg)

    # 4) contract needle(分层铁律 3):落 user 消息,每档 recall+conflict
    #    各一。conflict 旧约 slot 在新约前约 4 条消息;档 0 没有更早的空间:
    #    旧约落 slot 0、新约挪 slot 2,实际序位进金账,曲线仍按设计档记。
    for step_index, step in enumerate(positions):
        new_slot = user_slot_for(step_index, positions, n_slots)
        old_slot = max(0, new_slot - 4)
        if new_slot <= old_slot:
            new_slot = min(n_slots - 1, old_slot + 2)
            if new_slot <= old_slot:
                raise AssertionError(f"{draft_id} user 消息太少,contract 冲突对没处放")
        fact_id = f"KC{step_index:02d}"
        old_value = f"{CONTRACT_CONFLICT_PREFIX[lang]}-{contract_conflict_old_num(step_index)}"
        new_value = f"{CONTRACT_CONFLICT_PREFIX[lang]}-{contract_conflict_new_num(step_index)}"
        old_sentence = CONTRACT_OLD_SENTENCE[lang].format(
            key=contract_conflict_key(step_index), value=old_value)
        new_sentence = CONTRACT_NEW_SENTENCE[lang].format(
            key=contract_conflict_key(step_index), old=old_value, value=new_value)
        # 新约 needle 化;旧约只种进旧 slot 的正文,金账 old_slot 指路。
        plant_contract(step, fact_id, "conflict", new_sentence, contract_conflict_key(step_index),
                       new_value, old_value, new_slot, old_slot)
        user_turns[old_slot]["text"] += "\n" + old_sentence
        user_turns[old_slot]["insertions"] += 1

    for step_index, step in enumerate(positions):
        fact_id = f"KR{step_index:02d}"
        value = f"{CONTRACT_RECALL_PREFIX[lang]}-{contract_recall_num(step_index)}"
        sentence = CONTRACT_RECALL_SENTENCE[lang].format(
            key=contract_recall_key(step_index), value=value)
        plant_contract(step, fact_id, "recall", sentence, contract_recall_key(step_index),
                       value, None, user_slot_for(step_index, positions, n_slots))

    # 5) 收尾:段引用回填 + 临时键出清。
    for needle in needles:
        if needle["carrier"] != "tool_result":
            continue
        seg = segments[needle["seg_index"]]
        needle["path"] = seg["path"]
        needle["tool_use_id"] = seg["tool_use_id"]
        needle["seg_length_class"] = seg["length_class"]
    for seg in segments:
        seg.pop("insertions")
        seg.pop("conflict_claimed")
    for turn in user_turns:
        turn.pop("insertions")

    return {
        "experiment": "compact_position",
        "draft_id": draft_id,
        "lang": lang,
        "repeat": repeat,
        "seed": seed,
        "params": {
            "segments": n_segments,
            "positions": positions,
            "needles_per_position": needles_per_position,
            "long_result_bytes": long_result_bytes,
            "class_distribution": {name: weight for name, weight in CLASS_DISTRIBUTION},
            "user_turns": len(user_turns),
        },
        "segments": segments,
        "user_turns": user_turns,
        "needles": needles,
    }


# ---- 底稿断言 ---------------------------------------------------------------


def verify_draft(draft: dict, positions: list[int], needles_per_position: int) -> None:
    segments = draft["segments"]
    user_turns = draft["user_turns"]
    needles = draft["needles"]
    n_slots = len(user_turns)
    full_text = "\n".join(seg["text"] for seg in segments) + "\n" + \
        "\n".join(turn["text"] for turn in user_turns)

    # 位置档全覆盖 + 分层计数(每档:evidence recall npp + evidence conflict 1
    # + contract recall 1 + contract conflict 1)。
    designed = sorted({n["position_pct"] for n in needles})
    assert designed == sorted(positions), f"位置档缺失: {designed}"
    recalls = [n for n in needles if n["probe_kind"] == "recall" and n["layer"] == "evidence"]
    conflicts = [n for n in needles if n["probe_kind"] == "conflict" and n["layer"] == "evidence"]
    assert len(recalls) == len(positions) * needles_per_position, "evidence recall 数不对"
    assert len(conflicts) == len(positions), "evidence conflict 数不对"
    contract_recalls = [n for n in needles if n["layer"] == "contract" and n["probe_kind"] == "recall"]
    contract_conflicts = [n for n in needles if n["layer"] == "contract" and n["probe_kind"] == "conflict"]
    assert len(contract_recalls) == len(positions), "contract recall 数不对"
    assert len(contract_conflicts) == len(positions), "contract conflict 数不对"

    # carrier 与 source 字段自洽:evidence 带段引用、contract 带消息序位。
    for needle in needles:
        if needle["layer"] == "evidence":
            assert needle["carrier"] == "tool_result", "evidence needle 必落 tool result"
            assert needle["slot"] is None and 0 <= needle["seg_index"] < len(segments)
        else:
            assert needle["carrier"] == "user_turn", "contract needle 必落 user 消息"
            assert needle["seg_index"] is None and 0 <= needle["slot"] < n_slots

    # 值出现次数(recall=1;conflict 新值=1、旧值=2)——全文含 user 消息。
    for needle in needles:
        is_recall = needle["probe_kind"] == "recall"
        expected_count = 1 if is_recall else 2
        value = needle["expected_value"] if is_recall else needle["old_value"]
        count = full_text.count(value)
        assert count == expected_count, (
            f"{draft['draft_id']} {needle['fact_id']}: 值 {value} 出现 {count} 次,期望 {expected_count}")
        if not is_recall:
            assert full_text.count(needle["expected_value"]) == 1, (
                f"{draft['draft_id']} {needle['fact_id']}: 新值应恰好出现一次")

    # needle 句在场:值唯一性已保证,这里再钉"句本身完整在场"。
    for needle in needles:
        if needle["layer"] == "contract" and needle["probe_kind"] == "conflict":
            carrier = user_turns[needle["old_slot"]]["text"]
            assert needle["old_value"] in carrier, (
                f"{draft['draft_id']} {needle['fact_id']}: 旧约不在 old_slot 正文里")

    # conflict 旧声明严格在新声明之前(段下标或消息序位)。
    for needle in conflicts:
        assert needle["old_seg_index"] is not None and needle["old_seg_index"] < needle["seg_index"], (
            f"{draft['draft_id']} {needle['fact_id']}: 旧段不在新段之前")
    for needle in contract_conflicts:
        assert needle["old_slot"] is not None and needle["old_slot"] < needle["slot"], (
            f"{draft['draft_id']} {needle['fact_id']}: 旧约消息不在新约之前")

    # 插入带与余量(evidence 段内)。
    for needle in needles:
        if needle["carrier"] != "tool_result":
            continue
        assert 20.0 <= needle["offset_pct_in_seg"] <= 80.0, (
            f"{draft['draft_id']} {needle['fact_id']}: 插入偏移 {needle['offset_pct_in_seg']}% 出带")
        if needle["seg_length_class"] == "long":
            seg = segments[needle["seg_index"]]
            assert byte_len(seg["text"]) > 384 * 2, "long 段装不下双侧余量"

    # 段长档口径自洽。
    for seg in segments:
        assert seg["length_class"] == class_for_size(byte_len(seg["text"]), draft["params"]["long_result_bytes"]), (
            f"{draft['draft_id']} seg{seg['index']}: 长度档与实际字节数不符")

    # evidence 冲突对同 path(折叠路 NewVersion 的前提)。
    for needle in conflicts:
        assert segments[needle["old_seg_index"]]["path"] == needle["path"], (
            f"{draft['draft_id']} {needle['fact_id']}: 冲突对新旧段 path 不一致")

    # 问题非空(问答判卷的题面全在金账上)。
    for needle in needles:
        assert needle["key"] and needle["key"] in needle["question"], (
            f"{draft['draft_id']} {needle['fact_id']}: 问题缺 key")


def draft_digest(draft: dict) -> str:
    return hashlib.sha256(
        json.dumps(draft, ensure_ascii=False, sort_keys=True).encode("utf-8")).hexdigest()


# ---- 主流程 -----------------------------------------------------------------


def run(out_dir: Path, segments: int, repeats: int, langs: list[str], positions: list[int],
        needles_per_position: int, seed_base: int, long_result_bytes: int) -> int:
    drafts_dir = out_dir / "drafts"
    drafts_dir.mkdir(parents=True, exist_ok=True)
    gold_path = out_dir / "needle_gold.jsonl"
    gold_count = 0
    with gold_path.open("w", encoding="utf-8") as gold:
        for repeat in range(1, repeats + 1):
            for lang in langs:
                seed = seed_base + repeat * 1000 + LANG_SEED_OFFSET.get(lang, 13)
                draft = generate_draft(lang, repeat, seed, positions, segments,
                                       needles_per_position, long_result_bytes)
                verify_draft(draft, positions, needles_per_position)
                path = drafts_dir / f"{draft['draft_id']}.json"
                path.write_text(
                    json.dumps(draft, ensure_ascii=False, indent=1), encoding="utf-8")
                for needle in draft["needles"]:
                    row = dict(needle)
                    row["draft_id"] = draft["draft_id"]
                    row["repeat"] = repeat
                    row["seed"] = seed
                    gold.write(json.dumps(row, ensure_ascii=False) + "\n")
                    gold_count += 1
    print(f"generate: {len(langs) * repeats} 份底稿 -> {drafts_dir}")
    print(f"generate: {gold_count} 条 needle 金账 -> {gold_path}")
    return gold_count


def self_check() -> int:
    # doctest(位置换算、消息序位与档位判定的口径先钉住)。
    failures = doctest.testmod().failed
    assert failures == 0, f"doctest 挂了 {failures} 条"
    # 小矩阵:4 档位置 × 1 repeat × zh,en 两种措辞都造。
    for lang in DEFAULT_LANGS:
        draft = generate_draft(lang, 1, 20260904 + 7, [0, 25, 50, 95], 24, 1,
                               DEFAULT_LONG_RESULT_BYTES)
        verify_draft(draft, [0, 25, 50, 95], 1)
    # 确定性:同 seed 两造逐字节一致。
    a = generate_draft("zh", 2, 4711, DEFAULT_POSITIONS, DEFAULT_SEGMENTS, 1,
                       DEFAULT_LONG_RESULT_BYTES)
    b = generate_draft("zh", 2, 4711, DEFAULT_POSITIONS, DEFAULT_SEGMENTS, 1,
                       DEFAULT_LONG_RESULT_BYTES)
    assert draft_digest(a) == draft_digest(b), "同 seed 两次生成不一致"
    # 默认全矩阵走一遍断言(不落盘)。
    for lang in DEFAULT_LANGS:
        draft = generate_draft(lang, 1, 20260905, DEFAULT_POSITIONS, DEFAULT_SEGMENTS,
                               DEFAULT_NEEDLES_PER_POSITION, DEFAULT_LONG_RESULT_BYTES)
        verify_draft(draft, DEFAULT_POSITIONS, DEFAULT_NEEDLES_PER_POSITION)
    print("generate: 自检全过(doctest + 小矩阵 + 确定性 + 默认全矩阵断言)")
    return 0


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--segments", type=int, default=DEFAULT_SEGMENTS, help="填充段数")
    parser.add_argument("--repeats", type=int, default=DEFAULT_REPEATS, help="每措辞重复次数")
    parser.add_argument("--langs", default=",".join(DEFAULT_LANGS), help="措辞形,逗号分隔")
    parser.add_argument("--positions", default=",".join(str(p) for p in DEFAULT_POSITIONS),
                        help="位置档表,逗号分隔(0-95)")
    parser.add_argument("--needles-per-position", type=int, default=DEFAULT_NEEDLES_PER_POSITION,
                        help="每位置档 evidence recall needle 数")
    parser.add_argument("--seed-base", type=int, default=DEFAULT_SEED_BASE, help="种子基")
    parser.add_argument("--long-result-bytes", type=int, default=DEFAULT_LONG_RESULT_BYTES,
                        help="long 档下限(与产品 long_result_bytes 对齐)")
    parser.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent / "results",
                        help="落件目录(底稿在其 drafts/ 子目录)")
    parser.add_argument("--self-check", action="store_true", help="只跑自检,不落盘")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.self_check:
        return self_check()
    positions = [int(p) for p in args.positions.split(",") if p.strip() != ""]
    langs = [lang.strip() for lang in args.langs.split(",") if lang.strip() != ""]
    if not positions or any(p < 0 or p > 95 for p in positions):
        print("generate: 位置档表非法(须在 0-95 内)", file=sys.stderr)
        return 2
    run(args.out_dir, args.segments, args.repeats, langs, positions,
        args.needles_per_position, args.seed_base, args.long_result_bytes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
