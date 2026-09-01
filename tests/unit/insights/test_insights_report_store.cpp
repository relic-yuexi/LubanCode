// 报告仓的验收册(Token 账本单 A5):
//   1. 报告落位与文件名(workspace label 折叠、since 段);
//   2. latest.* 原子替换:重写后内容换新、无 .tmp 残留;
//   3. 清单按文件名排序;
//   4. clean --derived-only:只删派生摘要,Journal 与报告不动;
//   5. containment:词法逃逸(..)不放行。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "insights/derived_store.hpp"
#include "insights/report_store.hpp"

using namespace lubancode;
using namespace lubancode::insights;

namespace {

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::filesystem::path TempRoot(const char* name) {
    const auto root = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

}  // namespace

TEST_CASE("报告落位:文件名折叠 + latest 原子替换") {
    const auto home = TempRoot("lubancode-a5-store");
    CHECK(WorkspaceFileLabel("lubancode-0123456789ab") == "lubancode");
    // 非安全字符逐字节折 '_'(hash 段先剥掉;"我的 仓库!" 是 14 个 UTF-8 字节)。
    CHECK(WorkspaceFileLabel("我的 仓库!-0123456789ab") == std::string(14, '_'));
    CHECK(WorkspaceFileLabel("") == "ws");  // 空给兜底名

    const InsightsWriteResult first =
        WriteInsightsReportFiles(home, "20260831-101500", "lubancode", 7, "{\"a\":1}", "<html>1");
    REQUIRE(first.ok);
    CHECK(first.paths.json_path.filename() == "20260831-101500-lubancode-7d.json");
    CHECK(first.paths.html_path.filename() == "20260831-101500-lubancode-7d.html");
    CHECK(ReadFile(first.paths.latest_json) == "{\"a\":1}");
    CHECK(ReadFile(first.paths.latest_html) == "<html>1");

    // 再写一轮:latest 换新,历史保留,无 .tmp 残留。
    const InsightsWriteResult second =
        WriteInsightsReportFiles(home, "20260831-110000", "lubancode", 7, "{\"a\":2}", "<html>2");
    REQUIRE(second.ok);
    CHECK(ReadFile(second.paths.latest_json) == "{\"a\":2}");
    CHECK(std::filesystem::file_size(first.paths.json_path) > 0);  // 历史还在
    const auto reports = ListInsightsReports(home);
    REQUIRE(reports.size() == 4);  // 两轮 × json/html
    CHECK(reports[0].path.filename() < reports[2].path.filename());  // 排序稳定

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(home, ec)) {
        const std::string name = entry.path().filename().string();
        CHECK(name.find(".tmp") == std::string::npos);
    }
}

TEST_CASE("clean --derived-only:只删派生摘要,Journal 与报告不动") {
    const auto root = TempRoot("lubancode-a5-clean") / "sessions";
    const auto session = root / "20260831-000001-CL0001";
    {
        std::error_code ec;
        std::filesystem::create_directories(session / "derived" / kDerivedAnalyzerDir, ec);
        std::filesystem::create_directories(session / "artifacts", ec);
        WriteFile(session / "main.jsonl", "{\"journal\":true}\n");
        WriteFile(SessionSummaryPath(session), "{\"summary\":true}");
        WriteFile(session / "derived" / kDerivedAnalyzerDir / "extra.json", "{}");
    }
    const auto home = TempRoot("lubancode-a5-clean-home");
    WriteInsightsReportFiles(home, "20260831-101500", "ws", 30, "{}", "<html>");

    const InsightsCleanPlan plan = PlanInsightsDerivedClean({root});
    REQUIRE(plan.ok);
    REQUIRE(plan.items.size() == 2);  // session-summary.json + extra.json(同层)
    std::uintmax_t bytes = 0;
    for (const auto& item : plan.items) {
        bytes += item.bytes;
    }
    CHECK(plan.total_bytes == bytes);

    const InsightsCleanResult result = ApplyInsightsClean(plan);
    CHECK(result.deleted_files == 2);
    CHECK(result.errors.empty());
    CHECK(std::filesystem::is_regular_file(session / "main.jsonl"));  // Journal 不动
    CHECK(!std::filesystem::exists(SessionSummaryPath(session)));      // 摘要没了
    CHECK(std::filesystem::is_regular_file(home / "reports" /
                                            "20260831-101500-ws-30d.json"));  // 报告保留

    // 空账:再列一遍是空的,apply 是 no-op。
    const InsightsCleanPlan empty = PlanInsightsDerivedClean({root});
    CHECK(empty.items.empty());
}

TEST_CASE("containment:词法逃逸不放行") {
    const auto root = TempRoot("lubancode-a5-contain");
    CHECK(IsPathContained(root, root / "child" / "file.json"));
    CHECK(IsPathContained(root, root));
    CHECK(!IsPathContained(root, root / ".." / "escape.json"));
    CHECK(!IsPathContained(root, root.parent_path() / "outside.json"));
    CHECK(!IsPathContained(root / "missing", root / "missing" / "x"));
}
