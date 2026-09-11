// 工具调用操作账实现(§4.14-4.15/§4.18-4.20)。
#include "trajectory/v3/tool_action.hpp"

#include "hooks/hash.hpp"
#include "trajectory/canonical_json.hpp"

namespace lubancode::trajectory::v3 {

std::string ComputeToolIdempotencyKey(std::string_view action_id,
                                      const ToolIdentity& identity,
                                      std::string_view effective_args_hash,
                                      std::string_view execution_scope_hash,
                                      std::string_view key_version) {
    nlohmann::json material = nlohmann::json::object(
        {{"version", std::string(key_version)},
         {"action_id", std::string(action_id)},
         {"tool_identity", identity.ToJson()},
         {"effective_args_hash", std::string(effective_args_hash)},
         {"execution_scope_hash", std::string(execution_scope_hash)}});
    auto canonical = CanonicalJsonDump(material);
    if (!canonical.has_value()) {
        return std::string();
    }
    return hooks::Sha256Hex(*canonical);
}

ToolActionSession ToolActionSession::Admit(V3Writer& writer, std::string turn_id,
                                           std::string step_id, std::string action_id,
                                           std::string reason,
                                           std::optional<std::string> assistant_message_ref,
                                           std::optional<std::string> provider_tool_call_id,
                                           nlohmann::json extra_payload, Durability durability) {
    ToolActionSession session(std::move(turn_id), std::move(step_id), std::move(action_id));
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", session.action_id_}, {"attempt", session.attempt_},
         {"reason", std::move(reason)}});
    if (assistant_message_ref.has_value()) {
        payload["assistantMessageRef"] = *assistant_message_ref;
    }
    if (provider_tool_call_id.has_value()) {
        // provider 原样配对号:与来源请求、assistant 块一起存,不作主键(§4.15)。
        payload["provider_tool_call_id"] = *provider_tool_call_id;
    }
    for (auto it = extra_payload.begin(); it != extra_payload.end(); ++it) {
        payload[it.key()] = it.value();
    }
    session.Emit(writer, EventKindV3::ToolExecutionPending, OpStatus::Pending,
                 std::move(payload), durability);
    return session;
}

WriteReceipt ToolActionSession::Start(V3Writer& writer, std::string effective_args_ref,
                                      ToolIdentity identity,
                                      std::optional<std::string> idempotency_key,
                                      nlohmann::json extra_payload, Durability durability) {
    if (started_ || terminal_ != Terminal::None) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "", "v3tool.already_started",
                            "同一 attempt 不得二次 started;终态后重开须走下一 attempt"};
    }
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_},
         {"attempt", attempt_},
         {"effectiveArgsRef", std::move(effective_args_ref)},
         {"toolIdentity", identity.ToJson()}});
    if (idempotency_key.has_value() && !idempotency_key->empty()) {
        payload["idempotencyKey"] = *idempotency_key;
    }
    for (auto it = extra_payload.begin(); it != extra_payload.end(); ++it) {
        payload[it.key()] = it.value();
    }
    WriteReceipt receipt =
        Emit(writer, EventKindV3::ToolExecutionStarted, OpStatus::Running, std::move(payload),
             durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        started_ = true;
    }
    return receipt;
}

WriteReceipt ToolActionSession::Wait(V3Writer& writer, std::string reason,
                                     std::string wait_ref, Durability durability) {
    if (!started_ || terminal_ != Terminal::None) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "", "v3tool.not_running",
                            "waiting 只落在已开始且未终态的执行上(§4.14)"};
    }
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_},
         {"attempt", attempt_},
         {"reason", std::move(reason)},
         {"waitRef", std::move(wait_ref)}});
    WriteReceipt receipt =
        Emit(writer, EventKindV3::ToolExecutionWaiting, OpStatus::Pending, std::move(payload),
             durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        waiting_ = true;
    }
    return receipt;
}

WriteReceipt ToolActionSession::Resume(V3Writer& writer, Durability durability) {
    if (!waiting_) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "", "v3tool.not_waiting",
                            "没有等待就没有 resumed(§4.14:不为快工具虚造等待)"};
    }
    WriteReceipt receipt = Emit(writer, EventKindV3::ToolExecutionResumed, OpStatus::Running,
                                nlohmann::json::object({{"tool_call_id", action_id_},
                                                        {"attempt", attempt_}}),
                                durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        waiting_ = false;
    }
    return receipt;
}

WriteReceipt ToolActionSession::Finish(V3Writer& writer, std::optional<std::int64_t> exit_code,
                                       std::optional<std::uint64_t> execution_duration_ms,
                                       Durability durability) {
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_}, {"attempt", attempt_}});
    // exit_code=0 只说退出状态;进程未退出/非进程工具不假填(§4.16)。
    payload["exit_code"] = exit_code.has_value() ? nlohmann::json(*exit_code)
                                                 : nlohmann::json(nullptr);
    if (execution_duration_ms.has_value()) {
        payload["executionDurationMs"] = *execution_duration_ms;  // 单调时钟量(§4.14)
    }
    return CloseTerminal(writer, EventKindV3::ToolExecutionFinished, std::move(payload),
                         Terminal::Finished, durability);
}

WriteReceipt ToolActionSession::Fail(V3Writer& writer, std::string error_code,
                                     std::optional<std::uint64_t> execution_duration_ms,
                                     Durability durability) {
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_},
         {"attempt", attempt_},
         {"error_code", std::move(error_code)}});
    if (execution_duration_ms.has_value()) {
        payload["executionDurationMs"] = *execution_duration_ms;
    }
    return CloseTerminal(writer, EventKindV3::ToolExecutionFailed, std::move(payload),
                         Terminal::Failed, durability);
}

WriteReceipt ToolActionSession::Cancel(V3Writer& writer, std::string phase, std::string reason,
                                       Durability durability) {
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_},
         {"attempt", attempt_},
         {"phase", std::move(phase)},
         {"reason", std::move(reason)}});
    return CloseTerminal(writer, EventKindV3::ToolExecutionCancelled, std::move(payload),
                         Terminal::Cancelled, durability);
}

WriteReceipt ToolActionSession::Reject(V3Writer& writer, std::string reason,
                                       Durability durability) {
    // 参数/权限/准入拒绝:没有执行,不带 started;仍要配对错误 tool 消息(§4.20)。
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_}, {"reason", std::move(reason)}});
    return CloseTerminal(writer, EventKindV3::ToolExecutionRejected, std::move(payload),
                         Terminal::Rejected, durability);
}

WriteReceipt ToolActionSession::MarkUnknown(V3Writer& writer, std::string reason,
                                            Durability durability) {
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_},
         {"attempt", attempt_},
         {"reason", std::move(reason)}});
    return CloseTerminal(writer, EventKindV3::ToolExecutionUnknown, std::move(payload),
                         Terminal::Unknown, durability);
}

WriteReceipt ToolActionSession::BeginNextAttempt(V3Writer& writer, std::string reason,
                                                 Durability durability) {
    if (terminal_ == Terminal::None) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                            "v3tool.attempt_open",
                            "上一 attempt 未终态,不得开下一 attempt(§4.14:先记终态再重试)"};
    }
    ++attempt_;
    started_ = false;
    waiting_ = false;
    terminal_ = Terminal::None;
    return Emit(writer, EventKindV3::ToolExecutionPending, OpStatus::Pending,
                nlohmann::json::object({{"tool_call_id", action_id_},
                                        {"attempt", attempt_},
                                        {"reason", std::move(reason)}}),
                durability);
}

WriteReceipt ToolActionSession::PersistedResult(V3Writer& writer,
                                                const std::vector<nlohmann::json>& result_ref,
                                                std::optional<std::string> execution_event_ref,
                                                std::optional<std::uint64_t> attempt,
                                                Durability durability) {
    nlohmann::json refs = nlohmann::json::array();
    for (const auto& ref : result_ref) {
        refs.push_back(ref);
    }
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_},
         {"attempt", attempt.value_or(attempt_)},
         {"result_ref", std::move(refs)},
         {"executionEventRef",
          execution_event_ref.value_or(last_event_id_.value_or(std::string()))}});
    return Emit(writer, EventKindV3::ToolResultPersisted, std::nullopt, std::move(payload),
                durability);
}

WriteReceipt ToolActionSession::PersistFailed(V3Writer& writer, std::string reason,
                                              std::optional<std::uint64_t> attempt,
                                              Durability durability) {
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_},
         {"attempt", attempt.value_or(attempt_)},
         {"reason", std::move(reason)}});
    return Emit(writer, EventKindV3::ToolResultPersistFailed, std::nullopt, std::move(payload),
                durability);
}

WriteReceipt ToolActionSession::SelectResult(
    V3Writer& writer, const std::vector<std::string>& source_result_event_refs,
    const std::vector<std::string>& hook_effect_event_refs, std::string_view effective_outcome,
    std::optional<std::uint64_t> attempt, Durability durability) {
    nlohmann::json sources = nlohmann::json::array();
    for (const auto& ref : source_result_event_refs) {
        sources.push_back(ref);
    }
    nlohmann::json hooks = nlohmann::json::array();
    for (const auto& ref : hook_effect_event_refs) {
        hooks.push_back(ref);
    }
    nlohmann::json payload = nlohmann::json::object(
        {{"tool_call_id", action_id_},
         {"attempt", attempt.value_or(attempt_)},
         {"sourceResultEventRefs", sources},
         {"hookEffectEventRefs", hooks},
         {"effectiveOutcome", std::string(effective_outcome)}});
    WriteReceipt receipt =
        Emit(writer, EventKindV3::ToolResultSelected, std::nullopt, std::move(payload),
             durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        selected_event_id_ = receipt.id;
    }
    return receipt;
}

WriteReceipt ToolActionSession::AppendToolMessage(V3Writer& writer, std::string content,
                                                  std::optional<std::string> result_selection_ref,
                                                  bool is_error, Durability durability) {
    MessageDraft draft;
    draft.turn_id = turn_id_;
    draft.step_id = step_id_;
    draft.action_id = action_id_;
    draft.purpose = MessagePurpose::Conversation;
    draft.origin = MessageOrigin::SessionRuntime;
    // message.tool_call_id 用本地全局调用号,由 wire adapter 映射回
    // provider 原始号(§4.18)。is_error 只写真值(P1-B):读取侧 contains()
    // 认键,缺键 = false。
    draft.message = nlohmann::json::object(
        {{"role", "tool"}, {"tool_call_id", action_id_}, {"content", std::move(content)}});
    if (is_error) {
        draft.message["is_error"] = true;
    }
    draft.result_selection_ref = std::move(result_selection_ref);
    WriteReceipt receipt = writer.AppendMessage(std::move(draft), durability);
    if (receipt.status != WriteReceipt::Status::Committed) {
        return receipt;
    }
    return writer.AdmitMessages({receipt.id}, durability);
}

ToolActionSession::ToolActionSession(std::string turn_id, std::string step_id,
                                     std::string action_id)
    : turn_id_(std::move(turn_id)),
      step_id_(std::move(step_id)),
      action_id_(std::move(action_id)) {}

WriteReceipt ToolActionSession::Emit(V3Writer& writer, EventKindV3 kind,
                                     std::optional<OpStatus> status, nlohmann::json payload,
                                     Durability durability) {
    EventDraft draft;
    draft.kind = kind;
    draft.status = status;
    draft.turn_id = turn_id_;
    draft.step_id = step_id_;
    draft.action_id = action_id_;
    draft.payload = std::move(payload);
    WriteReceipt receipt = writer.AppendEvent(std::move(draft), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        last_event_id_ = receipt.id;
    }
    return receipt;
}

WriteReceipt ToolActionSession::CloseTerminal(V3Writer& writer, EventKindV3 kind,
                                              nlohmann::json payload, Terminal terminal,
                                              Durability durability) {
    if (terminal_ != Terminal::None) {
        // 每次尝试最多一个执行终态;迟到响应另记观察事件,不改旧终态(§4.14)。
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                            "v3tool.already_terminal",
                            "本 attempt 已有终态;迟到事实另记观察事件"};
    }
    std::optional<OpStatus> status;
    switch (terminal) {
        case Terminal::Finished: status = OpStatus::Done; break;
        case Terminal::Failed: status = OpStatus::Failed; break;
        case Terminal::Cancelled: status = OpStatus::Cancelled; break;
        case Terminal::Rejected: status = OpStatus::Rejected; break;
        case Terminal::Unknown: status = OpStatus::Unknown; break;
        case Terminal::None: break;
    }
    WriteReceipt receipt = Emit(writer, kind, status, std::move(payload), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        terminal_ = terminal;
    }
    return receipt;
}

}  // namespace lubancode::trajectory::v3
