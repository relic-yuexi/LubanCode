// 轨迹旁路模型请求桥的实现(AR-12 机械拆分:自 trajectory_session.cpp
// 按桥类边界拆出,方法体一字未动;合同见 trajectory_bypass_bridge.hpp)。

#include "runtime/trajectory_bypass_bridge.hpp"

#include <utility>

#include "platform/log_sink.hpp"
#include "runtime/trajectory_bridge_internal.hpp"  // 与主桥共用的事实构造(口径只此一处)

namespace lubancode::runtime {

// trajectory v3 简称(本件内 v3:: 一律指 trajectory::v3;runtime 命名空间
// 下裸写 v3:: 解析不到 trajectory::v3)。
namespace v3 = ::lubancode::trajectory::v3;

// 原实现文件顶部的类型简称集随段迁移(与主桥同款)。
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

}  // namespace

// ---------------------------------------------------------------------------
// TrajectoryBypassBridge(Token 账本单 A1)
// ---------------------------------------------------------------------------

TrajectoryBypassBridge::TrajectoryBypassBridge(trajectory::TrajectoryRecorder& recorder,
                                               trajectory::EventScope base_scope,
                                               TrajectoryTurnBridge::Identity identity)
    : recorder_(&recorder), base_scope_(std::move(base_scope)), identity_(std::move(identity)) {}

TrajectoryBypassBridge::TrajectoryBypassBridge(v3::V3Writer* v3_writer, V3SessionBooks* v3_books,
                                               trajectory::EventScope identity_scope,
                                               TrajectoryTurnBridge::Identity identity,
                                               accounting::RequestPurpose purpose)
    : v3_writer_(v3_writer), v3_books_(v3_books), purpose_(purpose), base_scope_(std::move(identity_scope)),
      identity_(std::move(identity)) {}

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
    const trajectory::RecordReceipt receipt = recorder_->Record(std::move(request), durability);
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
    if (V3Mode()) {
        return V3RequestPrepared(request, ctx);
    }
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

bool TrajectoryBypassBridge::OnRequestSent(const std::string& request_id) {
    if (V3Mode()) {
        return V3RequestSent(request_id);
    }
    const auto it = request_prepared_.find(request_id);
    if (it == request_prepared_.end()) {
        return true;  // 簿里没有(prepared 没落稳):不拦,账早已如实
    }
    const auto receipt =
        Put(EventKind::ModelRequestSent, request_id, Actor::Host, Origin::ScheduledHost,
            nlohmann::json{{"prepared_event_id", it->second}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.request.sent(bypass)");
        return false;  // P1-C/FA-03:sent 记不住,采样停在发送边界
    }
    return true;
}

void TrajectoryBypassBridge::OnUsageRecorded(const std::string& request_id, const api::Usage& usage,
                                             bool reported_by_provider,
                                             const std::string& provider_response_id, int cache_epoch,
                                             bool prefix_append_only, bool cache_read_reported_by_provider,
                                             bool cache_creation_reported_by_provider,
                                             const std::string& usage_anomaly) {
    if (V3Mode()) {
        (void)cache_epoch;
        (void)prefix_append_only;
        (void)cache_read_reported_by_provider;
        (void)cache_creation_reported_by_provider;
        (void)usage_anomaly;
        V3UsageRecorded(request_id, usage, reported_by_provider, provider_response_id);
        return;
    }
    // 读/写明报位分开落,异常账非空才落(与主桥同一条,C2/C4)。
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
    if (V3Mode()) {
        return V3OutputCompleted(request_id, assistant, stop_reason, provider_response_id);
    }
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
    if (V3Mode()) {
        V3OutputFailed(request_id, reason);
        return;
    }
    const auto receipt =
        Put(EventKind::ModelOutputFailed, request_id, Actor::Model, Origin::ProviderModel,
            nlohmann::json{{"reason", reason.empty() ? "failed" : reason}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.failed(bypass)");
    }
    CloseTurn(false, false, reason);
}

void TrajectoryBypassBridge::OnOutputCancelled(const std::string& request_id, agent::OutputCancelSource source) {
    if (V3Mode()) {
        V3OutputCancelled(request_id, source);
        return;
    }
    // 同 §4.2:旁路取消也按真实来源落名(旧的泛名 "cancelled" 退役;字段
    // 类型不变,旧 stream 照读)。
    const auto receipt =
        Put(EventKind::ModelOutputCancelled, request_id, Actor::Model, Origin::ProviderModel,
            nlohmann::json{{"reason", agent::OutputCancelSourceText(source)}}, Durability::ProcessCrash);
    if (receipt.status != RecordReceipt::Status::Committed) {
        NoteError(receipt, "model.output.cancelled(bypass)");
    }
    CloseTurn(false, true, "cancelled");
}

// ---------------------------------------------------------------------------
// TrajectoryBypassBridge 的 v3 写模式(取消误报 ESC 单 Bug 2):旁路请求
// 走 v3 typed 事件合同——system/转写 user 落消息行(purpose 按用途,不进
// conversation 链),prepared/sent/终态/usage 落事件行。一桥一采样;内部
// 回合号 memory-turn-<n>,parentTurnId 挂触发它的主回合,请求号走 writer
// 全局池(request-<n>,与主回合请求同一发号器不撞号)。
// ---------------------------------------------------------------------------

void TrajectoryBypassBridge::NoteV3Error(const v3::WriteReceipt& receipt, const char* where) {
    const std::string note = std::string(where) + ":" + receipt.error_code +
                             (receipt.error_message.empty() ? std::string()
                                                            : " (" + receipt.error_message + ")");
    recent_errors_.push_back(note);
    if (error_sink_ != nullptr) {
        error_sink_->push_back(note);
    }
    platform::LogSink::Instance().Error("trajectory", "v3 旁路落账失败: " + note);
}

void TrajectoryBypassBridge::V3NotifyCommitted(const v3::WriteReceipt& receipt) {
    if (receipt.status == v3::WriteReceipt::Status::Committed && commit_wake_ != nullptr) {
        telemetry::CommitWake wake;
        wake.workspace_key = base_scope_.workspace_key;
        wake.session_id = base_scope_.session_id;
        wake.stream_id = wake_stream_id_;
        commit_wake_->Notify(wake);
    }
}

std::string TrajectoryBypassBridge::V3RequestPrepared(const api::Request& request,
                                                      const agent::RequestPreparedContext& ctx) {
    // 桥只认 memory_extract / title_refine:工厂(NewBypassBridge)已把门,
    // 这里是防御性第二道——消息 purpose/回合号铺法都按用途分,别的用途
    // 进来只会写错账。
    const v3::MessagePurpose message_purpose = V3MessagePurpose();
    if (purpose_ != accounting::RequestPurpose::MemoryExtract &&
        purpose_ != accounting::RequestPurpose::TitleRefine) {
        return std::string();
    }
    // 一桥一采样:v2 用小 turn 的开合守门,v3 没有轮账,用请求簿守。
    if (!v3_requests_.empty()) {
        return std::string();
    }
    // T12-A 同门:compact 换账失败的场,旁路请求也不放行——不发新模型
    // 请求,空串即"prepared 记不住"的既有语义。
    if (v3_books_ != nullptr && v3_books_->execution_blocked) {
        return std::string();
    }
    // 回合号铺法按用途分:
    //   memory_extract——内部回合(memory-turn-*),parentTurnId 挂触发
    //     主回合(在场才挂),不冒充真人回合;
    //   title_refine(T11-A,§4.34)——独立 step 归首问主回合:turnId 直接
    //     用首问回合号,不另铸内部回合,不挂 parentTurnId(自己不挂自己)。
    //     旁路在回合收口后的空闲边界跑,active_main_turn_id 仍是首问回合
    //     (BeginTurn 起 EndTurn 不清);取不到主回合号就不接账——不拿内部
    //     回合冒充,采样本体照跑(丢的只是这笔细账)。
    std::string turn_id;
    std::optional<std::string> parent_turn_id;
    if (purpose_ == accounting::RequestPurpose::MemoryExtract) {
        turn_id = v3_writer_->NewMemoryTurnId();
        if (v3_books_ != nullptr && !v3_books_->active_main_turn_id.empty()) {
            parent_turn_id = v3_books_->active_main_turn_id;
        }
    } else {
        if (v3_books_ == nullptr || v3_books_->active_main_turn_id.empty()) {
            return std::string();
        }
        turn_id = v3_books_->active_main_turn_id;
    }
    const char* system_cause = purpose_ == accounting::RequestPurpose::MemoryExtract
                                   ? "memory_extraction_prompt"
                                   : "session_title_prompt";
    // 1) 本次请求真用的 system:旁路自带提示词,与主链根无关——照实
    //    落一枚 system 消息(turnId 恒 null,§schema),prepared 引它。
    std::string system_message_id;
    if (!request.system.empty()) {
        v3::MessageDraft system;
        system.purpose = message_purpose;
        system.origin = v3::MessageOrigin::SessionRuntime;
        system.system_meta = nlohmann::json{{"cause", system_cause}};
        system.message = nlohmann::json{{"role", "system"}, {"content", request.system}};
        const auto receipt =
            v3_writer_->AppendMessage(std::move(system), trajectory::Durability::ProcessCrash);
        V3NotifyCommitted(receipt);
        if (receipt.status != v3::WriteReceipt::Status::Committed) {
            NoteV3Error(receipt, "bypass system");
            return std::string();  // §4.4:引用没落稳,请求不得发出
        }
        system_message_id = receipt.id;
    }
    // 2) 转写 user 消息:旁路材料,不 Admit 进链——conversation 历史一个
    //    字不混。
    std::string input_message_id;
    if (!request.messages.empty()) {
        std::string transcript_text;
        for (const auto& block : request.messages.front().content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                if (!transcript_text.empty()) transcript_text += "\n";
                transcript_text += text->text;
            }
        }
        if (!transcript_text.empty()) {
            v3::MessageDraft user;
            user.turn_id = turn_id;
            user.parent_turn_id = parent_turn_id;
            user.purpose = message_purpose;
            user.origin = v3::MessageOrigin::SessionRuntime;
            user.display = v3::DisplayMode::Collapsed;  // 内部回合材料默认折叠(§4.67.6 同款)
            user.message = nlohmann::json{{"role", "user"}, {"content", std::move(transcript_text)}};
            const auto receipt =
                v3_writer_->AppendMessage(std::move(user), trajectory::Durability::ProcessCrash);
            V3NotifyCommitted(receipt);
            if (receipt.status != v3::WriteReceipt::Status::Committed) {
                NoteV3Error(receipt, "bypass user");
                return std::string();
            }
            input_message_id = receipt.id;
        }
    }
    if (system_message_id.empty()) {
        // 没带 system 的旁路请求:prepared 的 systemMessageRef 是必填引用,
        // 拿主链根顶替就是造假——如实拒绝接账,采样停在边界。
        NoteV3Error(v3::WriteReceipt{v3::WriteReceipt::Status::Rejected, "", 0, "",
                                     "v3bypass.no_system", "旁路请求没带 system,无处落 systemMessageRef"},
                    "model.request.prepared(bypass)");
        return std::string();
    }
    // 3) prepared:purpose 用请求真用途的合同名(memory_extract/title_
    //    refine);预算(timeoutBudgetSecs)在这落——"预算多久"只有这儿知道。
    const std::string request_id = v3_writer_->NewRequestId();
    const std::string step_id = v3_writer_->NewStepId();
    nlohmann::json provider_snapshot = nlohmann::json{{"provider", identity_.provider},
                                                      {"wire", identity_.wire},
                                                      {"model", request.model}};
    if (request.max_tokens.has_value()) {
        provider_snapshot["parameters"] = nlohmann::json{{"maxOutputTokens", *request.max_tokens}};
    }
    if (ctx.timeout_budget_secs > 0) {
        provider_snapshot["timeoutBudgetSecs"] = ctx.timeout_budget_secs;
    }
    const auto prepared = v3_writer_->PrepareRequest(
        request_id, turn_id, step_id, accounting::PurposeName(ctx.purpose), system_message_id,
        input_message_id.empty() ? std::vector<std::string>{} : std::vector<std::string>{input_message_id},
        std::move(provider_snapshot), std::nullopt, trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(prepared);
    if (prepared.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(prepared, "model.request.prepared(bypass)");
        return std::string();  // §7.4:prepared 记不住,不发模型
    }
    V3RequestBook book;
    book.step_id = step_id;
    book.model = request.model;
    book.turn_id = turn_id;
    v3_requests_.emplace(request_id, std::move(book));
    return request_id;
}

bool TrajectoryBypassBridge::V3RequestSent(const std::string& request_id) {
    const auto it = v3_requests_.find(request_id);
    if (it == v3_requests_.end()) {
        return true;  // 簿里没有(prepared 没落稳):不拦,账早已如实
    }
    v3::EventDraft draft;
    draft.kind = v3::EventKindV3::ModelRequestSent;
    draft.status = v3::OpStatus::Done;
    draft.request_id = request_id;
    draft.turn_id = it->second.turn_id;
    draft.step_id = it->second.step_id;
    draft.payload = nlohmann::json{{"channel", identity_.channel},
                                   {"deliveryScope", "local_transport"}};
    const auto receipt = v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.request.sent(bypass)");
        return false;  // P1-C/FA-03:sent 记不住,采样停在发送边界
    }
    return true;
}

void TrajectoryBypassBridge::V3UsageRecorded(const std::string& request_id, const api::Usage& usage,
                                             bool reported_by_provider,
                                             const std::string& provider_response_id) {
    const auto it = v3_requests_.find(request_id);
    if (it == v3_requests_.end()) {
        return;
    }
    it->second.usage = reported_by_provider ? std::optional<nlohmann::json>(UsageToJson(usage)) : std::nullopt;
    it->second.usage_reported = reported_by_provider;
    it->second.provider_response_id = provider_response_id;
    if (it->second.output_committed) {
        V3FlushUsageAppended(it->second, request_id);
    }
}

void TrajectoryBypassBridge::V3FlushUsageAppended(const V3RequestBook& book,
                                                  const std::string& request_id) {
    v3::EventDraft draft;
    draft.kind = v3::EventKindV3::ModelUsageAppended;
    draft.request_id = request_id;
    draft.turn_id = book.turn_id;
    draft.step_id = book.step_id;
    draft.payload = nlohmann::json{
        {"usage", book.usage.has_value() ? *book.usage : nlohmann::json(nullptr)},
        {"reportedByProvider", book.usage_reported}};
    if (!book.provider_response_id.empty()) {
        draft.payload["providerResponseId"] = book.provider_response_id;
    }
    const auto receipt = v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.usage.appended(bypass)");
    }
}

bool TrajectoryBypassBridge::V3EnsureStreamStarted(const std::string& request_id) {
    const auto it = v3_requests_.find(request_id);
    if (it == v3_requests_.end()) {
        return false;  // 簿没有的请求不伪造流
    }
    V3RequestBook& book = it->second;
    if (book.stream_started) {
        return true;
    }
    book.stream_id = v3_writer_->NewStreamId();
    book.reserved_message_id = v3_writer_->NewMessageId();
    const auto receipt = v3_writer_->BeginStreamResponse(
        request_id, book.stream_id, book.turn_id, book.step_id, book.reserved_message_id,
        trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.response.started(bypass)");
        return false;  // started 记不住,终态定稿不得越过(§4.4 同款栅栏)
    }
    book.stream_started = true;
    return true;
}

// T11-A:旁路用途 → 消息 purpose(memory_extract→memory_extract,
// title_refine→session_title;其余用途工厂已拦,这里给 conversation 仅作
// 类型兜底,不会真走到)。
v3::MessagePurpose TrajectoryBypassBridge::V3MessagePurpose() const {
    if (purpose_ == accounting::RequestPurpose::MemoryExtract) {
        return v3::MessagePurpose::MemoryExtract;
    }
    if (purpose_ == accounting::RequestPurpose::TitleRefine) {
        return v3::MessagePurpose::SessionTitle;
    }
    return v3::MessagePurpose::Conversation;
}

bool TrajectoryBypassBridge::V3OutputCompleted(const std::string& request_id, const api::Message& assistant,
                                               const std::string& stop_reason,
                                               const std::string& provider_response_id) {
    const auto it = v3_requests_.find(request_id);
    if (it == v3_requests_.end() || it->second.output_committed) {
        return false;  // 请求簿没有/已收口:不重复成行
    }
    V3RequestBook& book = it->second;
    // 旁路采样零 delta(SampleModel 不转灌流事件),懒起 started 保闭环
    // 形状——与主桥"非流式后端零片段路"同一形状(§4.43)。
    if (!V3EnsureStreamStarted(request_id)) {
        return false;
    }
    nlohmann::json content = nlohmann::json::array();
    for (const auto& block : assistant.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            content.push_back(nlohmann::json{{"type", "text"}, {"text", text->text}});
        } else if (const auto* thinking = std::get_if<api::ThinkingBlock>(&block)) {
            content.push_back(nlohmann::json{{"type", "thinking"}, {"text", thinking->text}, {"signature", thinking->signature}, {"responses_item", thinking->responses_item}});
        }
        // 旁路采样无工具调用,ToolUse/ToolResult 不该出现,出现了也不入账。
    }
    nlohmann::json body = nlohmann::json{{"role", "assistant"}, {"content", std::move(content)}};
    const auto receipt = v3_writer_->CompleteStreamResponse(
        request_id, book.stream_id, book.turn_id, book.step_id, book.reserved_message_id,
        std::move(body), identity_.provider, identity_.wire, book.model,
        provider_response_id.empty() ? nlohmann::json(nullptr)
                                     : nlohmann::json(provider_response_id),
        book.usage.has_value() ? *book.usage : nlohmann::json(nullptr),
        stop_reason.empty() ? std::string("end_turn") : stop_reason,
        V3MessagePurpose(), std::nullopt,
        stop_reason == "length" || stop_reason == "max_tokens"
            ? std::optional<v3::CompletionStatus>(v3::CompletionStatus::Truncated)
            : std::nullopt);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.response.completed(bypass)");
        return false;  // §7.4:输出记不住,不装成功
    }
    book.output_committed = true;
    return true;
}

void TrajectoryBypassBridge::V3OutputFailed(const std::string& request_id, const std::string& reason) {
    const auto it = v3_requests_.find(request_id);
    if (it == v3_requests_.end()) {
        return;
    }
    v3::EventDraft draft;
    draft.kind = v3::EventKindV3::ModelResponseFailed;
    draft.status = v3::OpStatus::Failed;
    draft.request_id = request_id;
    draft.turn_id = it->second.turn_id;
    draft.step_id = it->second.step_id;
    draft.payload = nlohmann::json{{"reason", reason.empty() ? std::string("failed") : reason}};
    const auto receipt = v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.response.failed(bypass)");
    }
    // 半截失败的 usage 不沉没:没有 assistant 行可挂,appended 是唯一落点。
    V3FlushUsageAppended(it->second, request_id);
}

void TrajectoryBypassBridge::V3OutputCancelled(const std::string& request_id,
                                               agent::OutputCancelSource source) {
    const auto it = v3_requests_.find(request_id);
    if (it == v3_requests_.end()) {
        return;
    }
    // 旁路流没起过(采样不转灌流事件,收不到任何 delta):裸 cancelled
    // 事件 + 真实来源,不伪造流不伪造 assistant;usage 照 appended 出账。
    v3::EventDraft draft;
    draft.kind = v3::EventKindV3::ModelResponseCancelled;
    draft.status = v3::OpStatus::Cancelled;
    draft.request_id = request_id;
    draft.turn_id = it->second.turn_id;
    draft.step_id = it->second.step_id;
    draft.payload = nlohmann::json{{"reason", agent::OutputCancelSourceText(source)}};
    const auto receipt = v3_writer_->AppendEvent(std::move(draft), trajectory::Durability::ProcessCrash);
    V3NotifyCommitted(receipt);
    if (receipt.status != v3::WriteReceipt::Status::Committed) {
        NoteV3Error(receipt, "model.response.cancelled(bypass)");
    }
    V3FlushUsageAppended(it->second, request_id);
}


}  // namespace lubancode::runtime
