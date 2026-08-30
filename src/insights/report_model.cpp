#include "insights/report_model.hpp"

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

nlohmann::json InsightsReport::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = kInsightsReportSchema;
    json["schema_version"] = kInsightsReportSchemaVersion;
    json["analyzer_version"] = kInsightsAnalyzerVersion;
    json["generated_at"] = generated_at;
    nlohmann::json scope_json = nlohmann::json::object();
    scope_json["workspace_key"] = scope.workspace_key;
    scope_json["since"] = scope.since;
    scope_json["until"] = scope.until;
    scope_json["all_workspaces"] = scope.all_workspaces;
    json["scope"] = std::move(scope_json);
    nlohmann::json coverage_json = nlohmann::json::object();
    coverage_json["sessions_found"] = coverage.sessions_found;
    coverage_json["sessions_verified"] = coverage.sessions_verified;
    coverage_json["sessions_analyzed"] = coverage.sessions_analyzed;
    coverage_json["sessions_pending"] = coverage.sessions_pending;
    coverage_json["sessions_excluded"] = coverage.sessions_excluded;
    json["coverage"] = std::move(coverage_json);
    nlohmann::json usage_json = nlohmann::json::object();
    usage_json["requests_total"] = usage.requests_total;
    usage_json["requests_with_usage"] = usage.requests_with_usage;
    usage_json["requests_unknown"] = usage.requests_unknown;
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
    json["analysis_mode"] = analysis_mode;
    nlohmann::json sessions = nlohmann::json::array();
    for (const auto& session : this->sessions) {
        sessions.push_back(session.ToJson());
    }
    json["sessions"] = std::move(sessions);
    nlohmann::json findings = nlohmann::json::array();
    for (const auto& finding : this->findings) {
        findings.push_back(finding.ToJson());
    }
    json["findings"] = std::move(findings);
    return json;
}

std::optional<InsightsReport> InsightsReport::FromJsonStrict(const nlohmann::json& json,
                                                             std::string* error) {
    if (!json.is_object()) {
        *error = "report 须是 object";
        return std::nullopt;
    }
    InsightsReport report;
    std::string schema;
    if (!ReadString(json, "schema", &schema) || schema != kInsightsReportSchema) {
        *error = "schema 名不是 " + std::string(kInsightsReportSchema);
        return std::nullopt;
    }
    if (!json.contains("schema_version") || !json.at("schema_version").is_number_integer() ||
        json.at("schema_version").get<int>() != kInsightsReportSchemaVersion) {
        *error = "schema_version 只认 1";
        return std::nullopt;
    }
    if (!ReadString(json, "generated_at", &report.generated_at) ||
        !ReadString(json, "analysis_mode", &report.analysis_mode)) {
        *error = "generated_at/analysis_mode 必填";
        return std::nullopt;
    }
    if (!json.contains("scope") || !json.at("scope").is_object()) {
        *error = "scope 须是 object";
        return std::nullopt;
    }
    const auto& scope = json.at("scope");
    if (!ReadString(scope, "workspace_key", &report.scope.workspace_key) ||
        !ReadString(scope, "since", &report.scope.since) ||
        !ReadString(scope, "until", &report.scope.until) ||
        !scope.contains("all_workspaces") || !scope.at("all_workspaces").is_boolean() ||
        scope.size() != 4) {
        *error = "scope 四键不合";
        return std::nullopt;
    }
    report.scope.all_workspaces = scope.at("all_workspaces").get<bool>();
    if (!json.contains("coverage") || !json.at("coverage").is_object()) {
        *error = "coverage 须是 object";
        return std::nullopt;
    }
    const auto& coverage = json.at("coverage");
    if (!ReadUint(coverage, "sessions_found", &report.coverage.sessions_found) ||
        !ReadUint(coverage, "sessions_verified", &report.coverage.sessions_verified) ||
        !ReadUint(coverage, "sessions_analyzed", &report.coverage.sessions_analyzed) ||
        !ReadUint(coverage, "sessions_pending", &report.coverage.sessions_pending) ||
        !ReadUint(coverage, "sessions_excluded", &report.coverage.sessions_excluded) ||
        coverage.size() != 5) {
        *error = "coverage 五键不合";
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
    if (!ReadUint(usage, "requests_total", &report.usage.requests_total) ||
        !ReadUint(usage, "requests_with_usage", &report.usage.requests_with_usage) ||
        !ReadUint(usage, "requests_unknown", &report.usage.requests_unknown) ||
        !read_int("input_tokens", &report.usage.input_tokens) ||
        !read_int("cache_read_tokens", &report.usage.cache_read_tokens) ||
        !read_int("cache_creation_tokens", &report.usage.cache_creation_tokens) ||
        !read_int("output_tokens", &report.usage.output_tokens) ||
        !read_int("reasoning_tokens", &report.usage.reasoning_tokens)) {
        *error = "usage 八键不合";
        return std::nullopt;
    }
    if (!usage.contains("cost") || !usage.at("cost").is_object()) {
        *error = "usage.cost 须是 object";
        return std::nullopt;
    }
    const auto& cost = usage.at("cost");
    if (!ReadString(cost, "status", &report.usage.cost_status) ||
        !ReadString(cost, "currency", &report.usage.cost_currency) ||
        !cost.contains("micros") || !cost.at("micros").is_number_integer() || cost.size() > 4) {
        *error = "usage.cost 四键不合";
        return std::nullopt;
    }
    report.usage.cost_micros = cost.at("micros").get<std::int64_t>();
    ReadString(cost, "price_table_id", &report.usage.price_table_id);
    if (usage.size() != 9) {
        *error = "usage 未知键";
        return std::nullopt;
    }
    if (!json.contains("sessions") || !json.at("sessions").is_array()) {
        *error = "sessions 须是数组";
        return std::nullopt;
    }
    for (const auto& item : json.at("sessions")) {
        const auto session = SessionInsightSummary::FromJsonStrict(item, error);
        if (!session.has_value()) {
            return std::nullopt;
        }
        report.sessions.push_back(std::move(*session));
    }
    if (!json.contains("findings") || !json.at("findings").is_array()) {
        *error = "findings 须是数组";
        return std::nullopt;
    }
    for (const auto& item : json.at("findings")) {
        const auto finding = Finding::FromJsonStrict(item, error);
        if (!finding.has_value()) {
            return std::nullopt;
        }
        report.findings.push_back(std::move(*finding));
    }
    if (json.size() != 10) {
        *error = "report 未知键";
        return std::nullopt;
    }
    return report;
}

}  // namespace lubancode::insights
