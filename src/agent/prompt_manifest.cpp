#include "agent/prompt_manifest.hpp"

#include <algorithm>
#include <string_view>

namespace lubancode::agent {
namespace {

bool ReadString(const nlohmann::json& json, const char* key, std::string* out) {
    return json.contains(key) && json.at(key).is_string() &&
           (*out = json.at(key).get<std::string>(), true);
}

bool ReadInt64(const nlohmann::json& json, const char* key, std::int64_t* out) {
    return json.contains(key) && json.at(key).is_number_integer() &&
           (*out = json.at(key).get<std::int64_t>(), true);
}

}  // namespace

nlohmann::json PromptSegment::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["segment_id"] = segment_id;
    json["role"] = role;
    json["source_kind"] = source_kind;
    json["source_ref"] = source_ref;
    json["source_hash"] = source_hash.has_value() ? nlohmann::json(*source_hash)
                                                  : nlohmann::json(nullptr);
    json["rendered_hash"] = rendered_hash;
    json["rendered_tokens_estimated"] = rendered_tokens_estimated;
    json["order"] = order;
    json["volatile"] = volatile_segment;
    json["overrides"] = overrides;
    return json;
}

std::optional<PromptSegment> PromptSegment::FromJsonStrict(const nlohmann::json& json,
                                                           std::string* error) {
    if (!json.is_object()) {
        *error = "segment 须是 object";
        return std::nullopt;
    }
    PromptSegment segment;
    if (!ReadString(json, "segment_id", &segment.segment_id) ||
        !ReadString(json, "role", &segment.role) ||
        !ReadString(json, "source_kind", &segment.source_kind) ||
        !ReadString(json, "source_ref", &segment.source_ref) ||
        !ReadString(json, "rendered_hash", &segment.rendered_hash)) {
        *error = "segment 缺必填字段";
        return std::nullopt;
    }
    if (json.contains("source_hash") && !json.at("source_hash").is_null()) {
        if (!json.at("source_hash").is_string()) {
            *error = "source_hash 须是字符串或 null";
            return std::nullopt;
        }
        segment.source_hash = json.at("source_hash").get<std::string>();
    }
    if (!ReadInt64(json, "rendered_tokens_estimated", &segment.rendered_tokens_estimated) ||
        segment.rendered_tokens_estimated < 0) {
        *error = "rendered_tokens_estimated 须是非负整数";
        return std::nullopt;
    }
    if (!json.contains("order") || !json.at("order").is_number_integer()) {
        *error = "order 须是整数";
        return std::nullopt;
    }
    segment.order = json.at("order").get<int>();
    if (!json.contains("volatile") || !json.at("volatile").is_boolean()) {
        *error = "volatile 须是 bool";
        return std::nullopt;
    }
    segment.volatile_segment = json.at("volatile").get<bool>();
    if (json.contains("overrides") && json.at("overrides").is_array()) {
        for (const auto& item : json.at("overrides")) {
            if (!item.is_string()) {
                *error = "overrides 条目须是字符串";
                return std::nullopt;
            }
            segment.overrides.push_back(item.get<std::string>());
        }
    } else {
        *error = "overrides 须是数组";
        return std::nullopt;
    }
    if (json.size() != 10) {
        *error = "segment 未知键";
        return std::nullopt;
    }
    return segment;
}

nlohmann::json PromptManifest::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = kPromptManifestSchema;
    json["schema_version"] = kPromptManifestSchemaVersion;
    json["assembly_version"] = assembly_version;
    json["resolved_prompt_hash"] = resolved_prompt_hash;
    json["resolved_prompt_tokens_estimated"] = resolved_prompt_tokens_estimated;
    json["stable_prefix_hash"] = stable_prefix_hash;
    nlohmann::json segments_json = nlohmann::json::array();
    // 次序稳定是合同:写出前按 order 排,同 order 按 segment_id 兜底。
    std::vector<const PromptSegment*> sorted;
    sorted.reserve(this->segments.size());
    for (const auto& segment : this->segments) {
        sorted.push_back(&segment);
    }
    std::sort(sorted.begin(), sorted.end(), [](const PromptSegment* a, const PromptSegment* b) {
        if (a->order != b->order) {
            return a->order < b->order;
        }
        return a->segment_id < b->segment_id;
    });
    for (const PromptSegment* segment : sorted) {
        segments_json.push_back(segment->ToJson());
    }
    json["segments"] = std::move(segments_json);
    json["layers"] = layers;
    nlohmann::json soul_json = nlohmann::json::object();
    soul_json["name"] = soul.name;
    soul_json["hash"] = soul.hash;
    soul_json["tokens_estimated"] = soul.tokens_estimated;
    json["soul"] = std::move(soul_json);
    nlohmann::json instructions_json = nlohmann::json::object();
    instructions_json["hash"] = model_instructions.hash;
    instructions_json["tokens_estimated"] = model_instructions.tokens_estimated;
    json["model_instructions"] = std::move(instructions_json);
    return json;
}

std::optional<PromptManifest> PromptManifest::FromJsonStrict(const nlohmann::json& json,
                                                             std::string* error) {
    if (!json.is_object()) {
        *error = "manifest 须是 object";
        return std::nullopt;
    }
    PromptManifest manifest;
    std::string schema;
    if (!ReadString(json, "schema", &schema) || schema != kPromptManifestSchema) {
        *error = "schema 名不是 " + std::string(kPromptManifestSchema);
        return std::nullopt;
    }
    if (!json.contains("schema_version") || !json.at("schema_version").is_number_integer() ||
        json.at("schema_version").get<int>() != kPromptManifestSchemaVersion) {
        *error = "schema_version 只认 1";
        return std::nullopt;
    }
    if (!ReadString(json, "assembly_version", &manifest.assembly_version) ||
        !ReadString(json, "resolved_prompt_hash", &manifest.resolved_prompt_hash) ||
        !ReadString(json, "stable_prefix_hash", &manifest.stable_prefix_hash)) {
        *error = "manifest 缺必填字段";
        return std::nullopt;
    }
    if (!ReadInt64(json, "resolved_prompt_tokens_estimated",
                   &manifest.resolved_prompt_tokens_estimated) ||
        manifest.resolved_prompt_tokens_estimated < 0) {
        *error = "resolved_prompt_tokens_estimated 须是非负整数";
        return std::nullopt;
    }
    if (!json.contains("segments") || !json.at("segments").is_array()) {
        *error = "segments 须是数组";
        return std::nullopt;
    }
    for (const auto& item : json.at("segments")) {
        const auto segment = PromptSegment::FromJsonStrict(item, error);
        if (!segment.has_value()) {
            return std::nullopt;
        }
        manifest.segments.push_back(std::move(*segment));
    }
    if (!json.contains("layers") || !json.at("layers").is_array()) {
        *error = "layers 须是数组";
        return std::nullopt;
    }
    for (const auto& item : json.at("layers")) {
        if (!item.is_string()) {
            *error = "layers 条目须是字符串";
            return std::nullopt;
        }
        manifest.layers.push_back(item.get<std::string>());
    }
    if (!json.contains("soul") || !json.at("soul").is_object()) {
        *error = "soul 须是 object";
        return std::nullopt;
    }
    const auto& soul = json.at("soul");
    if (!ReadString(soul, "name", &manifest.soul.name) ||
        !ReadString(soul, "hash", &manifest.soul.hash) ||
        !ReadInt64(soul, "tokens_estimated", &manifest.soul.tokens_estimated) ||
        soul.size() != 3) {
        *error = "soul 三键(name/hash/tokens_estimated)缺一或多余";
        return std::nullopt;
    }
    if (!json.contains("model_instructions") || !json.at("model_instructions").is_object()) {
        *error = "model_instructions 须是 object";
        return std::nullopt;
    }
    const auto& instructions = json.at("model_instructions");
    if (!ReadString(instructions, "hash", &manifest.model_instructions.hash) ||
        !ReadInt64(instructions, "tokens_estimated",
                   &manifest.model_instructions.tokens_estimated) ||
        instructions.size() != 2) {
        *error = "model_instructions 两键(hash/tokens_estimated)缺一或多余";
        return std::nullopt;
    }
    if (json.size() != 10) {
        *error = "manifest 未知键";
        return std::nullopt;
    }
    return manifest;
}

nlohmann::json RequestSnapshotMetadata::RequestShape::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["model"] = model;
    json["message_count"] = message_count;
    json["tool_count"] = tool_count;
    json["parameters_hash"] = parameters_hash;
    json["toolset_hash"] = toolset_hash;
    json["tool_definition_tokens_estimated"] = tool_definition_tokens_estimated;
    return json;
}

std::optional<RequestSnapshotMetadata::RequestShape>
RequestSnapshotMetadata::RequestShape::FromJsonStrict(const nlohmann::json& json, std::string* error) {
    if (!json.is_object()) {
        *error = "request_shape 须是 object";
        return std::nullopt;
    }
    RequestShape shape;
    if (!ReadString(json, "model", &shape.model) ||
        !ReadString(json, "parameters_hash", &shape.parameters_hash) ||
        !ReadString(json, "toolset_hash", &shape.toolset_hash)) {
        *error = "request_shape 缺必填字段";
        return std::nullopt;
    }
    const auto read_uint = [&](const char* key, std::uint64_t* out) {
        return json.contains(key) && json.at(key).is_number_unsigned() &&
               (*out = json.at(key).get<std::uint64_t>(), true);
    };
    if (!read_uint("message_count", &shape.message_count) ||
        !read_uint("tool_count", &shape.tool_count)) {
        *error = "message_count/tool_count 须是无符号整数";
        return std::nullopt;
    }
    if (!ReadInt64(json, "tool_definition_tokens_estimated",
                   &shape.tool_definition_tokens_estimated) ||
        shape.tool_definition_tokens_estimated < 0) {
        *error = "tool_definition_tokens_estimated 须是非负整数";
        return std::nullopt;
    }
    if (json.size() != 6) {
        *error = "request_shape 未知键";
        return std::nullopt;
    }
    return shape;
}

nlohmann::json RequestSnapshotMetadata::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = kRequestSnapshotSchema;
    json["schema_version"] = kRequestSnapshotSchemaVersion;
    json["request_shape"] = request_shape.ToJson();
    json["prompt_manifest"] = prompt_manifest.ToJson();
    json["content_policy"] = content_policy;
    return json;
}

std::optional<RequestSnapshotMetadata> RequestSnapshotMetadata::FromJsonStrict(
    const nlohmann::json& json, std::string* error) {
    if (!json.is_object()) {
        *error = "snapshot 须是 object";
        return std::nullopt;
    }
    RequestSnapshotMetadata snapshot;
    std::string schema;
    if (!ReadString(json, "schema", &schema) || schema != kRequestSnapshotSchema) {
        *error = "schema 名不是 " + std::string(kRequestSnapshotSchema);
        return std::nullopt;
    }
    if (!json.contains("schema_version") || !json.at("schema_version").is_number_integer() ||
        json.at("schema_version").get<int>() != kRequestSnapshotSchemaVersion) {
        *error = "schema_version 只认 1";
        return std::nullopt;
    }
    if (!json.contains("request_shape") || !json.at("request_shape").is_object()) {
        *error = "request_shape 须是 object";
        return std::nullopt;
    }
    const auto shape = RequestShape::FromJsonStrict(json.at("request_shape"), error);
    if (!shape.has_value()) {
        return std::nullopt;
    }
    snapshot.request_shape = *shape;
    if (!json.contains("prompt_manifest") || !json.at("prompt_manifest").is_object()) {
        *error = "prompt_manifest 须是 object";
        return std::nullopt;
    }
    const auto manifest = PromptManifest::FromJsonStrict(json.at("prompt_manifest"), error);
    if (!manifest.has_value()) {
        return std::nullopt;
    }
    snapshot.prompt_manifest = std::move(*manifest);
    if (!ReadString(json, "content_policy", &snapshot.content_policy) ||
        snapshot.content_policy != "metadata_only") {
        *error = "content_policy A0 只认 metadata_only";
        return std::nullopt;
    }
    if (json.size() != 5) {
        *error = "snapshot 未知键";
        return std::nullopt;
    }
    return snapshot;
}

}  // namespace lubancode::agent
