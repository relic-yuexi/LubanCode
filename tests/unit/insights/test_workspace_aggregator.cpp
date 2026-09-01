// WorkspaceAggregator 的验收册(Token 账本单 A5):
//   1. micro 场 usage 照收、摩擦/交互样本不算它(§9.1);
//   2. 摩擦 rollup 按封闭表次序,样本场上限 5;
//   3. prompt 规则汇总(场数证据 + 首场措辞保留);
//   4. 信号 rollup 按规则钉死的 id 汇总(FS-04 不因只开一条改号);
//   5. cache 比例分母 0 给 unknown;unknown 请求数单列。
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "insights/feature_signal.hpp"
#include "insights/workspace_aggregator.hpp"

using namespace lubancode;
using namespace lubancode::insights;

namespace {

SessionInsightSummary MakeSession(const std::string& id, std::int64_t input,
                                  std::int64_t output) {
    SessionInsightSummary summary;
    summary.source.session_id = id;
    summary.source.integrity = "verified";
    summary.coverage.requests_total = 1;
    summary.coverage.requests_with_usage = 1;
    summary.usage.requests_total = 1;
    summary.usage.requests_with_usage = 1;
    summary.usage.input_tokens = input;
    summary.usage.output_tokens = output;
    return summary;
}

InsightsReport ReportOf(std::vector<SessionInsightSummary> sessions) {
    InsightsReport report;
    report.generated_at = "2026-08-31T00:00:00Z";
    report.sessions = std::move(sessions);
    return report;
}

}  // namespace

TEST_CASE("micro 场:usage 照收,摩擦与交互样本不算它") {
    std::vector<SessionInsightSummary> sessions;
    SessionInsightSummary normal = MakeSession("s-normal", 1000, 200);
    normal.work.turns = 2;
    normal.work.tool_calls = 3;
    normal.friction_events = {"cancelled"};
    sessions.push_back(normal);

    SessionInsightSummary micro = MakeSession("s-micro", 500, 50);
    micro.work.turns = 1;  // <2 且无工具/验证/outcome → micro(§9.1)
    micro.friction_events = {"tool.execution_failure"};
    sessions.push_back(micro);

    const WorkspaceAggregate agg = AggregateInsights(ReportOf(std::move(sessions)));
    CHECK(agg.sessions == 2);
    CHECK(agg.micro_sessions == 1);
    CHECK(agg.sample_sessions == 1);
    // usage 全收(micro 的 500 也进账)。
    CHECK(agg.input_tokens == 1500);
    CHECK(agg.output_tokens == 250);
    // 摩擦只算非 micro 场:cancelled 1 场在册,tool.execution_failure 不进。
    REQUIRE(agg.frictions.size() == 1);
    CHECK(agg.frictions[0].category == "cancelled");
    CHECK(agg.frictions[0].sessions == 1);
    CHECK(agg.sessions_cancelled == 1);
}

TEST_CASE("摩擦 rollup:封闭表次序,样本场封顶 5") {
    std::vector<SessionInsightSummary> sessions;
    for (int i = 0; i < 7; ++i) {
        // 封闭表里 verification.failure(第 8 位)排在 cancelled(第 15)前。
        SessionInsightSummary session = MakeSession("s-" + std::to_string(i), 10, 10);
        session.work.turns = 2;
        session.friction_events = {"cancelled", "verification.failure"};
        sessions.push_back(session);
    }
    const WorkspaceAggregate agg = AggregateInsights(ReportOf(std::move(sessions)));
    REQUIRE(agg.frictions.size() == 2);
    CHECK(agg.frictions[0].category == "verification.failure");
    CHECK(agg.frictions[1].category == "cancelled");
    CHECK(agg.frictions[0].sessions == 7);
    REQUIRE(agg.frictions[0].sample_session_ids.size() == 5);  // 样本封顶
    CHECK(agg.sessions_with_verification_failure == 7);
}

TEST_CASE("prompt 规则汇总:场数证据 + 规则措辞保留,severity 取最重") {
    std::vector<SessionInsightSummary> sessions;
    for (int i = 0; i < 3; ++i) {
        SessionInsightSummary session = MakeSession("s-" + std::to_string(i), 10, 10);
        session.work.turns = 2;
        Finding finding;
        finding.finding_id = "P-AUD-R03";
        finding.category = "cache.prefix_break";
        finding.severity = i == 0 ? FindingSeverity::Info : FindingSeverity::Warning;
        finding.confidence = FindingConfidence::High;
        finding.summary = "同 epoch 前缀断";
        session.prompt_findings.push_back(finding);
        sessions.push_back(session);
    }
    const WorkspaceAggregate agg = AggregateInsights(ReportOf(std::move(sessions)));
    REQUIRE(agg.prompt_rollups.size() == 1);
    const Finding& rollup = agg.prompt_rollups[0];
    CHECK(rollup.finding_id == "P-AUD-R03");
    CHECK(rollup.severity == FindingSeverity::Warning);  // 最重一档
    CHECK(rollup.summary == "同 epoch 前缀断");           // 规则措辞保留
    CHECK(rollup.scope == "workspace");
    REQUIRE(rollup.evidence.size() == 2);
    CHECK(rollup.evidence[0].metric == "sessions_affected");
    CHECK(rollup.evidence[0].value == 3);
    CHECK(rollup.evidence[1].metric == "sample_sessions");
}

TEST_CASE("信号 rollup:FS-04 只开一条也用本命 id,跨场汇总") {
    REQUIRE(FindFeatureSignal("FS-04").has_value());
    CHECK(FindFeatureSignal("FS-99") == std::nullopt);

    std::vector<SessionInsightSummary> sessions;
    for (int i = 0; i < 2; ++i) {
        SessionInsightSummary session = MakeSession("s-" + std::to_string(i), 10, 10);
        session.work.turns = 2;
        session.feature_signals = {"FS-04"};
        sessions.push_back(session);
    }
    const WorkspaceAggregate agg = AggregateInsights(ReportOf(std::move(sessions)));
    REQUIRE(agg.signals.size() == 1);
    CHECK(agg.signals[0].signal_id == "FS-04");
    CHECK(agg.signals[0].sessions == 2);
    CHECK(agg.signals[0].feature.find("子代理") != std::string::npos);
    CHECK(!agg.signals[0].action.empty());
    CHECK(!agg.signals[0].precondition.empty());
}

TEST_CASE("usage 账:unknown 单列不折 0;cache 比例分母 0 给 unknown") {
    SUBCASE("有 unknown 请求") {
        SessionInsightSummary session = MakeSession("s", 100, 200);
        session.usage.requests_total = 3;
        session.usage.requests_with_usage = 2;  // coverage 同步给 2/3
        session.coverage.requests_total = 3;
        session.coverage.requests_with_usage = 2;
        const WorkspaceAggregate agg = AggregateInsights(ReportOf({session}));
        CHECK(agg.requests_total == 3);
        CHECK(agg.requests_with_usage == 2);
        CHECK(agg.requests_unknown == 1);
    }
    SUBCASE("实测输入全 0:比例 unknown,不写 0%") {
        SessionInsightSummary session = MakeSession("s", 0, 5);
        session.usage.input_tokens = 0;
        session.usage.cache_read_tokens = 0;
        session.usage.cache_creation_tokens = 0;
        session.usage.requests_with_usage = 1;
        const WorkspaceAggregate agg = AggregateInsights(ReportOf({session}));
        CHECK(agg.cache_read_ratio_percent == std::nullopt);
    }
    SUBCASE("正常比例:分母 = 输入合计") {
        SessionInsightSummary session = MakeSession("s", 300, 100);
        session.usage.cache_read_tokens = 700;  // base=1000,读 70%
        const WorkspaceAggregate agg = AggregateInsights(ReportOf({session}));
        REQUIRE(agg.cache_read_ratio_percent.has_value());
        CHECK(*agg.cache_read_ratio_percent == 70);
    }
}

TEST_CASE("outcome 分布与 provisional 场账") {
    std::vector<SessionInsightSummary> sessions;
    SessionInsightSummary a = MakeSession("s-a", 10, 10);
    a.work.turns = 2;
    a.work.outcome = "passed";
    sessions.push_back(a);
    SessionInsightSummary b = MakeSession("s-b", 10, 10);
    b.work.turns = 2;
    b.work.outcome = "partial";
    sessions.push_back(b);
    SessionInsightSummary active = MakeSession("s-active", 10, 10);
    active.work.turns = 2;
    active.source.integrity = "provisional";
    sessions.push_back(active);
    const WorkspaceAggregate agg = AggregateInsights(ReportOf(std::move(sessions)));
    REQUIRE(agg.outcome_counts.size() == 2);
    CHECK(agg.outcome_counts[0].outcome == "partial");  // 字典序
    CHECK(agg.outcome_counts[1].outcome == "passed");
    CHECK(agg.provisional_sessions == 1);
    CHECK(agg.sessions_outcome_assessed == 2);
}
