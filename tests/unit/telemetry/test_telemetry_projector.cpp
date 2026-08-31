// 纯 TelemetryProjector 测试(端云协同可观测单 §11 映射/§14 投影规矩/
// §29.3,T0 验收线"同一 Journal 重放两次 ids/span tree/metrics 完全稳定;
// D1 fixture 无正文与路径"):
//   - golden Journal(trajectory v1 夹具)投出稳定 span 树与 metrics;
//   - 确定性:投影两次逐字节相同;换钥匙 id 变、结构不变;
//   - D1:全量输出无正文、无用户文本、无路径;
//   - 坏链停整条 stream(source_corrupt),不跳坏行接着猜;
//   - 合成 Journal:approval/compact/verification/retry/悬空收口。
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "telemetry/contract.hpp"
#include "telemetry/identity.hpp"
#include "telemetry/projector.hpp"
#include "trajectory/recorder.hpp"

using namespace lubancode::telemetry;
using namespace lubancode::trajectory;

#ifndef LUBANCODE_TEST_FIXTURES_DIR
#define LUBANCODE_TEST_FIXTURES_DIR "."
#endif

namespace {

class FixedClock : public RecorderClock {
public:
    std::int64_t WallMs() const override { return 1759000000000LL; }
    std::int64_t MonotonicNs() const override { return 777777000LL; }
};

ProjectorOptions TestOptions() {
    ProjectorOptions options;
    options.projection_key = "test-projection-key-v1";  // 测试假钥匙,非真密
    options.resource.service_version = "0.26.0-test";
    options.resource.service_instance_id = "proc-test-0001";
    options.resource.os_type = "windows";
    options.resource.host_arch = "amd64";
    options.resource.device_instance_id = "device-test-0001";
    options.resource.workspace_key = "ws-test-000000000000";
    options.resource.frontend = "terminal";
    options.resource.trajectory_schema_version = 1;
    return options;
}

std::filesystem::path GoldenJournal() {
    return std::filesystem::path(LUBANCODE_TEST_FIXTURES_DIR) / "trajectory" / "v1" /
           "golden_main.jsonl";
}

// 合成 Journal 的落账桩(照 test_journal_recorder 的 Harness 裁剪)。
struct JournalHarness {
    FixedClock clock;
    std::filesystem::path dir;
    std::optional<TrajectoryRecorder> recorder;

    explicit JournalHarness(const char* tag) {
        dir = std::filesystem::temp_directory_path() /
              ("lubancode-tel-proj-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "artifacts", ec);
        EventScope scope;
        scope.workspace_key = "ws-000000000000";
        scope.session_id = "20260830-031522-TEST01";
        scope.run_id = "main-test-1";
        scope.run_kind = RunKind::MainSession;
        scope.visibility = {Visibility::HostOnly};
        auto started = TrajectoryRecorder::Start(dir / "main.jsonl", dir / "artifacts", scope,
                                                 RecorderOptions{}, &clock);
        REQUIRE(started.has_value());
        recorder = std::move(*started);
    }

    static EventScope Scope(std::optional<std::string> turn, std::optional<std::string> request,
                            std::optional<std::string> call) {
        EventScope scope;
        scope.workspace_key = "ws-000000000000";
        scope.session_id = "20260830-031522-TEST01";
        scope.run_id = "main-test-1";
        scope.visibility = {Visibility::HostOnly};
        scope.turn_id = std::move(turn);
        scope.request_id = std::move(request);
        scope.call_id = std::move(call);
        return scope;
    }

    RecordReceipt Put(EventKind kind, EventScope scope, nlohmann::json payload,
                      EventLinks links = {}) {
        RecordRequest request;
        request.kind = kind;
        request.scope = std::move(scope);
        request.payload = std::move(payload);
        request.links = std::move(links);
        RecordReceipt receipt = recorder->Record(std::move(request), Durability::ProcessCrash);
        const std::string note =
            std::string(EventKindName(kind)) + " -> " + receipt.error_code;
        REQUIRE_MESSAGE(receipt.status == RecordReceipt::Status::Committed, note);
        return receipt;
    }

    void OpenTurn(const std::string& turn) {
        const RecordReceipt started = recorder->WriteRunStarted(
            nlohmann::json{{"start_reason", "process_launch"}}, Durability::ProcessCrash);
        REQUIRE(started.status == RecordReceipt::Status::Committed);
        Put(EventKind::TurnStarted, Scope(turn, std::nullopt, std::nullopt),
            nlohmann::json{{"trigger", "external_user"}});
        EventScope input_scope = Scope(turn, std::nullopt, std::nullopt);
        input_scope.actor = Actor::User;
        input_scope.origin = Origin::ExternalUser;
        Put(EventKind::InputReceived, input_scope,
            nlohmann::json{{"input_id", "input-0001"},
                           {"content", nlohmann::json::array({"text"})},
                           {"channel", "terminal"},
                           {"sender", nlohmann::json{{"kind", "local_user"}}}});
    }

    std::string SendRequest(const std::string& turn, const std::string& request_id,
                            std::optional<std::string> retry_of = std::nullopt) {
        EventScope prep = Scope(turn, request_id, std::nullopt);
        prep.actor = Actor::Model;
        prep.origin = Origin::ProviderModel;
        const RecordReceipt prepared =
            Put(EventKind::ModelRequestPrepared, prep,
                nlohmann::json{{"model", "demo-model"},
                               {"provider", "demo"},
                               {"wire", "responses"},
                               {"message_refs", nlohmann::json::array()}});
        EventLinks links;
        links.retry_of = std::move(retry_of);
        const RecordReceipt sent =
            Put(EventKind::ModelRequestSent, Scope(turn, request_id, std::nullopt),
                nlohmann::json{{"prepared_event_id", prepared.event_id}}, std::move(links));
        return sent.event_id;
    }
};

}  // namespace

TEST_CASE("golden Journal: span 树/属性/metrics 全投出") {
    const ProjectionReport report = ProjectJournalFile(GoldenJournal(), TestOptions());
    REQUIRE(report.ok);
    CHECK(report.events_projected == 19);
    CHECK(report.trace_id == DeriveTraceId("test-projection-key-v1",
                                           "20260830-031522-7K4M2P", "main-0001"));
    CHECK(report.workspace_key == "demo-000000000000");

    // §11.2 表内的起终对全在:run/turn/两枚 request/一枚 tool = 5 span。
    REQUIRE(report.spans.size() == 5);
    const TraceSpan& run = report.spans[0];
    const TraceSpan& turn = report.spans[1];
    const TraceSpan& request1 = report.spans[2];
    const TraceSpan& tool = report.spans[3];
    const TraceSpan& request2 = report.spans[4];
    CHECK(run.name == "lubancode.agent.run");
    CHECK(turn.name == "lubancode.agent.turn");
    CHECK(request1.name == "gen_ai.request");
    CHECK(tool.name == "lubancode.tool.execute");
    CHECK(request2.name == "gen_ai.request");

    // 树形(§11.1):run 根,turn 挂 run,request/tool 挂 turn。
    CHECK(run.parent_span_id.empty());
    CHECK(turn.parent_span_id == run.span_id);
    CHECK(request1.parent_span_id == turn.span_id);
    CHECK(tool.parent_span_id == turn.span_id);
    CHECK(request2.parent_span_id == turn.span_id);
    for (const TraceSpan& span : report.spans) {
        CHECK(span.trace_id == report.trace_id);
        CHECK(span.status == StatusCode::Ok);
        CHECK(span.source_terminal_event_id.empty() == false);
    }

    // D1 属性(§11.2/§11.4):run/turn/模型/工具的元数据,无正文。
    CHECK(run.attributes.at("lubancode.run.kind") == "main_session");
    CHECK(run.attributes.at("lubancode.run.start_reason") == "process_launch");
    CHECK(turn.attributes.at("lubancode.turn.trigger") == "external_user");
    CHECK(turn.attributes.at("lubancode.turn.outcome") == "succeeded");
    CHECK(request1.attributes.at("gen_ai.request.model") == "demo-model");
    CHECK(request1.attributes.at("gen_ai.request.provider") == "demo");
    CHECK(request1.attributes.at("gen_ai.request.stop_reason") == "tool_use");
    const std::string request1_attributes = request1.attributes.dump();
    CHECK_MESSAGE(request1.attributes.contains("gen_ai.usage.input_tokens"),
                  request1_attributes);
    CHECK(request1.attributes.at("gen_ai.usage.input_tokens") == 128);
    CHECK(request1.attributes.at("gen_ai.usage.output_tokens") == 64);
    CHECK(request1.attributes.at("gen_ai.usage.coverage") == "provider");
    CHECK(tool.attributes.at("tool.name") == "read_file");
    CHECK(tool.attributes.at("tool.kind") == "builtin");
    CHECK(tool.attributes.at("tool.effect_class") == "read_only_local");
    CHECK(tool.attributes.at("tool.outcome") == "succeeded");
    CHECK(tool.attributes.at("tool.input_bytes_bucket") == "<=1k");

    // 时间:金夹具固定钟,wall 1759000000000ms。
    CHECK(run.start_unix_nano == 1759000000000LL * 1000000);

    // metrics(§12.1 首批子集,按 name 稳定排序)。
    REQUIRE(report.metrics.size() >= 6);
    auto find_metric = [&report](const std::string& name, const std::string& labels_dump) {
        for (const MetricSample& metric : report.metrics) {
            if (metric.name == name && metric.labels.dump() == labels_dump) {
                return metric.value;
            }
        }
        return std::uint64_t{0};
    };
    CHECK(find_metric("lubancode.session.started_total", "{}") == 1);
    CHECK(find_metric("lubancode.turn.started_total",
                      R"({"trigger":"external_user"})") == 1);
    CHECK(find_metric("lubancode.turn.completed_total",
                      R"({"outcome":"succeeded"})") == 1);
    CHECK(find_metric("lubancode.model.request_total",
                      R"({"outcome":"completed","provider":"demo"})") == 2);
    CHECK(find_metric("lubancode.model.tokens", R"({"kind":"input"})") == 128);
    CHECK(find_metric("lubancode.model.tokens", R"({"kind":"output"})") == 64);
    CHECK(find_metric("lubancode.tool.call_total",
                      R"({"outcome":"succeeded","tool_kind":"builtin"})") == 1);

    // resource 已过 Redactor 且全键在表。
    for (auto it = report.resource_attributes.begin();
         it != report.resource_attributes.end(); ++it) {
        CHECK(IsAllowedResourceAttributeKey(it.key()));
    }
    CHECK(report.resource_attributes.at("lubancode.telemetry.schema_version") == 1);
}

TEST_CASE("确定性: 同 Journal 同钥匙投影两次逐字节相同") {
    const ProjectionReport first = ProjectJournalFile(GoldenJournal(), TestOptions());
    const ProjectionReport second = ProjectJournalFile(GoldenJournal(), TestOptions());
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    CHECK(first.ToJson().dump() == second.ToJson().dump());

    // 换钥匙:结构不变,trace/span id 全变(§9.2/§9.3 本地钥派生)。
    ProjectorOptions other = TestOptions();
    other.projection_key = "another-test-key-v2";
    const ProjectionReport third = ProjectJournalFile(GoldenJournal(), other);
    REQUIRE(third.ok);
    CHECK(third.spans.size() == first.spans.size());
    CHECK(third.trace_id != first.trace_id);
    for (std::size_t i = 0; i < first.spans.size(); ++i) {
        CHECK(third.spans[i].span_id != first.spans[i].span_id);
        CHECK(third.spans[i].name == first.spans[i].name);
    }
}

TEST_CASE("D1 无正文无路径: 全量输出不含用户文本/文件路径/正文键") {
    const ProjectionReport report = ProjectJournalFile(GoldenJournal(), TestOptions());
    REQUIRE(report.ok);
    const std::string dump = report.ToJson().dump();
    // 金夹具里的用户正文与模型正文(占位中文),一律不许出现在投影里。
    CHECK(dump.find("README") == std::string::npos);
    CHECK(dump.find("input.received") == std::string::npos);  // 不投正文事件
    CHECK(dump.find("blocks") == std::string::npos);
    CHECK(dump.find("read_file") != std::string::npos);  // 工具名是 D1 元数据
    // 正文类键不许出现("content" 须整键匹配,免误中自家的 content_included)。
    CHECK(dump.find("prompt") == std::string::npos);
    CHECK(dump.find("\"content\"") == std::string::npos);
}

TEST_CASE("坏链停整条 stream: 不跳坏行接着猜(§22.5)") {
    // 复制金夹具,把中段一行改坏(hash 链断)。
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      "lubancode-tel-corrupt";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path copy = dir / "corrupt.jsonl";
    std::FILE* source = std::fopen(GoldenJournal().string().c_str(), "rb");
    REQUIRE(source != nullptr);
    std::FILE* sink = std::fopen(copy.string().c_str(), "wb");
    REQUIRE(sink != nullptr);
    std::string buffer;
    int byte = 0;
    while ((byte = std::fgetc(source)) != EOF) {
        buffer.push_back(static_cast<char>(byte));
    }
    std::fclose(source);
    // 把第 10 行的 outcome 值换掉:canonical round-trip 过、链却断。
    std::size_t line_start = 0;
    for (int i = 0; i < 9; ++i) {
        line_start = buffer.find('\n', line_start) + 1;
    }
    const std::size_t hit = buffer.find("\"succeeded\"", line_start);
    REQUIRE(hit != std::string::npos);
    buffer.replace(hit, 11, "\"failed_xx\"");
    std::fwrite(buffer.data(), 1, buffer.size(), sink);
    std::fclose(sink);

    const ProjectionReport report = ProjectJournalFile(copy, TestOptions());
    CHECK_FALSE(report.ok);
    CHECK(report.error_code == "telemetry.source_corrupt");
    CHECK(report.spans.empty());

    // 文件不存在:io_error。
    const ProjectionReport missing =
        ProjectJournalFile(dir / "nope.jsonl", TestOptions());
    CHECK_FALSE(missing.ok);
    CHECK(missing.error_code == "telemetry.io_error");

    // 钥匙为空:options 报错,不给无钥投影。
    ProjectorOptions keyless = TestOptions();
    keyless.projection_key.clear();
    CHECK(ProjectJournalFile(GoldenJournal(), keyless).error_code ==
          "telemetry.options_missing_key");
}

TEST_CASE("合成 Journal: 审批/压缩/验证/重试/悬空收口") {
    JournalHarness harness("flows");
    const std::string turn = "turn-0001";
    harness.OpenTurn(turn);  // run.started + turn.started + input.received(状态机门槛)

    // 模型往返声明一个 call(带审批)。
    EventScope prep = JournalHarness::Scope(turn, "req-0001", std::nullopt);
    prep.actor = Actor::Model;
    prep.origin = Origin::ProviderModel;
    const RecordReceipt prepared =
        harness.Put(EventKind::ModelRequestPrepared, prep,
                    nlohmann::json{{"model", "demo-model"},
                                   {"provider", "demo"},
                                   {"wire", "responses"},
                                   {"message_refs", nlohmann::json::array()}});
    harness.Put(EventKind::ModelRequestSent,
                JournalHarness::Scope(turn, "req-0001", std::nullopt),
                nlohmann::json{{"prepared_event_id", prepared.event_id}});
    harness.Put(EventKind::ModelOutputCompleted,
                JournalHarness::Scope(turn, "req-0001", std::nullopt),
                nlohmann::json{{"output_id", "output-0001"},
                               {"blocks", nlohmann::json::array({nlohmann::json{
                                              {"type", "tool_call"},
                                              {"call_id", "call-0001"},
                                              {"name", "write_file"},
                                              {"arguments", nlohmann::json{{"path", "a.txt"}}}}})},
                               {"stop_reason", "tool_use"}});
    harness.Put(EventKind::ToolExecutionPlanned,
                JournalHarness::Scope(turn, "req-0001", "call-0001"),
                nlohmann::json{{"call_id", "call-0001"}, {"tool_name", "write_file"}});
    harness.Put(EventKind::ToolInputEffective,
                JournalHarness::Scope(turn, "req-0001", "call-0001"),
                nlohmann::json{{"call_id", "call-0001"},
                               {"tool_name", "write_file"},
                               {"source_kind", "builtin"},
                               {"effect_class", "write_local"},
                               {"effective_arguments", nlohmann::json{{"path", "a.txt"}}},
                               {"effective_arguments_sha256",
                                std::string(64, '0')}});
    // 审批:requested -> resolved(allow)。
    harness.Put(EventKind::ControlApprovalRequested,
                JournalHarness::Scope(turn, "req-0001", "call-0001"),
                nlohmann::json{{"approval_id", "appr-0001"}, {"call_id", "call-0001"}});
    harness.Put(EventKind::ControlApprovalResolved,
                JournalHarness::Scope(turn, "req-0001", "call-0001"),
                nlohmann::json{{"approval_id", "appr-0001"}, {"decision", "allow"}});
    harness.Put(EventKind::ToolExecutionStarted,
                JournalHarness::Scope(turn, "req-0001", "call-0001"),
                nlohmann::json{{"call_id", "call-0001"}, {"batch_id", "batch-01"},
                               {"position_in_batch", std::uint64_t{0}}});
    harness.Put(EventKind::ToolExecutionFinished,
                JournalHarness::Scope(turn, "req-0001", "call-0001"),
                nlohmann::json{{"outcome", "succeeded"}, {"duration_ms", 5}});
    harness.Put(EventKind::ToolResultCommitted,
                JournalHarness::Scope(turn, "req-0001", "call-0001"),
                nlohmann::json{{"call_id", "call-0001"},
                               {"content", nlohmann::json::array()},
                               {"is_error", false}});

    // 重试:req-0002 首发失败,再发带 retry_of。
    const std::string first_sent = harness.SendRequest(turn, "req-0002");
    harness.Put(EventKind::ModelOutputFailed,
                JournalHarness::Scope(turn, "req-0002", std::nullopt),
                nlohmann::json{{"reason", "provider 429"},
                               {"error_code", "provider_rate_limited"},
                               {"attempt", std::uint64_t{1}}});
    const std::string second_sent = harness.SendRequest(turn, "req-0002", first_sent);
    CHECK(second_sent != first_sent);
    harness.Put(EventKind::ModelOutputCompleted,
                JournalHarness::Scope(turn, "req-0002", std::nullopt),
                nlohmann::json{{"output_id", "output-0002"},
                               {"blocks", nlohmann::json::array()},
                               {"stop_reason", "end_turn"}});

    // 压缩与验证(会话级,无 turn)。
    harness.Put(EventKind::CompactRequested,
                JournalHarness::Scope(std::nullopt, std::nullopt, std::nullopt),
                nlohmann::json{{"trigger", "auto"}, {"old_epoch", std::uint64_t{0}}});
    harness.Put(EventKind::CompactApplied,
                JournalHarness::Scope(std::nullopt, std::nullopt, std::nullopt),
                nlohmann::json{{"old_state_hash", std::string(64, '1')},
                               {"new_state_hash", std::string(64, '2')},
                               {"source_event_span", nlohmann::json::array({1, 20})}});
    harness.Put(EventKind::VerificationStarted,
                JournalHarness::Scope(turn, std::nullopt, std::nullopt),
                nlohmann::json{{"verification_id", "verify-0001"}, {"kind", "command"}});
    harness.Put(EventKind::VerificationRecorded,
                JournalHarness::Scope(turn, std::nullopt, std::nullopt),
                nlohmann::json{{"verification_id", "verify-0001"},
                               {"kind", "command"},
                               {"passed", false},
                               {"producer", "host"}});
    // turn 收口,run 不收口(留给悬空收口断言)。
    harness.Put(EventKind::TurnCompleted, JournalHarness::Scope(turn, std::nullopt, std::nullopt),
                nlohmann::json{{"outcome", "failed"}});

    const ProjectionReport report =
        ProjectJournalFile(harness.dir / "main.jsonl", TestOptions());
    REQUIRE(report.ok);

    // span 清单:run/turn/req1/approval/tool/req2 首试/req2 重试/compact/verify。
    REQUIRE(report.spans.size() == 9);
    auto find_span = [&report](const std::string& name) -> const TraceSpan& {
        for (const TraceSpan& span : report.spans) {
            if (span.name == name) {
                return span;
            }
        }
        FAIL(("缺 span: " + name).c_str());
        return report.spans.front();
    };
    const TraceSpan& approval = find_span("lubancode.approval.wait");
    CHECK(approval.attributes.at("lubancode.approval.decision") == "allow");
    // 状态:失败 attempt 是 Error,重试成功的是 Ok;重试带 retry_of link。
    bool saw_error_attempt = false;
    bool saw_retry_link = false;
    for (const TraceSpan& span : report.spans) {
        if (span.name != "gen_ai.request") {
            continue;
        }
        if (span.status == StatusCode::Error) {
            saw_error_attempt = true;
            CHECK(span.attributes.at("error.type") == "provider_rate_limited");
        }
        for (const SpanLink& link : span.links) {
            if (link.relation == "retry_of") {
                saw_retry_link = true;
                CHECK(link.trace_id == report.trace_id);
                CHECK(IsValidSpanId(link.span_id));
            }
        }
    }
    CHECK(saw_error_attempt);
    CHECK(saw_retry_link);
    const TraceSpan& compact = find_span("lubancode.compact");
    CHECK(compact.attributes.at("lubancode.compact.trigger") == "auto");
    CHECK(compact.attributes.at("lubancode.compact.epoch") == 0);
    const TraceSpan& verification = find_span("lubancode.verification");
    CHECK(verification.attributes.at("lubancode.verification.passed") == false);
    CHECK(verification.attributes.at("lubancode.verification.kind") == "command");

    // 悬空收口:run 没 terminal,明标 missing,不冒充(§29.3)。
    const TraceSpan& run = report.spans[0];
    CHECK(run.attributes.at("lubancode.span.terminal") == "missing");
    CHECK(run.end_unix_nano == run.start_unix_nano);
    bool saw_warning = false;
    for (const std::string& warning : report.warnings) {
        if (warning == "open_span_missing_terminal:run") {
            saw_warning = true;
        }
    }
    CHECK(saw_warning);

    // metrics:审批决议/重试失败/验证失败各记一笔。
    auto find_metric = [&report](const std::string& name, const std::string& labels_dump) {
        for (const MetricSample& metric : report.metrics) {
            if (metric.name == name && metric.labels.dump() == labels_dump) {
                return metric.value;
            }
        }
        return std::uint64_t{0};
    };
    CHECK(find_metric("lubancode.approval.decision_total", R"({"decision":"allow"})") == 1);
    CHECK(find_metric("lubancode.model.request_total",
                      R"({"outcome":"completed","provider":"demo"})") == 2);
    CHECK(find_metric("lubancode.model.request_total",
                      R"({"outcome":"failed","provider":"demo"})") == 1);
    CHECK(find_metric("lubancode.verification.total", R"({"outcome":"failed"})") == 1);
    CHECK(find_metric("lubancode.compact.total", R"({"outcome":"applied"})") == 1);
}

TEST_CASE("空 Journal: 开张即关,run span 仍完整") {
    JournalHarness harness("minimal");
    harness.recorder->WriteRunStarted(nlohmann::json{{"start_reason", "process_launch"}},
                                      Durability::ProcessCrash);
    harness.recorder->FinishRun(EventKind::RunCompleted, "done", Durability::ProcessCrash);
    const ProjectionReport report =
        ProjectJournalFile(harness.dir / "main.jsonl", TestOptions());
    REQUIRE(report.ok);
    REQUIRE(report.spans.size() == 1);
    CHECK(report.spans[0].name == "lubancode.agent.run");
    CHECK(report.spans[0].status == StatusCode::Ok);
    CHECK(report.warnings.empty());
}
