// 轨迹会话账的实现(P0-2 运行时单一写口)。合同见 trajectory_session.hpp
// 文件头;事件 payload 形状照 todos/P0新轨迹记录_可重放与训练投影设计.todo
// §五与 P0-1 的 schema.cpp 逐字段钉死的样子。

#include "runtime/trajectory_session.hpp"

#include <cstdlib>
#include <utility>

#include "accounting/purpose.hpp"   // PurposeName(Token 账本单 A1)
#include "agent/context.hpp"        // EstimateUtf8Tokens:request_snapshot 的 token 估算
#include "config/config.hpp"
#include "hooks/hash.hpp"           // Sha256Hex:request_snapshot 的 parameters_hash
#include "platform/log_sink.hpp"
#include "platform/paths.hpp"
#include "tools/path_utils.hpp"     // Utf8ToPath:主目录文本转路径
#include "tools/tool_content.hpp"   // TextContent:富结果块的文本投影

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

bool ResolveTrajectoryEnabled(bool config_flag) {
    const std::optional<bool> env = TrajectoryEnvOpinion();
    return env.has_value() ? *env : config_flag;
}

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
    return recorder_.Record(std::move(request), durability);
}

void TrajectoryTurnBridge::NoteError(const RecordReceipt& receipt, const char* where) {
    recent_errors_.push_back(std::string(where) + ":" + receipt.error_code);
    platform::LogSink::Instance().Error("trajectory",
                                        std::string(where) + " 落账失败: " + receipt.error_code);
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
        if (!book.terminal) {
            const auto receipt =
                Put(EventKind::ToolExecutionCancelled, book.request_id, call_id, Actor::Tool,
                    Origin::BuiltinTool, nlohmann::json{{"reason", reason}});
            if (receipt.status == RecordReceipt::Status::Committed) {
                book.terminal = true;
                book.terminal_cancelled = true;
                book.terminal_event_id = receipt.event_id;
            } else {
                NoteError(receipt, "tool.execution.cancelled(dangling)");
            }
        }
    }
}

void TrajectoryTurnBridge::EndTurn(bool ok, bool cancelled, const std::string& reason) {
    if (!turn_open_) {
        return;
    }
    CancelDanglingCalls("turn_closed_unresolved");
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

}  // namespace

std::string TrajectoryTurnBridge::OnRequestPrepared(const api::Request& request,
                                                     const agent::RequestPreparedContext& ctx) {
    if (!turn_open_) {
        return std::string();
    }
    const std::string request_id = NextRequestId();
    nlohmann::json payload = nlohmann::json{{"model", request.model},
                                            {"provider", identity_.provider},
                                            {"wire", identity_.wire},
                                            // Token 账本单 A1:purpose 恒有效(AgentProfile.purpose
                                            // 默认值),不是"没接线就漏字段"——真实运行时路径
                                            // 永远给出诚实的枚举名。
                                            {"purpose", accounting::PurposeName(ctx.purpose)}};
    nlohmann::json message_refs = nlohmann::json::array();
    if (!last_input_event_id_.empty()) {
        message_refs.push_back(last_input_event_id_);
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

void TrajectoryTurnBridge::OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                                           bool reported_by_provider,
                                           const std::string& provider_response_id, int cache_epoch,
                                           bool prefix_append_only) {
    nlohmann::json payload = nlohmann::json{{"attempt", std::uint64_t{1}},
                                            {"reported_by_provider", reported_by_provider}};
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

bool TrajectoryTurnBridge::OnOutputCompleted(const std::string& request_id, const api::Message& assistant,
                                             const std::string& stop_reason,
                                             const std::string& provider_response_id) {
    nlohmann::json payload = nlohmann::json{{"output_id", NextOutputId()},
                                            {"blocks", MessageToBlocks(assistant)},
                                            {"stop_reason", stop_reason.empty() ? "end_turn" : stop_reason}};
    if (!provider_response_id.empty()) {
        payload["provider_response_id"] = provider_response_id;
    }
    const auto receipt =
        Put(EventKind::ModelOutputCompleted, request_id, std::nullopt, Actor::Model,
            Origin::ProviderModel, std::move(payload), Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.completed");
        return false;  // §7.4:输出记不住,不执行工具
    }
    // 声明本份 output 的 tool call(§6.1:call 由模型输出定义)。
    for (const auto& block : assistant.content) {
        if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            CallBook& book = calls_[call->id];
            book.request_id = request_id;
        }
    }
    return true;
}

void TrajectoryTurnBridge::OnOutputFailed(const std::string& request_id, const std::string& reason) {
    const auto receipt =
        Put(EventKind::ModelOutputFailed, request_id, std::nullopt, Actor::Model,
            Origin::ProviderModel, nlohmann::json{{"reason", reason}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.failed");
    }
}

void TrajectoryTurnBridge::OnOutputCancelled(const std::string& request_id) {
    const auto receipt =
        Put(EventKind::ModelOutputCancelled, request_id, std::nullopt, Actor::Model,
            Origin::ProviderModel, nlohmann::json{{"reason", "user_interrupt"}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.cancelled");
    }
}

void TrajectoryTurnBridge::OnToolTrace(const agent::ToolTraceEvent& event) {
    if (!turn_open_ || event.tool_use_id.empty()) {
        return;
    }
    CallBook& book = calls_[event.tool_use_id];
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
                payload["side_effects"] = nlohmann::json::array();
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
            // result_ref——payload 键集封闭,加未知键会被 schema 拒收。
            EventLinks links;
            if (!book.child_run_id.empty()) {
                links.child_run_id = book.child_run_id;
                const auto hash = child_terminal_hashes_.find(book.child_run_id);
                payload["result_ref"] = nlohmann::json{
                    {"kind", "child_stream"},
                    {"child_run_id", book.child_run_id},
                    {"child_terminal_event_hash",
                     hash != child_terminal_hashes_.end() ? hash->second : std::string()}};
                payload["side_effects"] = nlohmann::json::array();
            }
            const auto receipt = Put(kind, book.request_id, event.tool_use_id, Actor::Tool,
                                     Origin::BuiltinTool, std::move(payload), Durability::PowerLoss,
                                     std::move(links));
            if (receipt.status == RecordReceipt::Status::Committed) {
                book.terminal = true;
                book.terminal_cancelled = kind == EventKind::ToolExecutionCancelled;
                book.terminal_event_id = receipt.event_id;
            } else {
                NoteError(receipt, "tool.terminal");
            }
            return;
        }
        case agent::ToolTraceEventKind::ResultCommitted:
        case agent::ToolTraceEventKind::Verification:
        case agent::ToolTraceEventKind::RecoveryMarker:
        case agent::ToolTraceEventKind::McpLateResponse:
            return;  // result.committed 从消息正文翻(OnToolResultsCommitted);
                     // verification 是 P0-4 的账,迟到响应/恢复注记不进轨迹。
    }
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
            if (const auto* text = std::get_if<tools::TextContent>(&extra)) {
                content.push_back(nlohmann::json{{"type", "text"}, {"text", text->text}});
            }
        }
        nlohmann::json payload = nlohmann::json{{"call_id", result->tool_use_id},
                                                {"content", std::move(content)},
                                                {"is_error", result->is_error}};
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
    return started_io_failed_.count(started.execution_id) != 0;
}

void TrajectoryTurnBridge::AttachChildRun(const std::string& call_id, const std::string& agent_run_id) {
    calls_[call_id].child_run_id = agent_run_id;
}

void TrajectoryTurnBridge::NoteChildTerminal(const std::string& agent_run_id,
                                             const std::string& terminal_event_hash) {
    child_terminal_hashes_[agent_run_id] = terminal_event_hash;
}

// ---------------------------------------------------------------------------
// TrajectorySessionLedger
// ---------------------------------------------------------------------------

struct TrajectorySessionLedger::Impl {
    std::unique_ptr<trajectory::SessionManager> manager;
    trajectory::ActiveSession* active = nullptr;
    trajectory::RecorderOptions recorder_options;
    std::string main_run_id;
    // 子代理账:run_id -> 终态 hash(Finish 时填,父账边界引用用)。
    std::map<std::string, std::string> child_terminal_hashes;
    std::uint64_t subagent_counter = 0;
    // --continue 启动路的 resume 投影(没 resume 为空)。
    bool launch_resumed = false;
    std::vector<api::Message> launch_resume_history;
};

std::expected<TrajectorySessionLedger, std::string> TrajectorySessionLedger::Open(Options options) {
    if (options.trajectories_root.empty()) {
        const auto home = config::HomeLubancodeDir();
        if (!home.has_value()) {
            return std::unexpected("trajectory.no_home: 找不到主目录,轨迹账无处落");
        }
        options.trajectories_root = tools::Utf8ToPath(*home) / "trajectories";
    }
    if (options.workspace_root.empty()) {
        options.workspace_root = std::filesystem::current_path();
    }
    trajectory::SessionManagerOptions manager_options;
    manager_options.trajectories_root = options.trajectories_root;
    manager_options.workspace_root = options.workspace_root;
    manager_options.readable_workspace_name =
        options.readable_workspace_name.empty()
            ? options.workspace_root.filename().generic_string()
            : options.readable_workspace_name;
    manager_options.launch_cwd = options.launch_cwd;
    manager_options.lubancode_version = options.lubancode_version;
    manager_options.recorder.event_schema_version = options.event_schema_version;

    Impl impl;
    impl.recorder_options.event_schema_version = options.event_schema_version;
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
    scope.training_policy = TrainingPolicy::Metadata;
    return std::make_unique<TrajectoryTurnBridge>(*recorder, std::move(scope), std::move(identity));
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

std::expected<std::unique_ptr<TrajectorySubagentBridge>, std::string>
TrajectorySessionLedger::SpawnSubagent(const std::string& parent_call_id, const std::string& task_label) {
    if (impl_ == nullptr || impl_->active == nullptr) {
        return std::unexpected("trajectory.no_active_session");
    }
    const std::string agent_run_id =
        "agent-" + std::to_string(++impl_->subagent_counter) + "-" + impl_->main_run_id;
    auto stream = impl_->active->directory.ReserveSubagentStream(agent_run_id);
    if (!stream.has_value()) {
        return std::unexpected("trajectory.subagent_stream: " + stream.error());
    }
    trajectory::EventScope scope = impl_->active->main->base_scope();
    scope.run_id = agent_run_id;
    scope.run_kind = trajectory::RunKind::Subagent;
    scope.turn_id.reset();
    scope.request_id.reset();
    scope.call_id.reset();
    scope.visibility = {Visibility::HostOnly};
    scope.training_policy = TrainingPolicy::Metadata;
    auto recorder = trajectory::TrajectoryRecorder::Start(*stream, impl_->active->directory.artifacts_root(),
                                                          scope, impl_->recorder_options);
    if (!recorder.has_value()) {
        return std::unexpected("trajectory.subagent_recorder: " + recorder.error());
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
    links.parent_run_id = impl_->main_run_id;
    if (!parent_call_id.empty()) {
        links.parent_call_id = parent_call_id;
    }
    const auto started = recorder_owner->WriteRunStarted(std::move(payload),
                                                          trajectory::Durability::PowerLoss,
                                                          std::move(links));
    if (started.status != trajectory::RecordReceipt::Status::Committed) {
        return std::unexpected("trajectory.subagent_run_started: " + started.error_code);
    }

    TrajectoryTurnBridge::Identity identity;
    identity.provider = "subagent";
    identity.wire = "subagent";
    identity.channel = "subagent";
    auto bridge = std::make_unique<TrajectoryTurnBridge>(*recorder_owner, scope, identity);
    return std::unique_ptr<TrajectorySubagentBridge>(new SubagentBridgeImpl(
        std::move(recorder_owner), std::move(bridge), agent_run_id, &impl_->child_terminal_hashes));
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
    }
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
    }
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

}  // namespace lubancode::runtime
