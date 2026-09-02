#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""动态工具延迟挂载 §12.5 质量对照 runner(单子:动态工具PromptCache守恒与按需调用设计)。

同一批任务(eval/deferred_quality/tasks.json)、同模型(ccmoon/gpt-5.6-sol)、
温度 0,在 disabled / proxy_reference / legacy_expand 三档下各跑一遍
one_shot 管道,逐任务机判:

- 任务成败:required_calls 逐工具逐参数对照判据 + 误选工具零执行;
- 参数首发合格:该工具第一次调用的参数即过判据与 schema(重试救回记失败);
- 误选与重搜:misselect_targets 命中数 / tool_search 调用次数;
- 轮数与 token:model.request.prepared 计数 / model.usage.recorded 求和
  (input 为非缓存口径,cache_read/creation 分列,provider 没报写 unknown)。

钥匙安全:真 ~/.lubancode/config.json 只被拷进临时 USERPROFILE 下的临时
home(eval/deferred_quality/_run/home_<mode>/.lubancode/config.json),该目录
在 .gitignore 里,绝不进 git;任何进 git 的文件不含 api_key。

判定数据源:trajectory main.jsonl 的 tool.input.effective(tool_name 是
proxy 解引用后的真实目标名,effective_arguments 是复验后参数)与
model.usage.recorded。
"""

import argparse
import copy
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time

HERE_EVAL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "eval", "deferred_quality")
REAL_CONFIG = os.path.join(os.environ.get("USERPROFILE", r"C:\Users\moontidef"), ".lubancode", "config.json")
RUN_ROOT = os.path.join(HERE_EVAL, "_run")
MODES = ("disabled", "proxy_reference", "legacy_expand")
EXE = r"D:\lubancode\build\release\Release\lubancode.exe"
PROVIDER_NAME = "ccmoon"
TEMPERATURE = 0
TASK_TIMEOUT_SECS = 420


# ---------------------------------------------------------------- 配置与环境

def load_stub_tools():
    spec = importlib.util.spec_from_file_location("mcp_stub", os.path.join(HERE_EVAL, "mcp_stub_server.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return {t["name"]: t["inputSchema"] for t in mod.TOOLS}, mod


def build_home(mode: str) -> str:
    home = os.path.join(RUN_ROOT, "home_" + mode)
    shutil.rmtree(home, ignore_errors=True)
    luban = os.path.join(home, ".lubancode")
    os.makedirs(luban, exist_ok=True)
    cfg = json.load(open(REAL_CONFIG, encoding="utf-8"))
    cfg["deferred_tool_mode"] = mode
    cfg["mcpServers"] = {
        "demosuite": {
            "command": sys.executable,
            "args": [os.path.join(HERE_EVAL, "mcp_stub_server.py")],
            "env": {"PYTHONIOENCODING": "utf-8"},
        }
    }
    for prov in cfg.get("providers", []):
        if prov.get("name") == PROVIDER_NAME:
            extra = prov.get("extra_body") or {}
            extra["temperature"] = TEMPERATURE
            prov["extra_body"] = extra
            prov.setdefault("model_reasoning_effort", "high")
    with open(os.path.join(luban, "config.json"), "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=1)
    return home


def sessions_root(home: str):
    ws = os.path.join(home, ".lubancode", "workspaces")
    if not os.path.isdir(ws):
        return None
    roots = []
    for w in os.listdir(ws):
        s = os.path.join(ws, w, "sessions")
        if os.path.isdir(s):
            roots.append(s)
    return roots


def list_session_ids(home: str):
    ids = set()
    roots = sessions_root(home) or []
    for root in roots:
        for d in os.listdir(root):
            if os.path.isfile(os.path.join(root, d, "main.jsonl")):
                ids.add((root, d))
    return ids


# ---------------------------------------------------------------- schema 判定

def schema_errors(schema: dict, value, path: str = "$") -> list:
    """轻量 JSON Schema 校验:覆盖本套件用到的关键字。返回错误列表(空即合格)。"""
    errors = []
    if schema.get("type") == "object":
        if not isinstance(value, dict):
            return [f"{path}: 应为 object,实际 {type(value).__name__}"]
        for key in schema.get("required", []):
            if key not in value:
                errors.append(f"{path}: 缺必填字段 {key}")
        props = schema.get("properties", {})
        for key, item in value.items():
            if key in props:
                errors.extend(schema_errors(props[key], item, f"{path}.{key}"))
            elif schema.get("additionalProperties") is False:
                errors.append(f"{path}.{key}: 多余字段(additionalProperties=false)")
    elif schema.get("type") == "array":
        if not isinstance(value, list):
            return [f"{path}: 应为 array,实际 {type(value).__name__}"]
        if "minItems" in schema and len(value) < schema["minItems"]:
            errors.append(f"{path}: 少于 minItems={schema['minItems']}")
        items = schema.get("items")
        if items:
            for i, item in enumerate(value):
                errors.extend(schema_errors(items, item, f"{path}[{i}]"))
    elif schema.get("type") == "string":
        if not isinstance(value, str):
            errors.append(f"{path}: 应为 string,实际 {type(value).__name__}")
    elif schema.get("type") == "integer":
        if not isinstance(value, int) or isinstance(value, bool):
            errors.append(f"{path}: 应为 integer,实际 {value!r}")
    elif schema.get("type") == "number":
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            errors.append(f"{path}: 应为 number,实际 {value!r}")
    elif schema.get("type") == "boolean":
        if not isinstance(value, bool):
            errors.append(f"{path}: 应为 boolean,实际 {value!r}")
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path}: {value!r} 不在枚举 {schema['enum']}")
    if "minimum" in schema and isinstance(value, (int, float)) and not isinstance(value, bool):
        if value < schema["minimum"]:
            errors.append(f"{path}: {value} 低于 minimum={schema['minimum']}")
    if "maximum" in schema and isinstance(value, (int, float)) and not isinstance(value, bool):
        if value > schema["maximum"]:
            errors.append(f"{path}: {value} 超过 maximum={schema['maximum']}")
    return errors


def get_path(args: dict, path: str):
    """判据路径:sections.#0.kind -> args['sections'][0]['kind']。"""
    cur = args
    for seg in path.split("."):
        if seg.startswith("#"):
            if not isinstance(cur, list):
                return None, False
            idx = int(seg[1:])
            if idx >= len(cur):
                return None, False
            cur = cur[idx]
        else:
            if not isinstance(cur, dict) or seg not in cur:
                return None, False
            cur = cur[seg]
    return cur, True


def check_op(args: dict, check: dict) -> tuple:
    op = check["op"]
    if op == "optional_enum":
        val, ok = get_path(args, check["path"])
        if not ok:
            return True, ""
        if val in check["value"]:
            return True, ""
        return False, f"{check['path']}={val!r} 不在可选枚举 {check['value']}"
    val, ok = get_path(args, check["path"])
    if not ok:
        return False, f"缺字段 {check['path']}"
    if op == "eq":
        return (val == check["value"], "" if val == check["value"] else f"{check['path']}={val!r} != {check['value']!r}")
    if op == "eq_number":
        try:
            num = float(val)
        except (TypeError, ValueError):
            return False, f"{check['path']}={val!r} 不是数"
        return (abs(num - float(check["value"])) < 0.001, f"{check['path']}={val!r} != {check['value']}")
    if op == "contains_ci":
        hay = str(val).lower()
        hit = [v for v in check["value"] if str(v).lower() in hay]
        return (bool(hit), "" if hit else f"{check['path']}={val!r} 不含 {check['value']}")
    if op == "array_contains_ci":
        if not isinstance(val, list):
            return False, f"{check['path']} 不是数组"
        lower = [str(v).lower() for v in val]
        miss = [v for v in check["value"] if str(v).lower() not in lower]
        return (not miss, f"{check['path']} 缺列 {miss}")
    if op == "min_len":
        n = len(val) if isinstance(val, (list, str)) else -1
        return (n >= check["value"], f"{check['path']} 长度 {n} < {check['value']}")
    return False, f"未知判据 op {op}"


# ---------------------------------------------------------------- 轨迹解析

def parse_trajectory(main_jsonl: str, schemas: dict) -> dict:
    calls = []        # [(tool_name, effective_arguments)] 按 tool.execution 顺序
    search_count = 0
    turns = 0
    usage_total = {"input_tokens": 0, "cache_read_tokens": 0, "cache_creation_tokens": 0, "output_tokens": 0}
    usage_samples = 0
    cache_reported_any = False
    cache_epochs = set()
    with open(main_jsonl, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            evt = json.loads(line)
            kind = evt.get("kind", "")
            payload = evt.get("payload", {}) or {}
            if kind == "tool.input.effective":
                name = payload.get("tool_name", "")
                args = payload.get("effective_arguments", {}) or {}
                calls.append((name, args))
                if name == "tool_search":
                    search_count += 1
            elif kind == "model.request.prepared":
                turns += 1
                cache_epochs.add(payload.get("cache_epoch"))
            elif kind == "model.usage.recorded":
                usage_samples += 1
                for key in usage_total:
                    val = payload.get(key)
                    if isinstance(val, (int, float)):
                        usage_total[key] += val
                if payload.get("cache_reported_by_provider"):
                    cache_reported_any = True
    return {
        "calls": calls,
        "search_count": search_count,
        "turns": turns,
        "usage": usage_total,
        "usage_samples": usage_samples,
        "cache_reported": cache_reported_any,
        "cache_epochs": sorted(e for e in cache_epochs if e is not None),
    }


# ---------------------------------------------------------------- 判定

def judge_task(task: dict, traj: dict, schemas: dict) -> dict:
    calls = traj["calls"]
    call_names = [c[0] for c in calls]
    misselects = [n for n in call_names if n in set(task.get("misselect_targets", []))]

    required_results = []
    all_ok = True
    for req in task["required_calls"]:
        tool = req["tool"]
        occurrences = [(i, a) for i, (n, a) in enumerate(calls) if n == tool]
        schema = schemas.get(tool.split("__")[-1])
        first_shot_ok = None
        detail = []
        # 首次调用判定:checks + schema 都过才算首发合格
        if occurrences:
            _, first_args = occurrences[0]
            errs = [msg for c in req.get("checks", []) if not (r := check_op(first_args, c))[0] for msg in [r[1]]]
            schema_errs = schema_errors(schema, first_args) if schema else []
            first_shot_ok = not errs and not schema_errs
            detail = errs + schema_errs
        # 语义满足:任一次调用 checks 全过 + schema 全过
        semantic_ok = False
        for _, args in occurrences:
            errs = [msg for c in req.get("checks", []) if not (r := check_op(args, c))[0] for msg in [r[1]]]
            schema_errs = schema_errors(schema, args) if schema else []
            if not errs and not schema_errs:
                semantic_ok = True
                break
            detail = errs + schema_errs
        min_calls = req.get("min_calls", 1)
        count_ok = len(occurrences) >= min_calls
        ok = semantic_ok and count_ok
        all_ok = all_ok and ok
        required_results.append({
            "tool": tool,
            "calls_found": len(occurrences),
            "min_calls": min_calls,
            "semantic_ok": ok,
            "first_shot_ok": first_shot_ok,
            "first_shot_detail": detail[:4],
        })
    # 隔轮序判据(T6):ordering 字段手工判
    ordering_ok = True
    if "ordering" in task and task["id"] == "T6_reuse_after_gap":
        time_idx = [i for i, n in enumerate(call_names) if n == "mcp__demosuite__time_now_in_zone"]
        weather_idx = [i for i, n in enumerate(call_names) if n == "mcp__demosuite__weather_current"]
        ordering_ok = len(time_idx) >= 2 and bool(weather_idx) and time_idx[0] < weather_idx[0] < time_idx[-1]

    success = all_ok and not misselects and ordering_ok
    return {
        "success": success,
        "required": required_results,
        "misselect_count": len(misselects),
        "misselect_tools": sorted(set(misselects)),
        "ordering_ok": ordering_ok,
    }


# ---------------------------------------------------------------- 跑批

def run_task(mode: str, home: str, task: dict, work_dir: str, schemas: dict) -> dict:
    before = list_session_ids(home)
    if os.path.isdir(work_dir):
        for entry in os.listdir(work_dir):
            p = os.path.join(work_dir, entry)
            shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
    else:
        os.makedirs(work_dir, exist_ok=True)
    env = dict(os.environ)
    env["USERPROFILE"] = home
    env["PYTHONIOENCODING"] = "utf-8"
    t0 = time.time()
    failure = ""
    returncode = -1
    try:
        proc = subprocess.run(
            [EXE, "--yes", task["prompt"]],
            cwd=work_dir, env=env, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=TASK_TIMEOUT_SECS,
        )
        returncode = proc.returncode
        if returncode != 0:
            failure = "exit=%d; %s" % (returncode, (proc.stdout + proc.stderr)[-400:].replace("\n", " "))
    except subprocess.TimeoutExpired:
        failure = "timeout after %ds" % TASK_TIMEOUT_SECS
    wall = time.time() - t0
    after = list_session_ids(home)
    new = sorted(after - before)
    traj = {"calls": [], "search_count": 0, "turns": 0,
            "usage": {"input_tokens": None, "cache_read_tokens": None, "cache_creation_tokens": None, "output_tokens": None},
            "usage_samples": 0, "cache_reported": False, "cache_epochs": []}
    session_id = ""
    if new:
        root, session_id = new[-1]
        traj = parse_trajectory(os.path.join(root, session_id, "main.jsonl"), schemas)
    if not traj["calls"] and not failure:
        failure = "轨迹里没有任何工具调用"
    verdict = judge_task(task, traj, schemas)
    result = {
        "mode": mode,
        "task_id": task["id"],
        "shape": task["shape"],
        "session_id": session_id,
        "returncode": returncode,
        "failure": failure,
        "wall_seconds": round(wall, 1),
        "turns": traj["turns"],
        "tool_search_calls": traj["search_count"],
        "total_tool_calls": len(traj["calls"]),
        "call_sequence": [n for n, _ in traj["calls"]],
        "usage": traj["usage"],
        "usage_samples": traj["usage_samples"],
        "cache_reported_by_provider": traj["cache_reported"],
        "cache_epochs": traj["cache_epochs"],
    }
    result.update(verdict)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--modes", default="disabled,proxy_reference,legacy_expand")
    parser.add_argument("--tasks", default="all", help="all 或逗号分隔的任务 id")
    parser.add_argument("--out", default=os.path.join(HERE_EVAL, "results.json"))
    args = parser.parse_args()

    suite = json.load(open(os.path.join(HERE_EVAL, "tasks.json"), encoding="utf-8"))
    tasks = suite["tasks"] if args.tasks == "all" else [t for t in suite["tasks"] if t["id"] in args.tasks.split(",")]
    schemas, _stub = load_stub_tools()
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    os.makedirs(RUN_ROOT, exist_ok=True)
    work_dir = os.path.join(RUN_ROOT, "work")
    os.makedirs(work_dir, exist_ok=True)

    results = []
    for mode in modes:
        home = build_home(mode)
        print("== 档 %s(home=%s) ==" % (mode, home), flush=True)
        for task in tasks:
            print("  -> %s ..." % task["id"], end=" ", flush=True)
            res = run_task(mode, home, task, work_dir, schemas)
            results.append(res)
            print("%s | turns=%d searches=%d misselect=%d wall=%.0fs" % (
                "PASS" if res["success"] else "FAIL",
                res["turns"], res["tool_search_calls"], res["misselect_count"], res["wall_seconds"]), flush=True)

    with open(args.out, "w", encoding="utf-8") as f:
        json.dump({"suite": suite["suite"], "provider": PROVIDER_NAME, "temperature": TEMPERATURE,
                   "exe": EXE, "results": results}, f, ensure_ascii=False, indent=1)
    print("已写", args.out)

    # 汇总表
    print("\n任务            ", "  ".join("%-16s" % m for m in modes))
    for task in tasks:
        row = []
        for mode in modes:
            r = next((x for x in results if x["mode"] == mode and x["task_id"] == task["id"]), None)
            row.append("%-16s" % ("PASS" if r and r["success"] else "FAIL"))
        print("%-16s%s" % (task["id"], "  ".join(row)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
