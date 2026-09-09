#!/usr/bin/env python3
"""Provider 目录生成器(Provider 目录分片单第八节 A/B 批)。

维护源是 catalog/manifest.json + catalog/providers/*.json(公共模型池
catalog/models/*.json 由 B 批引入),本脚本把它们确定性合成发布产物
catalog/providers.json(schema v2,旧客户端与运行时唯一认识的格式)。

用法:
  python scripts/generate_provider_catalog.py            # 合成并原子替换产物
  python scripts/generate_provider_catalog.py --check    # 只读对账,不改文件
  python scripts/generate_provider_catalog.py --self-test# 生成器自测(临时目录夹具)

铁律(设计单第四节):合成只做展开与规范化,不改任何业务字段;revision
只在维护源里改;产物字节对同一份维护源恒定(不写时间戳、不遍历目录序);
校验全过才落盘,失败不动原产物。只依赖 Python 标准库,离线可跑。
"""

import argparse
import difflib
import json
import os
import re
import sys
import tempfile
import unittest

# ---------------------------------------------------------------------------
# 常量:与 catalog/providers.schema.json、src/config/provider_catalog.hpp 对齐。
# schema 演进时三处一起改,别让生成器与发布合同漂移。
# ---------------------------------------------------------------------------

SOURCE_SCHEMA_VERSION = 1
PUBLISH_SCHEMA_VERSION = 2
PRODUCT_RELPATH = "providers.json"
MANIFEST_RELPATH = "manifest.json"
MAX_PRODUCT_BYTES = 2 * 1024 * 1024  # kProviderCatalogMaxBytes:目录正文 2 MiB 帽

RE_PROVIDER_FILE = re.compile(r"^providers/[A-Za-z0-9][A-Za-z0-9._-]*\.json$")
RE_MODELS_FILE = re.compile(r"^models/[A-Za-z0-9][A-Za-z0-9._-]*\.json$")

WIRES = {
    "anthropic-messages",
    "openai-responses",
    "openai-chat-completions",
    "google-generate-content",
}
RE_BASE_URL = re.compile(r"^(https://|http://(localhost|127\.0\.0\.1)([:/]|$))")
RE_KEY_ENV = re.compile(r"^[A-Z_][A-Z0-9_]*$")
RE_REVISION = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
RE_DOCS_URL = re.compile(r"^https://")

# 发布格式(provider 级)的全部合法字段:providers.schema.json 的镜像。
PROVIDER_REQUIRED = ("name", "wire", "base_url", "key_env", "default_model", "models")
PROVIDER_FIELDS = frozenset(PROVIDER_REQUIRED) | {
    "description",
    "model_reasoning_effort",
    "native_web_search",
    "stream_usage",
    "reasoning_replay",
    "reasoning_delta_field",
    "reasoning_replay_field",
    "reasoning_dialect",
    "docs_url",
    "extra_body",
    "extra_headers",
}
# 发布格式(模型级)的全部合法字段;维护层专用字段(evidence/aliases)另行剥除。
MODEL_REQUIRED = ("name",)
MODEL_FIELDS = frozenset(MODEL_REQUIRED) | {
    "description",
    "context_window",
    "max_output",
    "default_think",
    "capabilities",
    "deferred_tools",
    "reasoning",
}


class CatalogError(Exception):
    """维护源或合成结果不合合同。message 带文件与 JSON 路径,便于指认。"""


# ---------------------------------------------------------------------------
# 读取与序列化:固定 UTF-8、LF、indent=2;显式拒绝重复 JSON 键。
# ---------------------------------------------------------------------------


def load_json_strict(path, label):
    """读一份 JSON 文件;重复键、非法 UTF-8、语法错都按 CatalogError 报。"""

    def no_dup_keys(pairs):
        seen = set()
        for key, _ in pairs:
            if key in seen:
                raise CatalogError("%s: 重复 JSON 键 %r" % (label, key))
            seen.add(key)
        return dict(pairs)

    try:
        with open(path, "rb") as handle:
            raw = handle.read()
    except OSError as exc:
        raise CatalogError("%s: 打不开(%s)" % (label, exc))
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise CatalogError("%s: 不是合法 UTF-8(%s)" % (label, exc))
    try:
        return json.loads(text, object_pairs_hook=no_dup_keys)
    except json.JSONDecodeError as exc:
        raise CatalogError("%s: JSON 解析失败:%s" % (label, exc))


def canonical_json(value):
    """唯一合法产物排版:indent=2、ensure_ascii=False、末尾一个 LF。"""

    return json.dumps(value, indent=2, ensure_ascii=False) + "\n"


def resolve_catalog_path(catalog_dir, relpath, label):
    """把 manifest 里的相对路径钉在 catalog/ 内:拒绝对路径、..、反斜杠、
    越界符号链接。正则先收口,realpath 再兜底。"""
    if not isinstance(relpath, str):
        raise CatalogError("%s: 路径必须是字符串" % label)
    if "\\" in relpath or os.path.isabs(relpath):
        raise CatalogError("%s: 路径 %r 必须是 catalog/ 内的正斜杠相对路径" % (label, relpath))
    # 按 POSIX 段语义判穿越;不能用 os.path.normpath——Windows 会把斜杠翻成
    # 反斜杠,正路也被冤枉成越界。
    if any(part in ("", ".", "..") for part in relpath.split("/")):
        raise CatalogError("%s: 路径 %r 含目录穿越" % (label, relpath))
    target = os.path.join(catalog_dir, *relpath.split("/"))
    catalog_real = os.path.realpath(catalog_dir)
    target_real = os.path.realpath(target)
    if os.path.commonpath([catalog_real, target_real]) != catalog_real:
        raise CatalogError("%s: 路径 %r 越出 catalog/" % (label, relpath))
    return target


# ---------------------------------------------------------------------------
# 校验:发布字段的形状检查(镜像 providers.schema.json 的要害条目)。
# C++ 解析器(test_provider_catalog.cpp)是最终门,这里是维护侧的第一道。
# ---------------------------------------------------------------------------


def _require_string(obj, key, where, min_length=0):
    value = obj[key]
    if not isinstance(value, str) or len(value) < min_length:
        raise CatalogError("%s.%s 必须是长度 >= %d 的字符串" % (where, key, min_length))
    return value


def validate_model(model_id, model, where):
    """模型对象:封闭字段集 + 类型要害。返回可入产物的纯发布对象。"""
    if not isinstance(model, dict):
        raise CatalogError("%s 必须是 JSON object" % where)
    unknown = set(model) - MODEL_FIELDS
    if unknown:
        raise CatalogError("%s 有未知字段 %s(发布 schema 不认识)" % (where, sorted(unknown)))
    for key in MODEL_REQUIRED:
        if key not in model:
            raise CatalogError("%s 缺必填字段 %s" % (where, key))
    _require_string(model, "name", where, 1)
    if "description" in model and not isinstance(model["description"], str):
        raise CatalogError("%s.description 必须是字符串" % where)
    if "context_window" in model:
        value = model["context_window"]
        if isinstance(value, bool) or not (isinstance(value, int) and value >= 1) and not isinstance(value, str):
            raise CatalogError("%s.context_window 必须是正整数或字符串" % where)
    if "max_output" in model:
        value = model["max_output"]
        if isinstance(value, bool) or not isinstance(value, int) or value < 1:
            raise CatalogError("%s.max_output 必须是 >= 1 的整数" % where)
    if "default_think" in model and not isinstance(model["default_think"], str):
        raise CatalogError("%s.default_think 必须是字符串" % where)
    if "capabilities" in model:
        caps = model["capabilities"]
        if not isinstance(caps, dict) or any(not isinstance(v, bool) for v in caps.values()):
            raise CatalogError("%s.capabilities 必须是布尔值映射" % where)
    return model


def validate_provider(provider_id, provider, where):
    """Provider 对象:封闭字段集 + 端点/模型引用一致性。"""
    if not isinstance(provider, dict):
        raise CatalogError("%s 必须是 JSON object" % where)
    unknown = set(provider) - PROVIDER_FIELDS
    if unknown:
        raise CatalogError("%s 有未知字段 %s(发布 schema 不认识)" % (where, sorted(unknown)))
    for key in PROVIDER_REQUIRED:
        if key not in provider:
            raise CatalogError("%s 缺必填字段 %s" % (where, key))
    _require_string(provider, "name", where, 1)
    if provider["wire"] not in WIRES:
        raise CatalogError("%s.wire %r 不在四种协议里" % (where, provider["wire"]))
    if not RE_BASE_URL.match(provider["base_url"]):
        raise CatalogError("%s.base_url 必须是 HTTPS,或 localhost/127.0.0.1 回环" % where)
    if not RE_KEY_ENV.match(provider["key_env"]):
        raise CatalogError("%s.key_env 必须形如 OPENAI_API_KEY" % where)
    if "docs_url" in provider and not RE_DOCS_URL.match(provider["docs_url"]):
        raise CatalogError("%s.docs_url 必须 HTTPS" % where)
    if "reasoning_replay" in provider and provider["reasoning_replay"] not in ("never", "tool_episode"):
        raise CatalogError("%s.reasoning_replay 只认 never/tool_episode" % where)
    if "native_web_search" in provider and not isinstance(provider["native_web_search"], bool):
        raise CatalogError("%s.native_web_search 必须是布尔" % where)
    if "stream_usage" in provider and not isinstance(provider["stream_usage"], bool):
        raise CatalogError("%s.stream_usage 必须是布尔" % where)
    if "reasoning_delta_field" in provider:
        _require_string(provider, "reasoning_delta_field", where, 1)
    if "reasoning_replay_field" in provider:
        _require_string(provider, "reasoning_replay_field", where, 1)
    if "extra_headers" in provider:
        headers = provider["extra_headers"]
        if not isinstance(headers, dict) or any(not isinstance(v, str) for v in headers.values()):
            raise CatalogError("%s.extra_headers 必须是字符串映射" % where)
    if "extra_body" in provider and not isinstance(provider["extra_body"], dict):
        raise CatalogError("%s.extra_body 必须是 JSON object" % where)
    models = provider["models"]
    if not isinstance(models, dict) or not models:
        raise CatalogError("%s.models 必须是非空 JSON object" % where)
    for model_id, model in models.items():
        validate_model(model_id, model, "%s.models.%s" % (where, model_id))
    if provider["default_model"] not in models:
        raise CatalogError("%s.default_model %r 不在 models 中" % (where, provider["default_model"]))
    return provider


# ---------------------------------------------------------------------------
# 合成:manifest 顺序读分片,直接形态(第一批)整对象入册。
# ---------------------------------------------------------------------------


def load_manifest(catalog_dir):
    where = MANIFEST_RELPATH
    manifest = load_json_strict(os.path.join(catalog_dir, MANIFEST_RELPATH), where)
    if not isinstance(manifest, dict):
        raise CatalogError("%s 必须是 JSON object" % where)
    unknown = set(manifest) - {"source_schema_version", "revision", "shards"}
    if unknown:
        raise CatalogError("%s 有未知字段 %s" % (where, sorted(unknown)))
    if manifest.get("source_schema_version") != SOURCE_SCHEMA_VERSION:
        raise CatalogError(
            "%s.source_schema_version 须为 %d(维护格式演进须同步改本生成器)"
            % (where, SOURCE_SCHEMA_VERSION)
        )
    for key in ("revision", "shards"):
        if key not in manifest:
            raise CatalogError("%s 缺必填字段 %s" % (where, key))
    if not RE_REVISION.match(manifest["revision"]):
        raise CatalogError("%s.revision 必须是 YYYY-MM-DD" % where)
    shards = manifest["shards"]
    if not isinstance(shards, list) or not shards:
        raise CatalogError("%s.shards 必须是非空数组" % where)
    seen = set()
    for entry in shards:
        if not isinstance(entry, str) or not RE_PROVIDER_FILE.match(entry):
            raise CatalogError("%s.shards 条目 %r 须形如 providers/<名字>.json" % (where, entry))
        if entry in seen:
            raise CatalogError("%s.shards 重复引用 %r" % (where, entry))
        seen.add(entry)
    return manifest


def load_shard(catalog_dir, relpath):
    shard = load_json_strict(resolve_catalog_path(catalog_dir, relpath, MANIFEST_RELPATH), relpath)
    if not isinstance(shard, dict):
        raise CatalogError("%s 必须是 JSON object" % relpath)
    unknown = set(shard) - {"platform", "providers"}
    if unknown:
        raise CatalogError("%s 有未知字段 %s" % (relpath, sorted(unknown)))
    expected_stem = os.path.basename(relpath)[: -len(".json")]
    if shard.get("platform") != expected_stem:
        raise CatalogError(
            "%s.platform 须为 %r(与文件名一致,便于指认)" % (relpath, expected_stem)
        )
    if "providers" not in shard or not isinstance(shard["providers"], dict) or not shard["providers"]:
        raise CatalogError("%s.providers 必须是非空 JSON object" % relpath)
    return shard


def synth_product(catalog_dir):
    """合成发布产物。返回 (产物文本, 统计信息)。任何校验失败抛 CatalogError。"""
    manifest = load_manifest(catalog_dir)
    providers = {}  # 保序:manifest 分片顺序 × 片内键序 = 产物文档序
    for relpath in manifest["shards"]:
        shard = load_shard(catalog_dir, relpath)
        for provider_id, provider in shard["providers"].items():
            where = "%s.providers.%s" % (relpath, provider_id)
            if provider_id in providers:
                raise CatalogError(
                    "%s: Provider ID %r 与先前分片重复" % (where, provider_id)
                )
            providers[provider_id] = validate_provider(provider_id, provider, where)
    check_unregistered_files(catalog_dir, set(manifest["shards"]))
    product = {
        "schema_version": PUBLISH_SCHEMA_VERSION,
        "revision": manifest["revision"],
        "providers": providers,
    }
    text = canonical_json(product)
    size = len(text.encode("utf-8"))
    if size > MAX_PRODUCT_BYTES:
        raise CatalogError(
            "合成产物 %d 字节,超过 2 MiB 上限(kProviderCatalogMaxBytes)" % size
        )
    stats = {
        "providers": len(providers),
        "models": sum(len(p["models"]) for p in providers.values()),
        "bytes": size,
        "revision": manifest["revision"],
    }
    return text, stats


def check_unregistered_files(catalog_dir, registered_shards):
    """文件新增了却没进 manifest,是静默漏发的头号来源——这里直接红。"""
    referenced = set(registered_shards)
    providers_dir = os.path.join(catalog_dir, "providers")
    if os.path.isdir(providers_dir):
        for name in sorted(os.listdir(providers_dir)):
            if not name.endswith(".json"):
                continue
            relpath = "providers/" + name
            if relpath not in referenced:
                raise CatalogError(
                    "catalog/%s 未登记进 manifest.shards(要么登记,要么删文件)"
                    % relpath
                )
    models_dir = os.path.join(catalog_dir, "models")
    if os.path.isdir(models_dir):
        for name in sorted(os.listdir(models_dir)):
            if name.endswith(".json"):
                raise CatalogError(
                    "catalog/models/%s 存在但当前维护格式(v%d)没有公共模型池引用,"
                    "先登记引用或挪走" % (name, SOURCE_SCHEMA_VERSION)
                )


def write_product_atomic(catalog_dir, text):
    """全量校验过的文本才走到这里:临时文件 + os.replace,失败不留半套。"""
    product_path = os.path.join(catalog_dir, PRODUCT_RELPATH)
    fd, tmp_path = tempfile.mkstemp(dir=catalog_dir, prefix=".providers.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_path, product_path)
    except BaseException:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        raise


def run_generate(catalog_dir):
    text, stats = synth_product(catalog_dir)
    write_product_atomic(catalog_dir, text)
    return stats


def run_check(catalog_dir):
    """只读对账:合成一遍,与在库产物逐字节比。不一致时指出第一处。"""
    text, stats = synth_product(catalog_dir)
    product_path = os.path.join(catalog_dir, PRODUCT_RELPATH)
    label = PRODUCT_RELPATH
    if not os.path.exists(product_path):
        raise CatalogError("catalog/%s 不存在:先跑生成(不带 --check)" % label)
    current = open(product_path, "rb").read()
    expected = text.encode("utf-8")
    if current != expected:
        current_text = current.decode("utf-8", errors="replace")
        hint = first_diff_hint(current_text, text)
        raise CatalogError(
            "catalog/%s 与维护源不合(改了分片要重新生成并连同产物提交)。%s"
            % (label, hint)
        )
    return stats


def first_diff_hint(current_text, expected_text):
    current_lines = current_text.split("\n")
    expected_lines = expected_text.split("\n")
    matcher = difflib.SequenceMatcher(None, current_lines, expected_lines, autojunk=False)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag == "equal":
            continue
        return "第一处差异在第 %d 行附近:在库为 %r,合成为 %r" % (
            i1 + 1,
            current_lines[i1][:80] if i1 < len(current_lines) else "<EOF>",
            expected_lines[j1][:80] if j1 < len(expected_lines) else "<EOF>",
        )
    return "(仅末尾换行或字节差异)"


# ---------------------------------------------------------------------------
# 自测:临时目录夹具,覆盖单子第八节 A/B 批点名的失败形态。
# ---------------------------------------------------------------------------


def make_catalog(root, shards, revision="2026-09-09", shard_names=None):
    """搭一个最小维护源。shards: {相对名: {"platform": ..., "providers": {...}}}"""
    catalog_dir = os.path.join(root, "catalog")
    os.makedirs(os.path.join(catalog_dir, "providers"), exist_ok=True)
    names = shard_names or list(shards)
    manifest = {
        "source_schema_version": SOURCE_SCHEMA_VERSION,
        "revision": revision,
        "shards": ["providers/%s" % name for name in names],
    }
    with open(os.path.join(catalog_dir, MANIFEST_RELPATH), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(canonical_json(manifest))
    for name, body in shards.items():
        path = os.path.join(catalog_dir, "providers", name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(canonical_json(body))
    return catalog_dir


def sample_provider(provider_id="demo", default_model="demo-1"):
    return {
        "name": "Demo",
        "wire": "openai-chat-completions",
        "base_url": "https://api.example.test/v1",
        "key_env": "DEMO_API_KEY",
        "default_model": default_model,
        "models": {
            default_model: {"name": "Demo One"},
        },
    }


class GeneratorSelfTest(unittest.TestCase):
    def setUp(self):
        import shutil
        import tempfile

        self._tempfile = tempfile.TemporaryDirectory()
        self.root = self._tempfile.name
        self.addCleanup(shutil.rmtree, self.root, True)

    # -- 正路 ---------------------------------------------------------------

    def test_generate_then_check_and_idempotent(self):
        catalog_dir = make_catalog(
            self.root,
            {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}},
        )
        stats = run_generate(catalog_dir)
        self.assertEqual(stats["providers"], 1)
        self.assertEqual(stats["models"], 1)
        # 再生成一遍,字节不动(干净重复生成没有差异)。
        before = open(os.path.join(catalog_dir, PRODUCT_RELPATH), "rb").read()
        run_generate(catalog_dir)
        after = open(os.path.join(catalog_dir, PRODUCT_RELPATH), "rb").read()
        self.assertEqual(before, after)
        run_check(catalog_dir)  # 不抛即过

    def test_product_shape_and_order(self):
        catalog_dir = make_catalog(
            self.root,
            {
                "aaa.json": {"platform": "aaa", "providers": {"aaa": sample_provider("aaa")}},
                "bbb.json": {"platform": "bbb", "providers": {"bbb": sample_provider("bbb")}},
            },
        )
        run_generate(catalog_dir)
        product = load_json_strict(os.path.join(catalog_dir, PRODUCT_RELPATH), PRODUCT_RELPATH)
        self.assertEqual(product["schema_version"], 2)
        self.assertEqual(list(product["providers"]), ["aaa", "bbb"])  # manifest 序

    # -- 重复键 / 重复 ID ---------------------------------------------------

    def test_duplicate_json_key_rejected(self):
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}}
        )
        path = os.path.join(catalog_dir, "providers", "demo.json")
        text = open(path, encoding="utf-8").read().replace(
            '"name": "Demo One"', '"name": "Demo One", "name": "Demo Dup"'
        )
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("重复 JSON 键", str(ctx.exception))

    def test_duplicate_provider_id_rejected(self):
        catalog_dir = make_catalog(
            self.root,
            {
                "one.json": {"platform": "one", "providers": {"demo": sample_provider()}},
                "two.json": {"platform": "two", "providers": {"demo": sample_provider()}},
            },
        )
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("重复", str(ctx.exception))

    # -- 缺件 / 未登记 ------------------------------------------------------

    def test_missing_shard_rejected(self):
        catalog_dir = make_catalog(
            self.root,
            {
                "demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}},
                "ghost.json": {"platform": "ghost", "providers": {}},
            },
        )
        os.unlink(os.path.join(catalog_dir, "providers", "ghost.json"))
        with self.assertRaises(CatalogError):
            run_generate(catalog_dir)

    def test_duplicate_shard_entry_rejected(self):
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}}
        )
        manifest_path = os.path.join(catalog_dir, MANIFEST_RELPATH)
        manifest = json.load(open(manifest_path, encoding="utf-8"))
        manifest["shards"].append(manifest["shards"][0])
        with open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(canonical_json(manifest))
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("重复引用", str(ctx.exception))

    def test_unregistered_shard_file_rejected(self):
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}}
        )
        stray = os.path.join(catalog_dir, "providers", "stray.json")
        with open(stray, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(canonical_json({"platform": "stray", "providers": {"s": sample_provider("s")}}))
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("未登记", str(ctx.exception))

    # -- 路径越界 -----------------------------------------------------------

    def test_path_escape_rejected(self):
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}}
        )
        for evil in ("../providers/demo.json", "/etc/passwd", "providers\\demo.json", "providers/.."):
            manifest_path = os.path.join(catalog_dir, MANIFEST_RELPATH)
            manifest = json.load(open(manifest_path, encoding="utf-8"))
            manifest["shards"] = [evil]
            with open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(canonical_json(manifest))
            with self.assertRaises(CatalogError, msg=evil):
                run_generate(catalog_dir)

    def test_symlink_escape_rejected(self):
        outside = os.path.join(self.root, "outside.json")
        with open(outside, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(canonical_json({"platform": "evil", "providers": {"e": sample_provider("e")}}))
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}}
        )
        link = os.path.join(catalog_dir, "providers", "evil.json")
        try:
            os.symlink(outside, link)
        except (OSError, NotImplementedError):
            self.skipTest("本环境建不了符号链接")
        manifest_path = os.path.join(catalog_dir, MANIFEST_RELPATH)
        manifest = json.load(open(manifest_path, encoding="utf-8"))
        manifest["shards"].append("providers/evil.json")
        with open(manifest_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(canonical_json(manifest))
        with self.assertRaises(CatalogError):
            run_generate(catalog_dir)

    # -- 字段合同 -----------------------------------------------------------

    def test_default_model_must_exist(self):
        provider = sample_provider()
        provider["default_model"] = "missing"
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": provider}}}
        )
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("default_model", str(ctx.exception))

    def test_unknown_provider_and_model_fields_rejected(self):
        provider = sample_provider()
        provider["surprise"] = 1
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": provider}}}
        )
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("未知字段", str(ctx.exception))

        provider = sample_provider()
        provider["models"]["demo-1"]["surprise"] = 1
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": provider}}}
        )
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("未知字段", str(ctx.exception))

    def test_bad_wire_base_url_key_env_rejected(self):
        for key, value, needle in [
            ("wire", "smtp", "wire"),
            ("base_url", "ftp://x", "base_url"),
            ("key_env", "demo-key", "key_env"),
        ]:
            provider = sample_provider()
            provider[key] = value
            catalog_dir = make_catalog(
                self.root, {"demo.json": {"platform": "demo", "providers": {"demo": provider}}}
            )
            with self.assertRaises(CatalogError, msg=key):
                run_generate(catalog_dir)

    def test_empty_models_rejected(self):
        provider = sample_provider()
        provider["models"] = {}
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": provider}}}
        )
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("models", str(ctx.exception))

    def test_bad_revision_rejected(self):
        catalog_dir = make_catalog(
            self.root,
            {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}},
            revision="2026-9-9",
        )
        with self.assertRaises(CatalogError):
            run_generate(catalog_dir)

    # -- 产物帽与失败不改产物 ---------------------------------------------

    def test_size_cap_enforced(self):
        provider = sample_provider()
        provider["description"] = "x" * (MAX_PRODUCT_BYTES + 8)
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": provider}}}
        )
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("2 MiB", str(ctx.exception))

    def test_failed_generate_keeps_product(self):
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}}
        )
        run_generate(catalog_dir)
        product_path = os.path.join(catalog_dir, PRODUCT_RELPATH)
        good = open(product_path, "rb").read()
        shard_path = os.path.join(catalog_dir, "providers", "demo.json")
        shard = json.load(open(shard_path, encoding="utf-8"))
        shard["providers"]["demo"]["wire"] = "broken"
        with open(shard_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(canonical_json(shard))
        with self.assertRaises(CatalogError):
            run_generate(catalog_dir)
        self.assertEqual(open(product_path, "rb").read(), good)  # 原产物一字不动

    def test_check_catches_stale_product(self):
        catalog_dir = make_catalog(
            self.root, {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}}
        )
        run_generate(catalog_dir)
        product_path = os.path.join(catalog_dir, PRODUCT_RELPATH)
        text = open(product_path, encoding="utf-8").read().replace('"Demo One"', '"Demo Old"')
        with open(product_path, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        with self.assertRaises(CatalogError) as ctx:
            run_check(catalog_dir)
        self.assertIn("与维护源不合", str(ctx.exception))


def main(argv=None):
    parser = argparse.ArgumentParser(description="Provider 目录确定性生成器(分片 → providers.json)")
    parser.add_argument("--check", action="store_true", help="只读对账:合成一遍与在库产物比,不改文件")
    parser.add_argument("--self-test", action="store_true", help="跑生成器自测(临时目录夹具)")
    args = parser.parse_args(argv)

    # Windows 控制台默认代码页可能压不住中文报错;统一按 UTF-8 出,压不住时替换而非崩。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(GeneratorSelfTest)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1

    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    catalog_dir = os.path.join(repo_root, "catalog")
    try:
        if args.check:
            stats = run_check(catalog_dir)
        else:
            stats = run_generate(catalog_dir)
    except CatalogError as exc:
        print("[catalog:%s] 失败:%s" % (action_name(vars(args)), exc), file=sys.stderr)
        return 1
    print(
        "[catalog:%s] %s:providers=%d models=%d bytes=%d revision=%s"
        % (action_name(vars(args)), "OK", stats["providers"], stats["models"], stats["bytes"], stats["revision"])
    )
    return 0


def action_name(args_dict):
    return "check" if args_dict.get("check") else "generate"


if __name__ == "__main__":
    sys.exit(main())
