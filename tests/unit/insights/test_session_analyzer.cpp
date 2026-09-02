// Session Analyzer 的验收册(Token 账本单 A4):
//   1. 同一封口 session 重算,session-summary.json 字节相同(删 derived 重跑可比);
//   2. fresh 摘要直接复用,stale(改 analyzer 版本/改 Journal)删掉重算;
//   3. active/incomplete/corrupt 在 workspace 扫描里单列,坏账排除且理由可见;
//   4. 摘要合同:work/coverage/usage/friction/信号齐,版本账在册;
//   5. /prompt audit outcome 的抬升面只摆信号不越界。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "insights/derived_store.hpp"
#include "insights/report_model.hpp"  // kInsightsAnalyzerVersion(版本账)
#include "insights/session_analyzer.hpp"

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
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

// 一场"干活"的封口 session:一轮工具 + 两次验证(一败一成)+ outcome passed。
std::filesystem::path WorkingSession(const std::filesystem::path& dir,
                                     const std::string& session_id) {
    const FixedClock clock;
    FixtureStream stream(dir / "main.jsonl", dir / "artifacts", kWorkspaceKey, session_id,
                         "main-0001", trajectory::RunKind::MainSession, 2, clock);
    stream.StartRun();
    stream.StartTurn("turn-0001");
    UsageSpec probe;
    probe.input = 5000;
    probe.output = 300;
    stream.ModelExchange("turn-0001", "req-0001", "main_turn", probe, true, "call-0001");
    stream.RunTool("turn-0001", "call-0001", true);
    stream.RecordVerification("turn-0001", "ver-0001", false);
    UsageSpec fix;
    fix.input = 200;
    fix.cache_read = 5000;
    fix.output = 600;
    stream.ModelExchange("turn-0001", "req-0002", "main_turn", fix);
    stream.RecordVerification("turn-0001", "ver-0002", true);
    stream.AssessOutcome("turn-0001", "passed");
    stream.EndTurn("turn-0001");
    stream.Seal();
    WriteFixtureSessionJson(dir, kWorkspaceKey, session_id, "closed");
    return dir;
}

}  // namespace

TEST_CASE("A4 验收:同一封口 session 重算字节相同;fresh 复用;stale 重算") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-analyzer";
    const auto dir = PrepareDir(root / "s1");
    WorkingSession(dir, "20260831-000001-AN0001");

    SessionAnalyzeOptions options;
    const SessionAnalyzeResult first = AnalyzeSession(dir, options);
    REQUIRE(first.analyzed);
    REQUIRE(first.summary_written);
    const std::filesystem::path summary_path = SessionSummaryPath(dir);
    const std::string bytes_a = ReadFile(summary_path);
    CHECK(!bytes_a.empty());

    // 同输入再算:fresh 摘要直接复用,不重写。
    const SessionAnalyzeResult second = AnalyzeSession(dir, options);
    REQUIRE(second.analyzed);
    CHECK(second.summary_reused);
    CHECK(!second.summary_written);
    CHECK(ReadFile(summary_path) == bytes_a);

    // 删掉 derived 重算:字节一致(可重建,§16 A4 验收)。
    std::error_code ec;
    std::filesystem::remove_all(dir / "derived", ec);
    const SessionAnalyzeResult third = AnalyzeSession(dir, options);
    REQUIRE(third.summary_written);
    CHECK(ReadFile(summary_path) == bytes_a);

    // stale:篡改 analyzer 版本后判 stale,重算换新(字节回到真值)。
    {
        auto parsed = nlohmann::json::parse(bytes_a, nullptr, false);
        REQUIRE(!parsed.is_discarded());
        parsed["analyzer_version"] = "insights-v0-ancient";
        WriteFile(summary_path, parsed.dump(2) + "\n");
    }
    const DerivedReadResult stale_read = ReadExistingSessionSummary(dir);
    REQUIRE(stale_read.exists);
    REQUIRE(stale_read.parse_ok);
    CHECK(IsSummaryStale(stale_read, first.gate.stream_terminal_hashes));
    const SessionAnalyzeResult fourth = AnalyzeSession(dir, options);
    CHECK(fourth.summary_written);
    CHECK(ReadFile(summary_path) == bytes_a);
}

TEST_CASE("A4 摘要合同:work/coverage/usage/friction/版本账齐") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-contract";
    const auto dir = PrepareDir(root / "s2");
    WorkingSession(dir, "20260831-000002-AN0002");

    SessionAnalyzeOptions options;
    const SessionAnalyzeResult result = AnalyzeSession(dir, options);
    REQUIRE(result.analyzed);
    const SessionInsightSummary& summary = result.summary;

    CHECK(summary.analyzer_version == kInsightsAnalyzerVersion);
    CHECK(summary.source.integrity == "verified");
    CHECK(summary.source.session_id == "20260831-000002-AN0002");
    REQUIRE(summary.source.stream_terminal_hashes.size() == 1);
    CHECK(summary.coverage.runs_total == 1);
    CHECK(summary.coverage.runs_analyzed == 1);
    CHECK(summary.coverage.requests_total == 2);
    CHECK(summary.coverage.requests_with_usage == 2);
    CHECK(summary.coverage.outcomes_assessed == 1);
    CHECK(summary.work.turns == 1);
    CHECK(summary.work.tool_calls == 1);
    CHECK(summary.work.verifications == 2);
    CHECK(summary.work.outcome == "passed");
    CHECK(summary.usage.requests_with_usage == 2);
    CHECK(summary.usage.input_tokens == 5200);
    CHECK(summary.usage.cache_read_tokens == 5000);
    CHECK(summary.usage.cost_status == "not_priced");
    // 摩擦:工具失败 + 验证失败两类在册(排序稳定);passed 前有验证,
    // 不算 verification.missing。
    REQUIRE(summary.friction_events.size() == 2);
    CHECK(summary.friction_events[0] == "tool.execution_failure");
    CHECK(summary.friction_events[1] == "verification.failure");
    // runtime 层 finding 进摘要:夹具的 prepared 没带 snapshot,R01 点名
    // 属预期(缺 manifest 如实报,不冒充层变化分析)。
    REQUIRE(summary.prompt_findings.size() == 1);
    CHECK(summary.prompt_findings[0].finding_id == "P-AUD-R01");

    // JSON 落盘合法,能严格读回。
    const DerivedReadResult read_back = ReadExistingSessionSummary(dir);
    REQUIRE(read_back.parse_ok);
    CHECK(read_back.summary.source.session_id == summary.source.session_id);
}

TEST_CASE("workspace 扫描:active/incomplete/corrupt 单列;since 窗过滤") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-scan" / "sessions";
    {
        std::error_code wipe;
        std::filesystem::remove_all(root, wipe);
        std::filesystem::create_directories(root, wipe);
    }
    {
        const auto dir = PrepareDir(root / "20260831-000001-SCAN01");
        WorkingSession(dir, "20260831-000001-SCAN01");
    }
    {
        const auto dir = PrepareDir(root / "20260831-000002-SCAN02");
        const FixedClock clock;
        FixtureStream stream(dir / "main.jsonl", dir / "artifacts", kWorkspaceKey,
                             "20260831-000002-SCAN02", "main-0001",
                             trajectory::RunKind::MainSession, 2, clock);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec usage;
        usage.input = 100;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        stream.EndTurn("turn-0001");
        (void)stream.recorder().Close();  // 不封口
        WriteFixtureSessionJson(dir, kWorkspaceKey, "20260831-000002-SCAN02", "running");
    }
    {
        const auto dir = PrepareDir(root / "20260831-000003-SCAN03");
        WorkingSession(dir, "20260831-000003-SCAN03");
        const auto path = dir / "main.jsonl";
        std::string content = ReadFile(path);
        if (!content.empty() && content.back() == '\n') {
            content.pop_back();
        }
        WriteFile(path, content);  // 截断
    }
    {
        const auto dir = PrepareDir(root / "20260831-000004-SCAN04");
        WorkingSession(dir, "20260831-000004-SCAN04");
        const auto path = dir / "main.jsonl";
        std::string content = ReadFile(path);
        for (std::size_t i = content.size() / 2; i < content.size(); ++i) {
            if (content[i] == '0') {
                content[i] = '1';
                break;
            }
        }
        WriteFile(path, content);  // 坏链
    }
    {
        // 时间窗外:2020 年的场,since 30d 不收。
        const auto dir = PrepareDir(root / "20200101-000005-SCAN05");
        WorkingSession(dir, "20200101-000005-SCAN05");
    }

    const WorkspaceScanReport report =
        ScanWorkspaceSessions(root, "20260901", 30);
    CHECK(report.sessions_found == 4);
    REQUIRE(report.entries.size() == 4);
    CHECK(report.status_counts.at("analyzed") == 1);
    CHECK(report.status_counts.at("active") == 1);
    CHECK(report.status_counts.at("incomplete") == 1);
    CHECK(report.status_counts.at("corrupt") == 1);
    bool saw_reason = false;
    for (const auto& entry : report.entries) {
        if (entry.status == SessionGateStatus::Corrupt ||
            entry.status == SessionGateStatus::Incomplete) {
            if (!entry.reason.empty()) {
                saw_reason = true;
            }
        }
    }
    CHECK(saw_reason);
}

TEST_CASE("include_active:读高水位,成色 provisional,不写长期摘要") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-active";
    const auto dir = PrepareDir(root / "s3");
    const FixedClock clock;
    {
        FixtureStream stream(dir / "main.jsonl", dir / "artifacts", kWorkspaceKey,
                             "20260831-000003-AN0003", "main-0001",
                             trajectory::RunKind::MainSession, 2, clock);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec usage;
        usage.input = 100;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        stream.EndTurn("turn-0001");
        (void)stream.recorder().Close();
    }
    SessionAnalyzeOptions skip;
    const SessionAnalyzeResult skipped = AnalyzeSession(dir, skip);
    CHECK(!skipped.analyzed);
    CHECK(skipped.gate.status == SessionGateStatus::Active);

    SessionAnalyzeOptions include;
    include.include_active = true;
    const SessionAnalyzeResult analyzed = AnalyzeSession(dir, include);
    REQUIRE(analyzed.analyzed);
    CHECK(analyzed.provisional);
    CHECK(analyzed.summary.source.integrity == "provisional");
    CHECK(!analyzed.summary_written);  // active 不写长期摘要(§14.2)
}

// 单发轨迹断档单:one_shot 的 main run 与交互 main 同类——token 计入
// main 侧,不冒充子代理。若是归错了类,这笔 8M 输入的重会话会触发 FS-04
//(子代理 token 超主会话)的收窄建议;归对了就不出该信号。
TEST_CASE("one_shot 场: usage 计入 main 侧,不冒充子代理") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-oneshot-classify";
    const auto dir = PrepareDir(root / "s1");
    const FixedClock clock;
    {
        FixtureStream stream(dir / "main.jsonl", dir / "artifacts", kWorkspaceKey,
                             "20260831-000001-ONE001", "main-0001",
                             trajectory::RunKind::OneShot, 2, clock);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec usage;
        usage.input = 1000;
        usage.cache_read = 8000000;
        usage.output = 50000;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        stream.EndTurn("turn-0001");
        stream.Seal();
        WriteFixtureSessionJson(dir, kWorkspaceKey, "20260831-000001-ONE001", "closed");
    }
    const SessionAnalyzeResult analyzed = AnalyzeSession(dir, SessionAnalyzeOptions{});
    REQUIRE(analyzed.analyzed);
    CHECK(analyzed.summary.usage.requests_with_usage == 1);
    CHECK(analyzed.summary.usage.cache_read_tokens == 8000000);
    // usage 进了投影(合计),且没有"子代理超主会话"的收窄信号。
    bool saw_subagent_shrink = false;
    for (const auto& signal_id : analyzed.summary.feature_signals) {
        if (signal_id == "FS-04") {
            saw_subagent_shrink = true;
        }
    }
    CHECK_FALSE(saw_subagent_shrink);
}

TEST_CASE("功能信号:同类验证反复与子代理 token 超主会话") {
    SUBCASE("同类验证 ≥3 次 → 固化建议(先决写明)") {
        FeatureSignalInput input;
        input.session_id = "s";
        input.verification_kinds["build"] = 3;
        const std::vector<FeatureSignal> signals = DetectFeatureSignals(input);
        REQUIRE(signals.size() == 1);
        CHECK(signals[0].signal_id == "FS-01");
        CHECK(signals[0].feature.find("/skill") != std::string::npos);
        CHECK(!signals[0].precondition.empty());
        CHECK(signals[0].evidence[1].value == 3);
    }
    SUBCASE("子代理 token 超主会话 → 收窄建议") {
        FeatureSignalInput input;
        input.session_id = "s";
        input.main_tokens = 1000;
        input.subagent_tokens = 5000;
        const std::vector<FeatureSignal> signals = DetectFeatureSignals(input);
        REQUIRE(signals.size() == 1);
        // A5 起 id 按规则钉死(跨场可比):子代理收窄的本命 id 是 FS-04,
        // 只开这一条时也不从 1 重编。
        CHECK(signals[0].signal_id == "FS-04");
        CHECK(signals[0].feature.find("子代理") != std::string::npos);
    }
    SUBCASE("证据不足就不出:全零输入零信号") {
        const std::vector<FeatureSignal> signals = DetectFeatureSignals(FeatureSignalInput{});
        CHECK(signals.empty());
    }
}

TEST_CASE("outcome 抬升面:摩擦折叠成 finding,措辞不越界") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a4-outcome";
    std::vector<SessionAnalyzeResult> sessions;
    {
        const auto dir = PrepareDir(root / "s4");
        WorkingSession(dir, "20260831-000004-AN0004");
        sessions.push_back(AnalyzeSession(dir, SessionAnalyzeOptions{}));
    }
    {
        // 第二场:outcome passed 无验证。
        const auto dir = PrepareDir(root / "s5");
        const FixedClock clock;
        FixtureStream stream(dir / "main.jsonl", dir / "artifacts", kWorkspaceKey,
                             "20260831-000005-AN0005", "main-0001",
                             trajectory::RunKind::MainSession, 2, clock);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec usage;
        usage.input = 300;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        stream.AssessOutcome("turn-0001", "passed");
        stream.EndTurn("turn-0001");
        stream.Seal();
        WriteFixtureSessionJson(dir, kWorkspaceKey, "20260831-000005-AN0005", "closed");
        sessions.push_back(AnalyzeSession(dir, SessionAnalyzeOptions{}));
    }
    const std::vector<Finding> findings = AuditPromptOutcome(sessions);
    // 第一场:验证失败(有验证,不算 missing);第二场:passed 无验证。
    const auto missing = std::find_if(findings.begin(), findings.end(),
                                      [](const Finding& f) {
                                          return f.category == "outcome.verification_missing";
                                      });
    REQUIRE(missing != findings.end());
    CHECK(missing->finding_id == "P-AUD-O01");
    CHECK(missing->summary.find("不能断言假完成") != std::string::npos);
    const auto failure = std::find_if(findings.begin(), findings.end(),
                                      [](const Finding& f) {
                                          return f.category == "outcome.verification_failure";
                                      });
    REQUIRE(failure != findings.end());
    CHECK(failure->finding_id == "P-AUD-O03");
    // 证据落场次与事件引用。
    bool has_event = false;
    for (const auto& item : missing->evidence) {
        if (item.event_id.has_value()) {
            has_event = true;
        }
    }
    CHECK(has_event);
}
