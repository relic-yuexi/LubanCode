// 轨迹会话账的实现(P0-2 运行时单一写口)。合同见 trajectory_session.hpp
// 文件头;事件 payload 形状照 todos/P0新轨迹记录_可重放与训练投影设计.todo
// §五与 P0-1 的 schema.cpp 逐字段钉死的样子。

#include "runtime/trajectory_session.hpp"

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <ctime>
#include <utility>

#include "accounting/purpose.hpp"   // PurposeName(Token 账本单 A1)
#include "agent/context.hpp"        // EstimateUtf8Tokens:request_snapshot 的 token 估算
#include "config/config.hpp"
#include "hooks/hash.hpp"           // Sha256Hex:request_snapshot 的 parameters_hash
#include "platform/log_sink.hpp"
#include "platform/paths.hpp"
#include "tools/path_utils.hpp"     // Utf8ToPath:主目录文本转路径
#include "tools/tool_content.hpp"   // TextContent:富结果块的文本投影
#include "trajectory/safety.hpp"
#include "workspace/identity.hpp"  // P0-1:身份裁决(冻结身份的兜底路)

namespace lubancode::runtime {

// ---------------------------------------------------------------------------
// flag
// ---------------------------------------------------------------------------

namespace {

std::optional<bool> TrajectoryEnvOpinion() {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    const errno_t err = _dupenv_s(&buffer, &size, "LUBANCODE_TRAJECTORY");
    if (err != 0 || buffer == nullptr) {
        return std::nullopt;
    }
    std::string value = buffer;
    std::free(buffer);
#else
    const char* raw = std::getenv("LUBANCODE_TRAJECTORY");
    if (raw == nullptr) {
        return std::nullopt;
    }
    std::string value = raw;
#endif
    if (value.empty() || value == "auto") {
        return std::nullopt;  // 没意见,听配置
    }
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
        return false;
    }
    return std::nullopt;  // 写坏了不当意见,配置照旧(救命阀字段的待遇)
}

}  // namespace

// §12.2/§13 journal emergency reserve 首版起始值(16 MiB)。配置面
//(trajectory.journal_emergency_reserve_bytes)随 P0-6 的配置档落,本批
// 用起始常量并在 /doctor trajectory 里如实展示。
constexpr std::uint64_t kJournalEmergencyReserveBytes = 16ULL * 1024 * 1024;

// ---------------------------------------------------------------------------
// TrajectoryTurnBridge
// ---------------------------------------------------------------------------

namespace {

using trajectory::Actor;
using trajectory::Durability;
using trajectory::EventKind;
using trajectory::EventLinks;
using trajectory::EventScope;
using trajectory::Origin;
using trajectory::RecordReceipt;
using trajectory::TrainingPolicy;
using trajectory::Visibility;

// 闸前未执行一族的终态(trajectory 映射:没越过 started 边界的,一律
// cancelled,不冒充执行过)。
bool OutcomeMapsToCancelled(const agent::ToolTraceEvent& event) {
    switch (event.outcome) {
        case agent::ToolOutcome::CancelledBeforeStart:
        case agent::ToolOutcome::CancelledDuringRun:
        case agent::ToolOutcome::UnknownTool:
        case agent::ToolOutcome::Unavailable:
        case agent::ToolOutcome::SchemaRejected:
        case agent::ToolOutcome::HookDenied:
        case agent::ToolOutcome::PermissionDeclined:
        case agent::ToolOutcome::ModeDenied:
        case agent::ToolOutcome::ScopeGatePending:
        case agent::ToolOutcome::ScopeGateOverBudget:  // fail closed:同样没越过执行边界
        case agent::ToolOutcome::SpawnFailed:
        case agent::ToolOutcome::ResultStoreFailed:
            return true;
        default:
            return false;
    }
}

}  // namespace

TrajectoryTurnBridge::TrajectoryTurnBridge(trajectory::TrajectoryRecorder& recorder,
                                           trajectory::EventScope base_scope, Identity identity)
    : recorder_(recorder), base_scope_(std::move(base_scope)), identity_(std::move(identity)) {}

TrajectoryTurnBridge::~TrajectoryTurnBridge() = default;

RecordReceipt TrajectoryTurnBridge::Put(EventKind kind, std::optional<std::string> request_id,
                                        std::optional<std::string> call_id, Actor actor, Origin origin,
                                        nlohmann::json payload, Durability durability, EventLinks links) {
    trajectory::RecordRequest request;
    request.kind = kind;
    request.scope = base_scope_;
    request.scope.turn_id = turn_id_;
    request.scope.request_id = std::move(request_id);
    request.scope.call_id = std::move(call_id);
    // 空串归一成缺省:id 要求按 has_value 判,空串会骗过 schema 却在状态
    // 机里查空键(PTC 一类宿主合成调用没有所属请求,如实不带)。
    if (request.scope.request_id.has_value() && request.scope.request_id->empty()) {
        request.scope.request_id.reset();
    }
    if (request.scope.call_id.has_value() && request.scope.call_id->empty()) {
        request.scope.call_id.reset();
    }
    request.scope.actor = actor;
    request.scope.origin = origin;
    request.links = std::move(links);
    request.payload = std::move(payload);
    const trajectory::RecordReceipt receipt = recorder_.Record(std::move(request), durability);
    // T1 committed wake(§25.3):committed 才投;只投身份,不投正文。
    if (receipt.status == trajectory::RecordReceipt::Status::Committed &&
        commit_wake_ != nullptr) {
        telemetry::CommitWake wake;
        wake.workspace_key = base_scope_.workspace_key;
        wake.session_id = base_scope_.session_id;
        wake.stream_id = wake_stream_id_;
        commit_wake_->Notify(wake);
    }
    return receipt;
}

void TrajectoryTurnBridge::NoteError(const RecordReceipt& receipt, const char* where) {
    // P0-B:字段级 message 随行——日志要能看出缺 turn_id 还是 call_id,
    // 不能只剩 schema.missing_field 一枚稳定码。
    std::string note = std::string(where) + ":" + receipt.error_code;
    if (!receipt.error_message.empty()) {
        note += " (" + receipt.error_message + ")";
    }
    recent_errors_.push_back(note);
    if (error_sink_ != nullptr) {
        error_sink_->push_back(note);
    }
    platform::LogSink::Instance().Error("trajectory", "落账失败: " + note);
}

std::string TrajectoryTurnBridge::NextRequestId() {
    return "req-" + std::to_string(++request_counter_);
}

std::string TrajectoryTurnBridge::NextInputId() {
    return "input-" + std::to_string(++input_counter_);
}

std::string TrajectoryTurnBridge::NextOutputId() {
    return "output-" + std::to_string(++output_counter_);
}

std::string TrajectoryTurnBridge::NextVerificationId() {
    return "verify-" + std::to_string(++verification_counter_);
}

void TrajectoryTurnBridge::BeginTurn(const std::string& turn_id, const std::string& trigger) {
    turn_id_ = turn_id;
    calls_.clear();
    request_prepared_.clear();
    last_input_event_id_.clear();
    turn_open_ = true;
    // 起因照实写进 actor/origin(§5.1/§5.5):真人/排队是 user,宿主起的
    // (peer/scheduler/goal)是 host,不拿第一条 user 消息猜。
    Actor actor = Actor::Host;
    Origin origin = Origin::ScheduledHost;
    if (trigger == "external_user") {
        actor = Actor::User;
        origin = Origin::ExternalUser;
    } else if (trigger == "queued_user") {
        actor = Actor::User;
        origin = Origin::QueuedUser;
    } else if (trigger == "peer_agent") {
        origin = Origin::PeerAgent;
    } else if (trigger == "goal_continuation") {
        origin = Origin::GoalContinuation;
    }
    const auto receipt =
        Put(EventKind::TurnStarted, std::nullopt, std::nullopt, actor, origin,
            nlohmann::json{{"trigger", trigger}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "turn.started");
    }
}

void TrajectoryTurnBridge::RecordInput(const api::Message& user_message) {
    if (!turn_open_) {
        return;
    }
    const bool queued = identity_.channel == "queued";
    nlohmann::json content = nlohmann::json::array();
    for (const auto& block : user_message.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            content.push_back(nlohmann::json{{"type", "text"}, {"text", text->text}});
        } else if (const auto* image = std::get_if<api::ImageBlock>(&block)) {
            content.push_back(nlohmann::json{{"type", "image"}, {"filename", image->filename}});
        }
    }
    const auto receipt =
        Put(EventKind::InputReceived, std::nullopt, std::nullopt, Actor::User,
            queued ? Origin::QueuedUser : Origin::ExternalUser,
            nlohmann::json{{"input_id", NextInputId()},
                           {"content", std::move(content)},
                           {"channel", identity_.channel},
                           {"sender", nlohmann::json{{"kind", identity_.channel == "app_server" ? "remote_user" : "local_user"}}}},
            Durability::ProcessCrash);
    if (receipt.status == RecordReceipt::Status::Committed) {
        last_input_event_id_ = receipt.event_id;
    } else {
        NoteError(receipt, "input.received");
    }
}

void TrajectoryTurnBridge::CancelDanglingCalls(const std::string& reason) {
    for (auto& [call_id, book] : calls_) {
        if (book.terminal) {
            continue;
        }
        if (!book.declared) {
            // P0-E:无主账项不该存在(OnToolTrace 的 ownership 门挡在造册口),
            // 万一有也不造明知过不了 schema 的事件。这不是静默跳过——先落
            // 稳定诊断(recent_errors/error_sink,doctor 可见),turn 由
            // EndTurn 按真实结果收成 failed/cancelled。
            const std::string note = "trajectory.dangling_call_undeclared:" + call_id;
            recent_errors_.push_back(note);
            if (error_sink_ != nullptr) {
                error_sink_->push_back(note);
            }
            platform::LogSink::Instance().Error(
                "trajectory", "悬空调用未声明过(无主账项),不补 cancelled: " + call_id);
            continue;
        }
        if (book.request_id.empty() || call_id.empty()) {
            // P0-E:request_id/call_id 为空必过不了 schema(Put 把空串归一成
            // 缺省,required 一刀拒下)。不再发这枚事件;字段级诊断先行,
            // EndTurn 若落 turn.completed 会被状态机拦下并如实转 failed。
            const std::string missing = book.request_id.empty() ? "request_id" : "call_id";
            const std::string note = "trajectory.dangling_call_missing_field:" + missing + ":" + call_id;
            recent_errors_.push_back(note);
            if (error_sink_ != nullptr) {
                error_sink_->push_back(note);
            }
            platform::LogSink::Instance().Error(
                "trajectory", "悬空调用缺 " + missing + ",不补 cancelled,turn 按真实结果收口: " + call_id);
            continue;
        }
        const auto receipt = Put(EventKind::ToolExecutionCancelled, book.request_id, call_id,
                                 Actor::Tool, Origin::BuiltinTool, nlohmann::json{{"reason", reason}});
        if (receipt.status == RecordReceipt::Status::Committed) {
            book.terminal = true;
            book.terminal_cancelled = true;
            book.terminal_event_id = receipt.event_id;
        } else {
            NoteError(receipt, "tool.execution.cancelled(dangling)");
        }
    }
}

void TrajectoryTurnBridge::EndTurn(bool ok, bool cancelled, const std::string& reason) {
    if (!turn_open_) {
        return;
    }
    CancelDanglingCalls("turn_closed_unresolved");
    // §5.5 outcome.assessed:turn 终态前的证据裁断。本轮录过验证才落——
    // 没验证的 turn 没有可引的证据,训练侧自然进不了 success 门。
    AssessOutcome(ok, cancelled);
    RecordReceipt receipt;
    if (cancelled) {
        receipt = Put(EventKind::TurnCancelled, std::nullopt, std::nullopt, Actor::Host,
                      Origin::RecoveryRuntime, nlohmann::json{{"reason", reason.empty() ? "cancelled" : reason}});
    } else if (ok) {
        receipt = Put(EventKind::TurnCompleted, std::nullopt, std::nullopt, Actor::Host,
                      Origin::RecoveryRuntime, nlohmann::json{{"outcome", "succeeded"}});
        if (receipt.status != RecordReceipt::Status::Committed) {
            // 悬空账没补齐(异常路径):如实落 failed,不伪造 completed。
            NoteError(receipt, "turn.completed");
            receipt = Put(EventKind::TurnFailed, std::nullopt, std::nullopt, Actor::Host,
                          Origin::RecoveryRuntime,
                          nlohmann::json{{"reason", "trajectory.turn_close_rejected"},
                                         {"error_code", receipt.error_code}});
        }
    } else {
        receipt = Put(EventKind::TurnFailed, std::nullopt, std::nullopt, Actor::Host,
                      Origin::RecoveryRuntime,
                      nlohmann::json{{"reason", reason.empty() ? "failed" : reason}});
    }
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "turn.terminal");
    }
    turn_open_ = false;
}

namespace {

// request_snapshot_ref 的 metadata_only 底账(Token 账本单 §6.4)。ctx 的
// prefix 账缺席(has_prefix_account=false)时 toolset_hash 留空——不拿
// 假 hash 冒充,消费侧按空串识别"这份没有前缀账可对"。
agent::RequestSnapshotMetadata BuildRequestSnapshot(const api::Request& request,
                                                    const agent::RequestPreparedContext& ctx) {
    agent::RequestSnapshotMetadata snapshot;
    snapshot.request_shape.model = request.model;
    snapshot.request_shape.message_count = request.messages.size();
    snapshot.request_shape.tool_count = request.tools.size();
    snapshot.request_shape.toolset_hash = ctx.has_prefix_account ? ctx.tools_hash : std::string();
    std::int64_t tool_tokens = 0;
    nlohmann::json params_shape = nlohmann::json{{"model", request.model},
                                                 {"reasoning_effort", request.reasoning_effort}};
    if (request.max_tokens.has_value()) {
        params_shape["max_output_tokens"] = *request.max_tokens;
    }
    for (const api::ToolDefinition& tool : request.tools) {
        tool_tokens += static_cast<std::int64_t>(agent::EstimateUtf8Tokens(tool.name)) +
                       static_cast<std::int64_t>(agent::EstimateUtf8Tokens(tool.description)) +
                       static_cast<std::int64_t>(agent::EstimateUtf8Tokens(tool.input_schema.dump()));
    }
    snapshot.request_shape.tool_definition_tokens_estimated = tool_tokens;
    snapshot.request_shape.parameters_hash = hooks::Sha256Hex(params_shape.dump());
    if (ctx.has_prompt_manifest) {
        snapshot.prompt_manifest = ctx.prompt_manifest;
    }
    snapshot.content_policy = "metadata_only";
    return snapshot;
}

// 消息 content -> 规范 blocks 数组(主桥与旁路桥共用;模型中立,大正文
// 交 blob,由 recorder 的 offload 上限管)。
nlohmann::json MessageToBlocksJson(const api::Message& message) {
    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            blocks.push_back(nlohmann::json{{"type", "text"}, {"text", text->text}});
        } else if (const auto* thinking = std::get_if<api::ThinkingBlock>(&block)) {
            blocks.push_back(nlohmann::json{{"type", "thinking"}, {"text", thinking->text}});
        } else if (const auto* image = std::get_if<api::ModelImageBlock>(&block)) {
            // 图片正文永不内联:只落引用块(sha/path),base64 不进 Journal。
            blocks.push_back(nlohmann::json{{"type", "image_ref"},
                                            {"mime_type", image->mime_type},
                                            {"sha256", image->sha256},
                                            {"path", image->path}});
        } else if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            blocks.push_back(nlohmann::json{{"type", "tool_call"},
                                            {"call_id", call->id},
                                            {"provider_call_id", call->id},
                                            {"name", call->name},
                                            {"arguments", call->input}});
        }
    }
    return blocks;
}

// model.request.prepared 的 payload(Token 账本单 A1):主桥与旁路桥共用
// 一份构造——purpose/system_ref/request_snapshot_ref 的事实口径只此一处,
// 两只桥不许各写各的。
nlohmann::json BuildPreparedPayload(const api::Request& request, const agent::RequestPreparedContext& ctx,
                                    const TrajectoryTurnBridge::Identity& identity,
                                    const std::string& last_input_event_id) {
    nlohmann::json payload = nlohmann::json{{"model", request.model},
                                            {"provider", identity.provider},
                                            {"wire", identity.wire},
                                            // Token 账本单 A1:purpose 恒有效(AgentProfile.purpose
                                            // 默认值),不是"没接线就漏字段"——真实运行时路径
                                            // 永远给出诚实的枚举名。
                                            {"purpose", accounting::PurposeName(ctx.purpose)}};
    nlohmann::json message_refs = nlohmann::json::array();
    if (!last_input_event_id.empty()) {
        message_refs.push_back(last_input_event_id);
    }
    payload["message_refs"] = std::move(message_refs);
    if (request.max_tokens.has_value()) {
        payload["parameters"] = nlohmann::json{{"max_output_tokens", *request.max_tokens}};
    }
    if (ctx.has_prefix_account && ctx.cache_epoch > 0) {
        payload["cache_epoch"] = static_cast<std::uint64_t>(ctx.cache_epoch);
    }
    // system/toolset 正文照 system_ref(§11.2):recorder 按字段名自动
    // offload 超限正文成 blob(schema.cpp 的可 offload 字段集已含
    // system_ref),这里只管递字符串,不管落盘细节。toolset_ref 递的是
    // 工具定义的规范化摘要(名字+描述+schema),不是完整 wire 请求体。
    if (!request.system.empty()) {
        payload["system_ref"] = request.system;
    }
    // request_snapshot_ref:metadata_only 的形状账(§6.4),manifest 缺席
    // (没接 ResolvedPromptBuilder)时 prompt_manifest 是一份空壳——仍然
    // 写,因为 request_shape(model/tool 计数/token 估算)本身是独立于
    // manifest 的事实,不该因为 manifest 缺席就整份不落。
    {
        const agent::RequestSnapshotMetadata snapshot = BuildRequestSnapshot(request, ctx);
        payload["request_snapshot_ref"] = snapshot.ToJson();
        payload["request_snapshot_sha256"] = hooks::Sha256Hex(payload["request_snapshot_ref"].dump());
    }
    return payload;
}

}  // namespace

void TrajectoryTurnBridge::OnContextPressure(const agent::ContextPressure& pressure) {
    if (!turn_open_ || pressure.phase != agent::ContextPressure::Phase::PreflightExceeded) {
        return;
    }
    const auto receipt = Put(
        EventKind::ContextPressureRecorded, std::nullopt, std::nullopt, Actor::Host,
        Origin::BudgetGuard,
        nlohmann::json{{"phase", "preflight_exceeded"},
                       {"estimated_input_tokens",
                        static_cast<std::uint64_t>(pressure.estimated_input_tokens)},
                       {"reserved_output_tokens",
                        static_cast<std::uint64_t>(pressure.reserved_output_tokens)},
                       {"protocol_headroom_tokens",
                        static_cast<std::uint64_t>(pressure.protocol_headroom_tokens)},
                       {"window_tokens", static_cast<std::uint64_t>(pressure.window_tokens)},
                       {"reserve_clamped", pressure.reserve_clamped}},
        Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        // 这是可观测 metadata，不把一次落账故障改写成长任务预检业务结果；
        // 与 sent/usage 边界同样记错并继续，由 doctor 暴露坏账。
        NoteError(receipt, "context.pressure.recorded");
    }
}

std::string TrajectoryTurnBridge::OnRequestPrepared(const api::Request& request,
                                                     const agent::RequestPreparedContext& ctx) {
    if (!turn_open_) {
        return std::string();
    }
    const std::string request_id = NextRequestId();
    nlohmann::json payload = BuildPreparedPayload(request, ctx, identity_, last_input_event_id_);
    const auto receipt = Put(EventKind::ModelRequestPrepared, request_id, std::nullopt, Actor::Host,
                             Origin::RecoveryRuntime, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.request.prepared");
        return std::string();  // §7.4:prepared 记不住,不发模型
    }
    request_prepared_[request_id] = receipt.event_id;
    return request_id;
}

void TrajectoryTurnBridge::OnRequestSent(const std::string& request_id) {
    const auto it = request_prepared_.find(request_id);
    if (it == request_prepared_.end()) {
        return;
    }
    const auto receipt =
        Put(EventKind::ModelRequestSent, request_id, std::nullopt, Actor::Host,
            Origin::RecoveryRuntime,
            nlohmann::json{{"prepared_event_id", it->second}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.request.sent");
    }
}

void TrajectoryTurnBridge::OnRequestSentWithTurn(const std::string& request_id, int task_turn_index,
                                                 int turn_limit, int input_round_index) {
    const auto it = request_prepared_.find(request_id);
    if (it == request_prepared_.end()) {
        return;
    }
    // 任务 turn 账(§11.1,P1-1):sent 边界就是一枚 model turn 的 started——
    // permit 已提交(此后 API 错/流断都保留 attempted),task_turn_index 从
    // 1 起、limit/input round 一并落账。收口三态按 request_id 对回坐标,
    // verifier 据此核"不重号、不超 limit"。
    nlohmann::json payload = nlohmann::json{{"prepared_event_id", it->second},
                                            {"task_turn_index", static_cast<std::uint64_t>(task_turn_index)},
                                            {"turn_limit", static_cast<std::uint64_t>(turn_limit)},
                                            {"input_round_index", static_cast<std::uint64_t>(input_round_index)}};
    RequestTurnBook& book = request_turns_[request_id];
    book.task_turn_index = task_turn_index;
    book.turn_limit = turn_limit;
    book.input_round_index = input_round_index;
    const auto receipt = Put(EventKind::ModelRequestSent, request_id, std::nullopt, Actor::Host,
                             Origin::RecoveryRuntime, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.request.sent");
    }
}

void TrajectoryTurnBridge::OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                                           bool reported_by_provider,
                                           const std::string& provider_response_id, int cache_epoch,
                                           bool prefix_append_only, bool cache_reported_by_provider) {
    nlohmann::json payload = nlohmann::json{{"attempt", std::uint64_t{1}},
                                            {"reported_by_provider", reported_by_provider},
                                            {"cache_reported_by_provider", cache_reported_by_provider}};
    if (!provider_response_id.empty()) {
        payload["provider_response_id"] = provider_response_id;
    }
    // 数字只在 provider 明报时才算事实;没报不拿 0 冒充(Token 账本 A0)。
    if (reported_by_provider) {
        payload["input_tokens"] = usage.input_tokens;
        payload["cache_read_tokens"] = usage.cache_read_tokens;
        payload["cache_creation_tokens"] = usage.cache_creation_tokens;
        payload["output_tokens"] = usage.output_tokens;
        payload["reasoning_tokens"] = usage.output_reasoning_tokens;
    }
    // 前缀账(Token 账本单 A1,§7.2 cache 指标的地基):cache_epoch=0 表示
    // 这次调用没带前缀账(旧调用方/单测),不落——真实 epoch 从 1 起。
    if (cache_epoch > 0) {
        payload["cache_epoch"] = static_cast<std::uint64_t>(cache_epoch);
        payload["prefix_append_only"] = prefix_append_only;
    }
    const auto receipt =
        Put(EventKind::ModelUsageRecorded, request_id, std::nullopt, Actor::Host,
            Origin::RecoveryRuntime, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.usage.recorded");
    }
}

nlohmann::json TrajectoryTurnBridge::MessageToBlocks(const api::Message& message) {
    // 实现住在文件局部 MessageToBlocksJson(旁路桥同吃一份,口径只此一处)。
    return MessageToBlocksJson(message);
}

bool TrajectoryTurnBridge::OnOutputCompleted(const std::string& request_id, const api::Message& assistant,
                                             const std::string& stop_reason,
                                             const std::string& provider_response_id) {
    nlohmann::json payload = nlohmann::json{{"output_id", NextOutputId()},
                                            {"blocks", MessageToBlocks(assistant)},
                                            {"stop_reason", stop_reason.empty() ? "end_turn" : stop_reason}};
    if (!provider_response_id.empty()) {
        payload["provider_response_id"] = provider_response_id;
    }
    // 任务 turn 账(§11.1,P1-1):completed 边界带回 task_turn_index——同一枚
    // turn 的 started/completed 两处数字同源(sent 时记的请求簿)。
    if (const auto turn_it = request_turns_.find(request_id); turn_it != request_turns_.end()) {
        payload["task_turn_index"] = static_cast<std::uint64_t>(turn_it->second.task_turn_index);
    }
    const auto receipt =
        Put(EventKind::ModelOutputCompleted, request_id, std::nullopt, Actor::Model,
            Origin::ProviderModel, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.completed");
        return false;  // §7.4:输出记不住,不执行工具
    }
    // 声明本份 output 的 tool call(§6.1:call 由模型输出定义)。这是
    // calls_ 造册的唯一合法入口(P0-D ownership 不变量):旁路 trace 想靠
    // operator[] 反向创造模型事实,门都没有。
    for (const auto& block : assistant.content) {
        if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            if (call->id.empty()) {
                continue;  // P0-F:空 id 的 call 不入册(assembler 已挡,双保险)
            }
            CallBook& book = calls_[call->id];
            book.request_id = request_id;
            book.declared = true;
        }
    }
    return true;
}

void TrajectoryTurnBridge::OnOutputFailed(const std::string& request_id, const std::string& reason) {
    nlohmann::json payload = {{"reason", reason}};
    // 任务 turn 账(§11.1,P1-1):failed 也带 task_turn_index——失败请求保留
    // attempted,这枚 turn 有编号可对。
    if (const auto turn_it = request_turns_.find(request_id); turn_it != request_turns_.end()) {
        payload["task_turn_index"] = static_cast<std::uint64_t>(turn_it->second.task_turn_index);
    }
    const auto receipt =
        Put(EventKind::ModelOutputFailed, request_id, std::nullopt, Actor::Model,
            Origin::ProviderModel, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.failed");
    }
}

void TrajectoryTurnBridge::OnOutputCancelled(const std::string& request_id) {
    nlohmann::json payload = {{"reason", "user_interrupt"}};
    // 流中取消:permit 已消耗,attempted 保留(§6.4)——turn 坐标照带。
    if (const auto turn_it = request_turns_.find(request_id); turn_it != request_turns_.end()) {
        payload["task_turn_index"] = static_cast<std::uint64_t>(turn_it->second.task_turn_index);
    }
    const auto receipt =
        Put(EventKind::ModelOutputCancelled, request_id, std::nullopt, Actor::Model,
            Origin::ProviderModel, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.cancelled");
    }
}

void TrajectoryTurnBridge::OnToolTrace(const agent::ToolTraceEvent& event) {
    if (!turn_open_ || event.tool_use_id.empty()) {
        return;
    }
    // P0-D ownership 门:父桥只收模型声明过的 call(model.output.completed
    // 造册)。陌生 tool_use_id(子代理账没拿到时回灌的旁听 trace、上游
    // 串线的 call id)不进 calls_,不造册,不推进状态机——只进有界诊断
    // 投影。旁路 trace 不能靠 operator[] 反向创造模型事实。
    const auto book_it = calls_.find(event.tool_use_id);
    if (book_it == calls_.end()) {
        NoteUnownedToolTrace(event);
        return;
    }
    CallBook& book = book_it->second;
    switch (event.kind) {
        case agent::ToolTraceEventKind::Scheduled: {
            if (book.planned) {
                return;
            }
            const auto receipt =
                Put(EventKind::ToolExecutionPlanned, book.request_id, event.tool_use_id, Actor::Model,
                    Origin::ProviderModel,
                    nlohmann::json{{"call_id", event.tool_use_id}, {"tool_name", event.tool_name}});
            if (receipt.status == RecordReceipt::Status::Committed) {
                book.planned = true;
            } else {
                NoteError(receipt, "tool.execution.planned");
            }
            return;
        }
        case agent::ToolTraceEventKind::ExecutionStarted: {
            if (!book.planned) {
                const auto planned =
                    Put(EventKind::ToolExecutionPlanned, book.request_id, event.tool_use_id, Actor::Model,
                        Origin::ProviderModel,
                        nlohmann::json{{"call_id", event.tool_use_id}, {"tool_name", event.tool_name}});
                if (planned.status == RecordReceipt::Status::Committed) {
                    book.planned = true;
                }
            }
            if (!book.effective) {
                nlohmann::json effective =
                    nlohmann::json{{"call_id", event.tool_use_id},
                                   {"tool_name", event.tool_name},
                                   {"source_kind", agent::ToString(event.source_kind)},
                                   {"source_instance", event.source_instance},
                                   {"effect_class", agent::ToString(event.effect_class)},
                                   {"effective_arguments", event.effective_arguments.is_object()
                                                               ? event.effective_arguments
                                                               : nlohmann::json::object()},
                                   {"effective_arguments_sha256", event.effective_input_sha256},
                                   {"rewritten_by", nlohmann::json::array()}};
                const auto receipt =
                    Put(EventKind::ToolInputEffective, book.request_id, event.tool_use_id, Actor::Tool,
                        Origin::BuiltinTool, std::move(effective), Durability::ProcessCrash);
                if (receipt.status == RecordReceipt::Status::Committed) {
                    book.effective = true;
                } else {
                    NoteError(receipt, "tool.input.effective");
                }
            }
            // P0-4 细账料(§9.3):started 留下实际入参与来源,finished 拼
            // command/mcp 细账时从这翻。
            book.effective_arguments =
                event.effective_arguments.is_object() ? event.effective_arguments
                                                      : nlohmann::json::object();
            book.source_instance = event.source_instance;
            // 副作用边界:started 走 PowerLoss 栅栏(§7.4/§5.4)。
            nlohmann::json payload = nlohmann::json{{"call_id", event.tool_use_id},
                                                    {"attempt", std::uint64_t{1}}};
            if (!event.batch_id.empty()) {
                payload["batch_id"] = event.batch_id;
                payload["position_in_batch"] = static_cast<std::uint64_t>(
                    event.sequence_in_batch >= 0 ? event.sequence_in_batch : 0);
            }
            EventLinks links;
            if (!book.child_run_id.empty()) {
                links.child_run_id = book.child_run_id;  // 子代理边界引用(§3.5)
            }
            const auto receipt =
                Put(EventKind::ToolExecutionStarted, book.request_id, event.tool_use_id, Actor::Tool,
                    Origin::BuiltinTool, std::move(payload), Durability::PowerLoss, std::move(links));
            if (receipt.status == RecordReceipt::Status::Committed) {
                book.started = true;
            } else {
                NoteError(receipt, "tool.execution.started");
                started_io_failed_.insert(event.execution_id);
            }
            // P0-4 存储门(§12.2 storage_exhausted):副作用工具在 started
            // 落稳之后、execute 之前问一次磁盘 reserve;不足则把这只执行
            // 记入 storage_blocked_,ShouldBlockExecution 据此拦下——工具
            // 不跑,免得"跑完工具才悄悄丢结果"。
            const bool side_effect = event.effect_class != agent::EffectClass::ReadOnlyLocal &&
                                     event.effect_class != agent::EffectClass::ReadOnlyRemote;
            if (side_effect && !StorageAvailable()) {
                storage_blocked_.insert(event.execution_id);
                const std::string note = "storage_exhausted:" + event.tool_name;
                recent_errors_.push_back(note);
                if (error_sink_ != nullptr) {
                    error_sink_->push_back(note);
                }
                platform::LogSink::Instance().Error(
                    "trajectory", "磁盘 reserve 不足,拦下副作用工具: " + event.tool_name);
            }
            return;
        }
        case agent::ToolTraceEventKind::ExecutionFinished: {
            if (book.terminal) {
                return;  // 终态唯一,迟到不覆盖
            }
            const bool never_started = !book.started;
            EventKind kind = EventKind::ToolExecutionFailed;
            nlohmann::json payload;
            if (event.outcome == agent::ToolOutcome::Succeeded) {
                kind = EventKind::ToolExecutionFinished;
                payload["outcome"] = "succeeded";
                payload["duration_ms"] = event.duration_ms;
                nlohmann::json ref = nlohmann::json{{"sha256", event.result_ref.sha256},
                                                    {"bytes", event.result_ref.bytes},
                                                    {"kind", agent::ToString(event.result_ref.kind)}};
                payload["result_ref"] = std::move(ref);
                // P0-4 §9.3 side-effect 细账:file(undo token)/command(有效
                // 入参 + exit code)/mcp(server 身份 + jsonrpc id)。
                bool has_exit_code = false;
                std::int64_t exit_code = 0;
                payload["side_effects"] = BuildSideEffects(event, book, &has_exit_code, &exit_code);
                if (has_exit_code) {
                    payload["exit_code"] = exit_code;
                }
            } else if (event.outcome == agent::ToolOutcome::UnknownAfterStart) {
                kind = EventKind::ToolExecutionUnknown;
                payload["reason"] = event.error_code.empty() ? "unknown_after_start" : event.error_code;
                payload["duration_ms"] = event.duration_ms;
            } else if (never_started || OutcomeMapsToCancelled(event)) {
                // 闸前被收掉/拦下:没越过执行边界,落 cancelled,不冒充
                // 执行过(§6.2 约束 16;P0-2 起 cancelled 不要求 started)。
                kind = EventKind::ToolExecutionCancelled;
                payload["reason"] =
                    event.error_code.empty() ? agent::ToString(event.outcome) : event.error_code;
                if (event.duration_ms > 0) {
                    payload["duration_ms"] = event.duration_ms;
                }
            } else {
                kind = EventKind::ToolExecutionFailed;
                payload["reason"] =
                    event.error_code.empty() ? agent::ToString(event.outcome) : event.error_code;
                if (!event.error_code.empty()) {
                    payload["error_code"] = event.error_code;
                }
                payload["duration_ms"] = event.duration_ms;
            }
            // 子代理边界:agent 工具的执行终态把 child run 引用带上
            //(§3.5/§16.4:父子文件只传边界引用与 terminal hash,不内联
            // 子账细账)。child_run_id 走 relations;子账终态 hash 落
            // result_ref——但 result_ref 只在 schema 认它的终态 kind 上带
            //(finished/failed)。cancelled/unknown 的 payload 键集没有
            // result_ref,塞进去会被 schema 拒收,终态就丢了(ESC 掐在
            // agent 调用中途正是这一形状——child_run_id 照挂 relations,
            // hash 对账交给 verifier 实读子文件)。
            EventLinks links;
            if (!book.child_run_id.empty()) {
                links.child_run_id = book.child_run_id;
                if (kind == EventKind::ToolExecutionFinished || kind == EventKind::ToolExecutionFailed) {
                    const auto hash = child_terminal_hashes_.find(book.child_run_id);
                    payload["result_ref"] = nlohmann::json{
                        {"kind", "child_stream"},
                        {"child_run_id", book.child_run_id},
                        {"child_terminal_event_hash",
                         hash != child_terminal_hashes_.end() ? hash->second : std::string()}};
                    payload["side_effects"] = nlohmann::json::array();
                }
            }
            const auto receipt = Put(kind, book.request_id, event.tool_use_id, Actor::Tool,
                                     Origin::BuiltinTool, std::move(payload), Durability::PowerLoss,
                                     std::move(links));
            if (receipt.status == RecordReceipt::Status::Committed) {
                book.terminal = true;
                book.terminal_cancelled = kind == EventKind::ToolExecutionCancelled;
                book.terminal_event_id = receipt.event_id;
                // §5.5 stale invalidation:改动了文件就逐枚对账已录验证,
                // subject 命中的落 verification.invalidated(训练集不能拿
                // 旧测试给新代码作证)。
                if (!event.undo.path.empty()) {
                    InvalidateStaleVerifications(event.undo.path, receipt.event_id);
                }
            } else {
                NoteError(receipt, "tool.terminal");
            }
            return;
        }
        case agent::ToolTraceEventKind::Verification: {
            // hub 侧显式验证点(逐枚追踪单的 postcondition 证据):翻成
            // verification.recorded(§5.5)。label 兼作 kind,after_execution_
            // id 挂 causation,验证正文经 verify_detail 现折 facts。
            if (!turn_open_) {
                return;
            }
            nlohmann::json payload{{"verification_id", NextVerificationId()},
                                   {"kind", event.label.empty() ? "tool_postcondition" : event.label},
                                   {"passed", event.passed},
                                   {"producer", "tool_trace"}};
            if (!event.after_execution_id.empty()) {
                payload["subject"] = event.after_execution_id;
            }
            if (!event.verify_detail.empty()) {
                payload["facts"] = nlohmann::json{{"detail", event.verify_detail}};
            }
            payload["observed_after_seq"] = recorder_.next_seq() - 1;
            const std::string verification_id = payload.value("verification_id", std::string());
            const std::string kind = payload.value("kind", std::string());
            const std::string subject = payload.value("subject", std::string());
            const auto receipt = Put(EventKind::VerificationRecorded, std::nullopt, std::nullopt,
                                     Actor::Verifier, Origin::VerifierHost, std::move(payload),
                                     Durability::ProcessCrash);
            if (receipt.status != RecordReceipt::Status::Committed) {
                NoteError(receipt, "verification.recorded");
                return;
            }
            VerificationBook book;
            book.verification_id = verification_id;
            book.kind = kind;
            book.subject = subject;
            book.passed = event.passed;
            book.recorded = true;
            book.recorded_event_id = receipt.event_id;
            verifications_.push_back(std::move(book));
            return;
        }
        case agent::ToolTraceEventKind::ResultCommitted:
        case agent::ToolTraceEventKind::RecoveryMarker:
        case agent::ToolTraceEventKind::McpLateResponse:
            return;  // result.committed 从消息正文翻(OnToolResultsCommitted);
                     // 迟到响应/恢复注记不进轨迹。
    }
}

// 富结果块 -> 无损投影块(P0-2:structured content 与图片/音频 artifact
// ref 一个不丢)。文本块原样;二进制块只落引用(mime/尺寸/字节/sha/
// artifact 相对路径,字节永不内联 Journal);resource link 落 URI;embedded
// text 帽内原样、超帽带 artifact 引用与截断标记;未知块保 type 与摘要。
// 旧会话存档的 BlockToJson 投影随 P0-6 退役,这里是唯一真账。
nlohmann::json RichBlockToProjection(const tools::ToolContentBlock& block) {
    if (const auto* text = std::get_if<tools::TextContent>(&block)) {
        return nlohmann::json{{"type", "text"}, {"text", text->text}};
    }
    if (const auto* image = std::get_if<tools::ImageContent>(&block)) {
        nlohmann::json projection = nlohmann::json{{"type", "image_ref"},
                                                   {"mime_type", image->mime_type},
                                                   {"width", image->width},
                                                   {"height", image->height},
                                                   {"bytes", image->bytes},
                                                   {"sha256", image->sha256},
                                                   {"stored", image->artifact.stored}};
        if (image->artifact.stored) {
            projection["artifact_id"] = image->artifact.id;
            projection["path"] = image->artifact.path;
        }
        return projection;
    }
    if (const auto* audio = std::get_if<tools::AudioContent>(&block)) {
        nlohmann::json projection = nlohmann::json{{"type", "audio_ref"},
                                                   {"mime_type", audio->mime_type},
                                                   {"bytes", audio->bytes},
                                                   {"sha256", audio->sha256},
                                                   {"stored", audio->artifact.stored}};
        if (audio->artifact.stored) {
            projection["artifact_id"] = audio->artifact.id;
            projection["path"] = audio->artifact.path;
        }
        return projection;
    }
    if (const auto* link = std::get_if<tools::ResourceLinkContent>(&block)) {
        nlohmann::json projection = nlohmann::json{{"type", "resource_link"}, {"uri", link->uri}};
        if (!link->name.empty()) {
            projection["name"] = link->name;
        }
        if (!link->mime_type.empty()) {
            projection["mime_type"] = link->mime_type;
        }
        if (link->size >= 0) {
            projection["size"] = link->size;
        }
        return projection;
    }
    if (const auto* embedded = std::get_if<tools::EmbeddedTextResourceContent>(&block)) {
        nlohmann::json projection = nlohmann::json{{"type", "embedded_text"},
                                                   {"uri", embedded->uri},
                                                   {"text", embedded->text},
                                                   {"truncated", embedded->truncated}};
        if (embedded->artifact.has_value() && embedded->artifact->stored) {
            projection["artifact_id"] = embedded->artifact->id;
            projection["path"] = embedded->artifact->path;
        }
        return projection;
    }
    if (const auto* blob = std::get_if<tools::EmbeddedBlobResourceContent>(&block)) {
        nlohmann::json projection = nlohmann::json{{"type", "blob_ref"},
                                                   {"uri", blob->uri},
                                                   {"mime_type", blob->mime_type},
                                                   {"bytes", blob->bytes},
                                                   {"sha256", blob->sha256},
                                                   {"stored", blob->artifact.stored}};
        if (blob->artifact.stored) {
            projection["artifact_id"] = blob->artifact.id;
            projection["path"] = blob->artifact.path;
        }
        return projection;
    }
    if (const auto* unknown = std::get_if<tools::UnknownContent>(&block)) {
        return nlohmann::json{{"type", "unknown"},
                              {"original_type", unknown->original_type},
                              {"summary", unknown->summary}};
    }
    return nlohmann::json{{"type", "unknown"},
                          {"original_type", std::string()},
                          {"summary", std::string()}};
}

void TrajectoryTurnBridge::OnToolResultsCommitted(const std::string& batch_id, const api::Message& results) {
    (void)batch_id;
    if (!turn_open_) {
        return;
    }
    for (const auto& block : results.content) {
        const auto* result = std::get_if<api::ToolResultBlock>(&block);
        if (result == nullptr) {
            continue;
        }
        const auto it = calls_.find(result->tool_use_id);
        if (it == calls_.end() || !it->second.terminal || it->second.result_committed) {
            continue;
        }
        nlohmann::json content = nlohmann::json::array();
        if (!result->content.empty()) {
            content.push_back(nlohmann::json{{"type", "text"}, {"text", result->content}});
        }
        for (const auto& extra : result->blocks) {
            content.push_back(RichBlockToProjection(extra));
        }
        nlohmann::json payload = nlohmann::json{{"call_id", result->tool_use_id},
                                                {"content", std::move(content)},
                                                {"is_error", result->is_error}};
        // structuredContent 无损随行(P0-2):nullopt 不落键——"server 没给"
        // 与"给了空对象"在账上分得清。
        if (result->structured_content.has_value()) {
            payload["structured_content"] = *result->structured_content;
        }
        if (!it->second.terminal_event_id.empty()) {
            payload["derived_from_event"] = it->second.terminal_event_id;
        }
        const auto receipt =
            Put(EventKind::ToolResultCommitted, it->second.request_id, result->tool_use_id, Actor::Tool,
                Origin::BuiltinTool, std::move(payload), Durability::ProcessCrash);
        if (receipt.status == RecordReceipt::Status::Committed) {
            it->second.result_committed = true;
        } else {
            NoteError(receipt, "tool.result.committed");
        }
    }
}

bool TrajectoryTurnBridge::ShouldBlockExecution(const agent::ToolTraceEvent& started) {
    return started_io_failed_.count(started.execution_id) != 0 ||
           storage_blocked_.count(started.execution_id) != 0;
}

void TrajectoryTurnBridge::NoteUnownedToolTrace(const agent::ToolTraceEvent& event) {
    // P0-D 的有界诊断投影:一条一枚,带齐单子点名的六样身份;上限 32 条,
    // 溢出只计数不刷屏。进 recent_errors/error_sink(doctor 可见),不进
    // canonical 事件流,不动 calls_。
    std::string note = "trajectory.unowned_tool_trace: run_id=" + base_scope_.run_id +
                       " turn_id=" + turn_id_ + " execution_id=" + event.execution_id +
                       " call_id=" + event.tool_use_id + " tool_name=" + event.tool_name +
                       " parent_execution_id=" + event.parent_execution_id;
    if (unowned_trace_notes_.size() < 32) {
        unowned_trace_notes_.push_back(note);
    } else {
        ++unowned_trace_dropped_;
    }
    if (recent_errors_.size() < 64) {  // 有界:同一症状不无限刷错误环
        recent_errors_.push_back(note);
    }
    if (error_sink_ != nullptr && error_sink_->size() < 128) {
        error_sink_->push_back(note);
    }
    platform::LogSink::Instance().Warn("trajectory", "无主 tool trace(未由模型输出声明): " + note);
}

void TrajectoryTurnBridge::AttachChildRun(const std::string& call_id, const std::string& agent_run_id) {
    // P0-D:挂边也只认已声明的 call——operator[] 会给陌生 id 造空册,正是
    // 这次的污染路径之一。找不到就记一笔诊断,不造册。
    const auto it = calls_.find(call_id);
    if (it == calls_.end()) {
        const std::string note = "trajectory.attach_child_run_undeclared:" + call_id;
        recent_errors_.push_back(note);
        if (error_sink_ != nullptr) {
            error_sink_->push_back(note);
        }
        platform::LogSink::Instance().Warn(
            "trajectory", "AttachChildRun 指到未声明的 call,边界不挂: " + call_id);
        return;
    }
    it->second.child_run_id = agent_run_id;
}

void TrajectoryTurnBridge::NoteChildTerminal(const std::string& agent_run_id,
                                             const std::string& terminal_event_hash) {
    child_terminal_hashes_[agent_run_id] = terminal_event_hash;
}

// ---------------------------------------------------------------------------
// TrajectoryBypassBridge(Token 账本单 A1)
// ---------------------------------------------------------------------------

TrajectoryBypassBridge::TrajectoryBypassBridge(trajectory::TrajectoryRecorder& recorder,
                                               trajectory::EventScope base_scope,
                                               TrajectoryTurnBridge::Identity identity)
    : recorder_(recorder), base_scope_(std::move(base_scope)), identity_(std::move(identity)) {}

TrajectoryBypassBridge::~TrajectoryBypassBridge() = default;

RecordReceipt TrajectoryBypassBridge::Put(EventKind kind, std::optional<std::string> request_id, Actor actor,
                                          Origin origin, nlohmann::json payload, Durability durability) {
    if (dead_) {
        // 哑火桥:小 turn 开不成(主 turn 占着 stream),本枚采样不入账。
        RecordReceipt receipt;
        receipt.status = RecordReceipt::Status::Rejected;
        receipt.error_code = "state.turn_overlap";
        return receipt;
    }
    trajectory::RecordRequest request;
    request.kind = kind;
    request.scope = base_scope_;
    request.scope.turn_id = turn_id_;
    request.scope.request_id = std::move(request_id);
    request.scope.call_id.reset();  // 旁路请求没有工具调用
    if (request.scope.request_id.has_value() && request.scope.request_id->empty()) {
        request.scope.request_id.reset();
    }
    request.scope.actor = actor;
    request.scope.origin = origin;
    request.payload = std::move(payload);
    const trajectory::RecordReceipt receipt = recorder_.Record(std::move(request), durability);
    // T1 committed wake(与主桥同款)。
    if (receipt.status == trajectory::RecordReceipt::Status::Committed &&
        commit_wake_ != nullptr) {
        telemetry::CommitWake wake;
        wake.workspace_key = base_scope_.workspace_key;
        wake.session_id = base_scope_.session_id;
        wake.stream_id = wake_stream_id_;
        commit_wake_->Notify(wake);
    }
    return receipt;
}

void TrajectoryBypassBridge::NoteError(const RecordReceipt& receipt, const char* where) {
    recent_errors_.push_back(std::string(where) + ":" + receipt.error_code);
    platform::LogSink::Instance().Error("trajectory",
                                        std::string(where) + " 落账失败: " + receipt.error_code);
}

std::string TrajectoryBypassBridge::NextRequestId() {
    return "bypass-req-" + std::to_string(++request_counter_);
}

std::string TrajectoryBypassBridge::NextTurnId() {
    return "bypass-" + std::to_string(++turn_counter_);
}

std::string TrajectoryBypassBridge::NextInputId() {
    return "bypass-input-" + std::to_string(++input_counter_);
}

std::string TrajectoryBypassBridge::NextOutputId() {
    return "bypass-output-" + std::to_string(++output_counter_);
}

void TrajectoryBypassBridge::OpenTurn() {
    if (turn_open_) {
        return;
    }
    turn_id_ = NextTurnId();
    turn_open_ = true;
    // scheduled_host 小 turn:宿主自己起的后台活,不是真人回合。约束 18
    // 的 actor/origin 组合里 Host+ScheduledHost 合法(主桥 BeginTurn 同款)。
    const auto receipt =
        Put(EventKind::TurnStarted, std::nullopt, Actor::Host, Origin::ScheduledHost,
            nlohmann::json{{"trigger", "scheduled_host"}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        // 开不了小 turn(典型:主 turn 还开着,状态机一 stream 一 open
        // turn)——本桥哑火:后续事件一概不发,只记一笔缺口,不连环报
        // 错吓人。旁路采样本体照跑(调用方不依赖桥的成败),丢的只是
        // 这枚采样的 usage 细账。
        NoteError(receipt, "turn.started(bypass)");
        turn_open_ = false;
        dead_ = true;
    }
}

void TrajectoryBypassBridge::CloseTurn(bool ok, bool cancelled, const std::string& reason) {
    if (!turn_open_) {
        return;
    }
    RecordReceipt receipt;
    if (cancelled) {
        receipt = Put(EventKind::TurnCancelled, std::nullopt, Actor::Host, Origin::ScheduledHost,
                      nlohmann::json{{"reason", reason.empty() ? "cancelled" : reason}});
    } else if (ok) {
        receipt = Put(EventKind::TurnCompleted, std::nullopt, Actor::Host, Origin::ScheduledHost,
                      nlohmann::json{{"outcome", "succeeded"}});
    } else {
        receipt = Put(EventKind::TurnFailed, std::nullopt, Actor::Host, Origin::ScheduledHost,
                      nlohmann::json{{"reason", reason.empty() ? "failed" : reason}});
    }
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "turn.terminal(bypass)");
    }
    turn_open_ = false;
}

std::string TrajectoryBypassBridge::OnRequestPrepared(const api::Request& request,
                                                      const agent::RequestPreparedContext& ctx) {
    if (turn_open_) {
        // 一桥一采样:上一只小 turn 没收口又来一枚 prepared,是调用方把
        // 桥当长命对象复用了。拒收,不往同一 turn 里混两笔请求账。
        return std::string();
    }
    OpenTurn();
    // 状态机约束 3:首 sent 前须有 input.received。旁路请求的 input 就是
    // 请求自己的首条 user 消息(压缩材料/抽取转写/标题问句),照实记。
    last_input_event_id_.clear();
    if (!request.messages.empty()) {
        nlohmann::json content = nlohmann::json::array();
        for (const auto& block : request.messages.front().content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                content.push_back(nlohmann::json{{"type", "text"}, {"text", text->text}});
            }
        }
        const auto receipt =
            Put(EventKind::InputReceived, std::nullopt, Actor::Host, Origin::ScheduledHost,
                nlohmann::json{{"input_id", NextInputId()},
                               {"content", std::move(content)},
                               {"channel", identity_.channel},
                               {"sender", nlohmann::json{{"kind", "host"}}}},
                Durability::ProcessCrash);
        if (receipt.status == RecordReceipt::Status::Committed) {
            last_input_event_id_ = receipt.event_id;
        } else {
            NoteError(receipt, "input.received(bypass)");
        }
    }
    const std::string request_id = NextRequestId();
    nlohmann::json payload = BuildPreparedPayload(request, ctx, identity_, last_input_event_id_);
    const auto receipt = Put(EventKind::ModelRequestPrepared, request_id, Actor::Host,
                             Origin::ScheduledHost, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.request.prepared(bypass)");
        CloseTurn(false, false, "prepared_not_committed");
        return std::string();  // §7.4:prepared 记不住,不发模型
    }
    request_prepared_[request_id] = receipt.event_id;
    return request_id;
}

void TrajectoryBypassBridge::OnRequestSent(const std::string& request_id) {
    const auto it = request_prepared_.find(request_id);
    if (it == request_prepared_.end()) {
        return;
    }
    const auto receipt =
        Put(EventKind::ModelRequestSent, request_id, Actor::Host, Origin::ScheduledHost,
            nlohmann::json{{"prepared_event_id", it->second}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.request.sent(bypass)");
    }
}

void TrajectoryBypassBridge::OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                                             bool reported_by_provider,
                                             const std::string& provider_response_id, int cache_epoch,
                                             bool prefix_append_only, bool cache_reported_by_provider) {
    nlohmann::json payload = nlohmann::json{{"attempt", std::uint64_t{1}},
                                            {"reported_by_provider", reported_by_provider},
                                            {"cache_reported_by_provider", cache_reported_by_provider}};
    if (!provider_response_id.empty()) {
        payload["provider_response_id"] = provider_response_id;
    }
    // 数字只在 provider 明报时才算事实;没报不拿 0 冒充(与主桥同一条)。
    if (reported_by_provider) {
        payload["input_tokens"] = usage.input_tokens;
        payload["cache_read_tokens"] = usage.cache_read_tokens;
        payload["cache_creation_tokens"] = usage.cache_creation_tokens;
        payload["output_tokens"] = usage.output_tokens;
        payload["reasoning_tokens"] = usage.output_reasoning_tokens;
    }
    if (cache_epoch > 0) {
        payload["cache_epoch"] = static_cast<std::uint64_t>(cache_epoch);
        payload["prefix_append_only"] = prefix_append_only;
    }
    const auto receipt = Put(EventKind::ModelUsageRecorded, request_id, Actor::Host,
                             Origin::ScheduledHost, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.usage.recorded(bypass)");
    }
}

bool TrajectoryBypassBridge::OnOutputCompleted(const std::string& request_id, const api::Message& assistant,
                                               const std::string& stop_reason,
                                               const std::string& provider_response_id) {
    nlohmann::json payload = nlohmann::json{{"output_id", NextOutputId()},
                                            {"blocks", MessageToBlocksJson(assistant)},
                                            {"stop_reason", stop_reason.empty() ? "end_turn" : stop_reason}};
    if (!provider_response_id.empty()) {
        payload["provider_response_id"] = provider_response_id;
    }
    const auto receipt = Put(EventKind::ModelOutputCompleted, request_id, Actor::Model,
                             Origin::ProviderModel, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.completed(bypass)");
        CloseTurn(false, false, "output_not_committed");
        return false;
    }
    CloseTurn(true, false, "done");
    return true;
}

void TrajectoryBypassBridge::OnOutputFailed(const std::string& request_id, const std::string& reason) {
    const auto receipt =
        Put(EventKind::ModelOutputFailed, request_id, Actor::Model, Origin::ProviderModel,
            nlohmann::json{{"reason", reason.empty() ? "failed" : reason}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.failed(bypass)");
    }
    CloseTurn(false, false, reason);
}

void TrajectoryBypassBridge::OnOutputCancelled(const std::string& request_id) {
    const auto receipt = Put(EventKind::ModelOutputCancelled, request_id, Actor::Model,
                             Origin::ProviderModel, nlohmann::json{{"reason", "cancelled"}},
                             Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.cancelled(bypass)");
    }
    CloseTurn(false, true, "cancelled");
}

// ---------------------------------------------------------------------------
// P0-4:side-effect 细账 / verification / outcome(§9.3/§5.5)
// ---------------------------------------------------------------------------

namespace {

// 路径比对的规范形:统一正斜杠、去尾斜杠、Windows 大小写不敏感。
std::string NormalizeSubjectPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
#ifdef _WIN32
    for (char& c : path) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
#endif
    return path;
}

}  // namespace

nlohmann::json TrajectoryTurnBridge::BuildSideEffects(const agent::ToolTraceEvent& event,
                                                      const CallBook& book, bool* has_exit_code,
                                                      std::int64_t* exit_code) const {
    *has_exit_code = false;
    *exit_code = 0;
    nlohmann::json effects = nlohmann::json::array();
    const nlohmann::json& args = book.effective_arguments;
    // 入参字段取值的守门:类型不合就如实缺省,不让 value() 抛异常砸了终态
    // 事件的落账(细账是补充事实,不该绑架 canonical 终态)。
    const auto string_of = [&args](const char* key) -> std::string {
        const auto it = args.find(key);
        return it != args.end() && it->is_string() ? it->get<std::string>() : std::string();
    };

    // 文件工具(§9.3):path、preimage hash、postimage hash、undo ref。
    // undo 原文挂在 "text" 键下,超内联上限由 recorder 换成 BlobRef。
    if (!event.undo.path.empty()) {
        nlohmann::json file_effect;
        file_effect["kind"] = "file";
        file_effect["path"] = event.undo.path;
        file_effect["preimage_sha256"] = event.undo.preimage_sha256;
        file_effect["postimage_sha256"] = event.undo.postimage_sha256;
        file_effect["created_new_file"] = event.undo.created_new_file;
        file_effect["undo_ref"] = nlohmann::json{{"path", event.undo.path},
                                                 {"preimage_sha256", event.undo.preimage_sha256},
                                                 {"postimage_sha256", event.undo.postimage_sha256},
                                                 {"created_new_file", event.undo.created_new_file},
                                                 {"preimage_bytes", event.undo.preimage.size()},
                                                 {"text", event.undo.preimage}};
        effects.push_back(std::move(file_effect));
    }

    // 命令工具(§9.3):argv/shell mode、cwd、exit code、timeout/cancel。
    // run_command 合并 stdout/stderr 出一份输出,不拆谎称两流——合并结果
    // 由顶层 result_ref 与 tool.result.committed 承载,这里只记执行形状。
    const std::string command_text = string_of("command");
    if (!command_text.empty()) {
        nlohmann::json command_effect;
        command_effect["kind"] = "command";
        command_effect["command"] = command_text;
        const std::string shell = string_of("shell");
        if (!shell.empty()) {
            command_effect["shell_mode"] = shell;
        }
        const std::string cwd = string_of("cwd");
        if (!cwd.empty()) {
            command_effect["cwd"] = cwd;
        }
        const auto timeout = args.find("timeout_ms");
        if (timeout != args.end() && timeout->is_number_integer()) {
            command_effect["timeout_ms"] = timeout->get<std::int64_t>();
        }
        command_effect["combined_output_ref"] =
            nlohmann::json{{"sha256", event.result_ref.sha256},
                           {"bytes", event.result_ref.bytes}};
        if (event.details.contains("exit_code") && event.details.at("exit_code").is_number_integer()) {
            *has_exit_code = true;
            *exit_code = event.details.at("exit_code").get<std::int64_t>();
            command_effect["exit_code"] = *exit_code;
        }
        effects.push_back(std::move(command_effect));
    }

    // MCP(§9.3):server 身份、effective arguments 在 tool.input.effective、
    // response 由 result_ref 承载、latency 在顶层 duration_ms;这里补
    // jsonrpc 关联与来源档。
    if (event.source_kind == agent::ToolSourceKind::Mcp) {
        nlohmann::json mcp_effect;
        mcp_effect["kind"] = "mcp_call";
        mcp_effect["server"] =
            event.source_instance.empty() ? book.source_instance : event.source_instance;
        if (event.jsonrpc_request_id >= 0) {
            mcp_effect["jsonrpc_request_id"] = event.jsonrpc_request_id;
        }
        effects.push_back(std::move(mcp_effect));
    }
    return effects;
}

void TrajectoryTurnBridge::InvalidateStaleVerifications(const std::string& mutated_path,
                                                        const std::string& invalidated_by_event) {
    const std::string mutated = NormalizeSubjectPath(mutated_path);
    if (mutated.empty()) {
        return;
    }
    for (VerificationBook& book : verifications_) {
        if (!book.recorded || book.invalidated || book.subject.empty()) {
            continue;
        }
        if (NormalizeSubjectPath(book.subject) != mutated) {
            continue;
        }
        const auto receipt =
            Put(EventKind::VerificationInvalidated, std::nullopt, std::nullopt, Actor::Verifier,
                Origin::VerifierHost,
                nlohmann::json{{"verification_id", book.verification_id},
                               {"reason", "subject_modified"},
                               {"invalidated_by_event", invalidated_by_event}},
                Durability::ProcessCrash);
        if (receipt.status == RecordReceipt::Status::Committed) {
            book.invalidated = true;
        } else {
            NoteError(receipt, "verification.invalidated");
        }
    }
}

void TrajectoryTurnBridge::AssessOutcome(bool ok, bool cancelled) {
    // 只引 fresh(fresh=recorded 且未被 invalidated)的验证;没录过验证
    // 的 turn 不落 outcome.assessed(§11.5 的成功门自然把它挡在外面)。
    nlohmann::json evidence_refs = nlohmann::json::array();
    nlohmann::json criteria = nlohmann::json::array();
    bool any_recorded = false;
    for (const VerificationBook& book : verifications_) {
        if (!book.recorded || book.invalidated) {
            continue;
        }
        any_recorded = true;
        evidence_refs.push_back(nlohmann::json{{"verification_id", book.verification_id},
                                               {"event_id", book.recorded_event_id},
                                               {"kind", book.kind},
                                               {"passed", book.passed},
                                               {"fresh", true}});
        criteria.push_back(book.kind);
    }
    if (!any_recorded) {
        return;
    }
    const char* outcome = cancelled ? "cancelled" : (ok ? "succeeded" : "failed");
    const auto receipt =
        Put(EventKind::OutcomeAssessed, std::nullopt, std::nullopt, Actor::Verifier,
            Origin::VerifierHost,
            nlohmann::json{{"outcome", outcome},
                           {"evidence_refs", std::move(evidence_refs)},
                           {"criteria", std::move(criteria)}},
            Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "outcome.assessed");
    }
}

std::string TrajectoryTurnBridge::BeginVerification(const std::string& kind, const std::string& subject,
                                                    const std::string& producer) {
    if (!turn_open_) {
        return std::string();
    }
    VerificationBook book;
    book.verification_id = NextVerificationId();
    book.kind = kind;
    book.subject = subject;
    book.producer = producer;
    // schema 钉死:verification.started 只认 verification_id/kind/subject 三键,
    // producer 留给 recorded(那边是必填)。
    nlohmann::json payload{{"verification_id", book.verification_id}, {"kind", kind}};
    if (!subject.empty()) {
        payload["subject"] = subject;
    }
    const auto started =
        Put(EventKind::VerificationStarted, std::nullopt, std::nullopt, Actor::Verifier,
            Origin::VerifierHost, std::move(payload), Durability::ProcessCrash);
    if (started.status != RecordReceipt::Status::Committed) {
        NoteError(started, "verification.started");
        return std::string();  // §7.4:verification 记不住,不得判 verified
    }
    verifications_.push_back(std::move(book));
    return verifications_.back().verification_id;
}

void TrajectoryTurnBridge::FinishVerification(const std::string& verification_id, bool passed,
                                              const nlohmann::json& facts,
                                              const nlohmann::json& command_ref,
                                              const std::vector<std::string>& artifact_paths) {
    if (!turn_open_) {
        return;
    }
    VerificationBook* target = nullptr;
    for (VerificationBook& book : verifications_) {
        if (book.verification_id == verification_id && !book.recorded) {
            target = &book;
            break;
        }
    }
    if (target == nullptr) {
        return;
    }
    nlohmann::json payload{{"verification_id", verification_id},
                           {"kind", target->kind},
                           {"passed", passed},
                           {"producer", target->producer.empty() ? std::string("host") : target->producer}};
    if (!target->subject.empty()) {
        payload["subject"] = target->subject;
    }
    if (command_ref.is_object() && !command_ref.empty()) {
        payload["command_ref"] = command_ref;
    }
    if (facts.is_object() && !facts.empty()) {
        payload["facts"] = facts;
    }
    nlohmann::json refs = nlohmann::json::array();
    for (const std::string& path : artifact_paths) {
        refs.push_back(nlohmann::json{{"path", path}});
    }
    if (!refs.empty()) {
        payload["artifact_refs"] = std::move(refs);
    }
    payload["observed_after_seq"] = recorder_.next_seq() - 1;
    payload["fresh"] = true;
    const auto receipt = Put(EventKind::VerificationRecorded, std::nullopt, std::nullopt,
                             Actor::Verifier, Origin::VerifierHost, std::move(payload),
                             Durability::ProcessCrash);
    if (receipt.status == RecordReceipt::Status::Committed) {
        target->recorded = true;
        target->passed = passed;
        target->recorded_event_id = receipt.event_id;
    } else {
        NoteError(receipt, "verification.recorded");
    }
}

bool TrajectoryTurnBridge::StorageAvailable() const {
    // 保守门:recorder 坏了/账房路径未知时不放行副作用(§12.2 宁可拒写)。
    return trajectory::HasDiskReserve(recorder_.stream_path().parent_path(),
                                      kJournalEmergencyReserveBytes);
}

// ---------------------------------------------------------------------------
// TrajectorySessionLedger
// ---------------------------------------------------------------------------

struct TrajectorySessionLedger::Impl {
    std::unique_ptr<trajectory::SessionManager> manager;
    std::filesystem::path workspaces_root;  // P0-2:唯一持久化根(查询/管理面用)
    trajectory::ActiveSession* active = nullptr;
    trajectory::RecorderOptions recorder_options;
    // 轮桥/子代理账的默认 training_policy(单发轨迹断档单:单发 Exclude,
    // 交互 Metadata——NewTurnBridge/SpawnSubagent 从这取,不再写死)。
    trajectory::TrainingPolicy training_policy = trajectory::TrainingPolicy::Metadata;
    std::string main_run_id;
    std::string lubancode_version;
    std::string workspace_root_text;  // UTF-8,环境快照与 git 状态取材用
    // 子代理账:run_id -> 终态 hash(Finish 时填,父账边界引用用)。
    std::map<std::string, std::string> child_terminal_hashes;
    std::uint64_t subagent_counter = 0;
    // 测试故障注入(生产恒空;子代理空轨迹单 5.1):子账首枚 run.started
    // 提交前问一次。
    std::function<std::optional<std::string>()> subagent_start_fault;
    // --continue 启动路的 resume 投影(没 resume 为空)。
    bool launch_resumed = false;
    std::vector<api::Message> launch_resume_history;
    // T1 committed wake(§25.4):装配层挂 TelemetryService;默认空。
    telemetry::CommitObserver* telemetry_wake = nullptr;
};

// §12.1 user-only 权限:workspace 层与 session 层目录都收紧;设不住须
// 告警(errors 进 /doctor trajectory 的"最近 I/O 错误"账)。
void HardenLedgerDirectories(const trajectory::TrajectoryDirectory& directory,
                             std::vector<std::string>* errors) {
    if (trajectory::HardenDirectoryUserOnly(directory.workspace_dir())) {
        if (!trajectory::HardenDirectoryUserOnly(directory.session_dir())) {
            errors->push_back("permissions:session_dir_harden_failed");
            platform::LogSink::Instance().Error(
                "trajectory", "session 目录无法收紧为 user-only,敏感内容记录有泄露面");
        }
        return;
    }
    errors->push_back("permissions:workspace_dir_harden_failed");
    platform::LogSink::Instance().Error("trajectory",
                                        "workspace 目录无法收紧为 user-only,敏感内容记录有泄露面");
}

std::expected<TrajectorySessionLedger, std::string> TrajectorySessionLedger::Open(Options options) {
    std::filesystem::path home_dir;
    if (options.workspaces_root.empty()) {
        const auto home = config::HomeLubancodeDir();
        if (!home.has_value()) {
            return std::unexpected("trajectory.no_home: 找不到主目录,会话账无处落");
        }
        home_dir = tools::Utf8ToPath(*home);
        options.workspaces_root = home_dir / "workspaces";
    }
    // P0-1:身份只认装配层递进的冻结 WorkspaceIdentity;空身份才按兜底根
    //(或启动 cwd)现场四级裁决——同仓子目录/linked worktree 不再各立各
    // 的房。子代理与 Gateway 恢复路由调用方显式递身份,不吃这条兜底。
    if (!options.workspace_identity.valid()) {
        std::error_code cwd_ec;
        const std::filesystem::path start = options.workspace_root.empty()
                                                ? std::filesystem::current_path(cwd_ec)
                                                : options.workspace_root;
        if (start.empty()) {
            return std::unexpected("identity.no_boundary: 启动工作目录取不到,身份无从裁决");
        }
        if (home_dir.empty()) {
            const auto home = config::HomeLubancodeDir();
            if (home.has_value()) {
                home_dir = tools::Utf8ToPath(*home);
            }
        }
        auto resolved = workspace::ResolveWorkspaceIdentity(start, home_dir);
        if (resolved.has_value()) {
            options.workspace_identity = std::move(*resolved);
        } else if (!options.workspace_root.empty()) {
            // 显式递了根的旧调用(测试):裁决失败退 cwd_fallback 形状。
            options.workspace_identity = workspace::MakeFallbackIdentity(options.workspace_root);
        } else {
            return std::unexpected(resolved.error());
        }
    }
    options.workspace_root = options.workspace_identity.checkout_root;
    trajectory::SessionManagerOptions manager_options;
    manager_options.workspaces_root = options.workspaces_root;
    manager_options.identity = options.workspace_identity;
    manager_options.workspace_root = options.workspace_root;
    manager_options.launch_cwd = options.launch_cwd;
    manager_options.lubancode_version = options.lubancode_version;
    manager_options.approval_mode = options.approval_mode;
    // 单发轨迹断档单:one_shot 场的 main run 单列 run_kind,manifest 与
    // run.started 同源落 one_shot。
    manager_options.main_run_kind =
        options.one_shot ? trajectory::RunKind::OneShot : trajectory::RunKind::MainSession;
    manager_options.recorder.event_schema_version = options.event_schema_version;
    // 子代理空轨迹单 P0-C:main stream 同样走延迟开卷——正式 .jsonl 由
    // 首枚 run.started 提交事务独占创建,开张失败不在盘上留 0 字节文件。
    manager_options.recorder.defer_stream_create = true;

    Impl impl;
    impl.workspaces_root = options.workspaces_root;
    impl.recorder_options.event_schema_version = options.event_schema_version;
    impl.lubancode_version = options.lubancode_version;
    impl.workspace_root_text = platform::PathToUtf8(options.workspace_root);
    impl.training_policy = options.training_policy;
    impl.subagent_start_fault = options.subagent_start_fault;
    impl.manager = std::make_unique<trajectory::SessionManager>(std::move(manager_options));

    // --continue 启动路(§10.4):直接建 start_reason=resume 的新 session,
    // 不先造空 session。没有可恢复场(或源场验不过)回落普通开张,与旧路
    // --continue 的 quiet_if_none 语义一致;真出错(目录坏了开不出新场)
    // 照旧失败退出,不回退旧写口。
    if (options.resume_at_launch) {
        const std::string latest = impl.manager->LatestResumableSessionId();
        if (!latest.empty()) {
            trajectory::ResumeRequest resume;
            resume.source_session_id = options.resume_source_session_id.empty()
                                           ? latest
                                           : options.resume_source_session_id;
            resume.interactive = false;  // 启动路没有旧 requested 可指
            const auto resumed = impl.manager->ResumeAsNew(resume);
            if (resumed.error_code.empty()) {
                impl.active = impl.manager->active();
                impl.main_run_id = impl.active->manifest.main_run_id;
                impl.launch_resumed = true;
                impl.launch_resume_history = ProjectHistoryFromReplay(
                    [&resumed] {
                        trajectory::ReplayState projection;
                        projection.effective_conversation = resumed.effective_conversation;
                        return projection;
                    }());
                TrajectorySessionLedger ledger;
                ledger.impl_ = std::make_unique<Impl>(std::move(impl));
                HardenLedgerDirectories(ledger.impl_->active->directory, &ledger.io_errors_);
                return ledger;
            }
            // resume 失败回落普通开张:源场坏不拦人开新会话(明错留给
            // /doctor trajectory 查),与旧路 --continue 找不到档不报错同门。
        }
    }
    auto active = impl.manager->LaunchSession();
    if (!active.has_value()) {
        return std::unexpected("trajectory.launch_failed: " + active.error());
    }
    impl.active = *active;
    impl.main_run_id = impl.active->manifest.main_run_id;

    TrajectorySessionLedger ledger;
    ledger.impl_ = std::make_unique<Impl>(std::move(impl));
    HardenLedgerDirectories(ledger.impl_->active->directory, &ledger.io_errors_);
    return ledger;
}

TrajectorySessionLedger::TrajectorySessionLedger(TrajectorySessionLedger&&) noexcept = default;
TrajectorySessionLedger::~TrajectorySessionLedger() = default;

trajectory::TrajectoryRecorder* TrajectorySessionLedger::main() {
    return impl_ != nullptr && impl_->active != nullptr && impl_->active->main.has_value()
               ? &*impl_->active->main
               : nullptr;
}

std::unique_ptr<TrajectoryTurnBridge> TrajectorySessionLedger::NewTurnBridge(
    TrajectoryTurnBridge::Identity identity) {
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return nullptr;
    }
    trajectory::EventScope scope = impl_->active->main->base_scope();
    scope.visibility = {Visibility::HostOnly};
    scope.training_policy = impl_->training_policy;
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder, std::move(scope),
                                                         std::move(identity));
    // 桥按轮把落账错误推进账本的共享环(/doctor trajectory 从这读)。
    bridge->SetErrorSink(&io_errors_);
    // T1 committed wake:挂上后 main stream 每笔提交都投 wake(默认空)。
    if (impl_ != nullptr && impl_->telemetry_wake != nullptr) {
        bridge->SetCommitWake(impl_->telemetry_wake, "main.jsonl");
    }
    return bridge;
}

std::unique_ptr<TrajectoryBypassBridge> TrajectorySessionLedger::NewBypassBridge(
    TrajectoryTurnBridge::Identity identity) {
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr || impl_ == nullptr || impl_->active == nullptr) {
        return nullptr;
    }
    trajectory::EventScope scope = impl_->active->main->base_scope();
    scope.visibility = {Visibility::HostOnly};
    scope.training_policy = TrainingPolicy::Metadata;
    auto bridge =
        std::make_unique<TrajectoryBypassBridge>(*recorder, std::move(scope), std::move(identity));
    if (impl_->telemetry_wake != nullptr) {
        bridge->SetCommitWake(impl_->telemetry_wake, "main.jsonl");
    }
    return bridge;
}

void TrajectorySessionLedger::SetTelemetryWake(telemetry::CommitObserver* wake) {
    if (impl_ != nullptr) {
        impl_->telemetry_wake = wake;
    }
}

void TrajectorySessionLedger::NotifyCommitted_() const {
    if (impl_ == nullptr || impl_->telemetry_wake == nullptr || impl_->active == nullptr) {
        return;
    }
    telemetry::CommitWake wake;
    const trajectory::EventScope& scope = impl_->active->main->base_scope();
    wake.workspace_key = scope.workspace_key;
    wake.session_id = scope.session_id;
    wake.stream_id = "main.jsonl";
    impl_->telemetry_wake->Notify(wake);
}

namespace {

// 子代理桥的具体实现:持独立 recorder,Finish 落 run 终态并关柄。
class SubagentBridgeImpl : public TrajectorySubagentBridge {
public:
    SubagentBridgeImpl(std::unique_ptr<trajectory::TrajectoryRecorder> recorder,
                       std::unique_ptr<TrajectoryTurnBridge> bridge, std::string run_id,
                       std::map<std::string, std::string>* terminal_hashes)
        : recorder_(std::move(recorder)), bridge_(std::move(bridge)), run_id_(std::move(run_id)),
          terminal_hashes_(terminal_hashes) {}

    const std::string& run_id() const override { return run_id_; }
    TrajectoryTurnBridge& turn_bridge() override { return *bridge_; }

    std::string Finish(bool ok, const std::string& reason) override {
        if (finished_) {
            return terminal_hash_;
        }
        finished_ = true;
        const auto receipt = recorder_->FinishRun(
            ok ? trajectory::EventKind::RunCompleted : trajectory::EventKind::RunFailed, reason,
            trajectory::Durability::PowerLoss);
        if (receipt.status == trajectory::RecordReceipt::Status::Committed) {
            terminal_hash_ = receipt.event_hash;
        } else {
            terminal_hash_.clear();
        }
        if (terminal_hashes_ != nullptr) {
            (*terminal_hashes_)[run_id_] = terminal_hash_;
        }
        (void)recorder_->Close();
        return terminal_hash_;
    }

private:
    std::unique_ptr<trajectory::TrajectoryRecorder> recorder_;
    std::unique_ptr<TrajectoryTurnBridge> bridge_;
    std::string run_id_;
    std::map<std::string, std::string>* terminal_hashes_;
    std::string terminal_hash_;
    bool finished_ = false;
};

}  // namespace

std::expected<std::unique_ptr<TrajectorySubagentBridge>, SubagentSpawnFailure>
TrajectorySessionLedger::SpawnSubagent(const std::string& parent_call_id, const std::string& task_label,
                                       const std::string& parent_run_id) {
    if (impl_ == nullptr || impl_->active == nullptr) {
        SubagentSpawnFailure failure;
        failure.stage = "reserve_stream";
        failure.error_code = "trajectory.no_active_session";
        failure.detail = "会话账未开,子账无处落";
        return std::unexpected(std::move(failure));
    }
    const std::string agent_run_id =
        "agent-" + std::to_string(++impl_->subagent_counter) + "-" + impl_->main_run_id;
    SubagentSpawnFailure failure;
    failure.reserved_run_id = agent_run_id;
    // 本轮失败共同收尾:recorder 先放干净(Windows 攥着句柄删不掉文件),
    // 再按所有权凭据清未提交的 0 字节残留(路径=本次预留名、大小=0)。
    const auto fail_out = [this, &failure](std::string stage, std::string code, std::string detail,
                                           bool retryable) {
        failure.stage = std::move(stage);
        failure.error_code = std::move(code);
        failure.detail = std::move(detail);
        failure.retryable = retryable;
        if (impl_ != nullptr && impl_->active != nullptr) {
            const auto stream_path = impl_->active->directory.ReserveSubagentStream(failure.reserved_run_id);
            if (stream_path.has_value()) {
                (void)trajectory::DiscardUncommittedStream(*stream_path);
            }
        }
        io_errors_.push_back("subagent.start_failed:" + failure.stage + ":" + failure.error_code);
        platform::LogSink::Instance().Error(
            "trajectory", "子账开张失败[" + failure.stage + "]: " + failure.error_code +
                              (failure.detail.empty() ? std::string() : " (" + failure.detail + ")"));
        return std::unexpected(std::move(failure));
    };
    auto stream = impl_->active->directory.ReserveSubagentStream(agent_run_id);
    if (!stream.has_value()) {
        return fail_out("reserve_stream", "trajectory.subagent_stream", stream.error(),
                        /*retryable=*/false);
    }
    trajectory::EventScope scope = impl_->active->main->base_scope();
    scope.run_id = agent_run_id;
    scope.run_kind = trajectory::RunKind::Subagent;
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    scope.visibility = {Visibility::HostOnly};
    scope.training_policy = impl_->training_policy;
    // P0-C:子账走延迟开卷——正式 .jsonl 在首枚 run.started 提交事务里独占
    // 创建;开不成/写不进都不会留下 0 字节正式 stream。
    trajectory::RecorderOptions recorder_options = impl_->recorder_options;
    recorder_options.defer_stream_create = true;
    if (impl_->subagent_start_fault != nullptr) {
        recorder_options.inject_submit_reject = [hook = impl_->subagent_start_fault](
                                                     trajectory::EventKind kind)
                                                     -> std::optional<std::string> {
            if (kind == trajectory::EventKind::RunStarted && hook != nullptr) {
                return hook();
            }
            return std::nullopt;
        };
    }
    auto recorder = trajectory::TrajectoryRecorder::Start(*stream, impl_->active->directory.artifacts_root(),
                                                          scope, std::move(recorder_options));
    if (!recorder.has_value()) {
        return fail_out("recorder_start", "trajectory.subagent_recorder", recorder.error(),
                        /*retryable=*/false);
    }
    // 先把 recorder 落到堆上再让桥引用它——expected 里的值 move 走之后,
    // 引用会悬在 moved-from 壳上(桥的 recorder_ 是裸引用)。
    auto recorder_owner = std::make_unique<trajectory::TrajectoryRecorder>(std::move(*recorder));
    // run.started:父子边界(§3.5/§6.3)——relations 带 parent_run_id 与
    // parent_call_id,正文只有任务标签,不带父会话细账。
    nlohmann::json payload = nlohmann::json{{"run_kind", "subagent"},
                                            {"start_reason", "agent_tool_dispatch"},
                                            {"writer_version", impl_->recorder_options.recorder_version},
                                            {"min_reader_version", std::uint64_t{1}}};
    if (!task_label.empty()) {
        payload["task_ref"] = task_label;
    }
    trajectory::EventLinks links;
    // 嵌套轨迹边(递归派工单 P1-2):parent_run_id 非空 = 嵌套派工——它的
    // 父亲是派出它的那只子代理自己的 run,不是 main;空串(main 直派)按
    // 从前行为落回本场 main_run_id。
    links.parent_run_id = parent_run_id.empty() ? impl_->main_run_id : parent_run_id;
    if (!parent_call_id.empty()) {
        links.parent_call_id = parent_call_id;
    }
    const auto started = recorder_owner->WriteRunStarted(std::move(payload),
                                                          trajectory::Durability::PowerLoss,
                                                          std::move(links));
    if (started.status != trajectory::RecordReceipt::Status::Committed) {
        const bool io_failure = started.status == trajectory::RecordReceipt::Status::IoFailed;
        // recorder 攥着已开卷的句柄:先放掉再清残留。
        recorder_owner.reset();
        return fail_out("run_started",
                        "trajectory.subagent_run_started: " + started.error_code,
                        started.error_message, /*retryable=*/io_failure);
    }

    TrajectoryTurnBridge::Identity identity;
    identity.provider = "subagent";
    identity.wire = "subagent";
    identity.channel = "subagent";
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder_owner, scope, identity);
    // T1 committed wake:子账 stream 也投(subagents/<run>.jsonl,§14.3 多
    // stream 各自 cursor)。
    if (impl_->telemetry_wake != nullptr) {
        bridge->SetCommitWake(impl_->telemetry_wake,
                              "subagents/" + std::filesystem::path(*stream).filename().generic_string());
    }
    return std::unique_ptr<TrajectorySubagentBridge>(new SubagentBridgeImpl(
        std::move(recorder_owner), std::move(bridge), agent_run_id, &impl_->child_terminal_hashes));
}

void TrajectorySessionLedger::NoteSubagentStartFailed(const SubagentSpawnFailure& failure,
                                                      const std::string& parent_run_id,
                                                      const std::string& parent_call_id,
                                                      const std::string& turn_id) {
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;  // main 都没了,诊断只能进 io_errors(fail_out 那侧已记)
    }
    // 持有者与 SpawnSubagent 的 relations 同一口径:main 直派(空串)记
    // main_run_id;嵌套派工记派工者自己的 run。
    const std::string owner_run_id =
        parent_run_id.empty() ? impl_->main_run_id : parent_run_id;
    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::SubagentRunStartFailed;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Tool;
    request.scope.origin = trajectory::Origin::SubagentTool;
    request.scope.visibility = {Visibility::HostOnly};
    // 子账开张失败是宿主侧诊断事实,不进训练集。
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.scope.turn_id = turn_id.empty() ? std::optional<std::string>() : std::optional<std::string>(turn_id);
    request.scope.call_id =
        parent_call_id.empty() ? std::optional<std::string>() : std::optional<std::string>(parent_call_id);
    nlohmann::json payload = nlohmann::json{{"stage", failure.stage},
                                            {"error_code", failure.error_code},
                                            {"parent_run_id", owner_run_id}};
    if (!failure.detail.empty()) {
        // 事件只记稳定码与引用,不抄敏感绝对路径:io 细节里的会话目录
        // 原文换占位符。
        std::string detail = failure.detail;
        if (impl_ != nullptr && impl_->active != nullptr) {
            const std::string session_root =
                platform::PathToUtf8(impl_->active->directory.session_dir());
            const auto pos = detail.find(session_root);
            if (pos != std::string::npos) {
                detail.replace(pos, session_root.size(), "<session_dir>");
            }
        }
        payload["detail"] = std::move(detail);
    }
    if (!parent_call_id.empty()) {
        payload["parent_call_id"] = parent_call_id;
    }
    if (!failure.reserved_run_id.empty()) {
        payload["reserved_run_id"] = failure.reserved_run_id;
    }
    // stream_ref:session 相对引用(subagents/<file>.jsonl),不写绝对路径。
    if (!failure.reserved_run_id.empty() && impl_ != nullptr && impl_->active != nullptr) {
        payload["stream_ref"] = "subagents/" + failure.reserved_run_id + ".jsonl";
    }
    payload["retryable"] = failure.retryable;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::PowerLoss);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        const std::string note =
            "subagent.run.start_failed:" + receipt.error_code +
            (receipt.error_message.empty() ? std::string() : " (" + receipt.error_message + ")");
        io_errors_.push_back(note);
        platform::LogSink::Instance().Error("trajectory", "子账开张失败事件落不了: " + note);
        return;
    }
    NotifyCommitted_();
}

std::optional<std::string> TrajectorySessionLedger::ChildTerminalHash(const std::string& agent_run_id) const {
    const auto it = impl_->child_terminal_hashes.find(agent_run_id);
    if (it == impl_->child_terminal_hashes.end()) {
        return std::nullopt;
    }
    return it->second;
}

trajectory::CloseOutcome TrajectorySessionLedger::CloseSession(const std::string& reason) {
    trajectory::CloseRequest request;
    request.reason = reason;
    trajectory::NullClearParticipant participant;
    return impl_->manager->Close(request, &participant);
}

TrajectorySessionLedger::CwdChangeResult TrajectorySessionLedger::HandleCwdChange(
    const workspace::WorkspaceIdentity& new_identity) {
    CwdChangeResult result;
    result.workspace_key = new_identity.workspace_key;
    if (impl_ == nullptr || impl_->manager == nullptr || impl_->active == nullptr) {
        result.error = "trajectory.open_failed: 会话账未开,cwd 变化无处对账";
        return result;
    }
    if (new_identity.workspace_key != impl_->manager->workspace_key()) {
        // 跨 workspace:账一个字不写,交调用方封场换账(§4.5)。
        return result;
    }
    // 同 workspace:cwd.changed 事件 + 检出登记(worktree 进出房各记一笔)。
    PutControl_(trajectory::EventKind::ControlCwdChanged,
                nlohmann::json{{"cwd", platform::PathToUtf8(new_identity.launch_cwd)}});
    if (const auto touched = impl_->manager->RegisterCheckout(new_identity); !touched.has_value()) {
        result.error = touched.error();
    }
    result.same_workspace = true;
    return result;
}

// ---------------------------------------------------------------------------
// P0-3:clear 八步 / resume-as-new / replay 读口
// ---------------------------------------------------------------------------

std::vector<api::Message> ProjectHistoryFromReplay(const trajectory::ReplayState& state) {
    std::vector<api::Message> history;
    for (const auto& message : state.effective_conversation) {
        api::Message projected;
        switch (message.role) {
            case trajectory::ReplayMessage::Role::User:
                projected.role = api::Role::User;
                break;
            case trajectory::ReplayMessage::Role::Assistant:
                projected.role = api::Role::Assistant;
                break;
            case trajectory::ReplayMessage::Role::Tool:
                projected.role = api::Role::User;  // ToolResult 以 user 消息携带(与 hub 回喂同形)
                break;
        }
        if (!message.blocks.is_array()) {
            continue;
        }
        if (message.role == trajectory::ReplayMessage::Role::Tool) {
            // ToolResult:content blocks -> ToolResultBlock,call_id 配对(§11.2)。
            api::ToolResultBlock result;
            result.tool_use_id = message.call_id.value_or(std::string());
            for (const auto& block : message.blocks) {
                if (block.is_object() && block.value("type", std::string()) == "text" &&
                    block.contains("text")) {
                    result.content = block["text"].get<std::string>();
                    break;
                }
            }
            projected.content.push_back(std::move(result));
            history.push_back(std::move(projected));
            continue;
        }
        for (const auto& block : message.blocks) {
            if (!block.is_object()) {
                continue;
            }
            const std::string type = block.value("type", std::string());
            if (type == "text" && block.contains("text")) {
                api::TextBlock text;
                text.text = block["text"].get<std::string>();
                projected.content.push_back(std::move(text));
            } else if (type == "thinking" && block.contains("text")) {
                api::ThinkingBlock thinking;
                thinking.text = block["text"].get<std::string>();
                projected.content.push_back(std::move(thinking));
            } else if (type == "tool_call" && block.contains("call_id") && block.contains("name")) {
                api::ToolUseBlock call;
                call.id = block["call_id"].get<std::string>();
                call.name = block["name"].get<std::string>();
                if (block.contains("arguments")) {
                    call.input = block["arguments"];
                }
                projected.content.push_back(std::move(call));
            }
        }
        history.push_back(std::move(projected));
    }
    return history;
}

trajectory::ClearOutcome TrajectorySessionLedger::ClearSession(
    const trajectory::ClearRequest& request, trajectory::ClearParticipant* participant) {
    if (impl_ == nullptr || impl_->manager == nullptr) {
        trajectory::ClearOutcome outcome;
        outcome.error_code = "clear.no_active_session";
        return outcome;
    }
    const auto outcome = impl_->manager->Clear(request, participant);
    if (outcome.error_code.empty()) {
        // 账本跟着换场:active 指针(manager 内 std::optional 同址换值)、
        // run 号、选段器重置(新场不带旧 selection,§3.3.1)。
        impl_->active = impl_->manager->active();
        if (impl_->active != nullptr) {
            impl_->main_run_id = impl_->active->manifest.main_run_id;
        }
        impl_->child_terminal_hashes.clear();
        record_selection_ = nullptr;  // 惰性重建(RecordSelectionController)
        environment_captured_ = false;  // 新 run 须重采环境快照
    }
    return outcome;
}

TrajectoryResumeSummary TrajectorySessionLedger::ResumeInteractive(const std::string& source_session_id,
                                                                   const std::string& command_name) {
    TrajectoryResumeSummary summary;
    if (impl_ == nullptr || impl_->manager == nullptr) {
        summary.outcome.error_code = "resume.no_ledger";
        summary.outcome.message = "轨迹账本没开";
        return summary;
    }
    trajectory::SessionManager& manager = *impl_->manager;
    const bool has_active = manager.active() != nullptr;
    trajectory::ResumeRequest request;
    request.source_session_id = source_session_id;
    request.interactive = has_active;  // 交互路:有旧场才有跨 session requested 可指
    request.user_initiated = true;

    // 旧场(若有):requested 先 durable,随后 switch_to_resume 封口
    //(§10.4/§14.1 的 clear/resume 例外:旧 main 写 requested 与 terminal)。
    if (has_active) {
        const std::string command_id = "cmd-" + std::to_string(++command_counter_);
        const std::string boundary_operation_id =
            impl_->active->session_id() + ":resume";  // 稳定可追,不落随机
        auto* old_main = impl_->active->main.has_value() ? &*impl_->active->main : nullptr;
        std::string requested_event_id;
        if (old_main != nullptr) {
            trajectory::RecordRequest requested;
            requested.kind = trajectory::EventKind::ControlCommandRequested;
            requested.scope = old_main->base_scope();
            requested.scope.actor = trajectory::Actor::User;
            requested.scope.origin = trajectory::Origin::ExternalUser;
            requested.scope.visibility = {trajectory::Visibility::HostOnly};
            requested.scope.training_policy = trajectory::TrainingPolicy::Exclude;
            requested.payload["command_id"] = command_id;
            requested.payload["command_name"] = command_name;
            requested.payload["action_name"] = command_name;
            requested.payload["effect_class"] = "session_boundary";
            requested.payload["args_ref"] =
                nlohmann::json{{"source_session_id", source_session_id},
                               {"boundary_operation_id", boundary_operation_id}};
            requested.links.correlation_id = boundary_operation_id;
            const auto receipt = old_main->Record(requested, trajectory::Durability::PowerLoss);
            if (receipt.status == trajectory::RecordReceipt::Status::Committed) {
                requested_event_id = receipt.event_id;
            }
        }
        trajectory::CloseRequest close;
        close.reason = "switch_to_resume";
        trajectory::NullClearParticipant participant;
        const auto closed = manager.Close(close, &participant);
        if (!closed.error_code.empty()) {
            summary.outcome.error_code = "resume." + closed.error_code;
            summary.outcome.message = "封旧场失败: " + closed.message;
            return summary;
        }
        request.previous_session_id = closed.session_id;
        if (!requested_event_id.empty()) {
            request.boundary_command.command_id = command_id;
            request.boundary_command.requested_session_id = closed.session_id;
            request.boundary_command.requested_event_id = requested_event_id;
            request.boundary_command.boundary_operation_id = boundary_operation_id;
        } else {
            // requested 落不住(旧账坏):不硬造跨 session 生命周期,按
            // 启动路口径办(§14.1 的开口只在 requested 真落了才走)。
            request.interactive = false;
        }
    }
    summary.outcome = manager.ResumeAsNew(request);
    if (!summary.outcome.error_code.empty()) {
        return summary;
    }
    // 换场成功:账本指到新场,选段器重置,history 折叠投影交出去。
    impl_->active = manager.active();
    if (impl_->active != nullptr) {
        impl_->main_run_id = impl_->active->manifest.main_run_id;
    }
    impl_->child_terminal_hashes.clear();
    record_selection_ = nullptr;
    environment_captured_ = false;
    trajectory::ReplayState projection_state;
    // 投影只需要 effective conversation;从 outcome 的引用直接翻。
    projection_state.effective_conversation = summary.outcome.effective_conversation;
    summary.history = ProjectHistoryFromReplay(projection_state);
    return summary;
}

std::string TrajectorySessionLedger::LatestResumableSessionId() const {
    return impl_ != nullptr && impl_->manager != nullptr
               ? impl_->manager->LatestResumableSessionId()
               : std::string();
}

bool TrajectorySessionLedger::resumed_at_launch() const {
    return impl_ != nullptr && impl_->launch_resumed;
}

std::vector<api::Message> TrajectorySessionLedger::LaunchResumeHistory() const {
    return impl_ != nullptr ? impl_->launch_resume_history : std::vector<api::Message>();
}

trajectory::ReplayReport TrajectorySessionLedger::FoldMainReplay() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        trajectory::ReplayReport report;
        report.error_code = "replay.no_active_session";
        return report;
    }
    return trajectory::FoldStreamReplay(impl_->active->directory.main_stream_path());
}

trajectory::SessionVerifyReport TrajectorySessionLedger::VerifySession() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        trajectory::SessionVerifyReport report;
        report.error_code = "verify.no_active_session";
        return report;
    }
    return trajectory::VerifySessionDir(impl_->active->directory.session_dir());
}

TrajectorySessionLedger::ExactReplay TrajectorySessionLedger::ExactReplayMain() const {
    ExactReplay replay;
    const auto fold = FoldMainReplay();
    if (!fold.ok()) {
        replay.error_code = fold.error_code;
        return replay;
    }
    replay.ok = true;
    replay.state_hash = trajectory::ComputeReplayStateHash(fold.state);
    replay.state = std::move(fold.state);
    return replay;
}

RecordSelectionController& TrajectorySessionLedger::record_selection() {
    if (record_selection_ == nullptr) {
        record_selection_ = std::make_unique<RecordSelectionController>(*this);
    }
    return *record_selection_;
}

// 会话级控制事件(compact/record 一族)的公共落账口。
void TrajectorySessionLedger::PutControl_(trajectory::EventKind kind, nlohmann::json payload) {
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;
    }
    trajectory::RecordRequest request;
    request.kind = kind;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Host;
    request.scope.origin = trajectory::Origin::CompactRuntime;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        platform::LogSink::Instance().Error(
            "trajectory", std::string("control 落账失败: ") + receipt.error_code);
        return;
    }
    NotifyCommitted_();
}

void TrajectorySessionLedger::RecordCompactRequested(const std::string& trigger, int old_epoch,
                                                     const std::string& input_state_hash) {
    nlohmann::json payload = nlohmann::json{{"trigger", trigger}};
    if (old_epoch > 0) {
        payload["old_epoch"] = static_cast<std::uint64_t>(old_epoch);
    }
    if (!input_state_hash.empty()) {
        payload["input_state_hash"] = input_state_hash;
    }
    PutControl_(trajectory::EventKind::CompactRequested, std::move(payload));
}

void TrajectorySessionLedger::RecordCompactApplied(const std::string& old_state_hash,
                                                   const std::string& new_state_hash,
                                                   std::uint64_t pre_tokens, std::uint64_t post_tokens,
                                                   int new_epoch) {
    PutControl_(trajectory::EventKind::CompactApplied,
                nlohmann::json{{"old_state_hash", old_state_hash},
                               {"new_state_hash", new_state_hash},
                               {"source_event_span", nlohmann::json::array({std::uint64_t{1}, SpanEndSeq()})},
                               {"pre_tokens", pre_tokens},
                               {"post_tokens", post_tokens},
                               {"epoch", static_cast<std::uint64_t>(new_epoch)}});
}

void TrajectorySessionLedger::RecordCompactFailed(const std::string& reason) {
    PutControl_(trajectory::EventKind::CompactFailed, nlohmann::json{{"reason", reason}});
}

std::uint64_t TrajectorySessionLedger::SpanEndSeq() {
    trajectory::TrajectoryRecorder* recorder = main();
    return recorder != nullptr ? recorder->next_seq() : 1;
}

void TrajectorySessionLedger::PutUserCommand_(trajectory::EventKind kind, nlohmann::json payload) {
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;
    }
    trajectory::RecordRequest request;
    request.kind = kind;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::User;
    request.scope.origin = trajectory::Origin::ExternalUser;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        platform::LogSink::Instance().Error(
            "trajectory", std::string("command 落账失败: ") + receipt.error_code);
        return;
    }
    NotifyCommitted_();
}

std::string TrajectorySessionLedger::BeginCommand(const std::string& command_name,
                                                  const std::string& action_name,
                                                  const std::string& effect_class) {
    const std::string command_id = "cmd-" + std::to_string(++command_counter_);
    PutUserCommand_(trajectory::EventKind::ControlCommandRequested,
                    nlohmann::json{{"command_id", command_id},
                                   {"command_name", command_name},
                                   {"action_name", action_name},
                                   {"effect_class", effect_class}});
    return command_id;
}

void TrajectorySessionLedger::EndCommand(const std::string& command_id, bool ok,
                                         const std::string& reason) {
    nlohmann::json payload = nlohmann::json{{"command_id", command_id}};
    if (ok) {
        payload["status"] = "ok";
        PutUserCommand_(trajectory::EventKind::ControlCommandCompleted, std::move(payload));
        return;
    }
    payload["reason"] = reason.empty() ? "command_failed" : reason;
    PutUserCommand_(trajectory::EventKind::ControlCommandFailed, std::move(payload));
}

// ---------------------------------------------------------------------------
// P0-4:环境快照 / 排队账 / 容量与存储(§9.1/§5.5/§12.2)
// ---------------------------------------------------------------------------

namespace {

// §9.1 的平台静态材料:os/arch 按 compile target 报,locale/timezone 现读
// (读不出就空串,由 gaps 如实记账,不造假)。
std::string DetectOsName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

std::string DetectArch() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return std::string();
#endif
}

std::string DetectLocale() {
    const char* current = std::setlocale(LC_ALL, nullptr);
    return current != nullptr ? current : std::string();
}

std::string DetectTimezone() {
#if defined(_LIBCPP_VERSION)
    // libc++(macOS 一系)未实现 C++20 tzdb——time_zone 类型都缺,编译期
    // 就炸(CI macos 腿首跑实证)。回落 C 接面:TZ 环境变量优先,否则
    // tzset 后 strftime %Z 拿时区缩写;拿不到空串,gaps 记"不可知",不猜。
    tzset();
    const char* env = std::getenv("TZ");
    if (env != nullptr && *env != '\0') {
        return std::string(env);
    }
    const std::time_t now = std::time(nullptr);
    const std::tm* local = std::localtime(&now);
    if (local != nullptr) {
        char buf[64] = {0};
        if (std::strftime(buf, sizeof(buf), "%Z", local) > 0) {
            return std::string(buf);
        }
    }
    return std::string();
#else
    try {
        const std::chrono::time_zone* zone = std::chrono::current_zone();
        if (zone != nullptr) {
            return std::string(zone->name());
        }
    } catch (...) {
        // 无 tzdata 的机器:空串,gaps 里记"不可知",不猜。
    }
    return std::string();
#endif
}

}  // namespace

std::string TrajectorySessionLedger::CaptureEnvironment(const EnvironmentFacts& facts) {
    if (environment_captured_) {
        return std::string();  // 一场 run 一次,幂等
    }
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return "trajectory.no_recorder";
    }
    trajectory::BlobStore blobs(impl_->active->directory.artifacts_root());
    trajectory::EnvironmentSnapshotInput input;
    input.lubancode_version = impl_->lubancode_version;
    input.os_name = DetectOsName();
    input.arch = DetectArch();
    input.locale = DetectLocale();
    input.timezone = DetectTimezone();
    input.cwd = platform::PathToUtf8(std::filesystem::current_path());
    // §3.2:仓库根从账本开张时的 workspace_root 递进(空 = 启动 cwd,如实
    // 记 in_repo=false 的缺口)。
    input.repository_root = impl_->workspace_root_text;
    input.git = trajectory::GatherGitStatus(impl_->workspace_root_text);
    input.provider = facts.provider;
    input.wire = facts.wire;
    input.model = facts.model;
    input.model_parameters = facts.model_parameters;
    if (!facts.system_prompt.empty()) {
        const auto prompt_ref =
            blobs.Store(facts.system_prompt, "text/markdown", trajectory::Durability::PowerLoss);
        if (prompt_ref.has_value()) {
            input.system_prompt_ref = *prompt_ref;
        }
    }
    input.toolset = facts.toolset;
    input.project_instruction_refs = facts.project_instruction_refs;
    input.loaded_skill_refs = facts.loaded_skill_refs;
    input.plugin_refs = facts.plugin_refs;
    input.config_snapshot_redacted = facts.config_snapshot_redacted;
    input.allowlisted_env = facts.allowlisted_env;

    const auto capture =
        trajectory::BuildEnvironmentCapturePayload(input, blobs, trajectory::Durability::PowerLoss);
    if (!capture.has_value()) {
        const std::string error = capture.error();
        io_errors_.push_back(error);
        platform::LogSink::Instance().Error("trajectory", "环境快照落盘失败: " + error);
        return error;
    }
    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::RunEnvironmentCaptured;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Host;
    request.scope.origin = trajectory::Origin::RecoveryRuntime;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Metadata;
    request.payload = capture->event_payload;
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::PowerLoss);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        io_errors_.push_back("run.environment.captured:" + receipt.error_code);
        return receipt.error_code;
    }
    environment_captured_ = true;
    return std::string();
}

void TrajectorySessionLedger::NoteQueueEnqueued(const std::string& item_id,
                                                const std::string& target_label,
                                                const std::string& reason) {
    nlohmann::json payload{{"item_id", item_id}, {"input_id", item_id}};
    if (!target_label.empty()) {
        payload["enqueue_reason"] = target_label;
    } else if (!reason.empty()) {
        payload["enqueue_reason"] = reason;
    }
    PutUserCommand_(trajectory::EventKind::ControlQueueItemEnqueued, std::move(payload));
}

void TrajectorySessionLedger::NoteQueueDequeued(const std::string& item_id, const std::string& reason) {
    // dequeue 是宿主泵的活(§5.5:宿主落下状态变更,actor=host),不冒充
    // 用户动作。
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;
    }
    nlohmann::json payload{{"item_id", item_id}, {"input_id", item_id}};
    if (!reason.empty()) {
        payload["reason"] = reason;
    }
    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::ControlQueueItemDequeued;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Host;
    request.scope.origin = trajectory::Origin::ScheduledHost;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        io_errors_.push_back("control.queue.item.dequeued:" + receipt.error_code);
    }
}

void TrajectorySessionLedger::NoteQueueCancelled(const std::string& item_id, const std::string& reason) {
    PutUserCommand_(trajectory::EventKind::ControlQueueItemCancelled,
                    nlohmann::json{{"item_id", item_id},
                                   {"reason", reason.empty() ? "user_removed" : reason}});
}

void TrajectorySessionLedger::NoteQueueExpired(const std::string& item_id, const std::string& reason) {
    // 过期也是宿主判的(泵的防死循环闸),actor=host。
    trajectory::TrajectoryRecorder* recorder = main();
    if (recorder == nullptr) {
        return;
    }
    nlohmann::json payload{{"item_id", item_id}};
    if (!reason.empty()) {
        payload["reason"] = reason;
    }
    trajectory::RecordRequest request;
    request.kind = trajectory::EventKind::ControlQueueItemExpired;
    request.scope = recorder->base_scope();
    request.scope.actor = trajectory::Actor::Host;
    request.scope.origin = trajectory::Origin::ScheduledHost;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        io_errors_.push_back("control.queue.item.expired:" + receipt.error_code);
    }
}

bool TrajectorySessionLedger::StorageAvailable() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        return false;
    }
    return trajectory::HasDiskReserve(impl_->active->directory.session_dir(),
                                      kJournalEmergencyReserveBytes);
}

trajectory::WorkspaceUsageReport TrajectorySessionLedger::WorkspaceUsage() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        return trajectory::WorkspaceUsageReport{};
    }
    const std::filesystem::path workspace_dir = impl_->active->directory.workspace_dir();
    return trajectory::ScanWorkspaceUsage(workspace_dir / "sessions",
                                          workspace_dir.filename().string());
}

trajectory::WorkspaceDoctorReport TrajectorySessionLedger::BuildDoctorReport() const {
    if (impl_ == nullptr || impl_->active == nullptr) {
        return trajectory::WorkspaceDoctorReport{};
    }
    const trajectory::TrajectoryDirectory& directory = impl_->active->directory;
    return trajectory::BuildWorkspaceDoctorReport(
        directory.workspace_dir().parent_path(), directory.workspace_dir(),
        directory.workspace_dir().filename().string(), impl_->active->session_id(), io_errors_);
}

std::vector<std::string> TrajectorySessionLedger::recent_io_errors() const {
    return io_errors_;
}

// ---------------------------------------------------------------------------
// RecordSelectionController
// ---------------------------------------------------------------------------

RecordSelectionController::RecordSelectionController(TrajectorySessionLedger& ledger) : ledger_(ledger) {}

std::string RecordSelectionController::Put_(trajectory::EventKind kind, nlohmann::json payload) {
    trajectory::TrajectoryRecorder* recorder = ledger_.main();
    if (recorder == nullptr) {
        return "trajectory.no_recorder";
    }
    trajectory::RecordRequest request;
    request.kind = kind;
    request.scope = recorder->base_scope();
    // 真人敲 slash:actor=user/origin=external_user(§5.5);annotation
    // 不冒充 conversation user(training_policy=exclude)。
    request.scope.actor = trajectory::Actor::User;
    request.scope.origin = trajectory::Origin::ExternalUser;
    request.scope.visibility = {trajectory::Visibility::HostOnly};
    request.scope.training_policy = trajectory::TrainingPolicy::Exclude;
    request.payload = std::move(payload);
    const auto receipt = recorder->Record(std::move(request), trajectory::Durability::ProcessCrash);
    if (receipt.status != trajectory::RecordReceipt::Status::Committed) {
        return receipt.error_code;
    }
    if (kind == trajectory::EventKind::RecordSelectionStarted) {
        start_event_hash_ = receipt.event_hash;
    }
    return std::string();
}

std::string RecordSelectionController::Start(const std::string& name, const std::string& goal,
                                             const std::vector<std::string>& variables,
                                             const std::string& acceptance) {
    if (active()) {
        return "record.already_active";
    }
    record_id_ = "record-" + std::to_string(++selection_counter_);
    paused_ = false;
    trajectory::TrajectoryRecorder* recorder = ledger_.main();
    nlohmann::json payload = nlohmann::json{{"record_id", record_id_},
                                            {"scope", "causal_tree"}};
    if (!name.empty()) {
        payload["goal"] = name;  // 名字即选段标注(§14.3:annotation,不进对话)
    }
    if (!goal.empty()) {
        payload["goal"] = goal;
    }
    if (!variables.empty()) {
        payload["variables"] = variables;
    }
    if (!acceptance.empty()) {
        payload["acceptance"] = acceptance;
    }
    if (recorder != nullptr && !recorder->last_event_hash().empty()) {
        payload["start_event_ref"] = nlohmann::json{{"event_hash", recorder->last_event_hash()}};
    }
    return Put_(trajectory::EventKind::RecordSelectionStarted, std::move(payload));
}

std::string RecordSelectionController::Pause() {
    if (!active() || paused_) {
        return "record.not_active";
    }
    const std::string error =
        Put_(trajectory::EventKind::RecordSelectionPaused, nlohmann::json{{"record_id", record_id_}});
    if (error.empty()) {
        paused_ = true;
    }
    return error;
}

std::string RecordSelectionController::Resume() {
    if (!active() || !paused_) {
        return "record.not_active";
    }
    const std::string error =
        Put_(trajectory::EventKind::RecordSelectionResumed, nlohmann::json{{"record_id", record_id_}});
    if (error.empty()) {
        paused_ = false;
    }
    return error;
}

std::string RecordSelectionController::Note(const std::string& text) {
    if (!active()) {
        return "record.not_active";
    }
    nlohmann::json note;
    note["text"] = text;
    return Put_(trajectory::EventKind::RecordSelectionNoteAdded,
                nlohmann::json{{"record_id", record_id_}, {"note_ref", std::move(note)}});
}

std::string RecordSelectionController::Stop(const std::string& verification) {
    if (!active()) {
        return "record.not_active";
    }
    trajectory::TrajectoryRecorder* recorder = ledger_.main();
    nlohmann::json payload = nlohmann::json{{"record_id", record_id_}};
    // 选段只圈 canonical 事件段:起点 hash 到当前末 hash 的整段(暂停区间
    // 仍在 Journal,不造事实缺口,§14.3)。
    payload["included_spans"] = nlohmann::json::array({nlohmann::json{
        {"start_event_hash", start_event_hash_},
        {"end_event_hash", recorder != nullptr ? recorder->last_event_hash() : std::string()}}});
    if (recorder != nullptr && !recorder->last_event_hash().empty()) {
        payload["source_terminal_hashes"] = nlohmann::json::array({recorder->last_event_hash()});
    }
    if (!verification.empty()) {
        // 口述"已验过"只是 claimed verification,不是 fresh evidence(§14.3)。
        payload["claimed_verification"] = verification;
    }
    const std::string error =
        Put_(trajectory::EventKind::RecordSelectionCompleted, std::move(payload));
    if (error.empty()) {
        record_id_.clear();
        paused_ = false;
    }
    return error;
}

std::string RecordSelectionController::Cancel() {
    if (!active()) {
        return "record.not_active";
    }
    const std::string error = Put_(trajectory::EventKind::RecordSelectionCancelled,
                                   nlohmann::json{{"record_id", record_id_}, {"reason", "user_cancel"}});
    if (error.empty()) {
        record_id_.clear();
        paused_ = false;
    }
    return error;
}

const std::string& TrajectorySessionLedger::session_id() const {
    static const std::string empty;
    return impl_ != nullptr && impl_->active != nullptr ? impl_->active->session_id() : empty;
}

std::filesystem::path TrajectorySessionLedger::session_dir() const {
    static const std::filesystem::path empty;
    // ActiveSession::session_dir() 按值回(const ref 会接到临时上)。
    return impl_ != nullptr && impl_->active != nullptr ? impl_->active->session_dir() : empty;
}

std::string TrajectorySessionLedger::workspace_key() const {
    return impl_ != nullptr && impl_->active != nullptr &&
                   impl_->active->main.has_value()
               ? impl_->active->main->base_scope().workspace_key
               : std::string();
}

// ---------------------------------------------------------------------------
// P0-2:会话读面与 workspace 管理面(命令/app-server 共用)
// ---------------------------------------------------------------------------

std::filesystem::path TrajectorySessionLedger::workspaces_root() const {
    static const std::filesystem::path empty;
    return impl_ != nullptr ? impl_->workspaces_root : empty;
}

trajectory::SessionIndexPage TrajectorySessionLedger::ListWorkspaceSessions(
    const trajectory::SessionIndexQuery& query) const {
    trajectory::SessionIndexQuery effective = query;
    if (!effective.all_workspaces && effective.current_workspace_key.empty()) {
        effective.current_workspace_key = workspace_key();
    }
    return trajectory::QueryWorkspaceSessions(workspaces_root(), effective);
}

std::vector<trajectory::PromptHistoryLine> TrajectorySessionLedger::ReadPromptHistory(
    std::size_t max_lines) const {
    return trajectory::ReadWorkspacePromptHistory(workspaces_root(), workspace_key(), max_lines);
}

std::vector<std::string> TrajectorySessionLedger::MakeTranscriptExcerpt(const std::string& target_id,
                                                                        std::size_t max_half) const {
    if (impl_ != nullptr && impl_->active != nullptr && target_id == this->session_id()) {
        return trajectory::MakeSessionTranscriptExcerpt(impl_->active->session_dir(), max_half);
    }
    // 别的场次:经索引定位目录(跨 workspace 也找得回)。
    trajectory::SessionIndexQuery query;
    query.all_workspaces = true;
    const auto page = trajectory::QueryWorkspaceSessions(workspaces_root(), query);
    for (const auto& summary : page.entries) {
        if (summary.session_id == target_id) {
            return trajectory::MakeSessionTranscriptExcerpt(
                tools::Utf8ToPath(summary.session_dir), max_half);
        }
    }
    return {};
}

std::string TrajectorySessionLedger::ArchiveSessionInWorkspace(const std::string& session_id) const {
    if (impl_ == nullptr) {
        return "session.open_failed: 账本未开";
    }
    const auto outcome = trajectory::ArchiveSessionDir(impl_->manager->workspace_dir(), session_id,
                                                       std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::system_clock::now()
                                                               .time_since_epoch())
                                                           .count());
    return outcome.ok() ? std::string() : outcome.error_code + ": " + outcome.message;
}

std::string TrajectorySessionLedger::UnarchiveSessionInWorkspace(const std::string& session_id) const {
    if (impl_ == nullptr) {
        return "session.open_failed: 账本未开";
    }
    const auto outcome = trajectory::UnarchiveSessionDir(
        impl_->manager->workspace_dir(), session_id,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    return outcome.ok() ? std::string() : outcome.error_code + ": " + outcome.message;
}

std::string TrajectorySessionLedger::DeleteSessionInWorkspace(const std::string& session_id,
                                                              const std::string& reason) const {
    if (impl_ == nullptr) {
        return "session.open_failed: 账本未开";
    }
    if (session_id == this->session_id()) {
        return "session.delete_active: 当前场先 /exit 封口再删";
    }
    const auto outcome = trajectory::DeleteSessionDir(
        impl_->manager->workspace_dir(), session_id, reason,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    return outcome.ok() ? std::string() : outcome.error_code + ": " + outcome.message;
}

void TrajectorySessionLedger::RecordTitleChanged(const std::string& title, const std::string& old_title) {
    nlohmann::json payload = nlohmann::json{{"title", title}};
    if (!old_title.empty()) {
        payload["old_title"] = old_title;
    }
    // /title 是真人敲的命令账;自动精炼采纳的标题同走这枚事件(actor
    // 如实分流留给后续批次,先把"标题变过"落成可回放事实)。
    PutUserCommand_(trajectory::EventKind::ControlTitleChanged, std::move(payload));
}

void TrajectorySessionLedger::RecordModeChanged(const std::string& mode, const std::string& reason,
                                                const std::string& old_mode) {
    nlohmann::json payload = nlohmann::json{{"mode", mode}, {"reason", reason}};
    if (!old_mode.empty()) {
        payload["old_mode"] = old_mode;
    }
    PutControl_(trajectory::EventKind::ControlModeChanged, std::move(payload));
}

}  // namespace lubancode::runtime
