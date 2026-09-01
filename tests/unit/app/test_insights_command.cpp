// /insights 命令面的验收册(Token 账本单 A5):纯函数钉住——解析、
// 七节摘要渲染、status/clean 的列账行。
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "app/commands/command_registry.hpp"  // SlashCommandTable(注册对账)
#include "app/commands/insights_commands.hpp"
#include "cli/theme.hpp"
#include "insights/report_store.hpp"

using namespace lubancode;
using namespace lubancode::app;

namespace {

bool Contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& line : lines) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 一份最小生成结果(渲染面只要字段齐)。
insights::InsightsGenerateResult MakeResult() {
    insights::InsightsGenerateResult result;
    result.ok = true;
    result.report.generated_at = "2026-08-31T12:00:00Z";
    result.report.scope.workspace_key = "ws-000000000000";
    result.report.scope.since = "2026-08-01";
    result.report.scope.until = "2026-08-31";
    result.counts.found = 5;
    result.counts.verified = 3;
    result.counts.analyzed = 3;
    result.counts.reused = 2;
    result.counts.written = 1;
    result.counts.excluded = 2;
    result.extras.workspace_names["ws-000000000000"] = "测试仓";

    insights::SessionInsightSummary session;
    session.source.session_id = "20260830-120000-B00001";
    session.work.turns = 2;
    session.work.tool_calls = 3;
    session.work.verifications = 1;
    session.work.outcome = "passed";
    session.usage.requests_total = 2;
    session.usage.requests_with_usage = 1;
    session.usage.input_tokens = 1200;
    session.usage.cache_read_tokens = 48000;
    session.usage.output_tokens = 1800;
    session.friction_events = {"cancelled"};
    session.feature_signals = {"FS-01"};
    result.report.sessions.push_back(session);

    result.aggregate = insights::AggregateInsights(result.report);
    result.extras.excluded.push_back(
        insights::InsightsExcludedEntry{"ws-000000000000", "20260831-000009-BAD009",
                                        "corrupt", "verify.chain_broken: hash mismatch"});
    return result;
}

}  // namespace

TEST_CASE("ParseInsightsCommand:裸敲/flag/子命令/坏词") {
    SUBCASE("裸敲 = 生成,默认 30 天/200 场/当前 workspace") {
        const ParsedInsightsCommand parsed = ParseInsightsCommand("");
        CHECK(parsed.mode == ParsedInsightsCommand::Mode::Generate);
        CHECK(parsed.since_days == 30);
        CHECK(parsed.max_sessions == 200);
        CHECK(!parsed.all_workspaces);
        CHECK(!parsed.include_active);
        CHECK(!parsed.json);
    }
    SUBCASE("flag 组合") {
        const ParsedInsightsCommand parsed =
            ParseInsightsCommand("--since 7d --sessions 50 --all-workspaces "
                                 "--include-active --json --show-paths");
        CHECK(parsed.mode == ParsedInsightsCommand::Mode::Generate);
        CHECK(parsed.since_days == 7);
        CHECK(parsed.max_sessions == 50);
        CHECK(parsed.all_workspaces);
        CHECK(parsed.include_active);
        CHECK(parsed.json);
        CHECK(parsed.show_paths);
    }
    SUBCASE("--since 不带 d 也认;坏值记 bad_word") {
        CHECK(ParseInsightsCommand("--since 14").since_days == 14);
        const ParsedInsightsCommand bad = ParseInsightsCommand("--since abc");
        CHECK(bad.invalid);
        CHECK(bad.bad_word == "abc");
        const ParsedInsightsCommand missing = ParseInsightsCommand("--since");
        CHECK(missing.invalid);
        CHECK(missing.bad_word == "--since");
    }
    SUBCASE("status / clean --derived-only") {
        CHECK(ParseInsightsCommand("status").mode == ParsedInsightsCommand::Mode::Status);
        const ParsedInsightsCommand clean = ParseInsightsCommand("clean --derived-only");
        CHECK(clean.mode == ParsedInsightsCommand::Mode::Clean);
        CHECK(clean.clean_derived_only);
        CHECK(!clean.invalid);
    }
    SUBCASE("后续批次旗:认得,不冒充") {
        const ParsedInsightsCommand parsed = ParseInsightsCommand("--model-review --open");
        CHECK(parsed.later_model_review);
        CHECK(parsed.later_open);
        CHECK(!parsed.invalid);
    }
    SUBCASE("坏词") {
        const ParsedInsightsCommand parsed = ParseInsightsCommand("frobnicate");
        CHECK(parsed.invalid);
        CHECK(parsed.bad_word == "frobnicate");
    }
}

TEST_CASE("FormatInsightsDigestLines:七节齐,路径与覆盖账在册") {
    const insights::InsightsGenerateResult result = MakeResult();
    const std::vector<std::string> lines = FormatInsightsDigestLines(
        result, std::filesystem::path("Z:/reports/a.json"),
        std::filesystem::path("Z:/reports/a.html"), /*show_paths=*/false);
    CHECK(Contains(lines, "Insights · ws-000000000000"));
    CHECK(Contains(lines, "概览"));
    CHECK(Contains(lines, "Token"));
    CHECK(Contains(lines, "Prompt"));
    CHECK(Contains(lines, "摩擦"));
    CHECK(Contains(lines, "交互形状"));
    CHECK(Contains(lines, "建议"));
    CHECK(Contains(lines, "覆盖"));
    CHECK(Contains(lines, "限制"));
    CHECK(Contains(lines, "1/2 笔有 provider usage"));
    CHECK(Contains(lines, "排除 20260831-000009-BAD009"));
    CHECK(Contains(lines, "fresh 复用 2"));
    CHECK(Contains(lines, "a.json"));
    CHECK(Contains(lines, "latest.*"));
    // show_paths 才显 workspace 明细。
    const std::vector<std::string> with_paths = FormatInsightsDigestLines(
        result, std::filesystem::path("a.json"), std::filesystem::path("a.html"),
        /*show_paths=*/true);
    CHECK(Contains(with_paths, "workspace   测试仓"));
    CHECK(!Contains(lines, "workspace   测试仓"));
}

TEST_CASE("FormatInsightsStatusLines 与 clean 列账") {
    std::vector<insights::InsightsReportFile> reports;
    insights::InsightsReportFile file;
    file.path = std::filesystem::path("Z:/insights/reports/20260831-101500-ws-7d.json");
    file.bytes = 2048;
    reports.push_back(file);
    const std::vector<std::string> status =
        FormatInsightsStatusLines(reports, "generated_at=2026-08-31T10:15:00Z · analyzer=x",
                                  3, std::filesystem::path("Z:/insights"));
    CHECK(Contains(status, "最近报告    generated_at=2026-08-31T10:15:00Z"));
    CHECK(Contains(status, "历史报告    1 份"));
    CHECK(Contains(status, "会话摘要    3 份"));

    insights::InsightsCleanPlan plan;
    plan.items.push_back(insights::InsightsCleanItem{
        std::filesystem::path("Z:/t/s1/derived/insights-v1/session-summary.json"),
        std::filesystem::path("Z:/t/s1/derived/insights-v1"), 1200, "derived-file"});
    plan.total_bytes = 1200;
    const std::vector<std::string> clean_lines = FormatInsightsCleanPlanLines(plan);
    CHECK(Contains(clean_lines, "将删 1 个文件"));
    CHECK(Contains(clean_lines, "session-summary.json"));
    // 空账。
    const std::vector<std::string> empty =
        FormatInsightsCleanPlanLines(insights::InsightsCleanPlan{});
    CHECK(Contains(empty, "没有可清的派生摘要"));
}

TEST_CASE("命令注册:/insights 有分派位且帮助面同名") {
    const lubancode::cli::ParsedSlashCommand parsed =
        lubancode::cli::ParseSlashCommand("/insights --since 7d");
    REQUIRE(parsed.command == lubancode::cli::SlashCommand::Insights);
    CHECK(parsed.args == "--since 7d");
    bool found = false;
    for (const auto& spec : lubancode::app::SlashCommandTable()) {
        if (spec.command == lubancode::cli::SlashCommand::Insights) {
            found = spec.handler != nullptr;
        }
    }
    CHECK(found);
    bool in_help = false;
    for (const auto& command : lubancode::cli::AllSlashCommands()) {
        if (std::string(command.name) == "/insights") {
            in_help = !std::string(command.description).empty();
        }
    }
    CHECK(in_help);
}
