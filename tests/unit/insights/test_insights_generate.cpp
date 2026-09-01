// /insights 生成管线的验收册(Token 账本单 A5):
//   1. 200-session fixture 出报告:coverage 齐、七节材料齐、增量复用;
//   2. 字节稳定:删 derived 重算 report.json 一致;fresh 全复用的第二轮
//      也一致(增量聚合是摘要的纯函数);
//   3. 五态场次:active/incomplete/corrupt 排除且理由可见;--include-active
//      读高水位成 provisional;时间窗外不收;
//   4. 一轮上限:--sessions N 只重算 N 间 stale,其余 pending;fresh 场
//      不占重算名额照进汇总;
//   5. 跨工作区:多 workspace ref 汇总,scope.all_workspaces 置位。
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "insights/derived_store.hpp"
#include "insights/html_renderer.hpp"
#include "insights/insights_generate.hpp"

#include "insights_fixtures.hpp"

using namespace lubancode;
using namespace lubancode::insights;
using namespace lubancode::insights_fixtures;

namespace {

constexpr const char* kWorkspaceKey = "ws-000000000000";

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

// 一场轻量封口场:一轮两请求(一败一成)+ 验证 + outcome。
// 200 场基准用它(每场 ~15 枚事件,不烧模型)。
void LightSession(const std::filesystem::path& dir, const std::string& session_id,
                  std::int64_t input_tokens, bool with_friction) {
    const FixedClock clock;
    FixtureStream stream(dir / "main.jsonl", dir / "artifacts", kWorkspaceKey, session_id,
                         "main-0001", trajectory::RunKind::MainSession, 2, clock);
    stream.StartRun();
    stream.StartTurn("turn-0001");
    UsageSpec first;
    first.input = input_tokens;
    first.output = 300;
    stream.ModelExchange("turn-0001", "req-0001", "main_turn", first, with_friction,
                         "call-0001");
    if (with_friction) {
        stream.RunTool("turn-0001", "call-0001", true);  // 工具失败 → 摩擦
    }
    UsageSpec second;
    second.input = 200;
    second.cache_read = input_tokens;
    second.output = 600;
    stream.ModelExchange("turn-0001", "req-0002", "main_turn", second);
    stream.RecordVerification("turn-0001", "ver-0001", true);
    stream.AssessOutcome("turn-0001", "passed");
    stream.EndTurn("turn-0001");
    stream.Seal();
    WriteFixtureSessionJson(dir, kWorkspaceKey, session_id, "closed");
}

InsightsWorkspaceRef MakeRef(const std::filesystem::path& sessions_root,
                             const std::string& key) {
    InsightsWorkspaceRef ref;
    ref.workspace_key = key;
    ref.readable_name = key;
    ref.sessions_root = sessions_root;
    return ref;
}

InsightsGenerateOptions MakeOptions(int since_days = 30, int max_sessions = 200,
                                    bool include_active = false) {
    InsightsGenerateOptions options;
    options.since_days = since_days;
    options.max_sessions = max_sessions;
    options.include_active = include_active;
    options.now_yyyymmdd = "20260831";
    options.generated_at = "2026-08-31T12:00:00Z";
    return options;
}

std::string SessionId(int index) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "20260830-120000-B%05d", index);
    return buffer;
}

}  // namespace

TEST_CASE("A5 验收:200-session fixture 出报告,七节齐,增量复用") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a5-200" / "sessions";
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        for (int i = 0; i < 200; ++i) {
            const auto dir = PrepareDir(root / SessionId(i));
            LightSession(dir, SessionId(i), 5000 + i, i % 4 == 0);
        }
    }
    const std::vector<InsightsWorkspaceRef> refs = {MakeRef(root, kWorkspaceKey)};

    // 第一轮:全部重算并落摘要。
    const InsightsGenerateResult first = GenerateInsightsReport(refs, MakeOptions());
    REQUIRE(first.ok);
    CHECK(first.counts.found == 200);
    CHECK(first.counts.verified == 200);
    CHECK(first.counts.analyzed == 200);
    CHECK(first.counts.written == 200);
    CHECK(first.counts.reused == 0);
    CHECK(first.counts.pending == 0);
    CHECK(first.counts.excluded == 0);
    CHECK(first.report.coverage.sessions_found == 200);
    CHECK(first.report.sessions.size() == 200);
    CHECK(first.report.scope.workspace_key == kWorkspaceKey);
    CHECK(first.report.scope.since == "2026-08-01");
    CHECK(first.report.scope.until == "2026-08-31");
    // usage 总账 = 摘要求和(每场两笔全报)。
    CHECK(first.aggregate.requests_total == 400);
    CHECK(first.aggregate.requests_with_usage == 400);
    CHECK(first.aggregate.requests_unknown == 0);
    CHECK(first.aggregate.verifications == 200);
    // 摩擦与 finding:50 场带工具失败 → friction rollup + INS finding。
    REQUIRE(first.aggregate.frictions.size() == 1);
    CHECK(first.aggregate.frictions[0].category == "tool.execution_failure");
    CHECK(first.aggregate.frictions[0].sessions == 50);
    bool has_friction_finding = false;
    for (const auto& finding : first.report.findings) {
        if (finding.finding_id == "INS-F06") {  // 封闭表第 6 位 = tool.execution_failure
            has_friction_finding = true;
            CHECK(finding.summary.find("50 场") != std::string::npos);
        }
    }
    CHECK(has_friction_finding);

    // HTML 七节齐。
    const std::string html =
        RenderInsightsHtml(first.report, first.aggregate, first.extras);
    for (const char* anchor : {"sec-overview", "sec-usage", "sec-prompt", "sec-friction",
                               "sec-shape", "sec-suggestions", "sec-coverage"}) {
        CHECK(html.find(anchor) != std::string::npos);
    }
    CHECK(html.find("id=\"session-rows\"") != std::string::npos);

    // 第二轮:fresh 全复用,report.json 字节一致(增量聚合是摘要的纯函数)。
    const InsightsGenerateResult second = GenerateInsightsReport(refs, MakeOptions());
    REQUIRE(second.ok);
    CHECK(second.counts.reused == 200);
    CHECK(second.counts.written == 0);
    CHECK(second.report.ToJson().dump(2) == first.report.ToJson().dump(2));

    // 删 derived 重算:字节仍一致(可重建,§十六 发布门)。
    for (int i = 0; i < 200; ++i) {
        std::error_code ec;
        std::filesystem::remove_all(root / SessionId(i) / "derived", ec);
    }
    const InsightsGenerateResult third = GenerateInsightsReport(refs, MakeOptions());
    REQUIRE(third.ok);
    CHECK(third.counts.written == 200);
    CHECK(third.report.ToJson().dump(2) == first.report.ToJson().dump(2));
}

TEST_CASE("五态场次:排除单列且理由可见;include_active 走高水位") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a5-mixed" / "sessions";
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        // 封口好账。
        LightSession(PrepareDir(root / "20260831-000001-MIX001"), "20260831-000001-MIX001",
                     1000, false);
        // active:不封口。
        {
            const auto dir = PrepareDir(root / "20260831-000002-MIX002");
            const FixedClock clock;
            FixtureStream stream(dir / "main.jsonl", dir / "artifacts", kWorkspaceKey,
                                 "20260831-000002-MIX002", "main-0001",
                                 trajectory::RunKind::MainSession, 2, clock);
            stream.StartRun();
            stream.StartTurn("turn-0001");
            UsageSpec usage;
            usage.input = 100;
            stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
            stream.EndTurn("turn-0001");
            (void)stream.recorder().Close();
            WriteFixtureSessionJson(dir, kWorkspaceKey, "20260831-000002-MIX002", "running");
        }
        // incomplete:截断尾行。
        {
            const auto dir = PrepareDir(root / "20260831-000003-MIX003");
            LightSession(dir, "20260831-000003-MIX003", 1000, false);
            std::string content = ReadFile(dir / "main.jsonl");
            if (!content.empty() && content.back() == '\n') {
                content.pop_back();
            }
            WriteFile(dir / "main.jsonl", content);
        }
        // 时间窗外(2020 年)。
        LightSession(PrepareDir(root / "20200101-000004-MIX004"), "20200101-000004-MIX004",
                     1000, false);
    }
    const std::vector<InsightsWorkspaceRef> refs = {MakeRef(root, kWorkspaceKey)};

    SUBCASE("默认:active/incomplete 排除,坏账理由可见") {
        const InsightsGenerateResult result = GenerateInsightsReport(refs, MakeOptions());
        REQUIRE(result.ok);
        CHECK(result.counts.found == 3);  // 2020 那场被时间窗挡掉
        CHECK(result.counts.verified == 1);
        CHECK(result.counts.excluded == 2);
        REQUIRE(result.extras.excluded.size() == 2);
        bool saw_active = false;
        bool saw_incomplete = false;
        for (const auto& entry : result.extras.excluded) {
            if (entry.status == "active") {
                saw_active = true;
                CHECK(entry.session_id == "20260831-000002-MIX002");
            }
            if (entry.status == "incomplete") {
                saw_incomplete = true;
                CHECK(!entry.reason.empty());
            }
        }
        CHECK(saw_active);
        CHECK(saw_incomplete);
        CHECK(result.report.sessions.size() == 1);
    }

    SUBCASE("--include-active:读高水位,成色 provisional,不写长期摘要") {
        InsightsGenerateOptions options = MakeOptions();
        options.include_active = true;
        const InsightsGenerateResult result = GenerateInsightsReport(refs, options);
        REQUIRE(result.ok);
        CHECK(result.report.sessions.size() == 2);
        CHECK(result.aggregate.provisional_sessions == 1);
        // active 场没有长期摘要(§14.2)。
        CHECK(!std::filesystem::exists(
            SessionSummaryPath(root / "20260831-000002-MIX002")));
    }
}

TEST_CASE("一轮上限:stale 只重算 N 间,其余 pending;fresh 不占名额") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a5-cap" / "sessions";
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        for (int i = 0; i < 3; ++i) {
            LightSession(PrepareDir(root / SessionId(i)), SessionId(i), 1000 + i, false);
        }
    }
    const std::vector<InsightsWorkspaceRef> refs = {MakeRef(root, kWorkspaceKey)};
    // 预跑一轮:三场全 fresh。
    REQUIRE(GenerateInsightsReport(refs, MakeOptions()).ok);
    // 再添两间新场(stale/missing),上限 1:只重算 1 间,1 间 pending;
    // 三间 fresh 照进汇总(§9.1 尾段)。
    for (int i = 3; i < 5; ++i) {
        LightSession(PrepareDir(root / SessionId(i)), SessionId(i), 1000 + i, false);
    }
    const InsightsGenerateResult result =
        GenerateInsightsReport(refs, MakeOptions(30, /*max_sessions=*/1));
    REQUIRE(result.ok);
    CHECK(result.counts.found == 5);
    CHECK(result.counts.reused == 3);
    CHECK(result.counts.written == 1);
    CHECK(result.counts.pending == 1);
    CHECK(result.report.coverage.sessions_pending == 1);
    CHECK(result.report.sessions.size() == 4);  // 3 fresh + 1 重算
}

TEST_CASE("跨工作区:多 workspace ref 汇总,all_workspaces 置位") {
    const auto base =
        std::filesystem::temp_directory_path() / "lubancode-a5-multi";
    const auto root_a = base / "ws-a" / "sessions";
    const auto root_b = base / "ws-b" / "sessions";
    {
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        std::filesystem::create_directories(root_a, ec);
        std::filesystem::create_directories(root_b, ec);
        LightSession(PrepareDir(root_a / "20260831-000001-WSA001"), "20260831-000001-WSA001",
                     1000, false);
        LightSession(PrepareDir(root_b / "20260831-000001-WSB001"), "20260831-000001-WSB001",
                     2000, false);
    }
    const std::vector<InsightsWorkspaceRef> refs = {MakeRef(root_a, "ws-aaaaaaaaaaaa"),
                                                    MakeRef(root_b, "ws-bbbbbbbbbbbb")};
    const InsightsGenerateResult result = GenerateInsightsReport(refs, MakeOptions());
    REQUIRE(result.ok);
    CHECK(result.report.scope.all_workspaces);
    CHECK(result.report.scope.workspace_key == "*");
    CHECK(result.report.sessions.size() == 2);
    CHECK(result.aggregate.input_tokens == 3000 + 200 + 200);  // 每场 1000/2000 + 200 第二笔
    CHECK(result.extras.workspace_names.size() == 2);
    // 归属账:两场各归各的 workspace。
    CHECK(result.extras.session_workspace.at("20260831-000001-WSA001") == "ws-aaaaaaaaaaaa");
    CHECK(result.extras.session_workspace.at("20260831-000001-WSB001") == "ws-bbbbbbbbbbbb");
}
