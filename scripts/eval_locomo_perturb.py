#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoCoMo-MC10 防作弊扰动器(记忆系统评测单 §2.1/§E0)。

输入 eval/locomo/locomo_mc10.json(JSONL,1986 题,每题带全场 haystack),
按场聚合后做三重扰动,破坏与预训练语料的字面匹配:

1. 实体改名:每场两位说话人(+昵称)与题面/正文中的专名(地名、品牌、
   作品名)整场一致替换,映射恒定,跨场可不同。替换表手工定制(普查见
   脚本开发记录),新词池为编造实体,与真实指涉无涉;写入前查场内冲突。
2. 日期平移:每场随机平移 N 天(seed 固定可复现),session 时间戳、正文
   文本日期(dmy/mdy/单年份三式)、选项与答案里的日期同规则平移,相对
   时间关系保持。
3. 题目改写:疑问词映射改句式 + 对话指向前缀 + 保守同义词替换,避开
   原题原文。选项文本轻扰(同义词),选项位次绝不动——判分对原
   correct_choice_index。

产物(eval/locomo/,均不进 git):
  perturbed.jsonl  每行一场:对话本体(扰动后)+ 该场全部题(扰动后)
  ledger.json      变换账本:每场 name_map/entity_map/shift_days/改写对照
  answers.json     qid -> correct_choice_index(判分用,答题流程永不读)
  answers.sha256   answers.json 的 SHA256

用法:
  python scripts/eval_locomo_perturb.py            # 产扰动数据集
  python scripts/eval_locomo_perturb.py --audit    # 一致性自检+20 题抽验清单
"""

import argparse
import hashlib
import json
import os
import random
import re
import sys
from datetime import date, timedelta

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------- 实体替换表
# 人名:北欧/欧陆风格编造名,同场恒定。昵称跟主名走(同截短逻辑)。
SPEAKER_SUBST = {
    "conv-26": {"Caroline": "Dorthe", "Melanie": "Astrid",
                "Mel": "Astri", "Caro": "Dorte"},
    "conv-30": {"Gina": "Sigrid", "Jon": "Bendt"},
    "conv-41": {"Maria": "Jorunn", "John": "Halvor"},
    "conv-42": {"Joanna": "Ingeborg", "Nate": "Fabian", "Jo": "Inga"},
    "conv-43": {"John": "Torkel", "Tim": "Eivind"},
    "conv-44": {"Audrey": "Solvej", "Andrew": "Mogens"},
    "conv-47": {"John": "Casimir", "James": "Anselm"},
    "conv-48": {"Deborah": "Randi", "Jolene": "Majken"},
    "conv-49": {"Sam": "Vilhelm", "Evan": "Preben"},
    "conv-50": {"Calvin": "Torben", "Dave": "Knud"},
}

# 专名(地名/品牌/作品/机构):编造同类别实体。长词先替换(避免子串遮蔽)。
ENTITY_SUBST = {
    "conv-26": {
        "LGBTQ": "QRAD", "LGBT": "QRD",
        "Grand Canyon": "Velden Gorge",
        "Four Seasons": "Copper Beeches",
    },
    "conv-30": {"Door Dash": "Meal Ferry", "DoorDash": "MealFerry"},
    "conv-41": {"Pacific Northwest": "Northern Lakeshore",
                # 干扰项人名列表里的 Andrew(非本场说话人):换编造常名
                "Andrew": "Anders"},
    "conv-42": {
        "Xenoblade Chronicles": "Starvale Chronicles",
        "Xenoblade": "Starvale",
        "Eternal Sunshine": "Endless Daylight",
        "Spotless Mind": "Unclouded Mind",
        "Fort Wayne": "Kestrel Falls",
        "Nintendo Switch": "Hinode Dock",
    },
    "conv-43": {
        "Harry Potter": "Garth Plummer",
        "Universal Studios": "Meridian Studios",
        "New York City": "Marrow Bay City",
        "New York": "Marrow Bay",
        "NYC": "MBC",
        "John Greene": "Nils Greenfield",
        "UK": "the North Isles",
    },
    "conv-47": {"The Witcher": "The Runesmith", "Witcher": "Runesmith"},
    "conv-48": {
        "Nintendo Switch": "Hinode Dock",
        "Walking Dead": "Rusting Fence",
    },
    "conv-50": {
        "Frank Ocean": "Neil River",
        "San Francisco": "Vesterport",
        "Times Square": "Guild Gate",
    },
}

# 全局通用表:干扰项里塞满开放世界实体(游戏/电影/地名),跨场出现。
# 与 per-conv 表合并应用(长词先换),同名映射各场一致。盖不住的长尾由
# E2 的 A 组裸分验收线兜底(§四:裸分显著超随机则加扰动强度重来)。
COMMON_ENTITY_SUBST = {
    "Grand Canyon": "Velden Gorge",
    "Pacific Northwest": "Northern Lakeshore",
    "Harry Potter": "Garth Plummer",
    "The Witcher": "The Runesmith", "Witcher": "Runesmith",
    "Xenoblade Chronicles": "Starvale Chronicles", "Xenoblade": "Starvale",
    "San Francisco": "Vesterport",
    "New York City": "Marrow Bay City", "New York": "Marrow Bay", "NYC": "MBC",
    "The Legend of Zelda": "The Saga of Eldra", "Zelda": "Eldra",
    "Breath of the Wild": "Winds of the Fen", "BOTW": "WOTF",
    "Lord of the Rings": "Warlords of the Reach",
    "The Matrix": "The Lattice",
    "Marvel Cinematic Universe": "Meridian Cinematic Universe",
    "Marvel's Spider-Man": "Meridian's Silk-Man", "Spider-Man": "Silk-Man",
    "Cyberpunk 2077": "Gridpunk 2077",
    "Elden Ring": "Ember Ring",
    "Skyrim": "Fellrim",
    "Fallout 4": "Dustfall 4",
    "Animal Crossing": "Beacon Crossing",
    "Super Mario Odyssey": "Super Rico Odyssey",
    "Mario Kart": "Rico Kart",
    "Minecraft": "Blockhaven",
    "Fortnite": "Fortline",
    "Call of Duty": "Call of Watch",
    "Valorant": "Valora",
    "Apex Legends": "Apex Chroniclers",
    "Overwatch 2": "Underwatch 2", "Overwatch": "Underwatch",
    "Persona 5": "Phantom 5",
    "Portal 2": "Gate 2",
    "Splatoon 3": "Sprayloon 3",
    "Divinity: Original Sin": "Verity: Original Sin",
    "Final Fantasy": "Final Reverie",
    "Dragon Quest": "Dragon Venture",
    "Stranger Things": "Curious Things",
    "Westworld": "Westmere",
    "Shadow and Bone": "Shade and Salt",
    "Eragon": "Arvagon",
    "His Dark Materials": "Her Pale Materials",
    "A Song of Ice and Fire": "A Saga of Mist and Ember",
    "A Storm of Swords": "A Storm of Spears",
    "Chronicles of Narnia": "Chronicles of Varna",
    "Back to the Future": "Forth to the Future",
    "Fight Club": "Brawl Club",
    "Pulp Fiction": "Pulp Fable",
    "Emily Dickinson": "Emma Dunmore",
}

# ---------------------------------------------------------------- 题目改写
# 疑问词句式映射(整句重排,当行捕获组)。
QUESTION_REWRITE = [
    (re.compile(r"^When did (.+?)\?$"), r"On which date did \1, to be exact?"),
    (re.compile(r"^When was (.+?)\?$"), r"On which date was \1, to be exact?"),
    (re.compile(r"^When will (.+?)\?$"), r"On which date will \1?"),
    (re.compile(r"^Where did (.+?)\?$"), r"In which place did \1?"),
    (re.compile(r"^Where was (.+?)\?$"), r"In which place was \1?"),
    (re.compile(r"^Who (.+?)\?$"), r"Which person \1?"),
    (re.compile(r"^How many (.+?)\?$"), r"What number of \1?"),
    (re.compile(r"^How much (.+?)\?$"), r"What quantity of \1?"),
    (re.compile(r"^How often (.+?)\?$"), r"At what frequency \1?"),
    (re.compile(r"^How long did (.+?)\?$"), r"For what length of time did \1?"),
]

# 其余句式的前缀(轮换使用,seed 固定)。
QUESTION_PREFIXES = [
    "Going by the dialogue, ",
    "Per the chat history, ",
    "Judging from the conversation, ",
    "As the exchange shows, ",
]

# 保守同义词表(双向各用一次,避免多义坑):题面/选项轻扰共用。
SYNONYMS = {
    "went": "headed", "go": "head", "said": "mentioned", "say": "mention",
    "told": "related", "tell": "relate", "buy": "purchase", "bought": "purchased",
    "like": "enjoy", "liked": "enjoyed", "likes": "enjoys",
    "love": "adore", "loved": "adored", "talk": "chat", "talked": "chatted",
    "asked": "inquired", "ask": "inquire", "see": "spot", "saw": "spotted",
    "get": "obtain", "got": "obtained", "make": "create", "made": "created",
    "think": "reckon", "thought": "reckoned",
    "know": "be aware", "knew": "was aware", "want": "wish", "wanted": "wished",
    "start": "begin", "started": "began",
    "help": "assist", "helped": "assisted", "find": "locate", "found": "located",
    "lose": "misplace", "lost": "misplaced", "move": "relocate", "moved": "relocated",
    "visit": "drop by", "visited": "dropped by", "attend": "be present at",
    "attended": "was present at",
    "big": "sizable", "small": "little", "good": "decent", "bad": "poor",
    "old": "aged", "new": "fresh", "happy": "glad", "sad": "downcast",
    "friend": "companion", "friends": "companions", "family": "kin",
    "job": "occupation", "school": "campus", "car": "vehicle",
    "trip": "journey", "party": "gathering", "book": "volume",
    "movie": "film", "song": "tune", "food": "meal", "gift": "present",
    "weekend": "rest days", "holiday": "break", "plan": "intend",
    "planned": "intended", "remember": "recall", "remembered": "recalled",
    "conversation": "dialogue", "mentioned": "brought up",
    "favorite": "preferred", "interesting": "engaging",
}

MONTHS = ["January", "February", "March", "April", "May", "June", "July",
          "August", "September", "October", "November", "December"]
MONTH_IDX = {m: i + 1 for i, m in enumerate(MONTHS)}

# 日期四式单遍合并:dmy / mdy / 月年 / 裸年。必须单遍——分次 sub 会让
# 前一式产出的新日期文本被后一式再次平移(级联错位,实测踩过)。
# alternation 的回溯保证 "May 2023" 落到月年式而非 mdy 的 "May 20"。
_MONTH_ALT = "January|February|March|April|May|June|July|August|September|October|November|December"
PAT_ALL_DATES = re.compile(
    r"\b(?:"
    r"(\d{1,2})(?:st|nd|rd|th)?\s+(?:of\s+)?(" + _MONTH_ALT + r")(?:,?\s+(\d{4}))?"
    r"|(?:(" + _MONTH_ALT + r")\s+(\d{1,2})(?:st|nd|rd|th)?(?:\s*,\s*(\d{4}))?)"
    r"|(?:(" + _MONTH_ALT + r")\s+(\d{4}))"
    r"|(19\d{2}|20[0-2]\d)"
    r")\b")
PAT_DMY = re.compile(
    r"\b(\d{1,2})(?:st|nd|rd|th)?\s+(?:of\s+)?(" + _MONTH_ALT + r")(?:,?\s+(\d{4}))?\b")
PAT_MDY = re.compile(
    r"\b(" + _MONTH_ALT + r")\s+(\d{1,2})(?:st|nd|rd|th)?(?:\s*,\s*(\d{4}))?\b")


def shift_days_for(conv_id: str, seed: int) -> int:
    # 每场独立定数。种子只用 conv_id 与显式 seed——Random 对象的 repr 带
    # 内存地址,塞进种子会让 N 每次进程漂移(实测踩过)。
    rng2 = random.Random(f"{conv_id}|{seed}")
    return rng2.randint(30, 900)


def ordinal_suffix(day: int) -> str:
    if 10 <= day % 100 <= 20:
        return "th"
    return {1: "st", 2: "nd", 3: "rd"}.get(day % 10, "th")


def shift_date(d: date, days: int) -> date:
    return d + timedelta(days=days)


def render_dmy(d: date, with_year: bool) -> str:
    if with_year:
        return f"{d.day} {MONTHS[d.month - 1]} {d.year}"
    return f"{d.day} {MONTHS[d.month - 1]}"


def render_mdy(d: date, with_year: bool) -> str:
    if with_year:
        return f"{MONTHS[d.month - 1]} {d.day}, {d.year}"
    return f"{MONTHS[d.month - 1]} {d.day}"


class DateShifter:
    """一场一个平移天数。无年日期按场内锚年补齐;单年份按年中锚。"""

    def __init__(self, days: int, anchor_year: int):
        self.days = days
        self.anchor_year = anchor_year

    def _resolve(self, day: int, month: str, year):
        y = year if year else self.anchor_year
        try:
            return date(y, MONTH_IDX[month], day)
        except ValueError:
            return None  # 出题方造的非法日期干扰项(31 June 之类):保留原文

    def shift_text(self, text: str) -> str:
        # 单遍替换:每个位置只被一种日期式命中一次,产出不会再被扫。
        def repl(m):
            g = m.groups()
            if g[0] is not None:  # dmy:g0=day g1=month g2=year?
                d = self._resolve(int(g[0]), g[1], int(g[2]) if g[2] else None)
                if d is None:
                    return m.group(0)
                return render_dmy(shift_date(d, self.days), bool(g[2]))
            if g[3] is not None:  # mdy:g3=month g4=day g5=year?
                d = self._resolve(int(g[4]), g[3], int(g[5]) if g[5] else None)
                if d is None:
                    return m.group(0)
                return render_mdy(shift_date(d, self.days), bool(g[5]))
            if g[6] is not None:  # 月年式(月 15 日锚):g6=month g7=year
                try:
                    d = date(int(g[7]), MONTH_IDX[g[6]], 15)
                except ValueError:
                    return m.group(0)
                nd = shift_date(d, self.days)
                return f"{MONTHS[nd.month - 1]} {nd.year}"
            # 裸年份(年中锚):g8
            nd = shift_date(date(int(g[8]), 7, 1), self.days)
            return str(nd.year)
        return PAT_ALL_DATES.sub(repl, text)


def subst_names(text: str, mapping: dict) -> str:
    """词边界替换,大小写两形(正文里 CAROLINE 大写、摘要里 Caroline 首字母)。"""
    for old, new in sorted(mapping.items(), key=lambda kv: -len(kv[0])):
        upper_old, upper_new = old.upper(), new.upper()
        title_old, title_new = old[0].upper() + old[1:], new[0].upper() + new[1:]
        text = re.sub(r"\b" + re.escape(upper_old) + r"\b", upper_new, text)
        text = re.sub(r"\b" + re.escape(title_old) + r"\b", title_new, text)
        if old != title_old:
            text = re.sub(r"\b" + re.escape(old) + r"\b", new, text)
    return text


def subst_entities(text: str, mapping: dict) -> str:
    for old, new in sorted(mapping.items(), key=lambda kv: -len(kv[0])):
        text = re.sub(r"\b" + re.escape(old) + r"\b", new, text)
    return text


def synonym_light(text: str, rng: random.Random, cap: int = 4) -> str:
    """保守同义词轻扰:全词边界,最多换 cap 个词,只换题面/选项。
    候选先按字典序排(跨进程稳定,set 迭代序受 PYTHONHASHSEED 摆布),
    再按 rng 抖动——保证同 seed 重跑字节级可复现。"""
    replaced = 0
    words = sorted(set(re.findall(r"[A-Za-z']+", text)))
    for w in sorted(words, key=lambda x: rng.random()):
        if replaced >= cap:
            break
        syn = SYNONYMS.get(w.lower())
        if not syn:
            continue
        out = syn if not w[0].isupper() else syn[0].upper() + syn[1:]
        text = re.sub(r"\b" + re.escape(w) + r"\b", out, text)
        replaced += 1
    return text


def rewrite_question(q: str, rng: random.Random) -> str:
    """句式映射 → 前缀注入 → 同义词轻扰。保义优先,改写保守。"""
    for pat, repl in QUESTION_REWRITE:
        m = pat.match(q)
        if m:
            body = m.group(1)
            body = synonym_light(body, rng, cap=3)
            return pat.sub(repl.replace("\\1", body), q)
    prefix = QUESTION_PREFIXES[rng.randrange(len(QUESTION_PREFIXES))]
    body = synonym_light(q, rng, cap=3)
    return prefix + body[0].lower() + body[1:]


def subst_all(text: str, name_map: dict, entity_map: dict) -> str:
    """先 entity 后 name:长专名(John Greene)须整体换,先被 name 表啃掉
    John 就再也对不上号了。全局通用表与 per-conv 表合并(per-conv 优先)。"""
    merged = dict(COMMON_ENTITY_SUBST)
    merged.update(entity_map)
    text = subst_entities(text, merged)
    text = subst_names(text, name_map)
    return text


def collect_text(record: dict) -> str:
    parts = []
    for s in record["haystack_sessions"]:
        for m in s:
            parts.append(m["content"])
    parts.extend(record["haystack_session_summaries"])
    return "\n".join(parts)


def conflict_check(conv_id: str, record: dict) -> list:
    """替换新词不得在原场文本中已出现(词边界),否则两义混淆。"""
    text = collect_text(record)
    problems = []
    for mapping, tag in ((SPEAKER_SUBST.get(conv_id, {}), "name"),
                         (ENTITY_SUBST.get(conv_id, {}), "entity")):
        for old, new in mapping.items():
            if re.search(r"\b" + re.escape(new) + r"\b", text, re.IGNORECASE):
                problems.append(f"{conv_id} {tag} 新词 {new} 在原文本已出现")
    return problems


def anchor_year(datetimes) -> int:
    years = [int(dt[:4]) for dt in datetimes if dt]
    years.sort()
    return years[len(years) // 2]


# ---------------------------------------------------------------- 主流程

def build(input_path: str, out_dir: str, seed: int) -> dict:
    rng = random.Random(seed)
    convs = {}
    order = []
    with open(input_path, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            cid = r["question_id"].split("_")[0]
            if cid not in convs:
                convs[cid] = []
                order.append(cid)
            convs[cid].append(r)

    ledger = {"seed": seed, "convs": {}}
    answers = {}
    out_records = []
    problems = []
    for cid in order:
        records = convs[cid]
        base = records[0]
        name_map = SPEAKER_SUBST.get(cid, {})
        entity_map = ENTITY_SUBST.get(cid, {})
        problems.extend(conflict_check(cid, base))
        days = shift_days_for(cid, seed)
        anchor = anchor_year(base["haystack_session_datetimes"])
        shifter = DateShifter(days, anchor)

        conv_out = {
            "conv_id": cid,
            "shift_days": days,
            "sessions": [],
            "questions": [],
        }
        # 会话本体(扰动):正文、摘要、时间戳。逐 session 一条记忆的灌入
        # 由检索评测侧做,这里只产数据。
        for idx, sess in enumerate(base["haystack_sessions"]):
            raw_lines = [m["content"] for m in sess]
            perturbed_lines = []
            for line_text in raw_lines:
                t = subst_all(line_text, name_map, entity_map)
                t = shifter.shift_text(t)
                perturbed_lines.append(t)
            summary_raw = base["haystack_session_summaries"][idx]
            summary = shifter.shift_text(subst_all(summary_raw, name_map, entity_map))
            dt_raw = base["haystack_session_datetimes"][idx]
            dt = (date(int(dt_raw[:4]), int(dt_raw[5:7]), int(dt_raw[8:10]))
                  + timedelta(days=days)).isoformat() + dt_raw[10:]
            conv_out["sessions"].append({
                "no": idx + 1,
                "id": base["haystack_session_ids"][idx],
                "datetime": dt,
                "summary": summary,
                "lines": perturbed_lines,
            })

        # 题目(扰动):题面改写+实体/日期替换;选项轻扰不改位次;answer 同步。
        for r in records:
            qid = r["question_id"]
            q_orig = r["question"]
            q_stage1 = subst_all(q_orig, name_map, entity_map)
            q_stage2 = shifter.shift_text(q_stage1)
            q_new = rewrite_question(q_stage2, rng)
            choices = []
            for c in r["choices"]:
                c1 = subst_all(c, name_map, entity_map)
                c1 = shifter.shift_text(c1)
                c1 = synonym_light(c1, rng, cap=2)
                choices.append(c1)
            ans = r["answer"]
            ans = subst_all(ans, name_map, entity_map)
            ans = shifter.shift_text(ans)
            conv_out["questions"].append({
                "qid": qid,
                "category": r["question_type"],
                "question": q_new,
                "choices": choices,
                "answer_text": ans,
                "answer_index": r["correct_choice_index"],
                "num_choices": r["num_choices"],
            })
            answers[qid] = r["correct_choice_index"]

        ledger["convs"][cid] = {
            "name_map": name_map,
            "entity_map": entity_map,
            "shift_days": days,
            "anchor_year": anchor,
            "rewrites": {
                q["qid"]: {"orig": o, "new": q["question"]}
                for q, o in zip(conv_out["questions"],
                                [r["question"] for r in records])
            },
        }
        out_records.append(conv_out)

    # 落盘
    os.makedirs(out_dir, exist_ok=True)
    perturbed_path = os.path.join(out_dir, "perturbed.jsonl")
    with open(perturbed_path, "w", encoding="utf-8") as f:
        for rec in out_records:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    ledger_path = os.path.join(out_dir, "ledger.json")
    with open(ledger_path, "w", encoding="utf-8") as f:
        json.dump(ledger, f, ensure_ascii=False, indent=1)
    answers_path = os.path.join(out_dir, "answers.json")
    with open(answers_path, "w", encoding="utf-8") as f:
        json.dump(answers, f, ensure_ascii=False, indent=0, sort_keys=True)
    digest = hashlib.sha256(open(answers_path, "rb").read()).hexdigest()
    with open(os.path.join(out_dir, "answers.sha256"), "w", encoding="utf-8") as f:
        f.write(digest + "\n")
    print(f"conv={len(out_records)} questions={len(answers)} shift_written")
    print(f"answers.sha256={digest}")
    if problems:
        print("!! 替换冲突", file=sys.stderr)
        for p in problems:
            print("  ", p, file=sys.stderr)
    return {"perturbed": perturbed_path, "ledger": ledger_path}


def word_boundary_hit(text: str, word: str) -> int:
    return len(re.findall(r"\b" + re.escape(word) + r"\b", text, re.IGNORECASE))


# 双射验证用的提取 pattern:dmy 允许 "8 May, 2023" 逗号变体(扰动器会把
# 它规范成无逗号式,两侧都要认得,否则多报假阳性)。
V_DMY = re.compile(r"\b(\d{1,2})(?:st|nd|rd|th)?\s+(?:of\s+)?(" + _MONTH_ALT +
                   r")(?:,)?\s+(\d{4})\b")
V_MDY = re.compile(r"\b(" + _MONTH_ALT + r")\s+(\d{1,2})(?:st|nd|rd|th)?\s*,\s*(\d{4})\b")


def extract_full_dates(text: str) -> list:
    from datetime import date as _date
    out = []
    for m in V_DMY.finditer(text):
        try:
            out.append(_date(int(m.group(3)), MONTH_IDX[m.group(2)], int(m.group(1))))
        except ValueError:
            pass
    for m in V_MDY.finditer(text):
        try:
            out.append(_date(int(m.group(3)), MONTH_IDX[m.group(1)], int(m.group(2))))
        except ValueError:
            pass
    return out


def audit_date_bijection(input_path: str, perturbed_path: str, ledger: dict,
                         failures: list) -> None:
    """带年完整日期的平移双射:扰动文本的日期集合 == 原文集合 + N 天。"""
    orig = {}
    with open(input_path, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            cid = r["question_id"].split("_")[0]
            if cid in orig:
                continue
            text = " ".join(m["content"] for s in r["haystack_sessions"] for m in s)
            text += " " + " ".join(r["haystack_session_summaries"])
            orig[cid] = text
    with open(perturbed_path, encoding="utf-8") as f:
        for line in f:
            conv = json.loads(line)
            cid = conv["conv_id"]
            days = ledger["convs"][cid]["shift_days"]
            text = "\n".join("\n".join(s["lines"]) for s in conv["sessions"])
            text += " " + " ".join(s["summary"] for s in conv["sessions"])
            want = sorted(set(d + timedelta(days=days) for d in extract_full_dates(orig[cid])))
            got = sorted(set(extract_full_dates(text)))
            if want != got:
                miss = [str(x) for x in set(want) - set(got)][:4]
                extra = [str(x) for x in set(got) - set(want)][:4]
                failures.append(f"{cid}: 日期平移双射破缺 缺{miss} 多{extra}")


def audit(input_path: str, out_dir: str, seed: int) -> bool:
    """一致性自检:同场旧名零残留、日期相对关系保持、20 题抽验清单。"""
    perturbed_path = os.path.join(out_dir, "perturbed.jsonl")
    ledger = json.load(open(os.path.join(out_dir, "ledger.json"), encoding="utf-8"))
    answers = json.load(open(os.path.join(out_dir, "answers.json"), encoding="utf-8"))
    sha_file = os.path.join(out_dir, "answers.sha256")
    digest_ok = hashlib.sha256(open(os.path.join(out_dir, "answers.json"), "rb").read()).hexdigest() \
        == open(sha_file, encoding="utf-8").read().strip()
    print(f"[1] answers.sha256 校验: {'ok' if digest_ok else 'MISMATCH'}")

    failures = []
    sample_lines = []
    rng = random.Random(seed ^ 0xA0D17)
    n_checked = 0
    with open(perturbed_path, encoding="utf-8") as f:
        for line in f:
            conv = json.loads(line)
            cid = conv["conv_id"]
            info = ledger["convs"][cid]
            body = "\n".join("\n".join(s["lines"]) for s in conv["sessions"])
            body += "\n" + "\n".join(s["summary"] for s in conv["sessions"])
            # 旧名残留(词边界):人名与专名都查
            for old in list(info["name_map"]) + list(info["entity_map"]):
                # "UK"/"New York" 等短词误伤风险:按词边界查,题面正文都算
                n = word_boundary_hit(body, old)
                if n:
                    failures.append(f"{cid}: 旧名 {old} 残留 {n} 次")
            # 日期相对关系:datetime 与第一个 session 的差保持
            days = info["shift_days"]
            dts = [s["datetime"][:10] for s in conv["sessions"]]
            from datetime import date as _d
            d0 = _d.fromisoformat(dts[0])
            for i, dt in enumerate(dts[1:], start=2):
                delta = (_d.fromisoformat(dt) - d0).days
                if delta <= 0:
                    failures.append(f"{cid}: session 序列日期非递增(session {i})")

    # 带年完整日期的平移双射(文本层,datetime 字段之外的正文/摘要日期)
    bijection_failures = []
    audit_date_bijection(input_path, perturbed_path, ledger, bijection_failures)
    failures.extend(bijection_failures)
    print(f"[4] 带年日期平移双射: "
          f"{'全部通过' if not bijection_failures else f'{len(bijection_failures)} 场破缺'}")

    # 20 题抽验清单(五类各 4 题,打题面+选项+答案+证据定位,人工过目)
    with open(perturbed_path, encoding="utf-8") as f:
        convs = [json.loads(l) for l in f]
    by_cat = {}
    for conv in convs:
        for q in conv["questions"]:
            by_cat.setdefault(q["category"], []).append((conv, q))
    sample_lines.append("# 抽验清单(每类 4 题,人工核对:改写保义、选项对位、答案在位)")
    for cat in sorted(by_cat):
        picks = rng.sample(by_cat[cat], min(4, len(by_cat[cat])))
        for conv, q in picks:
            n_checked += 1
            evidence = [s["no"] for s in conv["sessions"]
                        if q["answer_text"] and q["answer_text"] in
                        ("\n".join(s["lines"]) + " " + s["summary"])]
            orig = ledger["convs"][conv["conv_id"]]["rewrites"][q["qid"]]["orig"]
            sample_lines.append(
                f"\n## {q['qid']} [{cat}] conv={conv['conv_id']} "
                f"answer_index={q['answer_index']}\n"
                f"- 原题: {orig}\n- 改写: {q['question']}\n"
                f"- 答案文本: {q['answer_text']} | 选项[{q['answer_index']}]: "
                f"{q['choices'][q['answer_index']]}\n"
                f"- 答案文本命中 session: {evidence}")
            if q["answer_text"] != "Not answerable" and \
                    q["choices"][q["answer_index"]].strip() == "":
                failures.append(f"{q['qid']}: 正确选项为空")
            # 选项唯一性:平移是双射,但保留的非法日期干扰项理论上可与新
            # 日期撞车;撞了就报出来人工处置。
            if len(set(c.strip() for c in q["choices"])) != len(q["choices"]):
                failures.append(f"{q['qid']}: 扰动后选项出现重复")
    audit_path = os.path.join(out_dir, "audit_sample.md")
    with open(audit_path, "w", encoding="utf-8") as f:
        f.write("\n".join(sample_lines) + "\n")
    print(f"[2] 抽验清单 {n_checked} 题 -> {audit_path}")

    if failures:
        print("[3] 自检失败项:")
        for x in failures[:40]:
            print("   ", x)
        return False
    print("[3] 同场旧名零残留、session 日期序列非递减: 全部通过")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", default=os.path.join(HERE, "eval", "locomo", "locomo_mc10.json"))
    ap.add_argument("--out-dir", default=os.path.join(HERE, "eval", "locomo"))
    ap.add_argument("--seed", type=int, default=20260903)
    ap.add_argument("--audit", action="store_true", help="只跑自检(需先 build)")
    args = ap.parse_args()
    if not args.audit:
        build(args.input, args.out_dir, args.seed)
        print("build 完成")
    ok = audit(args.input, args.out_dir, args.seed)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
