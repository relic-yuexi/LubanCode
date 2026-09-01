// /doctor insights 的验收册(Token 账本单 A5,§10.4):逐条检查项落账,
// 缺什么明说,不重建报告、不调模型。
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "insights/derived_store.hpp"
#include "insights/insights_health.hpp"
#include "insights/report_store.hpp"
#include "insights/session_summary.hpp"

using namespace lubancode;
using namespace lubancode::insights;

namespace {

std::filesystem::path TempRoot(const char* name) {
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

const InsightsHealthLine* FindLine(const std::vector<InsightsHealthLine>& lines,
                                   const std::string& name) {
    for (const auto& line : lines) {
        if (line.name == name) {
            return &line;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("健康检查:账恒开、无报告、无价格表、目录可写") {
    const auto home = TempRoot("lubancode-a5-health");
    InsightsHealthInput input;
    input.trajectory_on = false;
    input.insights_home = home;
    input.pricing_loaded = false;
    input.pricing_note = "未配价格表";
    input.now_yyyymmdd = "20260831";

    const std::vector<InsightsHealthLine> lines = CheckInsightsHealth(input);
    REQUIRE(lines.size() >= 8);
    const auto* trajectory = FindLine(lines, "trajectory");
    REQUIRE(trajectory != nullptr);
    // P0-6:features.trajectory 开关删除,轨迹账恒开——健康行恒绿。
    CHECK(trajectory->status == ' ');
    CHECK(trajectory->detail.find("唯一事实账") != std::string::npos);
    const auto* derived = FindLine(lines, "derived 目录");
    REQUIRE(derived != nullptr);
    CHECK(derived->status == ' ');
    CHECK(derived->detail.find("可写") != std::string::npos);
    const auto* latest = FindLine(lines, "最近报告");
    REQUIRE(latest != nullptr);
    CHECK(latest->status == '!');  // 还没有报告 = warn
    const auto* pricing = FindLine(lines, "价格表");
    REQUIRE(pricing != nullptr);
    CHECK(pricing->status == '!');
    CHECK(pricing->detail.find("not_priced") != std::string::npos);
    const auto* renderer = FindLine(lines, "HTML renderer");
    REQUIRE(renderer != nullptr);
    CHECK(renderer->status == ' ');
    const auto* review = FindLine(lines, "model review");
    REQUIRE(review != nullptr);
    CHECK(review->status == ' ');
    CHECK(review->detail.find("默认关闭") != std::string::npos);
}

TEST_CASE("健康检查:有报告读回版本;stale/坏摘要计数") {
    const auto home = TempRoot("lubancode-a5-health2");
    const auto sessions = home / "sessions" / "20260831-000001-HT0001";
    {
        std::error_code ec;
        std::filesystem::create_directories(sessions, ec);
        // 一份 stale 摘要(analyzer 版本旧,严格解析要过——版本账才数得进)。
        SessionInsightSummary stale_summary;
        stale_summary.source.session_id = "20260831-000001-HT0001";
        stale_summary.analyzer_version = "insights-v1";  // 旧版本(A5 前的序号 id 时代)
        const DerivedWriteResult written =
            WriteSessionSummaryAtomic(sessions, stale_summary);
        REQUIRE(written.ok);
    }
    // 一份坏 JSON。
    {
        const auto bad = home / "sessions" / "20260831-000002-HT0002";
        std::error_code ec;
        std::filesystem::create_directories(bad / "derived" / kDerivedAnalyzerDir, ec);
        std::ofstream(bad / "derived" / kDerivedAnalyzerDir / "session-summary.json",
                      std::ios::binary)
            << "{not json";
    }
    // 写一份最新报告(latest.json)。
    const nlohmann::json latest = nlohmann::json{
        {"schema", kInsightsReportSchema},
        {"schema_version", kInsightsReportSchemaVersion},
        {"analyzer_version", kInsightsAnalyzerVersion},
        {"generated_at", "2026-08-31T10:00:00Z"}};
    {
        std::error_code ec;
        std::filesystem::create_directories(home / "insights", ec);
        std::ofstream out(home / "insights" / "latest.json", std::ios::binary);
        out << latest.dump(2);
    }

    InsightsHealthInput input;
    input.trajectory_on = true;
    input.insights_home = home / "insights";
    InsightsWorkspaceRef ref;
    ref.workspace_key = "ws";
    ref.sessions_root = home / "sessions";
    input.workspaces.push_back(ref);
    input.pricing_loaded = true;
    input.pricing_note = "user-list-price";
    input.now_yyyymmdd = "20260831";

    const std::vector<InsightsHealthLine> lines = CheckInsightsHealth(input);
    const auto* latest_line = FindLine(lines, "最近报告");
    REQUIRE(latest_line != nullptr);
    CHECK(latest_line->status == ' ');
    CHECK(latest_line->detail.find("2026-08-31T10:00:00Z") != std::string::npos);
    CHECK(latest_line->detail.find(kInsightsAnalyzerVersion) != std::string::npos);
    const auto* summaries = FindLine(lines, "摘要账");
    REQUIRE(summaries != nullptr);
    CHECK(summaries->detail.find("stale 1") != std::string::npos);
    CHECK(summaries->detail.find("坏 JSON 1") != std::string::npos);
    CHECK(summaries->detail.find("扫描 2 场") != std::string::npos);
}
