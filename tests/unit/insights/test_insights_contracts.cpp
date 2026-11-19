// Finding / SessionSummary / Report 合同测试(Token 账本单 §15.1 A0)。
#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "insights/finding.hpp"
#include "insights/report_model.hpp"
#include "insights/session_summary.hpp"

using namespace lubancode::insights;

namespace {

Finding MakeFinding() {
    Finding finding;
    finding.finding_id = "P-AUD-017";
    finding.category = "cache.prefix_churn";
    finding.severity = FindingSeverity::Warning;
    finding.confidence = FindingConfidence::High;
    finding.scope = "workspace";
    EvidenceItem evidence;
    evidence.metric = "tools_hash_changes";
    evidence.value = 6;
    evidence.session_id = "20260830-000001";
    finding.evidence.push_back(evidence);
    EvidenceItem counter;
    counter.metric = "provider_cache_capability";
    counter.value = "reported_supported";
    finding.counter_evidence.push_back(counter);
    finding.summary = "工具表在连续请求间频繁变化";
    finding.recommendation = "固定注册与序列化次序;动态索引后置";
    finding.origin = FindingOrigin::DeterministicRule;
    finding.rule_version = "prompt-audit-v1:P017";
    return finding;
}

SessionInsightSummary MakeSummary() {
    SessionInsightSummary summary;
    summary.source.session_id = "20260830-000001-FX001";
    summary.source.stream_terminal_hashes["main-0001"] = std::string(64, 'a');
    summary.coverage.runs_total = 4;
    summary.coverage.runs_analyzed = 4;
    summary.coverage.requests_total = 23;
    summary.coverage.requests_with_usage = 21;
    summary.coverage.outcomes_assessed = 1;
    summary.work.turns = 8;
    summary.work.tool_calls = 19;
    summary.work.files_touched = 6;
    summary.work.verifications = 3;
    summary.work.outcome = "partial";
    summary.usage.requests_total = 23;
    summary.usage.requests_with_usage = 21;
    summary.usage.input_tokens = 812000;
    summary.usage.cache_read_tokens = 691000;
    summary.usage.output_tokens = 42000;
    summary.usage.reasoning_tokens = 17000;
    summary.prompt_findings.push_back(MakeFinding());
    summary.friction_events = {"tool.repeated_retry"};
    summary.feature_signals = {"delayed_tool_index"};
    return summary;
}

}  // namespace

TEST_CASE("severity 与 confidence 两把尺子分开") {
    CHECK(std::string(FindingSeverityName(FindingSeverity::Info)) == "info");
    CHECK(std::string(FindingSeverityName(FindingSeverity::Warning)) == "warning");
    CHECK(std::string(FindingSeverityName(FindingSeverity::High)) == "high");
    CHECK(std::string(FindingConfidenceName(FindingConfidence::Low)) == "low");
    CHECK(std::string(FindingConfidenceName(FindingConfidence::Medium)) == "medium");
    CHECK(std::string(FindingConfidenceName(FindingConfidence::High)) == "high");
    CHECK(!FindingSeverityFromName("high_confidence").has_value());
    CHECK(!FindingConfidenceFromName("warning").has_value());
    // origin:本地规则与模型评议分开;模型只能给 reviewed_suggestion。
    CHECK(std::string(FindingOriginName(FindingOrigin::ModelReview)) == "reviewed_suggestion");
    CHECK(std::string(FindingOriginName(FindingOrigin::DeterministicRule)) ==
          "deterministic_rule");
}

TEST_CASE("Finding round-trip 与坏形拒收") {
    const Finding finding = MakeFinding();
    const nlohmann::json json = finding.ToJson();
    std::string error;
    const auto parsed = Finding::FromJsonStrict(json, &error);
    REQUIRE(parsed.has_value());
    INFO(error.c_str());
    CHECK(parsed->severity == FindingSeverity::Warning);
    CHECK(parsed->confidence == FindingConfidence::High);
    REQUIRE(parsed->evidence.size() == 1);
    CHECK(parsed->evidence[0].metric == "tools_hash_changes");
    CHECK(parsed->evidence[0].value == 6);
    CHECK(parsed->ToJson() == json);

    // 严重度认不得。
    nlohmann::json bad = json;
    bad["severity"] = "critical";
    CHECK(!Finding::FromJsonStrict(bad, &error).has_value());
    // 置信度空串。
    bad = json;
    bad["confidence"] = "";
    CHECK(!Finding::FromJsonStrict(bad, &error).has_value());
    // 证据缺 metric。
    bad = json;
    bad["evidence"][0].erase("metric");
    CHECK(!Finding::FromJsonStrict(bad, &error).has_value());
    // 未知键。
    bad = json;
    bad["prompt_text"] = "正文不许进 finding";
    CHECK(!Finding::FromJsonStrict(bad, &error).has_value());
}

TEST_CASE("session summary round-trip,terminal hash 与 analyzer version 是 stale 依据") {
    SUBCASE("nullable summary buckets retain source hashes and reject v1 coercion") {
        const auto flags = std::array{&usage_projection::Knowledge::input_tokens,
            &usage_projection::Knowledge::cache_read_tokens,
            &usage_projection::Knowledge::cache_creation_tokens,
            &usage_projection::Knowledge::output_tokens,
            &usage_projection::Knowledge::reasoning_tokens,
            &usage_projection::Knowledge::cost_micros};
        const auto keys = std::array{"input_tokens", "cache_read_tokens", "cache_creation_tokens",
                                     "output_tokens", "reasoning_tokens", "micros"};
        for (std::size_t index = 0; index < flags.size(); ++index) {
            auto boundary = MakeSummary();
            boundary.usage.knowledge.*flags[index] = false;
            auto encoded = boundary.ToJson();
            CHECK(encoded["schema_version"] == 2);
            const auto& value = index == 5 ? encoded["usage"]["cost"][keys[index]]
                                           : encoded["usage"][keys[index]];
            CHECK(value.is_null());
            std::string error;
            const auto restored = SessionInsightSummary::FromJsonStrict(encoded, &error);
            REQUIRE(restored.has_value());
            CHECK(!(restored->usage.knowledge.*flags[index]));
            CHECK(restored->source.stream_terminal_hashes == boundary.source.stream_terminal_hashes);
            CHECK(restored->source.session_id == boundary.source.session_id);
            CHECK(restored->ToJson() == encoded);
            encoded["schema_version"] = 1;
            CHECK(!SessionInsightSummary::FromJsonStrict(encoded, &error));
            encoded["schema_version"] = 2;
            if (index == 5) encoded["usage"]["cost"].erase(keys[index]);
            else encoded["usage"].erase(keys[index]);
            CHECK(!SessionInsightSummary::FromJsonStrict(encoded, &error));
        }
        auto epoch = MakeSummary();
        SummaryCacheEpoch row;
        row.run_id = "main-0001";
        row.cache_read_tokens = 123;
        row.knowledge.cache_read_tokens = false;
        epoch.cache_epochs.push_back(row);
        std::string error;
        const auto restored = SessionInsightSummary::FromJsonStrict(epoch.ToJson(), &error);
        REQUIRE(restored.has_value());
        REQUIRE(restored->cache_epochs.size() == 1);
        CHECK(!restored->cache_epochs[0].knowledge.cache_read_tokens);
        CHECK(restored->ToJson() == epoch.ToJson());
        auto aliased = MakeSummary().ToJson();
        aliased["schema_version"] = std::uint64_t{4294967297};
        CHECK(!SessionInsightSummary::FromJsonStrict(aliased, &error));
        for (const auto& invalid : {
                 nlohmann::json(static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max()) + 1),
                 nlohmann::json(1.0), nlohmann::json(true), nlohmann::json("1")}) {
            auto bad = MakeSummary().ToJson();
            bad["usage"]["input_tokens"] = invalid;
            CHECK(!SessionInsightSummary::FromJsonStrict(bad, &error));
            bad = MakeSummary().ToJson();
            bad["usage"]["cost"]["micros"] = invalid;
            CHECK(!SessionInsightSummary::FromJsonStrict(bad, &error));
        }
    }
    const SessionInsightSummary summary = MakeSummary();
    const nlohmann::json json = summary.ToJson();
    std::string error;
    const auto parsed = SessionInsightSummary::FromJsonStrict(json, &error);
    REQUIRE(parsed.has_value());
    INFO(error.c_str());
    CHECK(parsed->source.stream_terminal_hashes.at("main-0001") == std::string(64, 'a'));
    CHECK(parsed->coverage.requests_with_usage == 21);
    CHECK(parsed->usage.reasoning_tokens == 17000);
    CHECK(parsed->work.outcome == "partial");
    REQUIRE(parsed->prompt_findings.size() == 1);
    CHECK(parsed->ToJson() == json);

    // 坏形:integrity 认不得。
    nlohmann::json bad = json;
    bad["source"]["integrity"] = "probably";
    CHECK(!SessionInsightSummary::FromJsonStrict(bad, &error).has_value());
    // coverage 键多。
    bad = json;
    bad["coverage"]["extra"] = 1;
    CHECK(!SessionInsightSummary::FromJsonStrict(bad, &error).has_value());
}

TEST_CASE("report round-trip 与 unknown 单列") {
    SUBCASE("nullable report and nested summary restore both version boundaries") {
        InsightsReport boundary;
        boundary.generated_at = "2026-08-30T00:00:00Z";
        boundary.usage.knowledge.output_tokens = false;
        auto summary = MakeSummary();
        summary.usage.knowledge.input_tokens = false;
        boundary.sessions.push_back(summary);
        const auto encoded = boundary.ToJson();
        CHECK(encoded["schema_version"] == 2);
        CHECK(encoded["sessions"][0]["schema_version"] == 2);
        std::string error;
        const auto restored = InsightsReport::FromJsonStrict(encoded, &error);
        REQUIRE(restored.has_value());
        CHECK(!restored->usage.knowledge.output_tokens);
        REQUIRE(restored->sessions.size() == 1);
        CHECK(!restored->sessions[0].usage.knowledge.input_tokens);
        CHECK(restored->ToJson() == encoded);
        auto bad = encoded;
        bad["schema_version"] = 1;
        CHECK(!InsightsReport::FromJsonStrict(bad, &error));
        bad["usage"]["output_tokens"] = 0;
        CHECK(!InsightsReport::FromJsonStrict(bad, &error));
        bad = encoded;
        bad["usage"]["input_tokens"] = static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) + 1;
        CHECK(!InsightsReport::FromJsonStrict(bad, &error));
        CHECK(MakeSummary().ToJson()["schema_version"] == 1);
    }
    InsightsReport report;
    report.generated_at = "2026-08-30T00:00:00Z";
    report.scope.workspace_key = "ws-000000000000";
    report.scope.since = "2026-08-01";
    report.scope.until = "2026-08-30";
    report.coverage.sessions_found = 83;
    report.coverage.sessions_verified = 71;
    report.coverage.sessions_analyzed = 68;
    report.usage.requests_total = 1267;
    report.usage.requests_with_usage = 1204;
    report.usage.requests_unknown = 63;
    report.sessions.push_back(MakeSummary());
    report.findings.push_back(MakeFinding());
    const nlohmann::json json = report.ToJson();
    std::string error;
    const auto parsed = InsightsReport::FromJsonStrict(json, &error);
    REQUIRE(parsed.has_value());
    INFO(error.c_str());
    CHECK(parsed->coverage.sessions_pending == 0);
    CHECK(parsed->usage.requests_unknown == 63);
    REQUIRE(parsed->sessions.size() == 1);
    CHECK(parsed->sessions[0].source.session_id == "20260830-000001-FX001");
    CHECK(parsed->ToJson() == json);

    // 坏形:scope 键多(路径白名单之外的键)。
    nlohmann::json bad = json;
    bad["scope"]["absolute_path"] = "D:\\secret";
    CHECK(!InsightsReport::FromJsonStrict(bad, &error).has_value());
    // usage 键多。
    bad = json;
    bad["usage"]["estimated_total"] = 1;
    CHECK(!InsightsReport::FromJsonStrict(bad, &error).has_value());
}
