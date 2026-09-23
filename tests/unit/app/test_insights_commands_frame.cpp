// TUI 排版批 4(/insights 报告/status/clean 列账)的输出形状册。
//   - digest:标题句进 frame 标题(嵌上边框),七节按节名拆键值对、key 列
//     全表对齐;排除明细是覆盖节的续行(key 空,值列同栏);
//   - 80 列预算:width=80 时整行显示宽不超 80(单子验收"80 列仍可读");
//   - status/clean:标题(尾冒号剥掉)+ 报告清单续行;
//   - plain 主题:零转义、无框,标题降为独立内容行。

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "app/commands/insights_commands.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:对齐断言按显示列量
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

bool ContainsOne(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 一份最小生成结果(字段口径与 test_insights_command.cpp 的 MakeResult 同源)。
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

// 剥掉 CSI 序列(与 test_plugin_commands_frame.cpp 同一把手写的尺)。
std::string StripAnsiLight(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) {
                ++i;
            }
            if (i < text.size()) {
                ++i;  // 吃掉终结字母
            }
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

// key 段之后 value 的起始显示列(key 补齐 + 两格列距后应处处同列)。
int ValueStartCol(const std::string& row, const std::string& key) {
    const std::size_t at = row.find(key);
    if (at == std::string::npos) {
        return -1;
    }
    std::size_t i = at + key.size();
    while (i < row.size() && row[i] == ' ') {
        ++i;
    }
    return static_cast<int>(lubancode::cli::DisplayWidthUtf8(row.substr(0, i)));
}

constexpr const char* kBoxLightTopLeft = "\xe2\x94\x8c";  // ┌
constexpr const char* kBoxLightVert = "\xe2\x94\x82";     // │

std::string JoinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += "\n";
    }
    return out;
}

}  // namespace

TEST_CASE("digest: 标题进上边框,七节键值列全表对齐(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    const insights::InsightsGenerateResult result = MakeResult();
    const std::vector<std::string> lines =
        FormatInsightsDigestLines(result, std::filesystem::path("Z:/reports/a.json"),
                                  std::filesystem::path("Z:/reports/a.html"),
                                  /*show_paths=*/false, dark, /*width=*/0);
    const std::string joined = JoinLines(lines);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    // 标题嵌上边框(dark);key 列走 row_label 语义色。
    CHECK(ContainsOne(joined, "Insights · ws-000000000000"));
    CHECK(ContainsOne(joined, dark.row_label));
    // key 列对齐:概览/Token/摩擦的 value 起始显示列一致;排除续行同栏。
    int overview_col = -1;
    int token_col = -1;
    int friction_col = -1;
    int excluded_col = -1;
    for (const std::string& line : lines) {
        const std::string plain = StripAnsiLight(line);
        if (overview_col < 0) overview_col = ValueStartCol(plain, "概览");
        if (token_col < 0) token_col = ValueStartCol(plain, "Token");
        if (friction_col < 0) friction_col = ValueStartCol(plain, "摩擦");
        if (excluded_col < 0) {
            const std::size_t at = plain.find("排除 ");
            if (at != std::string::npos) {
                excluded_col = static_cast<int>(lubancode::cli::DisplayWidthUtf8(plain.substr(0, at)));
            }
        }
    }
    CHECK(overview_col > 0);
    CHECK(token_col > 0);
    CHECK(friction_col > 0);
    CHECK(excluded_col > 0);
    CHECK(overview_col == token_col);
    CHECK(overview_col == friction_col);
    CHECK(excluded_col == overview_col);
}

TEST_CASE("digest: 80 列预算下整行显示宽不超 80(长报告仍可读)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    const insights::InsightsGenerateResult result = MakeResult();
    // 长路径 + 长排除理由,逼出列帽。
    const std::vector<std::string> lines = FormatInsightsDigestLines(
        result, std::filesystem::path("Z:/very/long/path/to/insights/reports/a.json"),
        std::filesystem::path("Z:/very/long/path/to/insights/reports/a.html"),
        /*show_paths=*/false, dark, /*width=*/80);
    REQUIRE(!lines.empty());
    for (const std::string& line : lines) {
        CHECK(lubancode::cli::DisplayWidthUtf8(StripAnsiLight(line)) <= 80);
    }
    // 截断保字头:报告路径的头部仍在(值列截尾不截头)。
    CHECK(Contains(lines, "Z:/very/long"));
}

TEST_CASE("status 与 clean 列账:标题(尾冒号剥掉)+ 续行对齐(dark)") {
    const cli::Theme dark = cli::BuiltinTheme("dark");
    std::vector<insights::InsightsReportFile> reports;
    insights::InsightsReportFile file;
    file.path = std::filesystem::path("Z:/insights/reports/20260831-101500-ws-7d.json");
    file.bytes = 2048;
    reports.push_back(file);
    const std::vector<std::string> status =
        FormatInsightsStatusLines(reports, "generated_at=2026-08-31T10:15:00Z · analyzer=x", 3,
                                  std::filesystem::path("Z:/insights"), dark, /*width=*/0);
    const std::string joined = JoinLines(status);
    REQUIRE(ContainsOne(joined, kBoxLightTopLeft));
    CHECK(ContainsOne(joined, "Insights 状态"));
    // 报告文件是续行(key 空):文件名与"最近报告"的 value 同栏。
    int file_col = -1;
    int latest_col = -1;
    for (const std::string& line : status) {
        const std::string plain = StripAnsiLight(line);
        if (latest_col < 0) latest_col = ValueStartCol(plain, "最近报告");
        const std::size_t at = plain.find("20260831-101500-ws-7d.json");
        if (at != std::string::npos && file_col < 0) {
            file_col = static_cast<int>(lubancode::cli::DisplayWidthUtf8(plain.substr(0, at)));
        }
    }
    CHECK(latest_col > 0);
    CHECK(file_col == latest_col);

    insights::InsightsCleanPlan plan;
    plan.items.push_back(insights::InsightsCleanItem{
        std::filesystem::path("Z:/t/s1/derived/insights-v1/session-summary.json"),
        std::filesystem::path("Z:/t/s1/derived/insights-v1"), 1200, "derived-file"});
    plan.total_bytes = 1200;
    const std::vector<std::string> clean_lines =
        FormatInsightsCleanPlanLines(plan, dark, /*width=*/0);
    const std::string clean_joined = JoinLines(clean_lines);
    REQUIRE(ContainsOne(clean_joined, kBoxLightTopLeft));
    // 标题化的"将删 N 个文件"不带尾冒号(批 2 裁量 3):标题句以 ")" 收,
    // 后面直接跟边框横线,没有 "):" 连排。
    CHECK(ContainsOne(clean_joined, "将删 1 个文件"));
    CHECK(clean_joined.find("):") == std::string::npos);
    CHECK(ContainsOne(clean_joined, "session-summary.json"));
}

TEST_CASE("plain 主题:零转义、无框,标题降为独立行") {
    const cli::Theme plain = cli::BuiltinTheme("plain");
    const insights::InsightsGenerateResult result = MakeResult();
    const std::vector<std::string> lines =
        FormatInsightsDigestLines(result, std::filesystem::path("Z:/reports/a.json"),
                                  std::filesystem::path("Z:/reports/a.html"),
                                  /*show_paths=*/false, plain, /*width=*/0);
    const std::string joined = JoinLines(lines);
    CHECK(joined.find("\x1b") == std::string::npos);
    CHECK(!ContainsOne(joined, kBoxLightTopLeft));
    CHECK(!ContainsOne(joined, kBoxLightVert));
    // 标题独立成行,内容一字不少。
    CHECK(Contains(lines, "Insights · ws-000000000000 · 2026-08-01 至 2026-08-31"));
    CHECK(Contains(lines, "概览"));
    CHECK(Contains(lines, "排除 20260831-000009-BAD009"));
}
