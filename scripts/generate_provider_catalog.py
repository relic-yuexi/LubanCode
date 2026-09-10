#!/usr/bin/env python3
"""Provider 目录生成器(Provider 目录分片单第八节 A/B 批)。

维护源是 catalog/manifest.json + catalog/providers/*.json(端点形态的分片
引用 catalog/models/*.json 公共模型池),本脚本把它们确定性合成发布产物
catalog/providers.json(schema v2,旧客户端与运行时唯一认识的格式)。

用法:
  python scripts/generate_provider_catalog.py            # 合成并原子替换产物
  python scripts/generate_provider_catalog.py --check    # 只读对账,不改文件
  python scripts/generate_provider_catalog.py --self-test# 生成器自测(临时目录夹具)

铁律(设计单第四/六节):合成只做展开与规范化,不改任何业务字段;同平台多端点
的公共模型资料提到池里,端点差异留在端点;覆写按字段白名单合并,显式 false 是
值不是缺失;evidence/aliases 只住维护层,发布产物一个字节都不带;revision
只在维护源里改;产物字节对同一份维护源恒定;校验全过才落盘,失败不动原产物。
只依赖 Python 标准库,离线可跑。
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
# 常量:与 catalog/source.schema.json、catalog/providers.schema.json、
# src/config/provider_catalog.hpp 对齐。schema 演进时几处一起改。
# ---------------------------------------------------------------------------

SOURCE_SCHEMA_VERSION = 2
PUBLISH_SCHEMA_VERSION = 2
PRODUCT_RELPATH = "providers.json"
MANIFEST_RELPATH = "manifest.json"
MAX_PRODUCT_BYTES = 2 * 1024 * 1024  # kProviderCatalogMaxBytes:目录正文 2 MiB 帽

RE_PROVIDER_FILE = re.compile(r"^providers/[A-Za-z0-9][A-Za-z0-9._-]*\.json$")
RE_MODELS_FILE = re.compile(r"^models/[A-Za-z0-9][A-Za-z0-9._-]*\.json$")
RE_REVISION = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}$")
RE_BASE_URL = re.compile(r"^(https://|http://(localhost|127\.0\.0\.1)([:/]|$))")
RE_KEY_ENV = re.compile(r"^[A-Z_][A-Z0-9_]*$")
RE_DOCS_URL = re.compile(r"^https://")

WIRES = {
    "anthropic-messages",
    "openai-responses",
    "openai-chat-completions",
    "google-generate-content",
}

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
# 发布格式(模型级)的全部合法字段。
MODEL_REQUIRED = ("name",)
MODEL_FIELDS = frozenset(MODEL_REQUIRED) | {
    "description",
    "context_window",
    "max_context_window",
    "max_output",
    "default_think",
    "capabilities",
    "deferred_tools",
    "reasoning",
}
# 维护层专用字段:允许出现在维护源的模型条目里,合成时剥除,永不入产物。
MODEL_MAINTENANCE_FIELDS = frozenset({"evidence", "aliases"})
# 端点(维护形态)在 Provider 字段之外的键。
ENDPOINT_EXTRA_FIELDS = frozenset({"models", "endpoint_models", "model_overrides"})
ENDPOINT_FIELDS = (PROVIDER_FIELDS | ENDPOINT_EXTRA_FIELDS) - {"models"} | {"id", "models"}
# 覆写允许触碰的键 = 发布字段本身;capabilities/deferred_tools/reasoning 的
# 映射按类型定义合并,其余标量替换、数组整体替换。
REASONING_REPLACE_FIELDS = frozenset({"controls", "supportedEfforts", "wireDialect"})
REASONING_MERGE_FIELDS = frozenset({"dialect", "thinkingTokenLimits"})


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
    """把 manifest/分片里的相对路径钉在 catalog/ 内:拒绝对路径、..、反斜杠、
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


def validate_evidence(evidence, where):
    """维护层证据:来源、核对日期、类型分开记,不拿一个 true 冒充验证。"""
    if not isinstance(evidence, dict):
        raise CatalogError("%s.evidence 必须是 JSON object" % where)
    unknown = set(evidence) - {"source", "checked", "kind", "note"}
    if unknown:
        raise CatalogError("%s.evidence 有未知字段 %s" % (where, sorted(unknown)))
    if "source" not in evidence:
        raise CatalogError("%s.evidence 缺 source" % where)
    _require_string(evidence, "source", where + ".evidence", 1)
    if "checked" in evidence and not RE_REVISION.match(evidence["checked"]):
        raise CatalogError("%s.evidence.checked 必须是 YYYY-MM-DD" % where)
    for key in ("kind", "note"):
        if key in evidence and not isinstance(evidence[key], str):
            raise CatalogError("%s.evidence.%s 必须是字符串" % (where, key))


def validate_aliases(aliases, where):
    if not isinstance(aliases, list) or not aliases:
        raise CatalogError("%s.aliases 必须是非空字符串数组" % where)
    if any(not isinstance(a, str) or not a for a in aliases):
        raise CatalogError("%s.aliases 必须是非空字符串数组" % where)


def clean_model(model):
    """剥掉维护层专用字段,只留发布字段。"""
    return {key: value for key, value in model.items() if key not in MODEL_MAINTENANCE_FIELDS}


def validate_model(model_id, model, where, allow_maintenance=False):
    """模型对象:封闭字段集 + 类型要害 + 维护字段形状。返回剥净的发布对象。"""
    if not isinstance(model, dict):
        raise CatalogError("%s 必须是 JSON object" % where)
    allowed = MODEL_FIELDS | (MODEL_MAINTENANCE_FIELDS if allow_maintenance else frozenset())
    unknown = set(model) - allowed
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
    if "max_context_window" in model:
        value = model["max_context_window"]
        if isinstance(value, bool) or not isinstance(value, int) or value < 1:
            raise CatalogError("%s.max_context_window 必须是 >= 1 的整数" % where)
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
    if "evidence" in model:
        validate_evidence(model["evidence"], where)
    if "aliases" in model:
        validate_aliases(model["aliases"], where)
    return clean_model(model)


def validate_provider(provider_id, provider, where):
    """Provider 对象(发布形态):封闭字段集 + 端点/模型引用一致性。"""
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
    cleaned_models = {}
    for model_id, model in models.items():
        # 直接形态也允许携带 evidence/aliases 等维护字段;入产物前一律剥净。
        cleaned_models[model_id] = validate_model(
            model_id, model, "%s.models.%s" % (where, model_id), allow_maintenance=True
        )
    if provider["default_model"] not in models:
        raise CatalogError("%s.default_model %r 不在 models 中" % (where, provider["default_model"]))
    if cleaned_models != models:
        provider = dict(provider)
        provider["models"] = cleaned_models
    return provider


# ---------------------------------------------------------------------------
# 端点形态的覆写合并:公共模型 → 端点模型覆写,字段白名单,类型各自安放。
# ---------------------------------------------------------------------------


def merge_override(base, override, where):
    """把一份覆写并进基础模型对象(就地改 copy)。标量替换、数组整体替换、
    capabilities/deferred_tools 按键覆盖、reasoning 逐子字段处理。"""
    merged = dict(base)
    for key, value in override.items():
        if key not in MODEL_FIELDS:
            raise CatalogError("%s: 覆写字段 %r 不在模型字段白名单里" % (where, key))
        if value is None:
            raise CatalogError("%s.%s: 覆写不认 null(删字段语义不存在,写 endpoint_models 整份)" % (where, key))
        if key == "capabilities":
            if not isinstance(value, dict) or any(not isinstance(v, bool) for v in value.values()):
                raise CatalogError("%s.capabilities 覆写必须是布尔值映射" % where)
            caps = dict(merged.get("capabilities", {}))
            caps.update(value)  # 按键覆盖,只加不改结构,不删键
            merged["capabilities"] = caps
        elif key == "deferred_tools":
            if not isinstance(value, dict):
                raise CatalogError("%s.deferred_tools 覆写必须是 JSON object" % where)
            tools = dict(merged.get("deferred_tools", {}))
            tools.update(value)
            merged["deferred_tools"] = tools
        elif key == "reasoning":
            if not isinstance(value, dict):
                raise CatalogError("%s.reasoning 覆写必须是 JSON object" % where)
            reasoning = dict(merged.get("reasoning", {}))
            for sub, sub_value in value.items():
                if sub_value is None:
                    raise CatalogError("%s.reasoning.%s: 覆写不认 null" % (where, sub))
                if sub in REASONING_MERGE_FIELDS:
                    if not isinstance(sub_value, dict):
                        raise CatalogError("%s.reasoning.%s 覆写必须是 JSON object" % (where, sub))
                    nested = dict(reasoning.get(sub, {}))
                    nested.update(sub_value)
                    reasoning[sub] = nested
                else:
                    reasoning[sub] = sub_value  # controls/supportedEfforts/wireDialect 整体替换
            merged["reasoning"] = reasoning
        else:
            merged[key] = value  # 标量/数组:整体替换
    return merged


# ---------------------------------------------------------------------------
# 合成:manifest 顺序读分片;直接形态整对象入册,端点形态按池+覆写展开。
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
    allowed = {"platform", "providers", "models_ref", "endpoints", "evidence"}
    unknown = set(shard) - allowed
    if unknown:
        raise CatalogError("%s 有未知字段 %s" % (relpath, sorted(unknown)))
    expected_stem = os.path.basename(relpath)[: -len(".json")]
    if shard.get("platform") != expected_stem:
        raise CatalogError(
            "%s.platform 须为 %r(与文件名一致,便于指认)" % (relpath, expected_stem)
        )
    if "evidence" in shard:
        validate_evidence(shard["evidence"], relpath)
    forms = [key for key in ("providers", "endpoints") if key in shard]
    if len(forms) != 1:
        raise CatalogError(
            "%s 须恰选一种形态:providers(直接,整旧对象)或 endpoints(平台+端点)" % relpath
        )
    if "models_ref" in shard:
        if forms[0] != "endpoints":
            raise CatalogError("%s.models_ref 只属于端点形态" % relpath)
        ref = shard["models_ref"]
        if not isinstance(ref, str) or not RE_MODELS_FILE.match(ref):
            raise CatalogError("%s.models_ref %r 须形如 models/<名字>.json" % (relpath, ref))
        if ref != "models/%s.json" % expected_stem:
            raise CatalogError(
                "%s.models_ref 须指向本平台池 models/%s.json(稳定命名空间,不共享)" % (relpath, expected_stem)
            )
    if forms[0] == "providers":
        if not isinstance(shard["providers"], dict) or not shard["providers"]:
            raise CatalogError("%s.providers 必须是非空 JSON object" % relpath)
    else:
        if not isinstance(shard["endpoints"], list) or not shard["endpoints"]:
            raise CatalogError("%s.endpoints 必须是非空数组" % relpath)
        if "models_ref" not in shard and not any(
            "endpoint_models" in ep for ep in shard["endpoints"] if isinstance(ep, dict)
        ):
            raise CatalogError(
                "%s: 端点形态要么带 models_ref 公共池,要么端点自带 endpoint_models" % relpath
            )
    return shard


def load_models_pool(catalog_dir, relpath):
    pool = load_json_strict(resolve_catalog_path(catalog_dir, relpath, "shard"), relpath)
    if not isinstance(pool, dict):
        raise CatalogError("%s 必须是 JSON object" % relpath)
    unknown = set(pool) - {"owner", "models", "evidence"}
    if unknown:
        raise CatalogError("%s 有未知字段 %s" % (relpath, sorted(unknown)))
    expected_owner = os.path.basename(relpath)[: -len(".json")]
    if pool.get("owner") != expected_owner:
        raise CatalogError("%s.owner 须为 %r(与文件名一致,稳定命名空间)" % (relpath, expected_owner))
    if "evidence" in pool:
        validate_evidence(pool["evidence"], relpath)
    models = pool.get("models")
    if not isinstance(models, dict) or not models:
        raise CatalogError("%s.models 必须是非空 JSON object" % relpath)
    cleaned = {}
    for model_id, model in models.items():
        cleaned[model_id] = validate_model(
            model_id, model, "%s.models.%s" % (relpath, model_id), allow_maintenance=True
        )
    return cleaned


def expand_endpoint(shard, endpoint, pool, where):
    """端点 → 旧 Provider 对象。字段序照端点书写序,models 在原位展开。"""
    if not isinstance(endpoint, dict):
        raise CatalogError("%s 必须是 JSON object" % where)
    unknown = set(endpoint) - ENDPOINT_FIELDS
    if unknown:
        raise CatalogError("%s 有未知字段 %s" % (where, sorted(unknown)))
    if "id" not in endpoint or not isinstance(endpoint["id"], str) or not endpoint["id"]:
        raise CatalogError("%s.id 必须是非空字符串(旧 Provider ID,用户配置认它)" % where)
    endpoint_id = endpoint["id"]
    where = "%s(%s)" % (where, endpoint_id)
    models_list = endpoint.get("models")
    if not isinstance(models_list, list) or not models_list:
        raise CatalogError("%s.models 必须是非空数组(端点模型集合,显式声明)" % where)
    seen = set()
    for mid in models_list:
        if not isinstance(mid, str) or not mid:
            raise CatalogError("%s.models 条目必须是非空字符串" % where)
        if mid in seen:
            raise CatalogError("%s.models 重复列出 %r" % (where, mid))
        seen.add(mid)
    endpoint_models = endpoint.get("endpoint_models", {})
    if not isinstance(endpoint_models, dict):
        raise CatalogError("%s.endpoint_models 必须是 JSON object" % where)
    overrides = endpoint.get("model_overrides", {})
    if not isinstance(overrides, dict):
        raise CatalogError("%s.model_overrides 必须是 JSON object" % where)
    for mid in overrides:
        if mid not in seen:
            raise CatalogError("%s.model_overrides.%s 覆写的模型不在端点 models 集合里" % (where, mid))

    expanded = {}
    for mid in models_list:
        in_pool = mid in pool
        in_endpoint = mid in endpoint_models
        if in_pool and in_endpoint:
            raise CatalogError(
                "%s: 模型 %r 同时在公共池与 endpoint_models(要差异走 model_overrides,"
                "要整份私有只留 endpoint_models)" % (where, mid)
            )
        if in_endpoint:
            base = validate_model(
                mid, endpoint_models[mid], "%s.endpoint_models.%s" % (where, mid), allow_maintenance=True
            )
        elif in_pool:
            base = pool[mid]
        else:
            raise CatalogError("%s: 模型 %r 既不在公共池也不在 endpoint_models" % (where, mid))
        if mid in overrides:
            base = merge_override(base, overrides[mid], "%s.model_overrides.%s" % (where, mid))
        expanded[mid] = validate_model(mid, base, "%s.models.%s" % (where, mid))

    provider = {}
    for key, value in endpoint.items():
        if key in ("id", "endpoint_models", "model_overrides"):
            continue
        if key == "models":
            provider["models"] = expanded
        else:
            provider[key] = value
    if "models" not in provider:  # 列表在上面拦过,这里兜底
        provider["models"] = expanded
    validate_provider(endpoint_id, provider, where)
    return endpoint_id, provider


def synth_product(catalog_dir):
    """合成发布产物。返回 (产物文本, 统计信息)。任何校验失败抛 CatalogError。"""
    manifest = load_manifest(catalog_dir)
    providers = {}  # 保序:manifest 分片顺序 × 片内键序 = 产物文档序
    models_refs = set()
    for relpath in manifest["shards"]:
        shard = load_shard(catalog_dir, relpath)
        if "models_ref" in shard:
            models_refs.add(shard["models_ref"])
        if "providers" in shard:
            for provider_id, provider in shard["providers"].items():
                where = "%s.providers.%s" % (relpath, provider_id)
                if provider_id in providers:
                    raise CatalogError("%s: Provider ID %r 与先前分片重复" % (where, provider_id))
                providers[provider_id] = validate_provider(provider_id, provider, where)
        else:
            pool = {}
            if "models_ref" in shard:
                pool = load_models_pool(catalog_dir, shard["models_ref"])
            for index, endpoint in enumerate(shard["endpoints"]):
                where = "%s.endpoints[%d]" % (relpath, index)
                endpoint_id, provider = expand_endpoint(shard, endpoint, pool, where)
                if endpoint_id in providers:
                    raise CatalogError("%s: Provider ID %r 与先前分片重复" % (where, endpoint_id))
                providers[endpoint_id] = provider
    check_unregistered_files(catalog_dir, set(manifest["shards"]), models_refs)
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


def check_unregistered_files(catalog_dir, registered_shards, referenced_models):
    """文件新增了却没进 manifest/没被引用,是静默漏发的头号来源——这里直接红。"""
    providers_dir = os.path.join(catalog_dir, "providers")
    if os.path.isdir(providers_dir):
        for name in sorted(os.listdir(providers_dir)):
            if not name.endswith(".json"):
                continue
            relpath = "providers/" + name
            if relpath not in registered_shards:
                raise CatalogError(
                    "catalog/%s 未登记进 manifest.shards(要么登记,要么删文件)" % relpath
                )
    models_dir = os.path.join(catalog_dir, "models")
    if os.path.isdir(models_dir):
        for name in sorted(os.listdir(models_dir)):
            if not name.endswith(".json"):
                continue
            relpath = "models/" + name
            if relpath not in referenced_models:
                raise CatalogError(
                    "catalog/%s 没被任何分片的 models_ref 引用(要么引用,要么删文件)" % relpath
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
# 自测:临时目录夹具,覆盖单子第八节 A/B 批点名的失败形态与合并规则。
# ---------------------------------------------------------------------------


def _shard_filename(name):
    return name if name.endswith(".json") else name + ".json"


def _shard_stem(name):
    return name[: -len(".json")] if name.endswith(".json") else name


def make_catalog(root, shards, revision="2026-09-09"):
    """搭一个最小维护源。

    shards: {分片名(带不带 .json 都行): 分片对象};分片对象配 ("MODELS", 池对象)
    元组时,池落成 catalog/models/<名>.json 并在分片里记 models_ref。
    """
    catalog_dir = os.path.join(root, "catalog")
    os.makedirs(os.path.join(catalog_dir, "providers"), exist_ok=True)
    names = []
    for name, body in shards.items():
        pool = None
        if isinstance(body, tuple):
            body, pool = body
        if pool is not None:
            os.makedirs(os.path.join(catalog_dir, "models"), exist_ok=True)
            pool_path = os.path.join(catalog_dir, "models", _shard_filename(name))
            with open(pool_path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(canonical_json(pool))
        names.append(name)
    manifest = {
        "source_schema_version": SOURCE_SCHEMA_VERSION,
        "revision": revision,
        "shards": ["providers/%s" % _shard_filename(name) for name in names],
    }
    with open(os.path.join(catalog_dir, MANIFEST_RELPATH), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(canonical_json(manifest))
    for name, body in shards.items():
        pool = None
        if isinstance(body, tuple):
            body, pool = body
        if pool is not None:
            body = dict(body)
            body.setdefault("models_ref", "models/%s.json" % _shard_stem(name))
        shard_path = os.path.join(catalog_dir, "providers", _shard_filename(name))
        with open(shard_path, "w", encoding="utf-8", newline="\n") as handle:
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


def sample_pool(platform="demo"):
    return {
        "owner": platform,
        "models": {
            "demo-1": {"name": "Demo One", "max_output": 4096},
            "demo-2": {"name": "Demo Two"},
        },
    }


def sample_endpoint_shard(platform="demo", overrides=None, endpoint_models=None, models=None):
    endpoint = {
        "id": platform,
        "name": "Demo",
        "wire": "openai-chat-completions",
        "base_url": "https://api.example.test/v1",
        "key_env": "DEMO_API_KEY",
        "default_model": "demo-1",
        "models": models or ["demo-1", "demo-2"],
    }
    if endpoint_models:
        endpoint["endpoint_models"] = endpoint_models
    if overrides:
        endpoint["model_overrides"] = overrides
    shard = {"platform": platform, "endpoints": [endpoint]}
    return shard


class DirectFormSelfTest(unittest.TestCase):
    """A 批直接形态的失败形态。"""

    def setUp(self):
        import shutil
        import tempfile

        self._tempfile = tempfile.TemporaryDirectory()
        self.root = self._tempfile.name
        self.addCleanup(shutil.rmtree, self.root, True)

    def test_generate_then_check_and_idempotent(self):
        catalog_dir = make_catalog(
            self.root,
            {"demo.json": {"platform": "demo", "providers": {"demo": sample_provider()}}},
        )
        stats = run_generate(catalog_dir)
        self.assertEqual(stats["providers"], 1)
        self.assertEqual(stats["models"], 1)
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
        for key, value in [("wire", "smtp"), ("base_url", "ftp://x"), ("key_env", "demo-key")]:
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


class EndpointFormSelfTest(unittest.TestCase):
    """B 批端点形态:展开、合并规则、引用与命名空间。"""

    def setUp(self):
        import shutil
        import tempfile

        self._tempfile = tempfile.TemporaryDirectory()
        self.root = self._tempfile.name
        self.addCleanup(shutil.rmtree, self.root, True)

    def generate(self, shards):
        catalog_dir = make_catalog(self.root, shards)
        run_generate(catalog_dir)
        product = load_json_strict(os.path.join(catalog_dir, PRODUCT_RELPATH), PRODUCT_RELPATH)
        return catalog_dir, product

    def test_pool_expansion_with_subset_and_private_models(self):
        shard = sample_endpoint_shard(models=["demo-1"])
        # demo-3 是端点私有模型(池里没有),demo-1 从池继承
        shard["endpoints"][0]["endpoint_models"] = {
            "demo-3": {"name": "Demo Three Private", "max_output": 8}
        }
        shard["endpoints"][0]["models"] = ["demo-1", "demo-3"]
        catalog_dir, product = self.generate({"demo.json": (shard, sample_pool())})
        models = product["providers"]["demo"]["models"]
        self.assertEqual(list(models), ["demo-1", "demo-3"])  # 端点声明序
        self.assertEqual(models["demo-1"], {"name": "Demo One", "max_output": 4096})
        self.assertEqual(models["demo-3"], {"name": "Demo Three Private", "max_output": 8})

    def test_overrides_scalar_array_map_false_missing(self):
        pool = sample_pool()
        pool["models"]["demo-1"] = {
            "name": "Demo One",
            "max_output": 4096,
            "default_think": "high",
            "capabilities": {"reasoning": True, "tools": True},
            "reasoning": {
                "controls": [{"kind": "effort", "values": ["low", "high"]}],
                "supportedEfforts": ["low", "high"],
                "dialect": {"effort_path": "reasoning.effort", "verified": False},
            },
        }
        overrides = {
            "demo-1": {
                "max_output": 8192,  # 标量替换
                "default_think": "",  # 标量可写空串(合同允许)
                "capabilities": {"reasoning": False, "vision": True},  # 按键覆盖,不删键
                "reasoning": {
                    "supportedEfforts": ["low"],  # 数组整体替换
                    "dialect": {"verified": True},  # 嵌套映射按键覆盖
                },
            }
        }
        shard = sample_endpoint_shard(overrides=overrides, models=["demo-1", "demo-2"])
        catalog_dir, product = self.generate({"demo.json": (shard, pool)})
        model = product["providers"]["demo"]["models"]["demo-1"]
        self.assertEqual(model["max_output"], 8192)
        self.assertEqual(model["default_think"], "")
        # capabilities:reasoning 被 false 盖(显式 false 是值),tools 继承,vision 新增
        self.assertEqual(
            model["capabilities"], {"reasoning": False, "tools": True, "vision": True}
        )
        self.assertEqual(model["reasoning"]["supportedEfforts"], ["low"])
        self.assertEqual(model["reasoning"]["controls"], [{"kind": "effort", "values": ["low", "high"]}])
        self.assertEqual(
            model["reasoning"]["dialect"],
            {"effort_path": "reasoning.effort", "verified": True},
        )
        # 未覆写的 demo-2 原样继承
        self.assertEqual(product["providers"]["demo"]["models"]["demo-2"]["name"], "Demo Two")

    def test_override_null_and_unknown_rejected(self):
        for bad in ({"demo-1": {"max_output": None}}, {"demo-1": {"surprise": 1}}):
            shard = sample_endpoint_shard(overrides=bad, models=["demo-1"])
            with self.assertRaises(CatalogError):
                self.generate({"demo.json": (shard, sample_pool())})
        # 覆写不在端点集合里的模型
        shard = sample_endpoint_shard(overrides={"ghost": {"max_output": 1}}, models=["demo-1"])
        with self.assertRaises(CatalogError) as ctx:
            self.generate({"demo.json": (shard, sample_pool())})
        self.assertIn("不在端点 models 集合", str(ctx.exception))

    def test_model_in_pool_and_endpoint_both_rejected(self):
        shard = sample_endpoint_shard(models=["demo-1"])
        shard["endpoints"][0]["endpoint_models"] = {"demo-1": {"name": "Demo One Again"}}
        with self.assertRaises(CatalogError) as ctx:
            self.generate({"demo.json": (shard, sample_pool())})
        self.assertIn("同时在公共池", str(ctx.exception))

    def test_unresolvable_model_rejected(self):
        shard = sample_endpoint_shard(models=["demo-1", "ghost"])
        with self.assertRaises(CatalogError) as ctx:
            self.generate({"demo.json": (shard, sample_pool())})
        self.assertIn("ghost", str(ctx.exception))

    def test_duplicate_model_in_list_rejected(self):
        shard = sample_endpoint_shard(models=["demo-1", "demo-1"])
        with self.assertRaises(CatalogError) as ctx:
            self.generate({"demo.json": (shard, sample_pool())})
        self.assertIn("重复列出", str(ctx.exception))

    def test_default_model_must_be_in_endpoint_set(self):
        shard = sample_endpoint_shard(models=["demo-2"])
        shard["endpoints"][0]["default_model"] = "demo-1"
        with self.assertRaises(CatalogError) as ctx:
            self.generate({"demo.json": (shard, sample_pool())})
        self.assertIn("default_model", str(ctx.exception))

    def test_endpoint_id_collision_rejected(self):
        shard = sample_endpoint_shard()
        other = sample_endpoint_shard(models=["demo-1"])
        other["platform"] = "other"
        other["endpoints"][0]["id"] = "demo"
        with self.assertRaises(CatalogError) as ctx:
            self.generate(
                {
                    "demo.json": (shard, sample_pool()),
                    "other.json": (other, {"owner": "other", "models": {"demo-1": {"name": "x"}}}),
                }
            )
        self.assertIn("重复", str(ctx.exception))

    def test_models_ref_rules(self):
        # 引用别人的池:命名空间不共享
        shard = sample_endpoint_shard()
        shard["models_ref"] = "models/someone-else.json"
        pool = {"owner": "someone-else", "models": {"demo-1": {"name": "Demo One"}}}
        with self.assertRaises(CatalogError) as ctx:
            self.generate({"demo.json": (shard, pool)})
        self.assertIn("本平台池", str(ctx.exception))
        # 池文件 owner 与文件名不合(子案各开新目录,别让前案残料串味)
        shard = sample_endpoint_shard()
        pool = {"owner": "not-demo", "models": {"demo-1": {"name": "Demo One"}}}
        with self.assertRaises(CatalogError):
            self.generate({"demo.json": (shard, pool)})
        # 引用的池文件不存在
        catalog_dir = make_catalog(
            os.path.join(self.root, "no-pool"),
            {"demo.json": dict(sample_endpoint_shard(), models_ref="models/demo.json")},
        )
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("打不开", str(ctx.exception))

    def test_unreferenced_models_file_rejected(self):
        catalog_dir = make_catalog(
            self.root,
            {"demo.json": (sample_endpoint_shard(), sample_pool())},
        )
        stray = os.path.join(catalog_dir, "models", "stray.json")
        os.makedirs(os.path.dirname(stray), exist_ok=True)
        with open(stray, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(canonical_json({"owner": "stray", "models": {"x": {"name": "X"}}}))
        with self.assertRaises(CatalogError) as ctx:
            run_generate(catalog_dir)
        self.assertIn("没被任何分片", str(ctx.exception))

    def test_evidence_and_aliases_stripped_from_product(self):
        pool = sample_pool()
        pool["models"]["demo-1"]["evidence"] = {
            "source": "https://docs.example.test/demo",
            "checked": "2026-09-09",
            "kind": "official-doc",
        }
        pool["models"]["demo-1"]["aliases"] = ["demo-1-latest"]
        shard = sample_endpoint_shard(models=["demo-1", "demo-2"])
        catalog_dir, product = self.generate({"demo.json": (shard, pool)})
        model = product["providers"]["demo"]["models"]["demo-1"]
        self.assertNotIn("evidence", model)
        self.assertNotIn("aliases", model)
        # 直接形态里写了维护字段也照样剥(另开目录,免得残留分片被未登记检查冤枉)
        direct = {"platform": "solo", "providers": {"solo": sample_provider("solo", "solo")}}
        direct["providers"]["solo"]["models"]["solo"]["aliases"] = ["solo-v2"]
        catalog_dir2 = make_catalog(os.path.join(self.root, "solo"), {"solo.json": direct})
        run_generate(catalog_dir2)
        product2 = load_json_strict(os.path.join(catalog_dir2, PRODUCT_RELPATH), PRODUCT_RELPATH)
        self.assertNotIn("aliases", product2["providers"]["solo"]["models"]["solo"])

    def test_evidence_shape_validated(self):
        pool = sample_pool()
        pool["models"]["demo-1"]["evidence"] = {"checked": "not-a-date"}
        shard = sample_endpoint_shard(models=["demo-1"])
        with self.assertRaises(CatalogError):
            self.generate({"demo.json": (shard, pool)})

    def test_cross_platform_same_name_kept_separate(self):
        pool_a = {"owner": "alpha", "models": {"shared": {"name": "Shared A", "max_output": 1}}}
        pool_b = {"owner": "beta", "models": {"shared": {"name": "Shared B", "max_output": 2}}}
        shard_a = sample_endpoint_shard(platform="alpha")
        shard_a["endpoints"][0]["id"] = "alpha"
        shard_a["endpoints"][0]["models"] = ["shared"]
        shard_a["endpoints"][0]["default_model"] = "shared"
        shard_b = sample_endpoint_shard(platform="beta")
        shard_b["endpoints"][0]["id"] = "beta"
        shard_b["endpoints"][0]["models"] = ["shared"]
        shard_b["endpoints"][0]["default_model"] = "shared"
        catalog_dir, product = self.generate(
            {"alpha.json": (shard_a, pool_a), "beta.json": (shard_b, pool_b)}
        )
        self.assertEqual(product["providers"]["alpha"]["models"]["shared"]["max_output"], 1)
        self.assertEqual(product["providers"]["beta"]["models"]["shared"]["max_output"], 2)

    def test_endpoint_field_order_preserved(self):
        shard = sample_endpoint_shard(models=["demo-1"])
        endpoint = shard["endpoints"][0]
        # 把 models 挪到 name 前面,产物里 models 也应在 name 前面
        reordered = {"id": endpoint["id"], "models": ["demo-1"]}
        for key, value in endpoint.items():
            if key not in ("id", "models"):
                reordered[key] = value
        shard["endpoints"][0] = reordered
        catalog_dir, product = self.generate({"demo.json": (shard, sample_pool())})
        keys = list(product["providers"]["demo"])
        self.assertEqual(keys.index("models"), 0)
        self.assertNotIn("endpoint_models", keys)
        self.assertNotIn("model_overrides", keys)

    def test_multi_endpoint_shard_with_two_wires(self):
        shard = {
            "platform": "demo",
            "evidence": {"source": "catalog/providers.json@bd68cea3", "checked": "2026-09-09", "kind": "migration-field-compare"},
            "endpoints": [
                {
                    "id": "demo",
                    "name": "Demo",
                    "wire": "openai-chat-completions",
                    "base_url": "https://api.example.test/v1",
                    "key_env": "DEMO_API_KEY",
                    "default_model": "demo-1",
                    "models": ["demo-1", "demo-2"],
                },
                {
                    "id": "demo-anthropic",
                    "name": "Demo (Anthropic)",
                    "wire": "anthropic-messages",
                    "base_url": "https://api.example.test/anthropic",
                    "key_env": "DEMO_API_KEY",
                    "default_model": "demo-2",
                    "models": ["demo-2"],
                },
            ],
        }
        catalog_dir, product = self.generate({"demo.json": (shard, sample_pool())})
        self.assertEqual(list(product["providers"]), ["demo", "demo-anthropic"])
        self.assertEqual(list(product["providers"]["demo"]["models"]), ["demo-1", "demo-2"])
        self.assertEqual(list(product["providers"]["demo-anthropic"]["models"]), ["demo-2"])


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
        loader = unittest.defaultTestLoader
        suite = unittest.TestSuite(
            [loader.loadTestsFromTestCase(cls) for cls in (DirectFormSelfTest, EndpointFormSelfTest)]
        )
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
        "[catalog:%s] OK:providers=%d models=%d bytes=%d revision=%s"
        % (
            action_name(vars(args)),
            stats["providers"],
            stats["models"],
            stats["bytes"],
            stats["revision"],
        )
    )
    return 0


def action_name(args_dict):
    return "check" if args_dict.get("check") else "generate"


if __name__ == "__main__":
    sys.exit(main())
