// T07 / V3-GAP-02(SessionV3 旧设计清理单):Telemetry v3 投影册。
//
// ProjectV3LedgerFile 的映射合同逐项钉:
//   - session/turn/request/tool/compact/Hook 六族 span:起终锚、父子挂、
//     属性只出封闭键集;
//   - turn 无终态事件 → terminal=missing(partial),不用下一回合的记录
//     时间猜完整时长;缺起点(tool 终态无 started)→ missing_start 锚在
//     终事件上,时长 0;
//   - usage owner 驱动:assistant 唯一可累计,与 T06 ProjectV3Usage 同源
//     对表(metrics tokens == samples 逐项和);缺实报 coverage=unknown;
//     model.usage.appended 只作观察警告不累计;
//   - 确定性:同账同钥匙两投逐字节相同,换钥匙 id 变结构不变;
//   - D1:输出无正文(prompt/工具参数原文不入 span/metric);
//   - 子代理子账:session span 无 runKind 不暗填。
// 合法账一律经 V3Writer 现场生成;本册 format-neutral,不经 SessionManager
// 建场开关,不设进程级环境变量(全部测试编在同一只二进制里的教训)。
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "accounting/usage_projector.hpp"
#include "telemetry/contract.hpp"
#include "telemetry/identity.hpp"
#include "telemetry/projector.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "trajectory/v3/writer.hpp"

namespace v3 = lubancode::trajectory::v3;
using namespace lubancode::telemetry;
using namespace lubancode::accounting;

namespace {

class FixedClock : public v3::V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};

struct SessionsRoot {
    FixedClock clock;
    std::filesystem::path root;

    explicit SessionsRoot(const char* tag) {
        root = std::filesystem::temp_directory_path() /
               ("lubancode-tel-v3-" + std::string(tag));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }

    std::filesystem::path Dir(const std::string& session_id) {
        std::error_code ec;
        const std::filesystem::path dir = root / session_id;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    std::filesystem::path Ledger(const std::string& session_id) {
        return Dir(session_id) / (session_id + ".jsonl");
    }
};

std::optional<v3::V3Writer> StartSession(SessionsRoot& root, const std::string& session_id,
                                         const std::string& run_kind = "main_session") {
    v3::V3WriterOptions options;
    options.run_kind = run_kind;
    auto writer = v3::V3Writer::Start(root.Ledger(session_id), session_id,
                                      "run-" + session_id, "你是 LubanCode。",
                                      nlohmann::json::object(), options, &root.clock);
    if (!writer.has_value()) {
        return std::nullopt;
    }
    return std::move(*writer);
}

ProjectorOptions TestOptions() {
    ProjectorOptions options;
    options.projection_key = "test-projection-key-v3";
    options.resource.service_version = "0.26.0-test";
    options.resource.service_instance_id = "proc-test-v3";
    options.resource.os_type = "windows";
    options.resource.host_arch = "amd64";
    options.resource.device_instance_id = "device-test-v3";
    options.resource.workspace_key = "ws-test-000000000000";
    options.resource.frontend = "terminal";
    options.resource.trajectory_schema_version = 3;
    return options;
}

// 一轮完整请求链(prepared → sent → completed → assistant usage owner)。
// usage 传 json(nullptr) = 缺实报。
std::string InstallRequest(v3::V3Writer& writer, const std::string& turn_id,
                           const nlohmann::json& usage,
                           const std::string& finish_reason = "stop") {
    v3::MessageDraft user;
    user.turn_id = turn_id;
    user.purpose = v3::MessagePurpose::Conversation;
    user.origin = v3::MessageOrigin::Human;
    user.message = nlohmann::json::object({{"role", "user"},
                                           {"content", "机密问题:" + turn_id}});
    v3::WriteReceipt user_receipt =
        writer.AppendMessage(std::move(user), v3::Durability::PowerLoss);
    REQUIRE(user_receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({user_receipt.id}).status ==
            v3::WriteReceipt::Status::Committed);
    std::vector<std::string> input_refs;
    const auto& chain = writer.context().chain;
    for (std::size_t i = 1; i < chain.size(); ++i) {
        input_refs.push_back(chain[i].message_ref);
    }
    const std::string request_id = writer.NewRequestId();
    const std::string step_id = writer.NewStepId();
    nlohmann::json snapshot = nlohmann::json::object(
        {{"provider", "stub"}, {"wire", "openai"}, {"model", "stub-mini"}});
    REQUIRE(writer
                .PrepareRequest(request_id, turn_id, step_id, "conversation",
                                writer.context().system_message_ref, input_refs,
                                std::move(snapshot), std::nullopt, v3::Durability::PowerLoss)
                .status == v3::WriteReceipt::Status::Committed);
    {
        v3::EventDraft sent;
        sent.kind = v3::EventKindV3::ModelRequestSent;
        sent.status = v3::OpStatus::Done;
        sent.request_id = request_id;
        sent.turn_id = turn_id;
        sent.step_id = step_id;
        sent.payload = nlohmann::json{{"deliveryScope", "local_transport"}};
        REQUIRE(writer.AppendEvent(std::move(sent), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
    }
    {
        v3::EventDraft done;
        done.kind = v3::EventKindV3::ModelResponseCompleted;
        done.status = v3::OpStatus::Done;
        done.request_id = request_id;
        done.turn_id = turn_id;
        done.step_id = step_id;
        done.payload = nlohmann::json{{"finishReason", finish_reason}};
        REQUIRE(writer.AppendEvent(std::move(done), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
    }
    v3::MessageDraft assistant;
    assistant.turn_id = turn_id;
    assistant.step_id = step_id;
    assistant.request_id = request_id;
    assistant.purpose = v3::MessagePurpose::Conversation;
    assistant.origin = v3::MessageOrigin::SessionRuntime;
    assistant.provider = "stub";
    assistant.wire = "openai";
    assistant.model = "stub-mini";
    assistant.response_model = nlohmann::json(nullptr);
    assistant.usage = usage;
    assistant.message = nlohmann::json::object(
        {{"role", "assistant"}, {"content", "机密答复"}});
    v3::WriteReceipt receipt =
        writer.AppendMessage(std::move(assistant), v3::Durability::PowerLoss);
    REQUIRE(receipt.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(writer.AdmitMessages({receipt.id}).status == v3::WriteReceipt::Status::Committed);
    return request_id;
}

// 迟到实报观察(§五 owner 表:不倒改旧 message,不参与累计)。
void AppendLateUsage(v3::V3Writer& writer, const std::string& request_id,
                     const std::string& turn_id, std::int64_t in, std::int64_t out) {
    v3::EventDraft appended;
    appended.kind = v3::EventKindV3::ModelUsageAppended;
    appended.request_id = request_id;
    appended.turn_id = turn_id;
    appended.payload = nlohmann::json{{"usage", nlohmann::json{{"inputTokens", in},
                                                               {"outputTokens", out}}},
                                      {"reportedByProvider", true},
                                      {"providerResponseId", "resp-late"}};
    REQUIRE(writer.AppendEvent(std::move(appended), v3::Durability::PowerLoss).status ==
            v3::WriteReceipt::Status::Committed);
}

void EndSession(v3::V3Writer& writer, bool clean = true) {
    v3::EventDraft ended;
    ended.kind = v3::EventKindV3::SessionEnded;
    ended.payload = nlohmann::json{{"reason", clean ? "completed" : "crashed"},
                                   {"closeQuality", clean ? "clean" : "incomplete"}};
    REQUIRE(writer.AppendEvent(std::move(ended), v3::Durability::PowerLoss).status ==
            v3::WriteReceipt::Status::Committed);
}

const TraceSpan* FindSpan(const ProjectionReport& report, const std::string& name,
                          const std::string& needle = std::string()) {
    for (const TraceSpan& span : report.spans) {
        if (span.name != name) {
            continue;
        }
        if (!needle.empty() && span.source_event_id.find(needle) == std::string::npos &&
            (span.source_terminal_event_id.empty() ||
             span.source_terminal_event_id.find(needle) == std::string::npos)) {
            continue;
        }
        return &span;
    }
    return nullptr;
}

const TraceSpan* RequireSpan(const ProjectionReport& report, const std::string& name) {
    const TraceSpan* span = FindSpan(report, name);
    // doctest 的 MessageBuilder 不吃字面量 + std::string 拼接,先拼好。
    const std::string note = "缺 span: " + name;
    REQUIRE_MESSAGE(span != nullptr, note.c_str());
    return span;
}

const MetricSample* FindMetric(const ProjectionReport& report, const std::string& name,
                               const nlohmann::json& labels) {
    const std::string dump = labels.dump();
    for (const MetricSample& metric : report.metrics) {
        if (metric.name == name && metric.labels.dump() == dump) {
            return &metric;
        }
    }
    return nullptr;
}

bool HasWarning(const ProjectionReport& report, const std::string& needle) {
    for (const std::string& warning : report.warnings) {
        if (warning.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

ProjectionReport ProjectLedger(const std::filesystem::path& ledger,
                               const ProjectorOptions& options) {
    const auto read = v3::ReadV3Ledger(ledger);
    REQUIRE(read.has_value());
    return ProjectV3LedgerFile(*read, options);
}

}  // namespace

// ---------------------------------------------------------------------------
// 六族 span 的映射与父子挂
// ---------------------------------------------------------------------------

TEST_CASE("主账六族:session/turn/request/tool/compact/Hook 各归各") {
    SessionsRoot root("spans");
    std::string compact_id;
    std::string dispatch_id;
    {
        auto writer = StartSession(root, "S-SPAN");
        REQUIRE(writer.has_value());
        InstallRequest(*writer, "turn-000001",
                       nlohmann::json{{"inputTokens", 120},
                                      {"outputTokens", 30},
                                      {"cacheReadTokens", 8}});

        // 工具:ToolActionSession 全链(pending → started → finished)。
        auto action = v3::ToolActionSession::Admit(*writer, "turn-000001", "step-000002",
                                                   "action-000001", "queued", std::nullopt,
                                                   std::nullopt,
                                                   nlohmann::json{{"toolName", "read_file"}},
                                                   v3::Durability::PowerLoss);
        v3::ToolIdentity identity;
        identity.logical_name = "read_file";
        identity.registration_source = "builtin";
        REQUIRE(action
                    .Start(*writer, "args-action-000001-sha", identity, std::nullopt,
                           nlohmann::json{{"batchId", "batch-1"}, {"positionInBatch", 1}},
                           v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        REQUIRE(action.Finish(*writer, 0, 25, v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);

        // compact:requested → cancelled(v3 终态之一)。
        {
            v3::EventDraft requested;
            requested.kind = v3::EventKindV3::CompactRequested;
            requested.compact_id = "compact-000001";
            requested.turn_id = "compact-turn-000001";
            requested.parent_turn_id = "turn-000001";
            requested.payload = nlohmann::json{{"trigger", "manual"},
                                               {"reason", "test"},
                                               {"sourceContextRevision", 3}};
            REQUIRE(writer->AppendEvent(std::move(requested), v3::Durability::PowerLoss)
                        .status == v3::WriteReceipt::Status::Committed);
            compact_id = "compact-000001";
            v3::EventDraft cancelled;
            cancelled.kind = v3::EventKindV3::CompactCancelled;
            cancelled.status = v3::OpStatus::Cancelled;
            cancelled.compact_id = compact_id;
            cancelled.turn_id = "compact-turn-000001";
            cancelled.payload = nlohmann::json{{"reason", "user_abort"},
                                               {"sourceContextRevision", 3}};
            REQUIRE(writer->AppendEvent(std::move(cancelled), v3::Durability::PowerLoss)
                        .status == v3::WriteReceipt::Status::Committed);
        }

        // hook:dispatch.requested → started → completed。
        {
            v3::HookHandlerSpec spec;
            spec.hook_id = "hook-1";
            spec.handler_kind = "lua";
            spec.definition_hash = "defhash";
            spec.definition_order = 1;
            spec.failure_policy = "continue";
            dispatch_id = writer->NewHookDispatchId();
            auto dispatch = v3::HookDispatchSession::Dispatch(
                *writer, dispatch_id, "post_user_input", "turn-000001", std::nullopt,
                std::nullopt, {spec}, std::nullopt, v3::Durability::PowerLoss);
            REQUIRE(dispatch
                        .BeginInvocation(*writer, "hookinv-1", spec, v3::Durability::PowerLoss)
                        .status == v3::WriteReceipt::Status::Committed);
            REQUIRE(dispatch
                        .CompleteInvocation(*writer, std::nullopt, std::nullopt, 12,
                                            v3::Durability::PowerLoss)
                        .status == v3::WriteReceipt::Status::Committed);
        }
        EndSession(*writer);
    }

    const ProjectionReport report = ProjectLedger(root.Ledger("S-SPAN"), TestOptions());
    REQUIRE(report.ok);
    CHECK(report.session_id == "S-SPAN");
    CHECK(report.trace_id ==
          DeriveTraceId("test-projection-key-v3", "S-SPAN", "run-S-SPAN"));

    // session span:started → ended(clean=Ok),runKind 进属性。
    const TraceSpan* session = RequireSpan(report, "lubancode.session");
    CHECK(session->status == StatusCode::Ok);
    CHECK_FALSE(session->source_terminal_event_id.empty());
    CHECK(session->attributes.at("lubancode.run.kind") == "main_session");
    CHECK(session->parent_span_id.empty());  // 根

    // turn span:首条携带 turnId 的行起锚;v3 无终态事件 → missing(partial)。
    // 排序按锚 seq:主 turn 在前,compact 内部回合在后。
    std::vector<const TraceSpan*> turn_spans;
    for (const TraceSpan& span : report.spans) {
        if (span.name == "lubancode.agent.turn") {
            turn_spans.push_back(&span);
        }
    }
    REQUIRE(turn_spans.size() == 2);  // 主回合 + compact 内部回合
    const TraceSpan* main_turn = turn_spans[0];
    const TraceSpan* compact_turn = turn_spans[1];
    CHECK(main_turn->attributes.at("lubancode.span.terminal") == "missing");
    CHECK(main_turn->source_terminal_event_id.empty());
    CHECK(main_turn->parent_span_id == session->span_id);  // 主回合挂 session
    // 内部回合沿 parentTurnId 挂主回合(§4.6 回合归属)。
    CHECK(compact_turn->parent_span_id == main_turn->span_id);

    // request span:sent → completed;usage owner 五键;attempt 不伪造。
    const TraceSpan* request = RequireSpan(report, "gen_ai.request");
    CHECK(request->status == StatusCode::Ok);
    CHECK(request->attributes.at("gen_ai.request.model") == "stub-mini");
    CHECK(request->attributes.at("gen_ai.request.provider") == "stub");
    CHECK(request->attributes.at("gen_ai.request.stop_reason") == "stop");
    CHECK(request->attributes.at("gen_ai.usage.input_tokens") == 120);
    CHECK(request->attributes.at("gen_ai.usage.output_tokens") == 30);
    CHECK(request->attributes.at("gen_ai.usage.cache_read_tokens") == 8);
    CHECK(request->attributes.at("gen_ai.usage.coverage") == "provider");
    CHECK_FALSE(request->attributes.contains("gen_ai.request.attempt"));
    CHECK(request->parent_span_id == main_turn->span_id);

    // tool span:started → finished;名字/来源/批次位。
    const TraceSpan* tool = RequireSpan(report, "lubancode.tool.execute");
    CHECK(tool->status == StatusCode::Ok);
    CHECK(tool->attributes.at("tool.name") == "read_file");
    CHECK(tool->attributes.at("tool.kind") == "builtin");
    CHECK(tool->attributes.at("tool.batch_id") == "batch-1");
    CHECK(tool->attributes.at("tool.sequence_in_batch") == 1);
    CHECK(tool->attributes.at("tool.outcome") == "finished");
    CHECK(tool->parent_span_id == main_turn->span_id);

    // compact span:requested → cancelled;挂自己的内部回合(内部回合再挂
    // 主回合,§4.6)。
    const TraceSpan* compact = RequireSpan(report, "lubancode.compact");
    CHECK(compact->status == StatusCode::Error);
    CHECK(compact->status_description == "cancelled");
    CHECK(compact->attributes.at("lubancode.compact.trigger") == "manual");
    CHECK(compact->parent_span_id == compact_turn->span_id);

    // hook span:dispatch.requested → 最后一枚 invocation 终态。
    const TraceSpan* hook = RequireSpan(report, "lubancode.hook.dispatch");
    CHECK(hook->status == StatusCode::Ok);
    CHECK(hook->attributes.at("lubancode.hook.point") == "post_user_input");
    CHECK(hook->attributes.at("lubancode.hook.matched_handlers") == 1);

    // metrics:六族各就位。
    REQUIRE(FindMetric(report, "lubancode.session.started_total", nlohmann::json::object())
                != nullptr);
    REQUIRE(FindMetric(report, "lubancode.model.request_total",
                       nlohmann::json{{"provider", "stub"}, {"outcome", "completed"}}) != nullptr);
    REQUIRE(FindMetric(report, "lubancode.model.tokens",
                       nlohmann::json{{"kind", "input"}}) != nullptr);
    REQUIRE(FindMetric(report, "lubancode.tool.call_total",
                       nlohmann::json{{"tool_kind", "builtin"}, {"outcome", "finished"}}) !=
            nullptr);
    REQUIRE(FindMetric(report, "lubancode.compact.total",
                       nlohmann::json{{"outcome", "cancelled"}}) != nullptr);
    REQUIRE(FindMetric(report, "lubancode.hook.dispatch_total",
                       nlohmann::json{{"outcome", "completed"}}) != nullptr);

    // D1:正文不进任何输出。
    const std::string dump = report.ToJson().dump();
    CHECK(dump.find("机密问题") == std::string::npos);
    CHECK(dump.find("机密答复") == std::string::npos);
    CHECK(dump.find("args-action-000001") == std::string::npos);  // args 引用不外发
}

TEST_CASE("子代理子账:session span 不暗填 runKind;输入 origin 不改身份") {
    SessionsRoot root("sub");
    {
        auto writer = StartSession(root, "S-CHILD", /*run_kind=*/"");
        REQUIRE(writer.has_value());
        // 子账开局:委派 user(origin=parent_agent)。
        v3::MessageDraft delegation;
        delegation.turn_id = writer->NewTurnId();
        delegation.purpose = v3::MessagePurpose::Conversation;
        delegation.origin = v3::MessageOrigin::ParentAgent;
        delegation.message = nlohmann::json::object({{"role", "user"}, {"content", "查一层"}});
        v3::WriteReceipt receipt =
            writer->AppendMessage(std::move(delegation), v3::Durability::PowerLoss);
        REQUIRE(receipt.status == v3::WriteReceipt::Status::Committed);
        REQUIRE(writer->AdmitMessages({receipt.id}).status ==
                v3::WriteReceipt::Status::Committed);
        InstallRequest(*writer, "turn-000002",
                       nlohmann::json{{"inputTokens", 40}, {"outputTokens", 5}});
        EndSession(*writer);
    }
    const ProjectionReport report = ProjectLedger(root.Ledger("S-CHILD"), TestOptions());
    REQUIRE(report.ok);
    const TraceSpan* session = RequireSpan(report, "lubancode.session");
    CHECK_FALSE(session->attributes.contains("lubancode.run.kind"));  // 不暗填
    REQUIRE(FindMetric(report, "lubancode.model.tokens",
                       nlohmann::json{{"kind", "input"}}) != nullptr);
}

// ---------------------------------------------------------------------------
// partial 合同:缺起点 / 失败终态 / 未收口
// ---------------------------------------------------------------------------

TEST_CASE("缺起点:tool 终态无 started → missing_start 锚在终事件,时长 0") {
    SessionsRoot root("nostart");
    {
        auto writer = StartSession(root, "S-NOSTART");
        REQUIRE(writer.has_value());
        // 只 pending → finished(崩溃恢复的半份调用形状):无 started。
        auto action = v3::ToolActionSession::Admit(*writer, "turn-000001", "step-000001",
                                                   "action-000001", "queued", std::nullopt,
                                                   std::nullopt, nlohmann::json::object(),
                                                   v3::Durability::PowerLoss);
        REQUIRE(action.Finish(*writer, 0, std::nullopt, v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
        EndSession(*writer);
    }
    const ProjectionReport report = ProjectLedger(root.Ledger("S-NOSTART"), TestOptions());
    REQUIRE(report.ok);
    const TraceSpan* tool = RequireSpan(report, "lubancode.tool.execute");
    CHECK(tool->attributes.at("lubancode.span.terminal") == "missing_start");
    CHECK(tool->start_unix_nano == tool->end_unix_nano);  // 时长 0,不猜
    CHECK(tool->source_event_id == tool->source_terminal_event_id);
    CHECK(HasWarning(report, "span_missing_start:tool"));
    REQUIRE(FindMetric(report, "lubancode.tool.call_total",
                       nlohmann::json{{"tool_kind", "unknown"}, {"outcome", "finished"}}) !=
            nullptr);
}

TEST_CASE("失败终态:request failed/cancelled 与 session incomplete 各按实情收口") {
    SessionsRoot root("fail");
    std::string fail_request;
    {
        auto writer = StartSession(root, "S-FAIL");
        REQUIRE(writer.has_value());
        // 失败请求:sent → response.failed(无 assistant)。
        fail_request = writer->NewRequestId();
        const std::string step_id = writer->NewStepId();
        REQUIRE(writer
                    ->PrepareRequest(fail_request, "turn-000001", step_id, "conversation",
                                     writer->context().system_message_ref, {},
                                     nlohmann::json::object({{"provider", "stub"},
                                                             {"wire", "openai"},
                                                             {"model", "stub-mini"}}),
                                     std::nullopt, v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        v3::EventDraft sent;
        sent.kind = v3::EventKindV3::ModelRequestSent;
        sent.status = v3::OpStatus::Done;
        sent.request_id = fail_request;
        sent.turn_id = "turn-000001";
        sent.step_id = step_id;
        sent.payload = nlohmann::json{{"deliveryScope", "local_transport"}};
        REQUIRE(writer->AppendEvent(std::move(sent), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
        v3::EventDraft failed;
        failed.kind = v3::EventKindV3::ModelResponseFailed;
        failed.status = v3::OpStatus::Failed;
        failed.request_id = fail_request;
        failed.turn_id = "turn-000001";
        failed.step_id = step_id;
        failed.payload = nlohmann::json{{"reason", "provider 5xx"}};
        REQUIRE(writer->AppendEvent(std::move(failed), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
        EndSession(*writer, /*clean=*/false);  // 崩溃收场
    }
    const ProjectionReport report = ProjectLedger(root.Ledger("S-FAIL"), TestOptions());
    REQUIRE(report.ok);
    const TraceSpan* request = RequireSpan(report, "gen_ai.request");
    CHECK(request->status == StatusCode::Error);
    CHECK(request->status_description == "model_error");  // reason 是文本,不冒充稳定码
    CHECK(request->attributes.at("gen_ai.usage.coverage") == "unknown");  // 无 owner
    const TraceSpan* session = RequireSpan(report, "lubancode.session");
    CHECK(session->status == StatusCode::Error);
    CHECK(session->status_description == "incomplete");
    REQUIRE(FindMetric(report, "lubancode.model.request_total",
                       nlohmann::json{{"provider", "stub"}, {"outcome", "failed"}}) != nullptr);
}

TEST_CASE("未收口:在途 request/turn 不冒充终态(open span 入账只等 final flush)") {
    SessionsRoot root("open");
    {
        auto writer = StartSession(root, "S-OPEN");
        REQUIRE(writer.has_value());
        InstallRequest(*writer, "turn-000001", nlohmann::json(nullptr));
        // 第二请求只 sent,无终态(在途/崩溃形状)。
        const std::string request_id = writer->NewRequestId();
        const std::string step_id = writer->NewStepId();
        REQUIRE(writer
                    ->PrepareRequest(request_id, "turn-000002", step_id, "conversation",
                                     writer->context().system_message_ref, {},
                                     nlohmann::json::object({{"provider", "stub"},
                                                             {"wire", "openai"},
                                                             {"model", "stub-mini"}}),
                                     std::nullopt, v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        v3::EventDraft sent;
        sent.kind = v3::EventKindV3::ModelRequestSent;
        sent.status = v3::OpStatus::Done;
        sent.request_id = request_id;
        sent.turn_id = "turn-000002";
        sent.step_id = step_id;
        sent.payload = nlohmann::json{{"deliveryScope", "local_transport"}};
        REQUIRE(writer->AppendEvent(std::move(sent), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
        // 不落 session.ended:整卷未收口。
    }
    const ProjectionReport report = ProjectLedger(root.Ledger("S-OPEN"), TestOptions());
    REQUIRE(report.ok);
    const TraceSpan* session = RequireSpan(report, "lubancode.session");
    CHECK(session->attributes.at("lubancode.span.terminal") == "missing");
    CHECK(session->source_terminal_event_id.empty());
    std::size_t open_requests = 0;
    for (const TraceSpan& span : report.spans) {
        if (span.name == "gen_ai.request" && span.source_terminal_event_id.empty()) {
            open_requests += 1;
            CHECK(span.attributes.at("lubancode.span.terminal") == "missing");
        }
    }
    CHECK(open_requests == 1);  // 在途请求按 missing 收口,不冒充
    CHECK(HasWarning(report, "open_span_missing_terminal:request"));
}

// ---------------------------------------------------------------------------
// usage 与 T06 同源对表
// ---------------------------------------------------------------------------

TEST_CASE("usage 对表:metrics tokens 与 ProjectV3Usage samples 逐项相等") {
    SessionsRoot root("parity");
    {
        auto writer = StartSession(root, "S-PARITY");
        REQUIRE(writer.has_value());
        InstallRequest(*writer, "turn-000001",
                       nlohmann::json{{"inputTokens", 120},
                                      {"outputTokens", 30},
                                      {"cacheReadTokens", 8},
                                      {"cacheWriteTokens", 4},
                                      {"reasoningTokens", 6}});
        InstallRequest(*writer, "turn-000002", nlohmann::json(nullptr));  // 缺实报
        InstallRequest(*writer, "turn-000003",
                       nlohmann::json{{"inputTokens", 200}, {"outputTokens", 50}});
    }
    const auto read = v3::ReadV3Ledger(root.Ledger("S-PARITY"));
    REQUIRE(read.has_value());
    const ProjectionReport report = ProjectV3LedgerFile(*read, TestOptions());
    REQUIRE(report.ok);

    V3UsageProjectorContext context;
    context.is_subagent = false;
    context.run_kind = "main_session";
    const UsageProjection usage = ProjectV3Usage(*read, context);
    REQUIRE(usage.ok);

    const auto metric_value = [&](const char* kind) -> std::int64_t {
        const MetricSample* metric =
            FindMetric(report, "lubancode.model.tokens", nlohmann::json{{"kind", kind}});
        return metric != nullptr ? static_cast<std::int64_t>(metric->value) : 0;
    };
    std::int64_t expect_input = 0;
    std::int64_t expect_output = 0;
    std::int64_t expect_cache_read = 0;
    std::int64_t expect_cache_creation = 0;
    std::int64_t expect_reasoning = 0;
    std::int64_t reported = 0;
    std::int64_t unknown = 0;
    for (const UsageSample& sample : usage.samples) {
        if (!sample.usage.has_value()) {
            unknown += 1;
            continue;
        }
        reported += 1;
        expect_input += sample.usage->input_tokens;
        expect_output += sample.usage->output_tokens;
        expect_cache_read += sample.usage->cache_read_tokens;
        expect_cache_creation += sample.usage->cache_creation_tokens;
        expect_reasoning += sample.usage->output_reasoning_tokens;
    }
    CHECK(reported == 2);
    CHECK(unknown == 1);
    CHECK(metric_value("input") == expect_input);
    CHECK(metric_value("output") == expect_output);
    CHECK(metric_value("cache_read") == expect_cache_read);
    CHECK(metric_value("cache_creation") == expect_cache_creation);
    CHECK(metric_value("reasoning") == expect_reasoning);
}

TEST_CASE("迟到实报:appended 只作观察警告,不进 metrics 不改 coverage") {
    SessionsRoot root("late");
    {
        auto writer = StartSession(root, "S-LATE");
        REQUIRE(writer.has_value());
        const std::string request_id =
            InstallRequest(*writer, "turn-000001", nlohmann::json(nullptr));
        AppendLateUsage(*writer, request_id, "turn-000001", 130, 35);
    }
    const ProjectionReport report = ProjectLedger(root.Ledger("S-LATE"), TestOptions());
    REQUIRE(report.ok);
    const TraceSpan* request = RequireSpan(report, "gen_ai.request");
    CHECK(request->attributes.at("gen_ai.usage.coverage") == "unknown");
    CHECK_FALSE(request->attributes.contains("gen_ai.usage.input_tokens"));
    const MetricSample* tokens = FindMetric(report, "lubancode.model.tokens",
                                            nlohmann::json{{"kind", "input"}});
    CHECK(tokens == nullptr);  // 没报不写 0,appended 不累计
    CHECK(HasWarning(report, "usage.v3_appended_observed"));
    CHECK(HasWarning(report, "in=130"));
}

// ---------------------------------------------------------------------------
// 确定性与合同
// ---------------------------------------------------------------------------

TEST_CASE("确定性:同账同钥匙两投逐字节相同;换钥匙 id 变结构不变") {
    SessionsRoot root("stable");
    {
        auto writer = StartSession(root, "S-STABLE");
        REQUIRE(writer.has_value());
        InstallRequest(*writer, "turn-000001",
                       nlohmann::json{{"inputTokens", 10}, {"outputTokens", 2}});
        EndSession(*writer);
    }
    const ProjectionReport first = ProjectLedger(root.Ledger("S-STABLE"), TestOptions());
    REQUIRE(first.ok);
    const ProjectionReport second = ProjectLedger(root.Ledger("S-STABLE"), TestOptions());
    REQUIRE(second.ok);
    CHECK(first.ToJson().dump() == second.ToJson().dump());

    ProjectorOptions other_key = TestOptions();
    other_key.projection_key = "another-key";
    const ProjectionReport rotated = ProjectLedger(root.Ledger("S-STABLE"), other_key);
    REQUIRE(rotated.ok);
    CHECK(rotated.trace_id != first.trace_id);
    REQUIRE(rotated.spans.size() == first.spans.size());
    for (std::size_t i = 0; i < first.spans.size(); ++i) {
        CHECK(rotated.spans[i].span_id != first.spans[i].span_id);
        CHECK(rotated.spans[i].name == first.spans[i].name);  // 结构不变
    }
}

TEST_CASE("排序与 D1:spans 按 seq 稳定序;输出无路径无正文") {
    SessionsRoot root("order");
    {
        auto writer = StartSession(root, "S-ORDER");
        REQUIRE(writer.has_value());
        for (int i = 1; i <= 12; ++i) {
            InstallRequest(*writer, "turn-" + std::to_string(i), nlohmann::json(nullptr));
        }
        EndSession(*writer);
    }
    const ProjectionReport report = ProjectLedger(root.Ledger("S-ORDER"), TestOptions());
    REQUIRE(report.ok);
    CHECK(report.spans.size() == 1 + 12 + 12);  // session + turns + requests
    // 排序稳定:与自身重放一致(确定性册已钉),这里钉 span 名序列单调。
    bool saw_session = false;
    for (const TraceSpan& span : report.spans) {
        if (span.name == "lubancode.session") {
            saw_session = true;
        }
    }
    CHECK(saw_session);
    const std::string dump = report.ToJson().dump();
    CHECK(dump.find("机密") == std::string::npos);
    CHECK(dump.find("/tmp") == std::string::npos);
    CHECK(dump.find("C:\\") == std::string::npos);
}

TEST_CASE("未覆盖域:title/verification/approval 无 span 材料,不伪造") {
    SessionsRoot root("uncovered");
    {
        auto writer = StartSession(root, "S-UNCOV");
        REQUIRE(writer.has_value());
        InstallRequest(*writer, "turn-000001", nlohmann::json(nullptr));
        // title.extracted(T11 域,无 span 映射):只投事件计数,不开 span。
        v3::EventDraft extracted;
        extracted.kind = v3::EventKindV3::TitleExtracted;
        extracted.title_generation_id = "title-000001";
        extracted.payload = nlohmann::json{{"title", "新标题"}};
        REQUIRE(writer->AppendEvent(std::move(extracted), v3::Durability::PowerLoss).status ==
                v3::WriteReceipt::Status::Committed);
        EndSession(*writer);
    }
    const ProjectionReport report = ProjectLedger(root.Ledger("S-UNCOV"), TestOptions());
    REQUIRE(report.ok);
    for (const TraceSpan& span : report.spans) {
        CHECK(span.name != "lubancode.title");
        CHECK(span.name != "lubancode.verification");
        CHECK(span.name != "lubancode.approval.wait");
    }
    const std::string dump = report.ToJson().dump();
    CHECK(dump.find("新标题") == std::string::npos);  // 域外材料不外发
}

TEST_CASE("hook.skipped 只计数;多 invocation 洋葱以最后一枚终态收口") {
    SessionsRoot root("hook");
    {
        auto writer = StartSession(root, "S-HOOK");
        REQUIRE(writer.has_value());
        InstallRequest(*writer, "turn-000001", nlohmann::json(nullptr));
        v3::HookHandlerSpec spec;
        spec.hook_id = "hook-1";
        spec.handler_kind = "lua";
        spec.definition_hash = "defhash";
        spec.definition_order = 1;
        spec.failure_policy = "continue";
        // skipped dispatch。
        v3::HookDispatchSession::Skip(*writer, writer->NewHookDispatchId(), "pre_tool_use",
                                      "no_match", std::nullopt, std::nullopt, std::nullopt,
                                      v3::Durability::PowerLoss);
        // 洋葱两枚 invocation:completed → failed(最后一枚定终局)。
        const std::string dispatch_id = writer->NewHookDispatchId();
        auto dispatch = v3::HookDispatchSession::Dispatch(
            *writer, dispatch_id, "pre_tool_use", "turn-000001", std::nullopt, std::nullopt,
            {spec, spec}, std::nullopt, v3::Durability::PowerLoss);
        REQUIRE(dispatch
                    .BeginInvocation(*writer, "hookinv-1", spec, v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        REQUIRE(dispatch
                    .CompleteInvocation(*writer, std::nullopt, std::nullopt, 5,
                                        v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        REQUIRE(dispatch
                    .BeginInvocation(*writer, "hookinv-2", spec, v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        REQUIRE(dispatch
                    .FailInvocation(*writer, "hook_timeout", std::nullopt,
                                    v3::Durability::PowerLoss)
                    .status == v3::WriteReceipt::Status::Committed);
        EndSession(*writer);
    }
    const ProjectionReport report = ProjectLedger(root.Ledger("S-HOOK"), TestOptions());
    REQUIRE(report.ok);
    std::size_t hook_spans = 0;
    const TraceSpan* failed_dispatch = nullptr;
    for (const TraceSpan& span : report.spans) {
        if (span.name != "lubancode.hook.dispatch") {
            continue;
        }
        hook_spans += 1;
        failed_dispatch = &span;
    }
    REQUIRE(hook_spans == 1);  // skipped 不开 span
    REQUIRE(failed_dispatch != nullptr);
    CHECK(failed_dispatch->status == StatusCode::Error);
    CHECK(failed_dispatch->status_description == "hook_timeout");
    CHECK(failed_dispatch->attributes.at("lubancode.hook.matched_handlers") == 2);
    REQUIRE(FindMetric(report, "lubancode.hook.dispatch_total",
                       nlohmann::json{{"outcome", "skipped"}}) != nullptr);
    REQUIRE(FindMetric(report, "lubancode.hook.dispatch_total",
                       nlohmann::json{{"outcome", "failed"}}) != nullptr);
}

TEST_CASE("坏账:验卷不过的账不投影(读取层拒,本件不做 IO 兜底)") {
    SessionsRoot root("bad");
    {
        auto writer = StartSession(root, "S-BAD");
        REQUIRE(writer.has_value());
        InstallRequest(*writer, "turn-000001", nlohmann::json(nullptr));
    }
    // 截尾(崩溃形状):ReadV3Ledger 拒——service 层按 source_corrupt 停流,
    // 这里钉"读不出就不该走到投影"。
    std::string bytes;
    {
        std::ifstream file(root.Ledger("S-BAD"), std::ios::binary);
        REQUIRE(file.is_open());
        bytes = std::string((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    }
    REQUIRE(!bytes.empty());
    {
        std::ofstream file(root.Ledger("S-BAD"), std::ios::binary | std::ios::trunc);
        file << bytes.substr(0, bytes.size() - 5);
    }
    CHECK_FALSE(v3::ReadV3Ledger(root.Ledger("S-BAD")).has_value());
}
