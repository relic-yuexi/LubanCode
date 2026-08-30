#include "accounting/usage_sample.hpp"

#include <string_view>

namespace lubancode::accounting {
namespace {

bool ReadString(const nlohmann::json& json, const char* key, std::string* out) {
    return json.contains(key) && json.at(key).is_string() &&
           (*out = json.at(key).get<std::string>(), true);
}

}  // namespace

const char* UsageSourceName(UsageSource source) {
    switch (source) {
        case UsageSource::ProviderReported:
            return "provider_reported";
        case UsageSource::Estimated:
            return "estimated";
        case UsageSource::Unknown:
            return "unknown";
    }
    return "";
}

std::optional<UsageSource> UsageSourceFromName(std::string_view name) {
    if (name == "provider_reported") return UsageSource::ProviderReported;
    if (name == "estimated") return UsageSource::Estimated;
    if (name == "unknown") return UsageSource::Unknown;
    return std::nullopt;
}

const char* CostStatusName(CostStatus status) {
    switch (status) {
        case CostStatus::Estimated:
            return "estimated";
        case CostStatus::ProviderReported:
            return "provider_reported";
        case CostStatus::NotPriced:
            return "not_priced";
        case CostStatus::NotApplicable:
            return "not_applicable";
    }
    return "";
}

std::optional<CostStatus> CostStatusFromName(std::string_view name) {
    if (name == "estimated") return CostStatus::Estimated;
    if (name == "provider_reported") return CostStatus::ProviderReported;
    if (name == "not_priced") return CostStatus::NotPriced;
    if (name == "not_applicable") return CostStatus::NotApplicable;
    return std::nullopt;
}

nlohmann::json CostEstimate::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["status"] = CostStatusName(status);
    if (!currency.empty()) {
        json["currency"] = currency;
    }
    json["micros"] = micros;
    if (!price_table_id.empty()) {
        json["price_table_id"] = price_table_id;
    }
    return json;
}

std::optional<CostEstimate> CostEstimate::FromJsonStrict(const nlohmann::json& json,
                                                         std::string* error) {
    if (!json.is_object()) {
        *error = "cost 须是 object";
        return std::nullopt;
    }
    CostEstimate cost;
    std::string status_name;
    if (!ReadString(json, "status", &status_name)) {
        *error = "cost.status 缺失或非字符串";
        return std::nullopt;
    }
    const auto status = CostStatusFromName(status_name);
    if (!status.has_value()) {
        *error = "cost.status 认不得: " + status_name;
        return std::nullopt;
    }
    cost.status = *status;
    ReadString(json, "currency", &cost.currency);
    if (json.contains("micros")) {
        if (!json.at("micros").is_number_integer()) {
            *error = "cost.micros 须是整数";
            return std::nullopt;
        }
        cost.micros = json.at("micros").get<std::int64_t>();
    }
    ReadString(json, "price_table_id", &cost.price_table_id);
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (it.key() != "status" && it.key() != "currency" && it.key() != "micros" &&
            it.key() != "price_table_id") {
            *error = "cost 未知键: " + it.key();
            return std::nullopt;
        }
    }
    return cost;
}

nlohmann::json SourceEventRef::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["stream"] = stream;
    json["event_id"] = event_id;
    json["event_hash"] = event_hash;
    return json;
}

std::optional<SourceEventRef> SourceEventRef::FromJsonStrict(const nlohmann::json& json,
                                                             std::string* error) {
    if (!json.is_object()) {
        *error = "source_event 须是 object";
        return std::nullopt;
    }
    SourceEventRef ref;
    if (!ReadString(json, "stream", &ref.stream) || !ReadString(json, "event_id", &ref.event_id) ||
        !ReadString(json, "event_hash", &ref.event_hash)) {
        *error = "source_event 缺 stream/event_id/event_hash";
        return std::nullopt;
    }
    if (json.size() != 3) {
        *error = "source_event 未知键";
        return std::nullopt;
    }
    return ref;
}

namespace {

// usage 五项(JSON 形态,unknown 时不写 usage 键,见 ToJson)。
std::optional<api::Usage> ParseUsageObject(const nlohmann::json& json, std::string* error) {
    if (!json.is_object()) {
        *error = "usage 须是 object";
        return std::nullopt;
    }
    api::Usage usage;
    const auto read_int = [&](const char* key, std::int64_t* out) {
        return json.contains(key) && json.at(key).is_number_integer() &&
               (*out = json.at(key).get<std::int64_t>(), true);
    };
    if (!read_int("input_tokens", &usage.input_tokens) ||
        !read_int("cache_read_tokens", &usage.cache_read_tokens) ||
        !read_int("cache_creation_tokens", &usage.cache_creation_tokens) ||
        !read_int("output_tokens", &usage.output_tokens) ||
        !read_int("reasoning_tokens", &usage.output_reasoning_tokens)) {
        *error = "usage 五项 token 字段缺一或非整数";
        return std::nullopt;
    }
    if (json.size() != 5) {
        *error = "usage 未知键";
        return std::nullopt;
    }
    if (usage.input_tokens < 0 || usage.cache_read_tokens < 0 || usage.cache_creation_tokens < 0 ||
        usage.output_tokens < 0 || usage.output_reasoning_tokens < 0) {
        *error = "usage token 字段不得为负";
        return std::nullopt;
    }
    // reasoning 是 output 的子集(§6.1 字段规矩)。
    if (usage.output_reasoning_tokens > usage.output_tokens) {
        *error = "reasoning_tokens 不得大于 output_tokens";
        return std::nullopt;
    }
    return usage;
}

nlohmann::json UsageToJson(const api::Usage& usage) {
    nlohmann::json json = nlohmann::json::object();
    json["input_tokens"] = usage.input_tokens;
    json["cache_read_tokens"] = usage.cache_read_tokens;
    json["cache_creation_tokens"] = usage.cache_creation_tokens;
    json["output_tokens"] = usage.output_tokens;
    json["reasoning_tokens"] = usage.output_reasoning_tokens;
    return json;
}

}  // namespace

nlohmann::json UsageSample::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = kUsageSampleSchema;
    json["schema_version"] = kUsageSampleSchemaVersion;
    json["workspace_key"] = workspace_key;
    json["session_id"] = session_id;
    json["run_id"] = run_id;
    json["run_kind"] = run_kind;
    if (turn_id.has_value()) {
        json["turn_id"] = *turn_id;
    }
    json["request_id"] = request_id;
    if (provider_response_id.has_value()) {
        json["provider_response_id"] = *provider_response_id;
    }
    json["attempt"] = attempt;
    // purpose:有则写线上名,缺则写 null(prepared 缺失是缺口,要点名)。
    if (purpose.has_value()) {
        json["purpose"] = PurposeName(*purpose);
    } else {
        json["purpose"] = nullptr;
    }
    json["provider"] = provider;
    json["wire"] = wire;
    json["model"] = model;
    json["usage_source"] = UsageSourceName(usage_source);
    if (usage.has_value()) {
        json["usage"] = UsageToJson(*usage);
        json["total_input_tokens"] = total_input_tokens;
        json["total_billed_shape_tokens"] = total_billed_shape_tokens;
    } else {
        json["usage"] = nullptr;
    }
    if (cache_epoch.has_value()) {
        json["cache_epoch"] = *cache_epoch;
    }
    if (prefix_append_only.has_value()) {
        json["prefix_append_only"] = *prefix_append_only;
    }
    json["cost"] = cost.ToJson();
    if (source_event.has_value()) {
        json["source_event"] = source_event->ToJson();
    }
    if (!request_outcome.empty()) {
        json["request_outcome"] = request_outcome;
    }
    if (incomplete_linkage) {
        json["incomplete_linkage"] = true;
    }
    if (legacy_owner) {
        json["legacy_owner"] = true;
    }
    if (legacy_inferred) {
        json["legacy_inferred"] = true;
    }
    return json;
}

std::optional<UsageSample> UsageSample::FromJsonStrict(const nlohmann::json& json,
                                                       std::string* error) {
    if (!json.is_object()) {
        *error = "sample 须是 object";
        return std::nullopt;
    }
    UsageSample sample;
    std::string schema;
    if (!ReadString(json, "schema", &schema) || schema != kUsageSampleSchema) {
        *error = "schema 名不是 " + std::string(kUsageSampleSchema);
        return std::nullopt;
    }
    if (!json.contains("schema_version") || !json.at("schema_version").is_number_integer() ||
        json.at("schema_version").get<int>() != kUsageSampleSchemaVersion) {
        *error = "schema_version 只认 1";
        return std::nullopt;
    }
    if (!ReadString(json, "workspace_key", &sample.workspace_key) ||
        !ReadString(json, "session_id", &sample.session_id) ||
        !ReadString(json, "run_id", &sample.run_id) ||
        !ReadString(json, "run_kind", &sample.run_kind) ||
        !ReadString(json, "request_id", &sample.request_id) ||
        !ReadString(json, "provider", &sample.provider) ||
        !ReadString(json, "wire", &sample.wire) || !ReadString(json, "model", &sample.model)) {
        *error = "身份字段缺一(workspace/session/run/run_kind/request/provider/wire/model)";
        return std::nullopt;
    }
    if (json.contains("turn_id")) {
        if (!json.at("turn_id").is_string()) {
            *error = "turn_id 须是字符串";
            return std::nullopt;
        }
        sample.turn_id = json.at("turn_id").get<std::string>();
    }
    if (json.contains("provider_response_id")) {
        if (!json.at("provider_response_id").is_string()) {
            *error = "provider_response_id 须是字符串";
            return std::nullopt;
        }
        sample.provider_response_id = json.at("provider_response_id").get<std::string>();
    }
    if (!json.contains("attempt") || !json.at("attempt").is_number_integer() ||
        json.at("attempt").get<int>() < 1) {
        *error = "attempt 须是 >= 1 的整数";
        return std::nullopt;
    }
    sample.attempt = json.at("attempt").get<int>();
    if (json.contains("purpose")) {
        const auto& purpose = json.at("purpose");
        if (purpose.is_null()) {
            // 缺 purpose 是明报的缺口,合法。
        } else if (purpose.is_string()) {
            const auto parsed = PurposeFromName(purpose.get<std::string>());
            if (!parsed.has_value()) {
                *error = "purpose 认不得: " + purpose.get<std::string>();
                return std::nullopt;
            }
            sample.purpose = *parsed;
        } else {
            *error = "purpose 须是字符串或 null";
            return std::nullopt;
        }
    }
    std::string source_name;
    if (!ReadString(json, "usage_source", &source_name)) {
        *error = "usage_source 缺失";
        return std::nullopt;
    }
    const auto source = UsageSourceFromName(source_name);
    if (!source.has_value()) {
        *error = "usage_source 认不得: " + source_name;
        return std::nullopt;
    }
    sample.usage_source = *source;
    // unknown ⇒ usage 必为 null;有 usage ⇒ 五项过检并与总数一致。
    if (json.contains("usage") && !json.at("usage").is_null()) {
        const auto usage = ParseUsageObject(json.at("usage"), error);
        if (!usage.has_value()) {
            return std::nullopt;
        }
        if (*source == UsageSource::Unknown) {
            *error = "usage_source=unknown 时不得带 usage 正文";
            return std::nullopt;
        }
        sample.usage = *usage;
        sample.total_input_tokens = api::TotalInputTokens(*usage);
        sample.total_billed_shape_tokens = sample.total_input_tokens + usage->output_tokens;
        if (json.contains("total_input_tokens")) {
            if (!json.at("total_input_tokens").is_number_integer() ||
                json.at("total_input_tokens").get<std::int64_t>() != sample.total_input_tokens) {
                *error = "total_input_tokens 与 usage 五项不合口径";
                return std::nullopt;
            }
        }
        if (json.contains("total_billed_shape_tokens")) {
            if (!json.at("total_billed_shape_tokens").is_number_integer() ||
                json.at("total_billed_shape_tokens").get<std::int64_t>() !=
                    sample.total_billed_shape_tokens) {
                *error = "total_billed_shape_tokens 与 usage 五项不合口径";
                return std::nullopt;
            }
        }
    } else if (*source != UsageSource::Unknown) {
        *error = "usage_source 非 unknown 时须带 usage 正文";
        return std::nullopt;
    }
    if (json.contains("cache_epoch")) {
        if (!json.at("cache_epoch").is_number_integer()) {
            *error = "cache_epoch 须是整数";
            return std::nullopt;
        }
        sample.cache_epoch = json.at("cache_epoch").get<int>();
    }
    if (json.contains("prefix_append_only")) {
        if (!json.at("prefix_append_only").is_boolean()) {
            *error = "prefix_append_only 须是 bool";
            return std::nullopt;
        }
        sample.prefix_append_only = json.at("prefix_append_only").get<bool>();
    }
    if (json.contains("cost")) {
        const auto cost = CostEstimate::FromJsonStrict(json.at("cost"), error);
        if (!cost.has_value()) {
            return std::nullopt;
        }
        sample.cost = *cost;
    }
    if (json.contains("source_event")) {
        const auto ref = SourceEventRef::FromJsonStrict(json.at("source_event"), error);
        if (!ref.has_value()) {
            return std::nullopt;
        }
        sample.source_event = *ref;
    }
    ReadString(json, "request_outcome", &sample.request_outcome);
    sample.incomplete_linkage = json.value("incomplete_linkage", false);
    sample.legacy_owner = json.value("legacy_owner", false);
    sample.legacy_inferred = json.value("legacy_inferred", false);
    for (auto it = json.begin(); it != json.end(); ++it) {
        static const char* kKnown[] = {"schema",
                                       "schema_version",
                                       "workspace_key",
                                       "session_id",
                                       "run_id",
                                       "run_kind",
                                       "turn_id",
                                       "request_id",
                                       "provider_response_id",
                                       "attempt",
                                       "purpose",
                                       "provider",
                                       "wire",
                                       "model",
                                       "usage_source",
                                       "usage",
                                       "total_input_tokens",
                                       "total_billed_shape_tokens",
                                       "cache_epoch",
                                       "prefix_append_only",
                                       "cost",
                                       "source_event",
                                       "request_outcome",
                                       "incomplete_linkage",
                                       "legacy_owner",
                                       "legacy_inferred"};
        bool known = false;
        for (const char* key : kKnown) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            *error = "sample 未知键: " + it.key();
            return std::nullopt;
        }
    }
    return sample;
}

}  // namespace lubancode::accounting
