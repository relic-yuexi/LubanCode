#include "insights/session_summary.hpp"

#include <string_view>

namespace lubancode::insights {
namespace {

bool ReadString(const nlohmann::json& json, const char* key, std::string* out) {
    return json.contains(key) && json.at(key).is_string() &&
           (*out = json.at(key).get<std::string>(), true);
}

bool ReadUint(const nlohmann::json& json, const char* key, std::uint64_t* out) {
    return json.contains(key) && json.at(key).is_number_unsigned() &&
           (*out = json.at(key).get<std::uint64_t>(), true);
}

}  // namespace

nlohmann::json SessionInsightSummary::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = kSessionSummarySchema;
    json["schema_version"] = kSessionSummarySchemaVersion;
    json["analyzer_version"] = analyzer_version;
    nlohmann::json source_json = nlohmann::json::object();
    source_json["session_id"] = source.session_id;
    nlohmann::json hashes = nlohmann::json::object();
    for (const auto& [run_id, hash] : source.stream_terminal_hashes) {
        hashes[run_id] = hash;
    }
    source_json["stream_terminal_hashes"] = std::move(hashes);
    source_json["integrity"] = source.integrity;
    json["source"] = std::move(source_json);
    nlohmann::json coverage_json = nlohmann::json::object();
    coverage_json["runs_total"] = coverage.runs_total;
    coverage_json["runs_analyzed"] = coverage.runs_analyzed;
    coverage_json["requests_total"] = coverage.requests_total;
    coverage_json["requests_with_usage"] = coverage.requests_with_usage;
    coverage_json["outcomes_assessed"] = coverage.outcomes_assessed;
    json["coverage"] = std::move(coverage_json);
    nlohmann::json work_json = nlohmann::json::object();
    work_json["turns"] = work.turns;
    work_json["tool_calls"] = work.tool_calls;
    work_json["files_touched"] = work.files_touched;
    work_json["verifications"] = work.verifications;
    work_json["outcome"] = work.outcome;
    json["work"] = std::move(work_json);
    nlohmann::json usage_json = nlohmann::json::object();
    usage_json["requests_total"] = usage.requests_total;
    usage_json["requests_with_usage"] = usage.requests_with_usage;
    usage_json["input_tokens"] = usage.input_tokens;
    usage_json["cache_read_tokens"] = usage.cache_read_tokens;
    usage_json["cache_creation_tokens"] = usage.cache_creation_tokens;
    usage_json["output_tokens"] = usage.output_tokens;
    usage_json["reasoning_tokens"] = usage.reasoning_tokens;
    nlohmann::json cost_json = nlohmann::json::object();
    cost_json["status"] = usage.cost_status;
    cost_json["currency"] = usage.cost_currency;
    cost_json["micros"] = usage.cost_micros;
    cost_json["price_table_id"] = usage.price_table_id;
    usage_json["cost"] = std::move(cost_json);
    json["usage"] = std::move(usage_json);
    nlohmann::json cache_epochs_json = nlohmann::json::array();
    for (const auto& epoch : cache_epochs) {
        cache_epochs_json.push_back(nlohmann::json{{"run_id", epoch.run_id},
                                                   {"cache_epoch", epoch.cache_epoch},
                                                   {"requests_total", epoch.requests_total},
                                                   {"requests_cache_reported", epoch.requests_cache_reported},
                                                   {"requests_cache_unknown", epoch.requests_cache_unknown},
                                                   {"input_tokens", epoch.input_tokens},
                                                   {"cache_read_tokens", epoch.cache_read_tokens},
                                                   {"cache_creation_tokens", epoch.cache_creation_tokens}});
    }
    json["cache_epochs"] = std::move(cache_epochs_json);
    nlohmann::json findings = nlohmann::json::array();
    for (const auto& finding : prompt_findings) {
        findings.push_back(finding.ToJson());
    }
    json["prompt_findings"] = std::move(findings);
    json["friction_events"] = friction_events;
    json["feature_signals"] = feature_signals;
    return json;
}

std::optional<SessionInsightSummary> SessionInsightSummary::FromJsonStrict(
    const nlohmann::json& json, std::string* error) {
    if (!json.is_object()) {
        *error = "summary 须是 object";
        return std::nullopt;
    }
    SessionInsightSummary summary;
    std::string schema;
    if (!ReadString(json, "schema", &schema) || schema != kSessionSummarySchema) {
        *error = "schema 名不是 " + std::string(kSessionSummarySchema);
        return std::nullopt;
    }
    if (!json.contains("schema_version") || !json.at("schema_version").is_number_integer() ||
        json.at("schema_version").get<int>() != kSessionSummarySchemaVersion) {
        *error = "schema_version 只认 1";
        return std::nullopt;
    }
    if (!ReadString(json, "analyzer_version", &summary.analyzer_version)) {
        *error = "analyzer_version 必填";
        return std::nullopt;
    }
    if (!json.contains("source") || !json.at("source").is_object()) {
        *error = "source 须是 object";
        return std::nullopt;
    }
    const auto& source = json.at("source");
    if (!ReadString(source, "session_id", &summary.source.session_id) ||
        !ReadString(source, "integrity", &summary.source.integrity) ||
        (summary.source.integrity != "verified" && summary.source.integrity != "provisional")) {
        *error = "source.session_id/integrity 不合";
        return std::nullopt;
    }
    if (!source.contains("stream_terminal_hashes") ||
        !source.at("stream_terminal_hashes").is_object()) {
        *error = "stream_terminal_hashes 须是 object";
        return std::nullopt;
    }
    for (auto it = source.at("stream_terminal_hashes").begin();
         it != source.at("stream_terminal_hashes").end(); ++it) {
        if (!it.value().is_string()) {
            *error = "stream_terminal_hashes 值须是字符串";
            return std::nullopt;
        }
        summary.source.stream_terminal_hashes[it.key()] = it.value().get<std::string>();
    }
    if (source.size() != 3) {
        *error = "source 未知键";
        return std::nullopt;
    }
    if (!json.contains("coverage") || !json.at("coverage").is_object()) {
        *error = "coverage 须是 object";
        return std::nullopt;
    }
    const auto& coverage = json.at("coverage");
    if (!ReadUint(coverage, "runs_total", &summary.coverage.runs_total) ||
        !ReadUint(coverage, "runs_analyzed", &summary.coverage.runs_analyzed) ||
        !ReadUint(coverage, "requests_total", &summary.coverage.requests_total) ||
        !ReadUint(coverage, "requests_with_usage", &summary.coverage.requests_with_usage) ||
        !ReadUint(coverage, "outcomes_assessed", &summary.coverage.outcomes_assessed) ||
        coverage.size() != 5) {
        *error = "coverage 五键不合";
        return std::nullopt;
    }
    if (!json.contains("work") || !json.at("work").is_object()) {
        *error = "work 须是 object";
        return std::nullopt;
    }
    const auto& work = json.at("work");
    if (!ReadUint(work, "turns", &summary.work.turns) ||
        !ReadUint(work, "tool_calls", &summary.work.tool_calls) ||
        !ReadUint(work, "files_touched", &summary.work.files_touched) ||
        !ReadUint(work, "verifications", &summary.work.verifications) ||
        !ReadString(work, "outcome", &summary.work.outcome) || work.size() != 5) {
        *error = "work 五键不合";
        return std::nullopt;
    }
    if (!json.contains("usage") || !json.at("usage").is_object()) {
        *error = "usage 须是 object";
        return std::nullopt;
    }
    const auto& usage = json.at("usage");
    const auto read_int = [&](const char* key, std::int64_t* out) {
        return usage.contains(key) && usage.at(key).is_number_integer() &&
               (*out = usage.at(key).get<std::int64_t>(), true);
    };
    if (!ReadUint(usage, "requests_total", &summary.usage.requests_total) ||
        !ReadUint(usage, "requests_with_usage", &summary.usage.requests_with_usage) ||
        !read_int("input_tokens", &summary.usage.input_tokens) ||
        !read_int("cache_read_tokens", &summary.usage.cache_read_tokens) ||
        !read_int("cache_creation_tokens", &summary.usage.cache_creation_tokens) ||
        !read_int("output_tokens", &summary.usage.output_tokens) ||
        !read_int("reasoning_tokens", &summary.usage.reasoning_tokens)) {
        *error = "usage 七键不合";
        return std::nullopt;
    }
    if (!usage.contains("cost") || !usage.at("cost").is_object()) {
        *error = "usage.cost 须是 object";
        return std::nullopt;
    }
    const auto& cost = usage.at("cost");
    if (!ReadString(cost, "status", &summary.usage.cost_status) ||
        !ReadString(cost, "currency", &summary.usage.cost_currency) || cost.size() > 4 ||
        !cost.contains("micros") || !cost.at("micros").is_number_integer()) {
        *error = "usage.cost 四键不合";
        return std::nullopt;
    }
    summary.usage.cost_micros = cost.at("micros").get<std::int64_t>();
    ReadString(cost, "price_table_id", &summary.usage.price_table_id);
    if (usage.size() != 8) {
        *error = "usage 未知键";
        return std::nullopt;
    }
    if (json.contains("cache_epochs")) {
        if (!json.at("cache_epochs").is_array()) {
            *error = "cache_epochs 须是数组";
            return std::nullopt;
        }
        for (const auto& item : json.at("cache_epochs")) {
            SummaryCacheEpoch epoch;
            if (!item.is_object() || item.size() != 8 ||
                !ReadString(item, "run_id", &epoch.run_id) ||
                !item.contains("cache_epoch") || !item.at("cache_epoch").is_number_integer() ||
                !ReadUint(item, "requests_total", &epoch.requests_total) ||
                !ReadUint(item, "requests_cache_reported", &epoch.requests_cache_reported) ||
                !ReadUint(item, "requests_cache_unknown", &epoch.requests_cache_unknown)) {
                *error = "cache_epochs 条目形状不合";
                return std::nullopt;
            }
            epoch.cache_epoch = item.at("cache_epoch").get<int>();
            for (const auto& [key, target] :
                 std::initializer_list<std::pair<const char*, std::int64_t*>>{
                     {"input_tokens", &epoch.input_tokens},
                     {"cache_read_tokens", &epoch.cache_read_tokens},
                     {"cache_creation_tokens", &epoch.cache_creation_tokens}}) {
                if (!item.contains(key) || !item.at(key).is_number_integer()) {
                    *error = "cache_epochs token 字段不合";
                    return std::nullopt;
                }
                *target = item.at(key).get<std::int64_t>();
            }
            summary.cache_epochs.push_back(std::move(epoch));
        }
    }
    if (!json.contains("prompt_findings") || !json.at("prompt_findings").is_array()) {
        *error = "prompt_findings 须是数组";
        return std::nullopt;
    }
    for (const auto& item : json.at("prompt_findings")) {
        const auto finding = Finding::FromJsonStrict(item, error);
        if (!finding.has_value()) {
            return std::nullopt;
        }
        summary.prompt_findings.push_back(std::move(*finding));
    }
    for (const char* key : {"friction_events", "feature_signals"}) {
        if (!json.contains(key) || !json.at(key).is_array()) {
            *error = std::string(key) + " 须是数组";
            return std::nullopt;
        }
        for (const auto& item : json.at(key)) {
            if (!item.is_string()) {
                *error = std::string(key) + " 条目须是字符串";
                return std::nullopt;
            }
            (key == std::string_view("friction_events") ? summary.friction_events
                                                        : summary.feature_signals)
                .push_back(item.get<std::string>());
        }
    }
    const std::size_t expected_keys = json.contains("cache_epochs") ? 11 : 10;
    if (json.size() != expected_keys) {
        *error = "summary 未知键";
        return std::nullopt;
    }
    return summary;
}

}  // namespace lubancode::insights
