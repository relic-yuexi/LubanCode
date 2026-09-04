// A0 验收线:不接真实 runtime,从十二场合成 Journal 稳定产出
// usage/session/report JSON(Token 账本单 §15.2/§16 A0)。
//
// 两条硬验收:
//   1. 同一批夹具在两个目录各建一回,产出的三份 JSON 字节一致(golden 可比);
//   2. 事实断言:unknown 不冒充 0、重试两笔分开、主/子账分开、
//      active/truncated/corrupt 不进分析分母。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/usage_projector.hpp"
#include "accounting/usage_sample.hpp"
#include "insights_fixtures.hpp"
#include "insights/report_model.hpp"
#include "insights/session_summary.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/schema.hpp"

namespace {

using namespace lubancode;
using namespace lubancode::insights_fixtures;

constexpr const char* kWorkspaceKey = "ws-000000000000";

struct StreamRecord {
    std::string run_id;
    std::filesystem::path path;
};

struct SessionRecord {
    std::string session_id;
    std::vector<StreamRecord> streams;
    bool sealed = true;
};

// 十二场夹具(§15.2)。session id 与 run id 全部定死,两个目录建出来字节
// 一样,golden 才可比。
std::vector<SessionRecord> BuildAll(const std::filesystem::path& root) {
    std::vector<SessionRecord> sessions;
    const FixedClock clock;
    int dir_seq = 0;
    const auto next_dir = [&](const char* tag) {
        ++dir_seq;
        return PrepareDir(root / (std::string(tag) + "-" + std::to_string(dir_seq)));
    };
    const auto open_stream = [&](const std::filesystem::path& dir, const std::string& session_id,
                                 const std::string& run_id, RunKind kind, int schema_version) {
        return FixtureStream(dir / (run_id + ".jsonl"), dir / "artifacts", kWorkspaceKey,
                             session_id, run_id, kind, schema_version, clock);
    };

    {
        // 1. 单请求纯文本,usage 全报。
        const auto dir = next_dir("f01");
        auto stream = open_stream(dir, "20260830-000001-FX001", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec usage;
        usage.input = 1200;
        usage.cache_read = 48000;
        usage.output = 1800;
        usage.reasoning = 900;
        usage.provider_response_id = "resp_x7";
        usage.cache_epoch = 3;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        stream.EndTurn("turn-0001");
        stream.Seal();
        sessions.push_back({"20260830-000001-FX001", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        // 2. 一轮三步工具,cache 逐步命中。
        const auto dir = next_dir("f02");
        auto stream = open_stream(dir, "20260830-000002-FX002", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec step1;
        step1.input = 20000;
        step1.output = 300;
        step1.reasoning = 100;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", step1, true, "call-0001");
        stream.RunTool("turn-0001", "call-0001");
        UsageSpec step2;
        step2.input = 800;
        step2.cache_read = 30000;
        step2.output = 350;
        step2.reasoning = 120;
        step2.cache_epoch = 2;
        stream.ModelExchange("turn-0001", "req-0002", "main_turn", step2, true, "call-0002");
        stream.RunTool("turn-0001", "call-0002");
        UsageSpec step3;
        step3.input = 600;
        step3.cache_read = 40000;
        step3.output = 400;
        step3.cache_epoch = 2;
        stream.ModelExchange("turn-0001", "req-0003", "main_turn", step3);
        stream.EndTurn("turn-0001");
        stream.Seal();
        sessions.push_back({"20260830-000002-FX002", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        // 3. provider 不报 usage。
        const auto dir = next_dir("f03");
        auto stream = open_stream(dir, "20260830-000003-FX003", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec usage;
        usage.reported = false;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        stream.EndTurn("turn-0001");
        stream.Seal();
        sessions.push_back({"20260830-000003-FX003", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        // 4. 失败后重试,前后都花 token。
        const auto dir = next_dir("f04");
        auto stream = open_stream(dir, "20260830-000004-FX004", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec attempt1;
        attempt1.input = 5000;
        attempt1.output = 60;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", attempt1, false, "", 1, true);
        UsageSpec attempt2;
        attempt2.input = 300;
        attempt2.cache_read = 5000;
        attempt2.output = 900;
        attempt2.reasoning = 300;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", attempt2, false, "", 2);
        stream.EndTurn("turn-0001");
        stream.Seal();
        sessions.push_back({"20260830-000004-FX004", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        // 5. main 派两只 subagent,各自独立账。
        const auto dir = next_dir("f05");
        auto main = open_stream(dir, "20260830-000005-FX005", "main-0001", RunKind::MainSession, 2);
        main.StartRun();
        main.StartTurn("turn-0001");
        UsageSpec dispatch;
        dispatch.input = 9000;
        dispatch.output = 200;
        main.ModelExchange("turn-0001", "req-0001", "main_turn", dispatch, true, "call-0001");
        main.RunTool("turn-0001", "call-0001");
        main.EndTurn("turn-0001");
        main.Seal();
        auto sub1 = open_stream(dir, "20260830-000005-FX005", "subagent-0001", RunKind::Subagent, 2);
        sub1.StartRun("subagent_dispatch");
        sub1.StartTurn("turn-0001", "peer_agent");
        UsageSpec sub1_usage;
        sub1_usage.input = 4000;
        sub1_usage.output = 700;
        sub1.ModelExchange("turn-0001", "req-0001", "subagent_turn", sub1_usage);
        sub1.EndTurn("turn-0001");
        sub1.Seal();
        auto sub2 = open_stream(dir, "20260830-000005-FX005", "subagent-0002", RunKind::Subagent, 2);
        sub2.StartRun("subagent_dispatch");
        sub2.StartTurn("turn-0001", "peer_agent");
        UsageSpec sub2_usage;
        sub2_usage.reported = false;
        sub2.ModelExchange("turn-0001", "req-0001", "subagent_turn", sub2_usage);
        sub2.EndTurn("turn-0001");
        sub2.Seal();
        sessions.push_back({"20260830-000005-FX005",
                            {{"main-0001", dir / "main-0001.jsonl"},
                             {"subagent-0001", dir / "subagent-0001.jsonl"},
                             {"subagent-0002", dir / "subagent-0002.jsonl"}},
                            true});
    }
    {
        // 6. workflow + retry。
        const auto dir = next_dir("f06");
        auto stream = open_stream(dir, "20260830-000006-FX006", "workflow-0001", RunKind::Workflow, 2);
        stream.StartRun("workflow_node");
        stream.StartTurn("turn-0001", "scheduled_host");
        UsageSpec attempt1;
        attempt1.input = 2000;
        attempt1.output = 40;
        stream.ModelExchange("turn-0001", "req-0001", "workflow_node", attempt1, false, "", 1, true);
        UsageSpec attempt2;
        attempt2.input = 150;
        attempt2.cache_read = 2000;
        attempt2.output = 500;
        stream.ModelExchange("turn-0001", "req-0001", "workflow_node", attempt2, false, "", 2);
        stream.EndTurn("turn-0001");
        stream.Seal();
        sessions.push_back({"20260830-000006-FX006", {{"workflow-0001", dir / "workflow-0001.jsonl"}}, true});
    }
    {
        // 7. compact map/reduce 独立 purpose。
        const auto dir = next_dir("f07");
        auto stream = open_stream(dir, "20260830-000007-FX007", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001", "scheduled_host");
        UsageSpec map_usage;
        map_usage.input = 60000;
        map_usage.cache_read = 20000;
        map_usage.output = 1200;
        stream.ModelExchange("turn-0001", "req-0001", "compact_map", map_usage);
        UsageSpec reduce_usage;
        reduce_usage.input = 1300;
        reduce_usage.output = 800;
        stream.ModelExchange("turn-0001", "req-0002", "compact_reduce", reduce_usage);
        stream.EndTurn("turn-0001");
        stream.Seal();
        sessions.push_back({"20260830-000007-FX007", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        // 8. toolset 排序抖,cache miss。
        const auto dir = next_dir("f08");
        auto stream = open_stream(dir, "20260830-000008-FX008", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        for (int i = 1; i <= 3; ++i) {
            UsageSpec usage;
            usage.input = 1000;
            usage.cache_read = i == 1 ? 0 : 100;
            usage.output = 200;
            const nlohmann::json snapshot{{"toolset_hash", "th-" + std::to_string(i)},
                                          {"prompt_manifest", "embed:manifest-" + std::to_string(i)}};
            stream.ModelExchange("turn-0001", "req-000" + std::to_string(i), "main_turn", usage,
                                 false, "", 1, false, false, snapshot);
        }
        stream.EndTurn("turn-0001");
        stream.Seal();
        sessions.push_back({"20260830-000008-FX008", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        // 9. prompt 多层重复与 override(v1 legacy stream:usage 挂 completed)。
        const auto dir = next_dir("f09");
        auto stream = open_stream(dir, "20260830-000009-FX009", "main-0001", RunKind::MainSession, 1);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec first;
        first.input = 8000;
        first.output = 500;
        const nlohmann::json snapshot{{"toolset_hash", "th-1"},
                                      {"prompt_manifest", "embed:dup-segments"}};
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", first, false, "", 1, false,
                             false, snapshot);
        UsageSpec second;
        second.input = 200;
        second.cache_read = 8000;
        second.output = 300;
        stream.ModelExchange("turn-0001", "req-0002", "main_turn", second);
        stream.EndTurn("turn-0001");
        stream.Seal();
        sessions.push_back({"20260830-000009-FX009", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        // 10. verification fail 后修好。
        const auto dir = next_dir("f10");
        auto stream = open_stream(dir, "20260830-000010-FX010", "main-0001", RunKind::MainSession, 2);
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
        sessions.push_back({"20260830-000010-FX010", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        // 11. 用户纠正、任务 partial。
        const auto dir = next_dir("f11");
        auto stream = open_stream(dir, "20260830-000011-FX011", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec first;
        first.input = 4000;
        first.output = 400;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", first);
        stream.EndTurn("turn-0001");
        stream.StartTurn("turn-0002");
        UsageSpec second;
        second.input = 300;
        second.cache_read = 4000;
        second.output = 900;
        stream.ModelExchange("turn-0002", "req-0002", "main_turn", second);
        stream.AssessOutcome("turn-0002", "partial");
        stream.EndTurn("turn-0002");
        stream.Seal();
        sessions.push_back({"20260830-000011-FX011", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        // 12. active / truncated / corrupt 各一份。
        const auto dir = next_dir("f12a");
        auto stream = open_stream(dir, "20260830-000012-FX012", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec usage;
        usage.input = 1000;
        usage.output = 100;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        stream.EndTurn("turn-0001");
        (void)stream.recorder().Close();  // 不写 run 终态:session 未封口
        sessions.push_back({"20260830-000012-FX012", {{"main-0001", dir / "main-0001.jsonl"}}, false});
    }
    {
        const auto dir = next_dir("f12b");
        auto stream = open_stream(dir, "20260830-000013-FX013", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec usage;
        usage.input = 1000;
        usage.output = 100;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        stream.EndTurn("turn-0001");
        stream.Seal();
        // 剥掉末尾换行:尾行截断(§16.3)。
        const auto path = dir / "main-0001.jsonl";
        std::ifstream in(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        if (!content.empty() && content.back() == '\n') {
            content.pop_back();
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
        sessions.push_back({"20260830-000013-FX013", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    {
        const auto dir = next_dir("f12c");
        auto stream = open_stream(dir, "20260830-000014-FX014", "main-0001", RunKind::MainSession, 2);
        stream.StartRun();
        stream.StartTurn("turn-0001");
        UsageSpec usage;
        usage.input = 1000;
        usage.output = 100;
        stream.ModelExchange("turn-0001", "req-0001", "main_turn", usage);
        stream.EndTurn("turn-0001");
        stream.Seal();
        // 中行偷改一个字符:hash 链断。
        const auto path = dir / "main-0001.jsonl";
        std::ifstream in(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const std::size_t middle = content.size() / 2;
        for (std::size_t i = middle; i < content.size(); ++i) {
            if (content[i] == '0') {
                content[i] = '1';
                break;
            }
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
        sessions.push_back({"20260830-000014-FX014", {{"main-0001", dir / "main-0001.jsonl"}}, true});
    }
    return sessions;
}

// ---- 投影与摘要(测试侧的确定性装配;A4 的分析器接管后按同一合同产) ----

struct AnalyzedSession {
    std::string session_id;
    bool verified = false;
    bool analyzed = false;
    accounting::UsageProjection usage;
    insights::SessionInsightSummary summary;
};

std::vector<trajectory::EventEnvelope> ParseStream(const std::filesystem::path& path) {
    std::vector<trajectory::EventEnvelope> envelopes;
    const auto lines = trajectory::ReadJournalLines(path);
    REQUIRE(lines.has_value());
    for (const auto& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        REQUIRE(!parsed.is_discarded());
        trajectory::EventEnvelope envelope;
        REQUIRE(!trajectory::ParseAndValidateEventLine(parsed, &envelope).has_value());
        envelopes.push_back(std::move(envelope));
    }
    return envelopes;
}

AnalyzedSession AnalyzeSession(const SessionRecord& record) {
    AnalyzedSession result;
    result.session_id = record.session_id;
    // IntegrityGate 的 A0 替身:每条 stream 验账;有一条不过整间排除。
    bool all_ok = true;
    std::vector<std::pair<std::string, std::vector<trajectory::EventEnvelope>>> streams;
    for (const auto& stream : record.streams) {
        const auto report = trajectory::VerifyJournalFile(stream.path);
        if (!report.ok) {
            all_ok = false;
        }
        streams.emplace_back(stream.run_id, ParseStream(stream.path));
    }
    result.verified = all_ok;
    if (!all_ok || !record.sealed) {
        return result;
    }
    result.analyzed = true;

    insights::SessionInsightSummary& summary = result.summary;
    summary.source.session_id = record.session_id;
    std::string last_outcome;
    for (auto& [run_id, envelopes] : streams) {
        auto projection = accounting::ProjectUsage(envelopes);
        REQUIRE(projection.ok);
        for (const auto& sample : projection.samples) {
            result.usage.samples.push_back(sample);
            summary.coverage.requests_total += 1;
            if (sample.usage.has_value()) {
                summary.usage.requests_with_usage += 1;
                summary.usage.input_tokens += sample.usage->input_tokens;
                summary.usage.cache_read_tokens += sample.usage->cache_read_tokens;
                summary.usage.cache_creation_tokens += sample.usage->cache_creation_tokens;
                summary.usage.output_tokens += sample.usage->output_tokens;
                summary.usage.reasoning_tokens += sample.usage->output_reasoning_tokens;
            }
        }
        summary.coverage.runs_total += 1;
        summary.coverage.runs_analyzed += 1;
        for (const auto& envelope : envelopes) {
            switch (envelope.kind) {
                case trajectory::EventKind::TurnStarted:
                    summary.work.turns += 1;
                    break;
                case trajectory::EventKind::ToolResultCommitted:
                    summary.work.tool_calls += 1;
                    break;
                case trajectory::EventKind::VerificationRecorded:
                    summary.work.verifications += 1;
                    break;
                case trajectory::EventKind::OutcomeAssessed:
                    summary.coverage.outcomes_assessed += 1;
                    last_outcome = envelope.payload.value("outcome", "");
                    break;
                default:
                    break;
            }
        }
        summary.source.stream_terminal_hashes[run_id] = envelopes.back().event_hash;
    }
    summary.usage.requests_total = summary.coverage.requests_total;
    summary.work.outcome = last_outcome;
    return result;
}

struct PipelineOutput {
    std::map<std::string, std::string> files;  // 相对名 -> JSON 字节
    std::vector<AnalyzedSession> analyzed;
    std::size_t sessions_found = 0;
    std::size_t sessions_excluded = 0;
};

PipelineOutput RunPipeline(const std::filesystem::path& root) {
    PipelineOutput output;
    const auto records = BuildAll(root);
    output.sessions_found = records.size();
    insights::InsightsReport report;
    report.generated_at = "2026-08-30T00:00:00Z";
    report.scope.workspace_key = kWorkspaceKey;
    report.scope.since = "2026-08-01";
    report.scope.until = "2026-08-30";
    report.coverage.sessions_found = records.size();
    std::string index;
    for (const auto& record : records) {
        index += record.session_id + ";";
        AnalyzedSession analyzed = AnalyzeSession(record);
        if (!analyzed.analyzed) {
            output.sessions_excluded += 1;
            continue;
        }
        report.coverage.sessions_verified += 1;
        report.coverage.sessions_analyzed += 1;
        report.usage.requests_total += analyzed.summary.usage.requests_total;
        report.usage.requests_with_usage += analyzed.summary.usage.requests_with_usage;
        report.usage.input_tokens += analyzed.summary.usage.input_tokens;
        report.usage.cache_read_tokens += analyzed.summary.usage.cache_read_tokens;
        report.usage.cache_creation_tokens += analyzed.summary.usage.cache_creation_tokens;
        report.usage.output_tokens += analyzed.summary.usage.output_tokens;
        report.usage.reasoning_tokens += analyzed.summary.usage.reasoning_tokens;
        report.sessions.push_back(analyzed.summary);
        // 三份 JSON:usage 样张 / session 摘要 / report。
        nlohmann::json samples_json = nlohmann::json::array();
        for (const auto& sample : analyzed.usage.samples) {
            samples_json.push_back(sample.ToJson());
        }
        output.files[record.session_id + ".usage.json"] = samples_json.dump(2);
        output.files[record.session_id + ".session-summary.json"] =
            analyzed.summary.ToJson().dump(2);
        output.analyzed.push_back(std::move(analyzed));
    }
    report.usage.requests_unknown =
        report.usage.requests_total - report.usage.requests_with_usage;
    report.coverage.sessions_pending = 0;
    report.coverage.sessions_excluded = output.sessions_excluded;
    output.files["report.json"] = report.ToJson().dump(2);
    output.files["index.txt"] = index;
    return output;
}

const AnalyzedSession* FindAnalyzed(const PipelineOutput& pipeline, const std::string& session_id) {
    for (const auto& session : pipeline.analyzed) {
        if (session.session_id == session_id) {
            return &session;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("A0 验收:合成 Journal 稳定产出 usage/session/report JSON,字节可比") {
    const auto root_a = std::filesystem::temp_directory_path() / "lubancode-a0-pipeline-a";
    const auto root_b = std::filesystem::temp_directory_path() / "lubancode-a0-pipeline-b";
    const PipelineOutput a = RunPipeline(root_a);
    const PipelineOutput b = RunPipeline(root_b);
    REQUIRE(a.files.size() == b.files.size());
    for (const auto& [name, bytes] : a.files) {
        INFO(name.c_str());
        REQUIRE(b.files.contains(name));
        CHECK(b.files.at(name) == bytes);
    }
    // 保存一份肉眼可查的产物(golden 诊断用,不进仓库)。
    for (const auto& [name, bytes] : a.files) {
        std::ofstream out(root_a / name, std::ios::binary | std::ios::trunc);
        out << bytes;
    }
}

TEST_CASE("A0 事实:coverage、unknown、重试、主子账、排除账") {
    const auto root = std::filesystem::temp_directory_path() / "lubancode-a0-facts";
    const PipelineOutput pipeline = RunPipeline(root);

    // 14 间 found(F1-F11 共 11 间 + F12 三间),11 间进分析,3 间排除。
    CHECK(pipeline.sessions_found == 14);
    CHECK(pipeline.sessions_excluded == 3);
    CHECK(pipeline.analyzed.size() == 11);

    // F1:全报的单请求,total_input 只走 TotalInputTokens 口径。
    const auto* f1 = FindAnalyzed(pipeline, "20260830-000001-FX001");
    REQUIRE(f1 != nullptr);
    REQUIRE(f1->usage.samples.size() == 1);
    CHECK(f1->usage.samples[0].usage_source == accounting::UsageSource::ProviderReported);
    CHECK(f1->usage.samples[0].total_input_tokens == 1200 + 48000);
    CHECK(f1->usage.samples[0].total_billed_shape_tokens == 1200 + 48000 + 1800);
    CHECK(f1->usage.samples[0].provider_response_id == std::optional<std::string>("resp_x7"));
    CHECK(f1->usage.samples[0].request_outcome == "completed");
    CHECK(!f1->usage.samples[0].legacy_owner);

    // F3:provider 不报,usage 为空、source=unknown,不拿 0 冒充。
    const auto* f3 = FindAnalyzed(pipeline, "20260830-000003-FX003");
    REQUIRE(f3 != nullptr);
    REQUIRE(f3->usage.samples.size() == 1);
    CHECK(f3->usage.samples[0].usage_source == accounting::UsageSource::Unknown);
    CHECK(!f3->usage.samples[0].usage.has_value());
    CHECK(f3->summary.usage.requests_with_usage == 0);
    CHECK(f3->summary.usage.input_tokens == 0);

    // F4:重试两笔分开,失败那笔的 token 照记。
    const auto* f4 = FindAnalyzed(pipeline, "20260830-000004-FX004");
    REQUIRE(f4 != nullptr);
    REQUIRE(f4->usage.samples.size() == 2);
    CHECK(f4->usage.samples[0].attempt == 1);
    CHECK(f4->usage.samples[0].request_outcome == "failed");
    CHECK(f4->usage.samples[0].usage.has_value());
    CHECK(f4->usage.samples[1].attempt == 2);
    CHECK(f4->usage.samples[1].request_outcome == "completed");

    // F5:主与两只 subagent 分账,runs_total=3;sub2 没报 usage。
    const auto* f5 = FindAnalyzed(pipeline, "20260830-000005-FX005");
    REQUIRE(f5 != nullptr);
    CHECK(f5->summary.coverage.runs_total == 3);
    REQUIRE(f5->usage.samples.size() == 3);
    CHECK(f5->usage.samples[0].run_kind == "main_session");
    CHECK(f5->usage.samples[1].run_kind == "subagent");
    CHECK(f5->usage.samples[1].purpose == accounting::RequestPurpose::SubagentTurn);
    CHECK(f5->usage.samples[2].usage_source == accounting::UsageSource::Unknown);
    CHECK(f5->summary.usage.requests_with_usage == 2);

    // F9:v1 legacy stream:usage 挂 completed,投影标 legacy_owner/legacy_inferred。
    const auto* f9 = FindAnalyzed(pipeline, "20260830-000009-FX009");
    REQUIRE(f9 != nullptr);
    REQUIRE(f9->usage.samples.size() == 2);
    CHECK(f9->usage.samples[0].legacy_owner);
    CHECK(f9->usage.samples[0].legacy_inferred);
    CHECK(f9->usage.samples[0].usage_source == accounting::UsageSource::ProviderReported);

    // F10/F11:验收与 outcome 进摘要。
    const auto* f10 = FindAnalyzed(pipeline, "20260830-000010-FX010");
    REQUIRE(f10 != nullptr);
    CHECK(f10->summary.work.verifications == 2);
    CHECK(f10->summary.work.outcome == "passed");
    const auto* f11 = FindAnalyzed(pipeline, "20260830-000011-FX011");
    REQUIRE(f11 != nullptr);
    CHECK(f11->summary.work.outcome == "partial");

    // F12 三间:active/truncated/corrupt 都不进分析。
    CHECK(FindAnalyzed(pipeline, "20260830-000012-FX012") == nullptr);
    CHECK(FindAnalyzed(pipeline, "20260830-000013-FX013") == nullptr);
    CHECK(FindAnalyzed(pipeline, "20260830-000014-FX014") == nullptr);

    // 报告总账:unknown 单列,不估数补进总数。
    std::uint64_t expected_total = 0;
    std::uint64_t expected_with = 0;
    for (const auto& session : pipeline.analyzed) {
        expected_total += session.summary.usage.requests_total;
        expected_with += session.summary.usage.requests_with_usage;
    }
    CHECK(expected_total > expected_with);
    CHECK(expected_with >= 14);
}
