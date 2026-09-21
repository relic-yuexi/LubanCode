// 轨迹主轮桥的实现(AR-12 机械拆分:自 trajectory_session.cpp 按桥类边界
// 拆出,方法体一字未动;合同见 trajectory_turn_bridge.hpp)。事件 payload
// 形状照 todos/P0新轨迹记录_可重放与训练投影设计.todo §五与 P0-1 的
// schema.cpp 逐字段钉死的样子。

#include "runtime/trajectory_turn_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <utility>

#include "accounting/purpose.hpp"   // PurposeName(Token 账本单 A1)
#include "agent/context.hpp"        // EstimateUtf8Tokens:request_snapshot 的 token 估算
#include "agent/context_events.hpp"  // Fingerprint64:prepared 行 inputView 的视图指纹(V3-REAL-06)
#include "hooks/hash.hpp"           // Sha256Hex:request_snapshot 的 parameters_hash
#include "platform/log_sink.hpp"
#include "platform/paths.hpp"
#include "runtime/trajectory_bridge_internal.hpp"  // 主桥与旁路桥共用的事实构造
#include "runtime/v3_tool_result_material.hpp"     // PreserveNativeToolPayload:capture/persist 的原生载荷保全
#include "tools/path_utils.hpp"     // Utf8ToPath:v3 结果仓/输出索引的路径拼接
#include "tools/tool_content.hpp"   // TextContent:富结果块的文本投影
#include "trajectory/metrics.hpp"   // HasDiskReserve:StorageAvailable 的磁盘 reserve 门
#include "trajectory/v3/tool_action.hpp"  // ToolActionSession:v3 工具操作账(接线点 1)

namespace lubancode::runtime {

// trajectory v3 简称(本件内 v3:: 一律指 trajectory::v3;runtime 命名空间
// 下裸写 v3:: 解析不到 trajectory::v3)。
namespace v3 = ::lubancode::trajectory::v3;

// v3 流式片段的攒批窗口(§4.43 schema 冻结:250ms 或 4 KiB 先到为准——
// 生产桥取字节窗,不引时钟依赖,终态前统一放行尾巴,两轴殊途同归)。
constexpr std::size_t kV3StreamBatchBytes = 4096;

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

// 实际发送视图的逐消息指纹(V3-REAL-06):role + 块数 + 各块正文投影
//(与 session 侧 HistoryStateHash 同款投影标记,非密码学真值;同一份消息
// 两次算必然同值——离线重放拿它逐块核对"引用还原的正文"与"实际模型
// 输入"是否一致)。
std::string RequestMessageViewFingerprint(const api::Message& message) {
    std::string buffer;
    buffer += message.role == api::Role::User
                  ? std::string("U")
                  : (message.role == api::Role::Assistant ? std::string("A") : std::string("?"));
    buffer += std::to_string(message.content.size());
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            buffer += "t" + text->text;
        } else if (const auto* thinking = std::get_if<api::ThinkingBlock>(&block)) {
            buffer += "k" + thinking->text + thinking->signature;
            if (!thinking->responses_item.is_null()) buffer += thinking->responses_item.dump();
        } else if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            buffer += "u" + call->id + call->name + call->input.dump();
        } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
            buffer += "r" + result->tool_use_id + result->content;
        } else if (const auto* image = std::get_if<api::ImageBlock>(&block)) {
            buffer += "i" + image->filename;
        } else {
            buffer += "x";
        }
    }
    return agent::Fingerprint64(buffer);
}

}  // namespace

// ---------------------------------------------------------------------------
// v3 写模式(接线点 1):回合簿与消息体拼装。
// ---------------------------------------------------------------------------

// v3 模式的回合簿:provider 调用号 -> Action 簿 + 请求簿(usage 暂存到
// assistant 落行时一并写,§4.12 usage 唯一 owner 是 assistant message)。
struct V3TurnBooks {
    struct Call {
        std::string action_id;             // v3 全局调用身份(writer 发号)
        std::string request_id;            // 声明它的请求
        std::string step_id;
        std::string assistant_message_ref;
        std::optional<v3::ToolActionSession> action;  // Admit(pending)后有值
        bool terminal = false;
        bool started = false;
        bool failed = false;               // 执行终态是 failed(选用口径用)
        bool tool_message_done = false;
        std::string terminal_event_id;
        std::string capture_event_id;
        bool capture_failed = false;
        std::optional<bool> capture_complete;
        std::string capture_reason;
    };
    struct Request {
        std::string step_id;
        std::string model;                 // 本次请求实际模型(来源三件套用)
        std::optional<nlohmann::json> usage;  // provider 实报;缺报 nullopt
        bool output_committed = false;     // assistant 已成行(此后 usage 走 appended)
        // 流式三件套的请求簿(§4.43,D1):started 懒起(响应开始才发号),
        // 片段按类型攒批,终态前放行尾巴后走 Complete/Interrupt 收口。
        std::string stream_id;             // 流身份(writer 发号,started 时定)
        std::string reserved_message_id;   // started 时预留的 assistant messageId
        std::string completed_event_id;    // model.response.completed 事件 id(异步 P2 证据)
        bool stream_started = false;       // model.response.started 已落稳
        std::string batch_text;            // 攒批:text(4 KiB 窗口一批)
        std::string batch_reasoning;       // 攒批:reasoning
        std::string received_text;         // 已收正文(中断定稿用,与批次同源)
        std::string received_reasoning;    // 已收思考(中断定稿用)
        std::uint64_t delta_seq = 0;       // 已落批次的末序号(接收水位)
    };
    std::map<std::string, Call> calls;      // key = provider tool_use id
    std::map<std::string, Request> requests;
};

// api::Usage -> v3 usage json(§五键名;只在 provider 明报时调用,缺报
// 走 null 不补 0)。非文本/内部口径另立账,这里只翻实报五件。
nlohmann::json UsageToJson(const api::Usage& usage) {
    return nlohmann::json{{"inputTokens", usage.input_tokens},
                          {"outputTokens", usage.output_tokens},
                          {"reasoningTokens", usage.output_reasoning_tokens},
                          {"cacheReadTokens", usage.cache_read_tokens},
                          {"cacheWriteTokens", usage.cache_creation_tokens}};
}

TrajectoryTurnBridge::TrajectoryTurnBridge(trajectory::TrajectoryRecorder& recorder,
                                           trajectory::EventScope base_scope, Identity identity)
    : recorder_(&recorder), base_scope_(std::move(base_scope)), identity_(std::move(identity)) {}

TrajectoryTurnBridge::TrajectoryTurnBridge(v3::V3Writer* v3_writer, V3SessionBooks* v3_books,
                                           trajectory::EventScope identity_scope, Identity identity)
    : v3_writer_(v3_writer), v3_books_(v3_books), v3_turn_(std::make_unique<V3TurnBooks>()),
      base_scope_(std::move(identity_scope)), identity_(std::move(identity)) {}

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
    const trajectory::RecordReceipt receipt = recorder_->Record(std::move(request), durability);
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

void TrajectoryTurnBridge::NoteV3Error(const v3::WriteReceipt& receipt, const char* where) {
    std::string note = std::string(where) + ":" + receipt.error_code;
    if (!receipt.error_message.empty()) {
        note += " (" + receipt.error_message + ")";
    }
    recent_errors_.push_back(note);
    if (error_sink_ != nullptr) {
        error_sink_->push_back(note);
    }
    platform::LogSink::Instance().Error("trajectory", "v3 落账失败: " + note);
}

// v3 提交后的 committed wake(与 Put 的 v2 漏斗同款,只投身份)。
void TrajectoryTurnBridge::V3NotifyCommitted(const v3::WriteReceipt& receipt) {
    if (receipt.status == v3::WriteReceipt::Status::Committed && commit_wake_ != nullptr) {
        telemetry::CommitWake wake;
        wake.workspace_key = base_scope_.workspace_key;
        wake.session_id = base_scope_.session_id;
        wake.stream_id = wake_stream_id_;
        commit_wake_->Notify(wake);
    }
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
    if (V3Mode()) {
        // v3 没有 turn.started 一类行:回合身份由各行信封的 turnId 携带
        //(§4.2"不能见到 user 角色就自行推断新回合"的反面——回合先立号,
        // 行再归属)。trigger 不单独落账;输入/请求行带 channel 事实。
        v3_turn_ = std::make_unique<V3TurnBooks>();
        // T11-D:验证簿回合制(同 calls_ 的回合纪律)——stale invalidation
        // 只对本回合已录验证对账,不跨回合追旧账。
        verifications_.clear();
        // 主回合号留给旁路桥挂 parentTurnId(取消误报 ESC 单 Bug 2):回合
        // 尾巴的抽取/摘要要能回答"哪只回合触发的"。EndTurn 不清——尾巴
        // 活儿多在回合收口之后跑,"最近一只"就是触发者。
        // T12-C:另立 main_turn_open 旗如实记"此刻在跑"——active_*
        // 粘账分不清在跑与收口,中途压缩递 parentTurnId 只认这只旗。
        if (v3_books_ != nullptr) {
            v3_books_->active_main_turn_id = turn_id;
            v3_books_->main_turn_open = true;
        }
        return;
    }
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
    if (V3Mode()) {
        V3RecordInput(user_message);
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
    if (V3Mode()) {
        // v3 没有 turn 终态行;收口纪律同款——已声明未终态的调用补
        // cancelled,配对完整,不留悬空 actionId。
        V3CancelDanglingActions(reason.empty() ? "turn_closed_unresolved" : reason);
        turn_open_ = false;
        // T12-C:回合收口,活动主轮清旗——active_main_turn_id 照旧粘住
        //(尾巴活儿挂账用),但"在跑"从这一刻起是 false,idle 压缩不伪称。
        if (v3_books_ != nullptr) {
            v3_books_->main_turn_open = false;
        }
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
            blocks.push_back(nlohmann::json{{"type", "thinking"}, {"text", thinking->text}, {"signature", thinking->signature}, {"responses_item", thinking->responses_item}});
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
    // 注:连接快照的 connection 块只落 v3(V3RequestPrepared,§八"落点沿
    // V3 请求账合同");v2 载荷键表封闭拒未知键(ValidatePayload),这里
    // 不写——v2 老账的形状一个字节不动。
    return payload;
}

void TrajectoryTurnBridge::OnContextPressure(const agent::ContextPressure& pressure) {
    if (!turn_open_ ||
        (pressure.phase != agent::ContextPressure::Phase::PreflightExceeded &&
         pressure.phase != agent::ContextPressure::Phase::PreflightDegraded)) {
        return;
    }
    if (V3Mode()) {
        // T11-E / V3-GAP-06:发送前容量压力与预算裁决落 context.pressure.
        // recorded(statusless)。verdict 三分:应急收窄放行(reserve_clamped)/
        // 拒发(exceeded_denied)/优雅降档(max_tokens_degraded,Phase::
        // PreflightDegraded 时)。四项数字账与 v2 同名事件同口径;remaining
        // 是窗口减三项后的余量。不复制累计用量——usage 唯一 owner 在
        // assistant message(§五),本行只是当次判定事实。落账失败记错继续
        // (与 v2 同门:metadata 不改写业务结果,doctor 暴露坏账)。
        const bool degraded = pressure.phase == agent::ContextPressure::Phase::PreflightDegraded;
        v3::EventDraft draft;
        draft.kind = v3::EventKindV3::ContextPressureRecorded;
        draft.turn_id = turn_id_;
        const std::uint64_t window = static_cast<std::uint64_t>(pressure.window_tokens);
        const std::uint64_t estimated = static_cast<std::uint64_t>(pressure.estimated_input_tokens);
        const std::uint64_t reserve = static_cast<std::uint64_t>(pressure.reserved_output_tokens);
        const std::uint64_t headroom =
            static_cast<std::uint64_t>(pressure.protocol_headroom_tokens);
        const std::uint64_t used = estimated + reserve + headroom;
        draft.payload = nlohmann::json{
            {"phase", "preflight"},
            {"verdict", degraded ? "max_tokens_degraded"
                                 : (pressure.reserve_clamped ? "reserve_clamped" : "exceeded_denied")},
            {"estimatedInputTokens", estimated},
            {"reservedOutputTokens", reserve},
            {"protocolHeadroomTokens", headroom},
            {"windowTokens", window},
            {"remainingTokens", window >= used ? window - used : std::uint64_t{0}}};
        const auto receipt =
            v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
        if (receipt.status != v3::WriteReceipt::Status::Committed) {
            NoteV3Error(receipt, "context.pressure.recorded");
        }
        return;
    }
    // v2 老路只认 PreflightExceeded(Phase::PreflightDegraded 是 T11-E 给
    // v3 新开的口,v2 老账形状一个字节不动)。
    if (pressure.phase != agent::ContextPressure::Phase::PreflightExceeded) {
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
    if (V3Mode()) {
        return V3RequestPrepared(request, ctx);
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

bool TrajectoryTurnBridge::OnRequestSent(const std::string& request_id) {
    if (V3Mode()) {
        return V3RequestSent(request_id);
    }
    const auto it = request_prepared_.find(request_id);
    if (it == request_prepared_.end()) {
        return true;  // 簿里没有(prepared 没落稳/重复 sent):不拦,账早已如实
    }
    const auto receipt =
        Put(EventKind::ModelRequestSent, request_id, std::nullopt, Actor::Host,
            Origin::RecoveryRuntime,
            nlohmann::json{{"prepared_event_id", it->second}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.request.sent");
        return false;  // 发送前写账硬闸(P1-C/FA-03):本地账写不动,不发模型
    }
    return true;
}

bool TrajectoryTurnBridge::OnRequestSentWithTurn(const std::string& request_id, int task_turn_index,
                                                  int turn_limit, int input_round_index) {
    if (V3Mode()) {
        // 任务 turn 账(§11.1)是 v2 的 sent 边界数字,v3 暂无对应 kind;
        // sent 本身照落。
        return V3RequestSent(request_id);
    }
    const auto it = request_prepared_.find(request_id);
    if (it == request_prepared_.end()) {
        return true;  // 同 OnRequestSent:簿里没有不拦
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
        return false;  // P1-C/FA-03:sent 记不住,不发模型
    }
    return true;
}

void TrajectoryTurnBridge::OnResponseStarted(const std::string& request_id) {
    if (V3Mode()) {
        V3ResponseStarted(request_id);
    }
    // v2 无流式事件账:响应边界由 output 三态收口,这里零行为。
}

void TrajectoryTurnBridge::OnStreamDelta(const std::string& request_id, const std::string& delta_type,
                                         const std::string& text) {
    if (V3Mode()) {
        V3StreamDelta(request_id, delta_type, text);
    }
    // v2 同上:片段账是 v3 §4.43 的概念,v2 不伪造。
}

void TrajectoryTurnBridge::OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                                           bool reported_by_provider,
                                           const std::string& provider_response_id, int cache_epoch,
                                           bool prefix_append_only, bool cache_read_reported_by_provider,
                                           bool cache_creation_reported_by_provider,
                                           const std::string& usage_anomaly) {
    if (V3Mode()) {
        V3UsageRecorded(request_id, usage, reported_by_provider, provider_response_id);
        return;
    }
    // 缓存读/写明报位分开落(C2):旧键 cache_reported_by_provider 不再写
    // (读侧 usage_projector 兼容两代);异常账(C4)非空才落——数字保留
    // 原数(可为负),矛盾由 anomaly 点名,schema 校验放行"负数必带点名"。
    nlohmann::json payload = nlohmann::json{{"attempt", std::uint64_t{1}},
                                            {"reported_by_provider", reported_by_provider},
                                            {"cache_read_reported_by_provider",
                                             cache_read_reported_by_provider},
                                            {"cache_creation_reported_by_provider",
                                             cache_creation_reported_by_provider}};
    if (!usage_anomaly.empty()) {
        payload["usage_anomaly"] = usage_anomaly;
    }
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
    if (V3Mode()) {
        return V3OutputCompleted(request_id, assistant, stop_reason, provider_response_id);
    }
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
    if (V3Mode()) {
        V3OutputFailed(request_id, reason);
        return;
    }
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

void TrajectoryTurnBridge::OnOutputCancelled(const std::string& request_id, agent::OutputCancelSource source) {
    if (V3Mode()) {
        V3OutputCancelled(request_id, source);
        return;
    }
    // 取消来源说真话(主会话输出预留占坑单 §4.2):真按键才记
    // user_interrupt——旧硬编码的冤枉账已废,三来源各记各名(规范名
    // 见 agent::OutputCancelSourceText;旧 stream 的 user_interrupt 值
    // 原样保留,兼容不破)。
    nlohmann::json payload = {{"reason", agent::OutputCancelSourceText(source)}};
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
    if (V3Mode()) {
        V3ToolTrace(event);
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
            payload["observed_after_seq"] = recorder_->next_seq() - 1;
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

ToolResultsCommitReceipt TrajectoryTurnBridge::OnToolResultsCommitted(const std::string& batch_id,
                                                                      const api::Message& results) {
    (void)batch_id;
    if (!turn_open_) {
        return {};  // 轮没开:无账可落,不拦
    }
    if (V3Mode()) {
        auto adopted = results;
        return V3ToolResultsCommitted(adopted);
    }
    ToolResultsCommitReceipt batch;
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
            // P1-A(FA-01):v2 同一合同——事件写不住,回执翻 Failed,主循环
            // 停止后续模型发送,不拿内存独有结果继续。
            if (batch.status == ToolResultsCommitReceipt::Status::Committed) {
                batch.status = ToolResultsCommitReceipt::Status::Failed;
                batch.error_code = "tool.result.committed:" + receipt.error_code;
            }
        }
    }
    return batch;
}

// ---------------------------------------------------------------------------
// TrajectoryTurnBridge 的 v3 写模式(session_switch.hpp 接线点 1):核心
// 会话轴——用户输入、请求准备/发出、assistant 定稿、工具操作账
//(tool_action)、结果仓(result_store)与最终 tool 消息——落 sessions/
// <id>/<id>.jsonl。v2 专有概念(turn/verification/任务 turn 账/context
// pressure)在 v3 事件表无对应 kind,不伪造行,各早退处已注留白。
// ---------------------------------------------------------------------------

bool TrajectoryTurnBridge::V3EnsureSystem(const std::string& system_content) {
    if (system_content.empty() || system_content == v3_books_->system_content) {
        return true;  // 没带 system/与当前根相同:不造假版本(§4.3)
    }
    nlohmann::json change;
    change["cause"] = "system_prompt_changed";
    change["settingsVersion"] = v3_books_->settings_version + 1;
    change["systemChanged"] = true;
    const auto switched = v3_writer_->SwitchSystem(
        system_content, std::move(change), v3::MessageOrigin::SessionRuntime,
        trajectory::Durability::PowerLoss);
    if (switched.change_event.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(switched.change_event, "system.change");
        return false;
    }
    V3NotifyCommitted(switched.change_event);
    if (switched.system_message.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(switched.system_message, "system message");
        return false;  // 三步没走完:内存不换根,变更显示未完成(§4.3)
    }
    V3NotifyCommitted(switched.system_message);
    if (switched.apply_event.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(switched.apply_event, "context.system.applied");
        return false;
    }
    V3NotifyCommitted(switched.apply_event);
    v3_books_->system_content = system_content;
    ++v3_books_->settings_version;
    return true;
}

void TrajectoryTurnBridge::V3RecordInput(const api::Message& user_message) {
    // 先记到达(input.received:来源渠道;§2.1"发生过什么"),再落消息
    // 正文与接纳。display=visible 的 user 消息本体就是到达事实的完整版。
    {
        v3::EventDraft received;
        received.kind = v3::EventKindV3::InputReceived;
        received.turn_id = turn_id_;
        received.payload = nlohmann::json{
            {"source", identity_.channel},
            {"senderKind", identity_.channel == "app_server" ? "remote_user" : "local_user"}};
        const auto receipt =
            v3_writer_->AppendEvent(std::move(received), trajectory::Durability::ProcessCrash);
        V3NotifyCommitted(receipt);
        if (receipt.status != v3::WriteReceipt::Status::Committed) {
            NoteV3Error(receipt, "input.received");
        }
    }
    // 正文:单块纯文本落 string(读取投影两读法都认);多块落 blocks 数组,
    // 图片只落引用块——base64 不进账(与 v2 RecordInput 的消毒同款)。
    nlohmann::json content;
    if (user_message.content.size() == 1) {
        if (const auto* text = std::get_if<api::TextBlock>(&user_message.content.front())) {
            content = nlohmann::json(text->text);
        }
    }
    if (content.is_null()) {
        content = nlohmann::json::array();
        for (const auto& block : user_message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                content.push_back(nlohmann::json{{"type", "text"}, {"text", text->text}});
            } else if (const auto* image = std::get_if<api::ImageBlock>(&block)) {
                content.push_back(
                    nlohmann::json{{"type", "image"}, {"filename", image->filename}});
            }
        }
    }
    v3::MessageDraft draft;
    draft.turn_id = turn_id_;
    draft.purpose = v3::MessagePurpose::Conversation;
    draft.origin = v3::MessageOrigin::Human;
    draft.display = v3::DisplayMode::Visible;
    draft.message = nlohmann::json{{"role", "user"}, {"content", std::move(content)}};
    const auto receipt =
        v3_writer_->AppendMessage(std::move(draft), trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "user message");
        return;
    }
    last_input_event_id_ = receipt.id;
    const auto admitted =
        v3_writer_->AdmitMessages({receipt.id}, trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(admitted);
    if (admitted.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(admitted, "context.input.applied(user)");
    }
}

std::string TrajectoryTurnBridge::V3RequestPrepared(const api::Request& request,
                                                    const agent::RequestPreparedContext& ctx) {
    // T12-A(V3-GAP-07 P0):请求最终准入门。compact applied 落稳但内存换账
    // 失败后,本场 books 置阻断——这里拦在引用对表之前:空串即"prepared
    // 记不住不发模型"的既有语义,loop 本步明败,模型请求/新工具/自动续跑
    // 一次都出不去。CLI/AppServer/Goal/Loop 的轮桥全走这同一道门,不在
    // 任何分支另加早退。恢复(resume 沿已提交链核验重建)发生在新场新
    // books 上,天然不携带本阻断。
    if (v3_books_ != nullptr && v3_books_->execution_blocked) {
        return std::string();
    }
    if (!V3EnsureSystem(request.system)) {
        return std::string();  // §4.4:引用没落稳,请求不得发出
    }
    const std::string request_id = v3_writer_->NewRequestId();
    const std::string step_id = v3_writer_->NewStepId();
    // inputMessageRefs:当前链上根之后的有序输入(§4.4"按实际输入顺序")。
    // 本棒里有效输入即链输入——runtime 的裁剪/compact 投影属后续棒,链
    // 不虚列不虚构。
    std::vector<std::string> input_refs;
    const auto& chain = v3_writer_->context().chain;
    for (std::size_t i = 1; i < chain.size(); ++i) {
        input_refs.push_back(chain[i].message_ref);
    }
    nlohmann::json provider_snapshot = nlohmann::json{{"provider", identity_.provider},
                                                      {"wire", identity_.wire},
                                                      {"model", request.model}};
    // 应用Worker接入单 §八:连接快照的冻结局,与 v2 BuildPreparedPayload
    // 同一份合同(见那边的注释);identity 没带整块不落。
    if (identity_.connection.is_object() && !identity_.connection.empty()) {
        nlohmann::json connection = nlohmann::json::object();
        if (identity_.connection.contains("endpoint") && identity_.connection["endpoint"].is_string()) {
            connection["endpoint"] = identity_.connection["endpoint"];
        }
        if (identity_.connection.contains("secretRef") && identity_.connection["secretRef"].is_string()) {
            connection["secretRef"] = identity_.connection["secretRef"];
        }
        if (identity_.connection.contains("configVersion") &&
            identity_.connection["configVersion"].is_string()) {
            connection["configVersion"] = identity_.connection["configVersion"];
        }
        if (!connection.empty()) {
            provider_snapshot["connection"] = std::move(connection);
        }
    }
    if (request.max_tokens.has_value()) {
        provider_snapshot["parameters"] = nlohmann::json{{"maxOutputTokens", *request.max_tokens}};
    }
    // 前缀缓存守恒单 §五 D:cache_epoch/追加律/断因完整进持久化请求账
    //(此前只 cache_epoch 随 usage 落,prepared 侧不落、断因两边都不落,
    // 验尸时对不上)。provider_snapshot 的键摊平进 prepared payload 顶层,
    // 这块落在 prefixAccount 下;断因空串如实落空——本步没断不是缺失字段。
    if (ctx.has_prefix_account) {
        provider_snapshot["prefixAccount"] = nlohmann::json{
            {"cacheEpoch", static_cast<std::uint64_t>(ctx.cache_epoch)},
            {"appendOnly", ctx.prefix_append_only},
            {"epochBreakReason", ctx.epoch_break_reason},
            {"systemHash", ctx.system_hash},
            {"toolsHash", ctx.tools_hash},
        };
    }
    // V3-REAL-06(最小可验修复):prepared 的 input_refs 指向链上消息原文,
    // 而实际发送的 request.messages 是 loop 定形的副本——同批工具结果可能
    // 合批进一条 User 容器、compact 存档头被收编进 system、图片按 artifact
    // 重灌。旧账只记引用,离线重放拿引用拼出来的可能是另一份正文。这里给
    // prepared 行补一册"实际发送视图"的指纹账:消息数与链引用数对账、逐
    // 消息指纹(role+块序+正文投影)、system 指纹。指纹一致 = 引用还原可
    // 逐块核对;divergent=true = 视图定形改了表示(合批/收编),按事实记
    // 账,不假装引用即正文。"先提交采用视图再备请求"的完整链路涉及消息
    // 主轴重排,不在本单内。
    {
        nlohmann::json view = nlohmann::json{{"messageCount", request.messages.size()},
                                             {"chainRefCount", input_refs.size()},
                                             {"divergent", request.messages.size() != input_refs.size()}};
        nlohmann::json fingerprints = nlohmann::json::array();
        for (const auto& message : request.messages) {
            fingerprints.push_back(RequestMessageViewFingerprint(message));
        }
        view["messageFingerprints"] = std::move(fingerprints);
        view["systemFingerprint"] = agent::Fingerprint64(request.system);
        provider_snapshot["inputView"] = std::move(view);
    }
    if (!request.tools.empty()) {
        nlohmann::json tools = nlohmann::json::array();
        for (const api::ToolDefinition& tool : request.tools) {
            tools.push_back(tool.name);
        }
        provider_snapshot["toolNames"] = std::move(tools);
    }
    const auto receipt = v3_writer_->PrepareRequest(
        request_id, turn_id_, step_id, "conversation",
        v3_writer_->context().system_message_ref, input_refs, std::move(provider_snapshot),
        std::nullopt, trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.request.prepared");
        return std::string();  // §4.4:prepared 记不住,不发模型
    }
    V3TurnBooks::Request book;
    book.step_id = step_id;
    book.model = request.model;
    v3_turn_->requests.emplace(request_id, std::move(book));
    return request_id;
}

bool TrajectoryTurnBridge::V3RequestSent(const std::string& request_id) {
    const auto it = v3_turn_->requests.find(request_id);
    if (it == v3_turn_->requests.end()) {
        return true;  // prepared 没落稳的请求,sent 不伪造;也不拦(账已如实)
    }
    v3::EventDraft draft;
    draft.kind = v3::EventKindV3::ModelRequestSent;
    draft.status = v3::OpStatus::Done;
    draft.request_id = request_id;
    draft.turn_id = turn_id_;
    draft.step_id = it->second.step_id;
    // 语义三段分清(P1-C/FA-03):prepared = 准备发送;本事件 = 本地交给
    // transport(deliveryScope 钉死 local_transport,不暗示远端收据——服务
    // 端确认要看 model.response.* 的事件);响应各事件才是远端事实。
    draft.payload = nlohmann::json{{"channel", identity_.channel},
                                   {"deliveryScope", "local_transport"}};
    const auto receipt =
        v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.request.sent");
        return false;  // 发送前写账硬闸:本地账写不动,不发模型
    }
    return true;
}

void TrajectoryTurnBridge::V3ResponseStarted(const std::string& request_id) {
    V3EnsureStreamStarted(request_id);
}

bool TrajectoryTurnBridge::V3EnsureStreamStarted(const std::string& request_id) {
    const auto it = v3_turn_->requests.find(request_id);
    if (it == v3_turn_->requests.end()) {
        return false;  // prepared 没落稳的请求,流不伪造
    }
    V3TurnBooks::Request& book = it->second;
    if (book.stream_started) {
        return true;  // 幂等:重放的 MessageStart 不重复起流
    }
    book.stream_id = v3_writer_->NewStreamId();
    book.reserved_message_id = v3_writer_->NewMessageId();
    const auto receipt =
        v3_writer_->BeginStreamResponse(request_id, book.stream_id, turn_id_, book.step_id,
                                        book.reserved_message_id,
                                        trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.response.started");
        return false;  // started 记不住,终态定稿不得越过(§4.4 同款栅栏)
    }
    book.stream_started = true;
    return true;
}

void TrajectoryTurnBridge::V3StreamDelta(const std::string& request_id,
                                         const std::string& delta_type, const std::string& text) {
    const auto it = v3_turn_->requests.find(request_id);
    if (it == v3_turn_->requests.end() || text.empty()) {
        return;  // 簿没有的请求不记;空片段不造批次
    }
    // 片段到即流已开始:懒起流(防 OnStreamDelta 先于 OnResponseStarted 的
    // 装配抖动,批次才有归属)。
    if (!V3EnsureStreamStarted(request_id)) {
        return;  // started 记不住,片段无处归属,不攒
    }
    V3TurnBooks::Request& book = it->second;
    const bool reasoning = delta_type == "reasoning";
    std::string& batch = reasoning ? book.batch_reasoning : book.batch_text;
    std::string& received = reasoning ? book.received_reasoning : book.received_text;
    received += text;
    batch += text;
    // 攒批(§4.43"片段以事件分批落盘"):批满即落,不足一批的尾巴由终态
    // 前统一放行。
    if (batch.size() >= kV3StreamBatchBytes) {
        V3FlushStreamBatch(request_id, reasoning);
    }
}

void TrajectoryTurnBridge::V3FlushStreamBatch(const std::string& request_id, bool reasoning) {
    const auto it = v3_turn_->requests.find(request_id);
    if (it == v3_turn_->requests.end()) {
        return;
    }
    V3TurnBooks::Request& book = it->second;
    std::string& batch = reasoning ? book.batch_reasoning : book.batch_text;
    if (batch.empty() || !book.stream_started) {
        return;  // 空批不落;流没起过就没有可归属的片段
    }
    const auto receipt = v3_writer_->AppendStreamDelta(
        request_id, book.stream_id, book.reserved_message_id, book.delta_seq + 1,
        reasoning ? "reasoning" : "text", nlohmann::json{{"text", batch}},
        trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.response.delta");
        return;  // 片段是观察账:记错不拦流,终态仍是栅栏
    }
    ++book.delta_seq;
    batch.clear();
}

void TrajectoryTurnBridge::V3FlushStreamBatches(const std::string& request_id) {
    V3FlushStreamBatch(request_id, /*reasoning=*/true);
    V3FlushStreamBatch(request_id, /*reasoning=*/false);
}

void TrajectoryTurnBridge::V3UsageRecorded(const std::string& request_id, const api::Usage& usage,
                                            bool reported_by_provider,
                                            const std::string& provider_response_id) {
    // §4.12:usage 的唯一 owner 是 assistant message(键必现,缺实报 null
    // 不补 0)。消息未成行 → 暂存进请求簿,OnOutputCompleted 落行时一并
    // 写;消息已成行后到的补报 → model.usage.appended 单独立账。
    const auto it = v3_turn_->requests.find(request_id);
    if (it == v3_turn_->requests.end()) {
        return;
    }
    if (it->second.output_committed) {
        v3::EventDraft draft;
        draft.kind = v3::EventKindV3::ModelUsageAppended;
        draft.request_id = request_id;
        draft.turn_id = turn_id_;
        draft.step_id = it->second.step_id;
        draft.payload = nlohmann::json{
            {"usage", reported_by_provider ? UsageToJson(usage) : nlohmann::json(nullptr)},
            {"reportedByProvider", reported_by_provider}};
        if (!provider_response_id.empty()) {
            draft.payload["providerResponseId"] = provider_response_id;
        }
        const auto receipt =
            v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
        V3NotifyCommitted(receipt);
        if (receipt.status != v3::WriteReceipt::Status::Committed) {
            NoteV3Error(receipt, "model.usage.appended");
        }
        return;
    }
    it->second.usage = reported_by_provider ? std::optional<nlohmann::json>(UsageToJson(usage))
                                            : std::nullopt;
}

bool TrajectoryTurnBridge::V3OutputCompleted(const std::string& request_id,
                                             const api::Message& assistant,
                                             const std::string& stop_reason,
                                             const std::string& provider_response_id) {
    const auto it = v3_turn_->requests.find(request_id);
    if (it == v3_turn_->requests.end() || it->second.output_committed) {
        return false;  // 请求簿没有/已收口:不重复成行
    }
    V3TurnBooks::Request& req = it->second;
    // 流式三件套(§4.43,D1):started 懒起(非流式后端零片段,同样保
    // started+completed+assistant 的闭环形状)→ 放行攒批尾巴 → 收齐定稿
    //(completed 事件 + 完整 assistant 以预留 id 成行 + 接纳,writer 一手包)。
    if (!V3EnsureStreamStarted(request_id)) {
        return false;  // started 记不住,不定稿——校验器只认三件套闭环
    }
    V3FlushStreamBatches(request_id);
    // 消息体:content(文本块)+ tool_calls(openai 形状——EffectiveConversation
    // FromV3 折叠认的形状,provider 号随块留档);thinking 块原样保(§4.42)。
    nlohmann::json content = nlohmann::json::array();
    nlohmann::json tool_calls = nlohmann::json::array();
    for (const auto& block : assistant.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            content.push_back(nlohmann::json{{"type", "text"}, {"text", text->text}});
        } else if (const auto* thinking = std::get_if<api::ThinkingBlock>(&block)) {
            content.push_back(nlohmann::json{{"type", "thinking"}, {"text", thinking->text}, {"signature", thinking->signature}, {"responses_item", thinking->responses_item}});
        } else if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            if (call->id.empty()) {
                continue;  // 空 id 的 call 不入账(与 v2 calls_ 造册口同款)
            }
            tool_calls.push_back(nlohmann::json{
                {"id", call->id},
                {"type", "function"},
                {"function", nlohmann::json{{"name", call->name},
                                            {"arguments", call->input.dump()}}}});
        }
    }
    nlohmann::json body = nlohmann::json{{"role", "assistant"}, {"content", std::move(content)}};
    if (!tool_calls.empty()) {
        body["tool_calls"] = std::move(tool_calls);
    }
    // 来源三件套(§4.44):本次实际出站的 provider/wire/model,不从会话
    // 当前设置倒推;responseModel 服务端没报就 null,不冒认。length 截断
    // 给 completion_status=truncated(§4.43)。
    const auto receipt = v3_writer_->CompleteStreamResponse(
        request_id, req.stream_id, turn_id_, req.step_id, req.reserved_message_id,
        std::move(body), identity_.provider, identity_.wire, req.model,
        provider_response_id.empty() ? nlohmann::json(nullptr)
                                     : nlohmann::json(provider_response_id),
        req.usage.has_value() ? *req.usage : nlohmann::json(nullptr),
        stop_reason.empty() ? std::string("end_turn") : stop_reason,
        v3::MessagePurpose::Conversation, std::nullopt,
        stop_reason == "length" || stop_reason == "max_tokens"
            ? std::optional<v3::CompletionStatus>(v3::CompletionStatus::Truncated)
            : std::nullopt,
        trajectory::Durability::PowerLoss, &req.completed_event_id);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.response.completed");
        return false;  // §7.4:输出记不住,不执行工具
    }
    req.output_committed = true;
    // 声明本份输出的 tool call(§6.1 的 v3 版):actionId 由 writer 发号,
    // provider 号原样留档作配对键(FoldToolActions 按 providerToolCallId
    // 映射回 actionId)。这是 v3_turn_->calls 造册的唯一合法入口;同时
    // 登记进会话共享账,子代理五步的 parentActionRef 从这查。
    for (const auto& block : assistant.content) {
        if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            if (call->id.empty()) {
                continue;
            }
            V3TurnBooks::Call& book = v3_turn_->calls[call->id];
            book.action_id = v3_writer_->NewActionId();
            book.request_id = request_id;
            book.step_id = it->second.step_id;
            book.assistant_message_ref = receipt.id;
            V3SessionBooks::DeclaredAction& declared = v3_books_->declared_actions[call->id];
            declared.action_id = book.action_id;
            declared.message_id = receipt.id;
            declared.turn_id = turn_id_;
            declared.step_id = book.step_id;
        }
    }
    return true;
}

void TrajectoryTurnBridge::V3OutputFailed(const std::string& request_id, const std::string& reason) {
    const auto it = v3_turn_->requests.find(request_id);
    v3::EventDraft draft;
    draft.kind = v3::EventKindV3::ModelResponseFailed;
    draft.status = v3::OpStatus::Failed;
    draft.request_id = request_id;
    draft.turn_id = turn_id_;
    if (it != v3_turn_->requests.end()) {
        draft.step_id = it->second.step_id;
    }
    draft.payload = nlohmann::json{{"reason", reason}};
    const auto receipt =
        v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.response.failed");
    }
}

void TrajectoryTurnBridge::V3OutputCancelled(const std::string& request_id,
                                             agent::OutputCancelSource source) {
    // 流中断路(§4.63,D1):流已起过的请求,已收内容按 writer 既有合同定稿
    // 成 interrupted assistant——cancelled 定稿事件(带接收水位)+ 正式
    // message(预留 id 成行,completionStatus=interrupted)+ 接纳;未收齐
    // 的调用/签名不伪造完整,usage 缺实报为 null 不补 0。定稿后迟到的
    // usage 走 model.usage.appended(V3UsageRecorded 的 appended 路)。
    const auto it = v3_turn_->requests.find(request_id);
    if (it != v3_turn_->requests.end() && it->second.stream_started) {
        V3TurnBooks::Request& book = it->second;
        if (book.output_committed) {
            return;  // 已收口(理论不到):不造第二终态
        }
        V3FlushStreamBatches(request_id);
        nlohmann::json content = nlohmann::json::array();
        if (!book.received_reasoning.empty()) {
            content.push_back(nlohmann::json{{"type", "thinking"}, {"text", book.received_reasoning}});
        }
        if (!book.received_text.empty()) {
            content.push_back(nlohmann::json{{"type", "text"}, {"text", book.received_text}});
        }
        nlohmann::json body = nlohmann::json{{"role", "assistant"}, {"content", std::move(content)}};
        const auto receipt = v3_writer_->InterruptStreamResponse(
            request_id, book.stream_id, turn_id_, book.step_id, book.reserved_message_id,
            std::move(body), identity_.provider, identity_.wire, book.model,
            /*received_through=*/book.delta_seq, book.usage);
        V3NotifyCommitted(receipt);
        if (receipt.status != v3::WriteReceipt::Status::Committed) {
            NoteV3Error(receipt, "model.response.cancelled");
            return;
        }
        book.output_committed = true;
        return;
    }
    // 流没起过的请求(发出即取消,一个片段没收到):裸 cancelled 事件,不
    // 伪造流不伪造 assistant。取消来源说真话(§4.2 纪律的 v3 版):真按键
    // 才记 user_interrupt。
    v3::EventDraft draft;
    draft.kind = v3::EventKindV3::ModelResponseCancelled;
    draft.status = v3::OpStatus::Cancelled;
    draft.request_id = request_id;
    draft.turn_id = turn_id_;
    if (it != v3_turn_->requests.end()) {
        draft.step_id = it->second.step_id;
    }
    draft.payload = nlohmann::json{{"reason", agent::OutputCancelSourceText(source)}};
    const auto receipt =
        v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.response.cancelled");
    }
}

void TrajectoryTurnBridge::V3ToolTrace(const agent::ToolTraceEvent& event) {
    std::lock_guard lock(*v3_books_->tool_results_mutex);
    // ownership 门(P0-D 的 v3 版):只认模型输出声明过的 call。陌生
    // tool_use_id 只进有界诊断投影,不造册、不推进操作账。
    const auto it = v3_turn_->calls.find(event.tool_use_id);
    if (it == v3_turn_->calls.end()) {
        NoteUnownedToolTrace(event);
        return;
    }
    V3TurnBooks::Call& book = it->second;
    const auto admit = [&]() {
        book.action = v3::ToolActionSession::Admit(
            *v3_writer_, turn_id_, book.step_id, book.action_id, "queued",
            book.assistant_message_ref, event.tool_use_id,
            nlohmann::json{{"toolName", event.tool_name}}, trajectory::Durability::ProcessCrash);
        if (!book.action->last_event_id().has_value()) {
            const std::string note = "tool.execution.pending:" + book.action_id;
            recent_errors_.push_back(note);
            if (error_sink_ != nullptr) {
                error_sink_->push_back(note);
            }
            platform::LogSink::Instance().Error("trajectory", "v3 落账失败: " + note);
        }
    };
    switch (event.kind) {
        case agent::ToolTraceEventKind::Scheduled: {
            if (book.action.has_value()) {
                return;  // pending 已落,批次重放不重复接纳
            }
            admit();
            return;
        }
        case agent::ToolTraceEventKind::ExecutionStarted: {
            if (!book.action.has_value()) {
                admit();  // 没等 Scheduled 直接 started:补接纳,不拒真执行
            }
            // effectiveArgsRef:入参快照的稳定引用(§4.15)。本棒以参数摘要
            // 指纹代快照文件——参数原文已在 assistant 调用块留档,不重复
            // 存 blob;引用形状合法(非空 string),链上可追。
            const std::string args_ref =
                "args-" + book.action_id + "-" + event.effective_input_sha256;
            v3::ToolIdentity identity;
            identity.logical_name = event.tool_name;
            identity.registration_source = agent::ToString(event.source_kind);
            nlohmann::json extra = nlohmann::json{{"toolName", event.tool_name}};
            if (!event.batch_id.empty()) {
                extra["batchId"] = event.batch_id;
                extra["positionInBatch"] = static_cast<std::uint64_t>(
                    event.sequence_in_batch >= 0 ? event.sequence_in_batch : 0);
            }
            const auto receipt = book.action->Start(*v3_writer_, args_ref, std::move(identity),
                                                    std::nullopt, std::move(extra),
                                                    trajectory::Durability::PowerLoss);
            V3NotifyCommitted(receipt);
            if (receipt.status == v3::WriteReceipt::Status::Committed) {
                book.started = true;
            } else {
                NoteV3Error(receipt, "tool.execution.started");
                started_io_failed_.insert(event.execution_id);
            }
            // 存储门(§12.2 同款):副作用工具在 started 落稳后问一次 reserve。
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
            if (book.terminal || !book.action.has_value()) {
                return;  // 终态唯一;没接纳过的执行不收口(不造无主账)
            }
            const bool never_started = !book.started;
            const std::uint64_t duration_value = static_cast<std::uint64_t>(
                event.duration_ms > 0 ? event.duration_ms : 0);
            v3::WriteReceipt receipt;
            if (event.outcome == agent::ToolOutcome::Succeeded) {
                std::optional<std::int64_t> exit_code;
                if (event.details.contains("exit_code") &&
                    event.details.at("exit_code").is_number_integer()) {
                    exit_code = event.details.at("exit_code").get<std::int64_t>();
                }
                receipt = book.action->Finish(
                    *v3_writer_, exit_code,
                    event.duration_ms > 0 ? std::optional<std::uint64_t>(duration_value)
                                          : std::nullopt,
                    trajectory::Durability::PowerLoss);
            } else if (event.outcome == agent::ToolOutcome::UnknownAfterStart) {
                receipt = book.action->MarkUnknown(
                    *v3_writer_,
                    event.error_code.empty() ? "unknown_after_start" : event.error_code,
                    trajectory::Durability::PowerLoss);
            } else if (never_started || OutcomeMapsToCancelled(event)) {
                // 闸前被收掉/拦下:没越过执行边界,落 cancelled,不冒充执行
                // 过(§6.2 约束 16 的 v3 版)。
                receipt = book.action->Cancel(
                    *v3_writer_, never_started ? "before_started" : "during_execution",
                    event.error_code.empty() ? agent::ToString(event.outcome) : event.error_code,
                    trajectory::Durability::PowerLoss);
            } else {
                receipt = book.action->Fail(
                    *v3_writer_,
                    event.error_code.empty() ? agent::ToString(event.outcome) : event.error_code,
                    event.duration_ms > 0 ? std::optional<std::uint64_t>(duration_value)
                                          : std::nullopt,
                    trajectory::Durability::PowerLoss);
            }
            V3NotifyCommitted(receipt);
            if (receipt.status == v3::WriteReceipt::Status::Committed) {
                book.terminal = true;
                book.terminal_event_id = receipt.id;
                if (event.outcome != agent::ToolOutcome::Succeeded &&
                    event.outcome != agent::ToolOutcome::UnknownAfterStart &&
                    !(never_started || OutcomeMapsToCancelled(event))) {
                    book.failed = true;
                }
                // §5.5 stale invalidation 的 v3 版(T11-D):改动了文件就逐枚
                // 对账本轮已录验证,subject 命中的落 tool.verification.
                // invalidated(reason=subject_modified)。只记观察:不改旧
                // recorded 行,也不触发工具重做。
                if (!event.undo.path.empty()) {
                    InvalidateStaleVerificationsV3(event.undo.path, receipt.id);
                }
            } else {
                NoteV3Error(receipt, "tool terminal");
            }
            return;
        }
        case agent::ToolTraceEventKind::Verification: {
            // T11-D:验证事实(tool.verification.recorded)。hub 侧显式验证
            // 点:label 兼作 kind,after_execution_id 挂被验执行(对回
            // actionId 进信封——验证关联工具);正文经 verify_detail 进
            // facts。是否"fresh"由读取侧折算(失效另记 invalidated)。
            v3::EventDraft recorded;
            recorded.kind = v3::EventKindV3::ToolVerificationRecorded;
            recorded.turn_id = turn_id_;
            recorded.step_id = book.step_id;
            recorded.action_id = book.action_id;
            recorded.payload = nlohmann::json{
                {"verificationId", NextVerificationId()},
                {"kind", event.label.empty() ? "tool_postcondition" : event.label},
                {"passed", event.passed},
                {"producer", "tool_trace"}};
            if (!event.after_execution_id.empty()) {
                recorded.payload["subject"] = event.after_execution_id;
            }
            if (!event.verify_detail.empty()) {
                recorded.payload["facts"] = nlohmann::json{{"detail", event.verify_detail}};
            }
            const auto receipt =
                v3_writer_->AppendEvent(std::move(recorded), trajectory::Durability::ProcessCrash);
            V3NotifyCommitted(receipt);
            if (receipt.status != v3::WriteReceipt::Status::Committed) {
                NoteV3Error(receipt, "tool.verification.recorded");
            }
            return;
        }
        case agent::ToolTraceEventKind::McpLateResponse: {
            // T11-D:迟到响应只记观察(tool.observation.late)。本 attempt 已
            // 有终态的,旧终态一个字节不动(tool_action 的"每尝试一个终态"
            // 纪律);本行不带 status,读面只当事实看,不据此重开执行。
            v3::EventDraft late;
            late.kind = v3::EventKindV3::ToolObservationLate;
            late.turn_id = turn_id_;
            late.step_id = book.step_id;
            late.action_id = book.action_id;
            late.payload = nlohmann::json{{"cause", "mcp_timeout_dropped"}};
            if (event.jsonrpc_request_id >= 0) {
                late.payload["jsonrpcRequestId"] = event.jsonrpc_request_id;
            }
            if (!event.source_instance.empty()) {
                late.payload["server"] = event.source_instance;
            }
            const auto receipt =
                v3_writer_->AppendEvent(std::move(late), trajectory::Durability::ProcessCrash);
            V3NotifyCommitted(receipt);
            if (receipt.status != v3::WriteReceipt::Status::Committed) {
                NoteV3Error(receipt, "tool.observation.late");
            }
            return;
        }
        case agent::ToolTraceEventKind::RecoveryMarker: {
            // T11-D:恢复注记(recovery.note.recorded)——恢复侧补的
            // append-only 观察(未知副作用/四档结论的缘由),不改旧行。
            v3::EventDraft note;
            note.kind = v3::EventKindV3::RecoveryNoteRecorded;
            note.turn_id = turn_id_;
            note.step_id = book.step_id;
            note.action_id = book.action_id;
            note.payload = nlohmann::json{{"note", event.note.empty() ? "recovery_marker"
                                                                      : event.note}};
            const auto receipt =
                v3_writer_->AppendEvent(std::move(note), trajectory::Durability::ProcessCrash);
            V3NotifyCommitted(receipt);
            if (receipt.status != v3::WriteReceipt::Status::Committed) {
                NoteV3Error(receipt, "recovery.note.recorded");
            }
            return;
        }
        case agent::ToolTraceEventKind::ResultCommitted:
            return;  // result.committed 从消息正文翻(V3ToolResultsCommitted)。
    }
}

ToolResultsCommitReceipt TrajectoryTurnBridge::CaptureToolResult(const api::ToolResultBlock& result) {
    if (!V3Mode()) return {};
    ToolResultsCommitReceipt outcome;
    const auto fail = [&outcome](std::string code) {
        outcome.status = ToolResultsCommitReceipt::Status::Failed;
        outcome.error_code = std::move(code);
    };
    if (!turn_open_ || v3_turn_ == nullptr) {
        fail("tool.capture.turn_not_open");
        return outcome;
    }
    std::lock_guard lock(*v3_books_->tool_results_mutex);
    const auto found = v3_turn_->calls.find(result.tool_use_id);
    if (found == v3_turn_->calls.end() || !found->second.terminal || !found->second.action.has_value()) {
        fail("tool.capture.missing_terminal");
        return outcome;
    }
    auto& book = found->second;
    if (!book.capture_event_id.empty()) return outcome;
    book.capture_complete = result.capture_complete;
    book.capture_reason = result.capture_reason;
    if (!v3_books_->captures.has_value()) {
        auto store = v3::ResultStore::Open(v3_writer_->path().parent_path(), "capture-");
        if (!store.has_value()) {
            book.capture_failed = true;
            const auto receipt = book.action->PersistFailed(*v3_writer_, "tool.capture.store_unavailable:" + store.error(),
                std::nullopt, trajectory::Durability::PowerLoss);
            V3NotifyCommitted(receipt);
            fail("tool.capture.store_unavailable");
            return outcome;
        }
        v3_books_->captures = std::move(*store);
    }
    v3::ResultStore::PersistRequest capture;
    capture.result_kind = "text";
    capture.content = result.content;
    if (result.structured_content.has_value()) capture.structured_content = *result.structured_content;
    capture.tool_call_id = book.action_id;
    capture.execution_event_ref = book.terminal_event_id;
    capture.preview_policy = {{"policy", "raw-capture-before-post-hook"}};
    capture.outputs.push_back(v3::ResultStore::ChannelOutput{
        "combined", "text/plain", result.content, result.capture_complete, result.capture_reason,
        static_cast<std::uint64_t>(result.content.size()), !result.capture_complete});
    PreserveNativeToolPayload(result, capture);
    const auto stored = v3_books_->captures->Persist(capture);
    if (!stored.ok) {
        book.capture_failed = true;
        const auto receipt = book.action->PersistFailed(*v3_writer_, stored.error, std::nullopt,
                                                       trajectory::Durability::PowerLoss);
        V3NotifyCommitted(receipt);
        fail("tool.capture.persist_failed:" + stored.error);
        return outcome;
    }
    const auto receipt = book.action->PersistedResult(*v3_writer_, stored.result_ref,
        book.terminal_event_id, std::nullopt, trajectory::Durability::PowerLoss);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        book.capture_failed = true;
        NoteV3Error(receipt, "tool.capture.persisted");
        fail("tool.capture.ledger_failed:" + receipt.error_code);
        return outcome;
    }
    book.capture_event_id = receipt.id;
    return outcome;
}

ToolResultsCommitReceipt TrajectoryTurnBridge::RewriteToolResultsForHistory(api::Message& results) {
    if (!V3Mode()) return {};
    if (!turn_open_ || v3_turn_ == nullptr) {
        ToolResultsCommitReceipt receipt;
        receipt.status = ToolResultsCommitReceipt::Status::Failed;
        receipt.error_code = "tool.preview.turn_not_open";
        return receipt;
    }
    return V3ToolResultsCommitted(results);
}

ToolResultsCommitReceipt TrajectoryTurnBridge::V3ToolResultsCommitted(api::Message& results) {
    std::unique_lock lock(*v3_books_->tool_results_mutex);
    if (action_summary_running_) {
        ToolResultsCommitReceipt busy;
        busy.status = ToolResultsCommitReceipt::Status::Failed;
        busy.error_code = "tool.summary.batch_busy";
        return busy;
    }
    // 结果链(§4.18):persisted(结果仓落 artifact)→ selected(选用声明)
    // → tool 消息(模型可见预览正文)→ 接纳进链。
    // Any missing persistence, selection, message or admission receipt fails
    // this batch. A committed preview must have an immutable source to recover.
    ToolResultsCommitReceipt batch;
    for (auto& block : results.content) {
        auto* result = std::get_if<api::ToolResultBlock>(&block);
        if (result == nullptr) {
            continue;
        }
        // 异步工具 P2:批次闸门接单的回执块——协调器的接单链已把
        // started/finished/persisted/selected 与接单 tool 消息全链落稳,
        // 这里跳过,不为同一枚调用重落第二条链。
        if (result->job_admission) {
            continue;
        }
        const auto it = v3_turn_->calls.find(result->tool_use_id);
        if (it != v3_turn_->calls.end() && it->second.tool_message_done) continue;
        if (it == v3_turn_->calls.end() || !it->second.terminal || !it->second.action.has_value()) {
            batch.status = ToolResultsCommitReceipt::Status::Failed;
            batch.error_code = "tool.preview.missing_terminal:" + result->tool_use_id;
            continue;
        }
        V3TurnBooks::Call& book = it->second;
        if (book.capture_failed) {
            batch.status = ToolResultsCommitReceipt::Status::Failed;
            batch.error_code = "tool.capture.failed:" + result->tool_use_id;
            continue;
        }
        if (book.capture_complete.has_value()) {
            result->capture_complete = *book.capture_complete;
            result->capture_reason = book.capture_reason;
        }
        const auto hard_fail = [&batch](const char* where, const std::string& code) {
            if (batch.status != ToolResultsCommitReceipt::Status::Failed) {
                batch.status = ToolResultsCommitReceipt::Status::Failed;
                batch.error_code = std::string(where) + ":" + code;
            }
        };
        // 结果仓:原文按 artifact 不可变落档(§4.16)。
        if (!v3_books_->results.has_value()) {
            if (auto store = v3::ResultStore::Open(v3_writer_->path().parent_path());
                store.has_value()) {
                v3_books_->results = std::move(*store);
            }
        }
        const std::string execution_event_ref = book.terminal_event_id;
        std::string persisted_event_id;
        std::optional<std::string> summary_event_ref;
        if (v3_books_->results.has_value()) {
            v3::ResultStore::PersistRequest persist;
            persist.result_kind = "text";
            persist.content = result->content;
            if (result->structured_content.has_value()) {
                persist.structured_content = *result->structured_content;
            }
            persist.execution_event_ref = execution_event_ref;
            persist.tool_call_id = book.action_id;
            const auto budget = std::min<std::uint64_t>(32768, std::min<std::uint64_t>(result->preview_budget_bytes, v3_writer_->context().preview_budget_bytes));
            persist.preview_policy = {{"policy", "v3-tool-preview"}, {"maxPreviewBytes", budget}, {"budgetsLadder", nlohmann::json::array({32768, 16384, 8192, 4096})}};
            persist.outputs.push_back(v3::ResultStore::ChannelOutput{
                "combined", "text/plain", result->content, result->capture_complete, result->capture_reason,
                static_cast<std::uint64_t>(result->content.size()), !result->capture_complete});
            PreserveNativeToolPayload(*result, persist);
            const auto persisted = v3_books_->results->Persist(persist);
            if (persisted.ok) {
                const auto receipt = book.action->PersistedResult(
                    *v3_writer_, persisted.result_ref, execution_event_ref, std::nullopt,
                    trajectory::Durability::PowerLoss);
                V3NotifyCommitted(receipt);
                if (receipt.status == v3::WriteReceipt::Status::Committed) {
                    persisted_event_id = receipt.id;
                    if (result->action_summary_requested && action_summary_backend_ != nullptr) {
                        ActionSummarySource source;
                        source.action_id = book.action_id;
                        source.parent_turn_id = book.action->turn_id();
                        source.persisted_event_ref = persisted_event_id;
                        if (!book.capture_event_id.empty()) source.source_result_event_refs.push_back(book.capture_event_id);
                        source.source_result_event_refs.push_back(persisted_event_id);
                        source.attempt = book.action->attempt();
                        source.execution_started = book.action->started();
                        source.result_refs = persisted.result_ref;
                        source.text = result->content;
                        switch (book.action->terminal()) {
                            case v3::ToolActionSession::Terminal::Finished: source.execution_state = "done"; break;
                            case v3::ToolActionSession::Terminal::Failed: source.execution_state = "failed"; break;
                            case v3::ToolActionSession::Terminal::Cancelled: source.execution_state = "cancelled"; break;
                            case v3::ToolActionSession::Terminal::Rejected: source.execution_state = "rejected"; break;
                            default: source.execution_state = "unknown"; break;
                        }
                        source.capture_complete = result->capture_complete;
                        source.capture_reason = result->capture_reason;
                        source.budget_bytes = budget;
                        const auto generation = action_summary_generation_;
                        const auto profile = action_summary_profile_;
                        auto* backend = action_summary_backend_;
                        auto* turn_identity = v3_turn_.get();
                        const auto* book_identity = &book;
                        const auto tool_use_id = result->tool_use_id;
                        int remaining = std::exchange(action_summary_calls_remaining_, 0);
                        action_summary_running_ = true;
                        lock.unlock();
                        ActionSummaryResult summary;
                        try {
                            summary = SummarizeActionResult(*v3_writer_, *backend, profile, source, remaining);
                        } catch (...) {
                            lock.lock();
                            action_summary_running_ = false;
                            throw;
                        }
                        lock.lock();
                        action_summary_running_ = false;
                        if (generation != action_summary_generation_ || v3_turn_.get() != turn_identity ||
                            v3_turn_->calls.find(tool_use_id) == v3_turn_->calls.end() ||
                            &v3_turn_->calls.at(tool_use_id) != book_identity) {
                            batch.status = ToolResultsCommitReceipt::Status::Failed;
                            batch.error_code = "tool.summary.source_scope_changed";
                            return batch;
                        }
                        action_summary_calls_remaining_ += remaining;
                        if (summary.persistence_failed) {
                            hard_fail("tool.summary.persist_failed", book.action_id);
                            continue;
                        }
                        if (summary.accepted) {
                            result->content = summary.text;
                            summary_event_ref = summary.terminal_event_ref;
                        }
                    }
                    auto request = PreviewFromPersistedMaterials(persist, persisted, budget,
                                                                 v3_writer_->path().parent_path());
                    if (!summary_event_ref && (result->content.size() > budget || !result->capture_complete || persist.outputs.size() > 1)) {
                        auto preview = v3::BuildToolPreview(request);
                        if (preview.listing_overflow) {
                            auto index = v3_books_->results->PersistListing(persisted.result_id + "-output-index.txt", preview.listing_text);
                            if (!index.has_value()) {
                                hard_fail("tool.preview.index_failed", book.action_id);
                                continue;
                            }
                            // output_index 与 full_output 同一追回口径:给模型
                            // 绝对路径,相对账留 result_ref(T17)。
                            request.output_index_path = platform::PathToUtf8(
                                v3_writer_->path().parent_path() / platform::Utf8ToPath(*index));
                            preview = v3::BuildToolPreview(request);
                        }
                        if (preview.preview_unrepresentable || preview.listing_overflow || preview.text.size() > budget) {
                            hard_fail("tool.preview.unrepresentable", book.action_id);
                            continue;
                        }
                        result->content = std::move(preview.text);
                    }
                    // Replace only text payloads; media retains its own accounting.
                    for (auto& payload : result->blocks) {
                        if (auto* text = std::get_if<tools::TextContent>(&payload)) text->text.clear();
                    }
                    if (!result->blocks.empty()) result->blocks.insert(result->blocks.begin(), tools::TextContent{result->content});
                    // Gemini prefers structured_content over text. The immutable
                    // result metadata retains it; runtime must use the adopted
                    // preview, matching the persisted tool message and resume.
                    result->structured_content.reset();

                } else {
                    NoteV3Error(receipt, "tool.result.persisted");
                    // artifact 落稳、ledger 事件没落:正文保住,溯源链缺一节,
                    // 记降级;tool 消息照落(writer 已坏时下一步会翻 Failed)。
                    batch.degraded_codes.push_back("tool.result.persisted_event:" + book.action_id);
                }
            } else {
                // 执行成功而存储失败(§4.18):保留 done,另报持久化失败,
                // 不改称"工具没有执行"。tool 消息照落(Degraded),选不了
                // 原文(persisted 事件没有),resultSelectionRef 缺席如实。
                const auto receipt = book.action->PersistFailed(
                    *v3_writer_, persisted.error, std::nullopt, trajectory::Durability::PowerLoss);
                V3NotifyCommitted(receipt);
                if (receipt.status != v3::WriteReceipt::Status::Committed) {
                    NoteV3Error(receipt, "tool.result.persist_failed");
                }
                batch.degraded_codes.push_back("tool.result.persist_failed:" + book.action_id);
            }
        } else {
            // 结果仓开不了:结果链(persisted→selected)立不起来,不伪造
            // 选用事件(schema 对空 sourceResultEventRefs 一刀拒),tool
            // 消息不落,如实留诊断。回执翻 Failed——请求停在结果边界,不拿
            // 内存独有结果继续发模型(FA-01)。
            const std::string note = "tool.result.store_unavailable:" + book.action_id;
            recent_errors_.push_back(note);
            if (error_sink_ != nullptr) {
                error_sink_->push_back(note);
            }
            platform::LogSink::Instance().Error("trajectory", "v3 落账失败: " + note);
            hard_fail("tool.result.store_unavailable", book.action_id);
            continue;
        }
        if (persisted_event_id.empty()) {
            hard_fail("tool.result.not_persisted", book.action_id);
            continue;
        }
        // 结果选用(§4.23):无改写也明确选择原结果(sourceResultEventRefs
        // 须非空——persisted 事件落稳才有得选)。effectiveOutcome 以回喂
        // 结果的 is_error 为准(P1-B/FA-02)——Hook 处理后真正交给模型的
        // 语义,不从原始执行终态猜。
        std::string selected_event_id;
        std::vector<std::string> source_result_event_refs;
        if (!book.capture_event_id.empty()) source_result_event_refs.push_back(book.capture_event_id);
        source_result_event_refs.push_back(persisted_event_id);
        if (!persisted_event_id.empty()) {
            const auto selected = book.action->SelectResult(
                *v3_writer_, source_result_event_refs, {},
                result->is_error ? "failed" : "done", std::nullopt, trajectory::Durability::PowerLoss,
                summary_event_ref);
            V3NotifyCommitted(selected);
            if (selected.status != v3::WriteReceipt::Status::Committed) {
                NoteV3Error(selected, "tool.result.selected");
                // 选用事件没落:正文保得住(tool 消息照落,引用缺席),记
                // 降级;writer 已坏时下一步翻 Failed。
                batch.degraded_codes.push_back("tool.result.selected_event:" + book.action_id);
            } else {
                selected_event_id = selected.id;
            }
        }
        if (selected_event_id.empty()) {
            hard_fail("tool.result.not_selected", book.action_id);
            continue;
        }
        // 最终 tool 消息:content 是模型可见的正文(runtime 回喂的这份就是
        // 模型将看到的),is_error 随行落档(P1-B:恢复投影从消息本体还原
        // 回喂语义,不从执行终态猜);resultSelectionRef 指回选用事件。
        const auto message = book.action->AppendToolMessage(
            *v3_writer_, result->content,
            selected_event_id.empty() ? std::optional<std::string>{}
                                      : std::optional<std::string>(selected_event_id),
            result->is_error, trajectory::Durability::PowerLoss);
        V3NotifyCommitted(message);
        if (message.status != v3::WriteReceipt::Status::Committed) {
            // 回执是接纳事件:消息本体可能已成行而链没接上——两种形状恢复
            // 投影都按缺口列(reader 的 message_not_admitted),这里统一翻
            // Failed,主循环停发(不得冒充有效上下文)。
            NoteV3Error(message, "tool message");
            hard_fail("tool.message", message.error_code.empty() ? book.action_id : message.error_code);
            continue;
        }
        book.tool_message_done = true;
        result->preview_committed = true;
    }
    if (batch.status == ToolResultsCommitReceipt::Status::Committed && !batch.degraded_codes.empty()) {
        batch.status = ToolResultsCommitReceipt::Status::Degraded;
    }
    return batch;
}

void TrajectoryTurnBridge::V3CancelDanglingActions(const std::string& reason) {
    for (auto& entry : v3_turn_->calls) {
        V3TurnBooks::Call& book = entry.second;
        if (book.terminal || !book.action.has_value()) {
            continue;
        }
        const auto receipt = book.action->Cancel(
            *v3_writer_, book.started ? "during_execution" : "before_started", reason,
            trajectory::Durability::PowerLoss);
        V3NotifyCommitted(receipt);
        if (receipt.status == v3::WriteReceipt::Status::Committed) {
            book.terminal = true;
        } else {
            NoteV3Error(receipt, "tool.execution.cancelled(dangling)");
        }
    }
}

bool TrajectoryTurnBridge::ShouldBlockExecution(const agent::ToolTraceEvent& started) {
    return started_io_failed_.count(started.execution_id) != 0 ||
           storage_blocked_.count(started.execution_id) != 0;
}

// ---- 异步工具 P2:闸门/规划器的账面查询口 ---------------------------------

std::optional<TrajectoryTurnBridge::V3CallOrigin> TrajectoryTurnBridge::V3DeclaredCallOrigin(
    const std::string& provider_call_id) const {
    if (!V3Mode() || v3_books_ == nullptr) {
        return std::nullopt;
    }
    const auto it = v3_books_->declared_actions.find(provider_call_id);
    if (it == v3_books_->declared_actions.end()) {
        return std::nullopt;
    }
    V3CallOrigin origin;
    origin.action_id = it->second.action_id;
    origin.message_id = it->second.message_id;
    origin.turn_id = it->second.turn_id;
    origin.step_id = it->second.step_id;
    return origin;
}

std::optional<std::string> TrajectoryTurnBridge::V3ReservedAssistantMessageId(
    const std::string& request_id) const {
    if (!V3Mode() || v3_turn_ == nullptr) {
        return std::nullopt;
    }
    const auto it = v3_turn_->requests.find(request_id);
    if (it == v3_turn_->requests.end() || it->second.reserved_message_id.empty()) {
        return std::nullopt;
    }
    return it->second.reserved_message_id;
}

std::optional<std::string> TrajectoryTurnBridge::V3ResponseEvidenceId(
    const std::string& request_id) const {
    if (!V3Mode() || v3_turn_ == nullptr) {
        return std::nullopt;
    }
    const auto it = v3_turn_->requests.find(request_id);
    if (it == v3_turn_->requests.end() || it->second.completed_event_id.empty()) {
        return std::nullopt;  // 没证据不宣称接纳(单 §5)
    }
    return it->second.completed_event_id;
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
    // 接线点 1:v3 场的父子边由 subagent.spawn.requested/linked 五步自己
    // 记账(parentActionRef → childSessionRef),calls_ 的 child_run_id 是
    // v2 事件 relations 的概念,不再挂。
    if (V3Mode()) {
        return;
    }
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
    // T11-D 的 v3 路:同判定,事件换 tool.verification.invalidated(statusless
    // 观察,不改旧 recorded 行)。
    if (V3Mode()) {
        InvalidateStaleVerificationsV3(mutated, invalidated_by_event);
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

// T11-D:stale invalidation 的 v3 实现——被改文件命中的已录验证逐枚落
// tool.verification.invalidated。被哪枚终态触发经 invalidated_by_action
// 对账(那是 tool.execution.* 的 event id);本行只记观察,不覆盖已提交
// 终态,也不触发工具重做。
void TrajectoryTurnBridge::InvalidateStaleVerificationsV3(const std::string& mutated_path,
                                                          const std::string& invalidated_by_action) {
    for (VerificationBook& book : verifications_) {
        if (!book.recorded || book.invalidated || book.subject.empty()) {
            continue;
        }
        if (NormalizeSubjectPath(book.subject) != mutated_path) {
            continue;
        }
        v3::EventDraft draft;
        draft.kind = v3::EventKindV3::ToolVerificationInvalidated;
        draft.turn_id = turn_id_;
        draft.payload = nlohmann::json{{"verificationId", book.verification_id},
                                       {"reason", "subject_modified"}};
        if (!invalidated_by_action.empty()) {
            draft.payload["invalidatedByAction"] = invalidated_by_action;
        }
        const auto receipt =
            v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
        V3NotifyCommitted(receipt);
        if (receipt.status == v3::WriteReceipt::Status::Committed) {
            book.invalidated = true;
        } else {
            NoteV3Error(receipt, "tool.verification.invalidated");
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
    // T11-D:v3 场起验只建内存簿(号保留),事实在 FinishVerification 一次
    // 成行(tool.verification.recorded)——v3 没有 started 半行,不伪造
    // 生命周期。回空 id 仍是"verification 记不住,不得判 verified success"
    //(§7.4):回合没开/账面坏一律空串。
    if (V3Mode()) {
        if (!turn_open_) {
            return std::string();
        }
        VerificationBook book;
        book.verification_id = NextVerificationId();
        book.kind = kind;
        book.subject = subject;
        book.producer = producer;
        verifications_.push_back(std::move(book));
        return verifications_.back().verification_id;
    }
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
    // T11-D:v3 路:一次成行 tool.verification.recorded。工具关联走信封
    // actionId(当前回合的工具账里按 subject 反查;查不到就不硬塞——
    // subject 不是 action 的验证不冒充工具关联)。产物按 path 引用,
    // command_ref 是判定材料对账。
    if (V3Mode()) {
        std::optional<std::string> action_id;
        if (!target->subject.empty()) {
            for (const auto& [call_id, call] : v3_turn_->calls) {
                if (call.action_id == target->subject || call_id == target->subject) {
                    action_id = call.action_id;
                    break;
                }
            }
        }
        v3::EventDraft draft;
        draft.kind = v3::EventKindV3::ToolVerificationRecorded;
        draft.turn_id = turn_id_;
        draft.action_id = action_id;
        draft.payload = nlohmann::json{
            {"verificationId", verification_id},
            {"kind", target->kind},
            {"passed", passed},
            {"producer", target->producer.empty() ? std::string("host") : target->producer}};
        if (!target->subject.empty()) {
            draft.payload["subject"] = target->subject;
        }
        if (command_ref.is_object() && !command_ref.empty()) {
            draft.payload["commandRef"] = command_ref;
        }
        if (facts.is_object() && !facts.empty()) {
            draft.payload["facts"] = facts;
        }
        nlohmann::json refs = nlohmann::json::array();
        for (const std::string& path : artifact_paths) {
            refs.push_back(nlohmann::json{{"path", path}});
        }
        if (!refs.empty()) {
            draft.payload["artifactRefs"] = std::move(refs);
        }
        const auto receipt =
            v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
        V3NotifyCommitted(receipt);
        if (receipt.status == v3::WriteReceipt::Status::Committed) {
            target->recorded = true;
            target->passed = passed;
            target->recorded_event_id = receipt.id;
        } else {
            NoteV3Error(receipt, "tool.verification.recorded");
        }
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
    payload["observed_after_seq"] = recorder_->next_seq() - 1;
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
    // v3 模式同一道门,量的是 v3 主账所在目录。
    if (V3Mode()) {
        return trajectory::HasDiskReserve(v3_writer_->path().parent_path(),
                                          kJournalEmergencyReserveBytes);
    }
    return trajectory::HasDiskReserve(recorder_->stream_path().parent_path(),
                                      kJournalEmergencyReserveBytes);
}


}  // namespace lubancode::runtime
