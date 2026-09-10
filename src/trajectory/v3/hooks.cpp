// hook 事件账实现(§4.22-4.24)。
#include "trajectory/v3/hooks.hpp"

namespace lubancode::trajectory::v3 {

HookDispatchSession HookDispatchSession::Dispatch(
    V3Writer& writer, std::string hook_dispatch_id, std::string hook_point,
    std::optional<std::string> turn_id, std::optional<std::string> step_id,
    std::optional<std::string> action_id, const std::vector<HookHandlerSpec>& handlers,
    std::optional<nlohmann::json> input_ref, Durability durability) {
    HookDispatchSession session(std::move(hook_dispatch_id), std::move(hook_point),
                                std::move(turn_id), std::move(step_id), std::move(action_id));
    nlohmann::json matched = nlohmann::json::array();
    for (const auto& handler : handlers) {
        matched.push_back(handler.ToJson());
    }
    nlohmann::json payload = nlohmann::json::object(
        {{"hookPoint", session.hook_point_}, {"matchedHandlers", std::move(matched)}});
    if (input_ref.has_value()) {
        payload["inputRef"] = std::move(*input_ref);
    }
    EventDraft draft = session.BaseDraft(EventKindV3::HookDispatchRequested);
    draft.payload = std::move(payload);
    writer.AppendEvent(std::move(draft), durability);
    return session;
}

WriteReceipt HookDispatchSession::Skip(V3Writer& writer, std::string hook_dispatch_id,
                                       std::string hook_point, std::string reason,
                                       std::optional<std::string> turn_id,
                                       std::optional<std::string> step_id,
                                       std::optional<std::string> action_id,
                                       Durability durability) {
    HookDispatchSession session(std::move(hook_dispatch_id), std::move(hook_point),
                                std::move(turn_id), std::move(step_id), std::move(action_id));
    EventDraft draft = session.BaseDraft(EventKindV3::HookSkipped);
    draft.payload = nlohmann::json::object({{"hookPoint", session.hook_point_},
                                            {"reason", std::move(reason)}});
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt HookDispatchSession::BeginInvocation(V3Writer& writer,
                                                  std::string hook_invocation_id,
                                                  const HookHandlerSpec& spec,
                                                  Durability durability) {
    if (invocation_started_ && !invocation_closed_) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                            "v3hook.invocation_open",
                            "上一 invocation 未收口(§4.48:能改同一对象的串行)"};
    }
    EventDraft draft = BaseDraft(EventKindV3::HookStarted);
    draft.status = OpStatus::Running;
    draft.payload = nlohmann::json::object({{"hookInvocationId", hook_invocation_id},
                                            {"hookId", spec.hook_id},
                                            {"handlerKind", spec.handler_kind},
                                            {"definitionHash", spec.definition_hash},
                                            {"definitionOrder", spec.definition_order},
                                            {"failurePolicy", spec.failure_policy}});
    WriteReceipt receipt = writer.AppendEvent(std::move(draft), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        current_invocation_id_ = std::move(hook_invocation_id);
        current_hook_id_ = spec.hook_id;
        invocation_started_ = true;
        invocation_closed_ = false;
    }
    return receipt;
}

WriteReceipt HookDispatchSession::CompleteInvocation(V3Writer& writer,
                                                     std::optional<std::string> decision,
                                                     std::optional<nlohmann::json> output_ref,
                                                     std::optional<std::uint64_t> duration_ms,
                                                     Durability durability) {
    if (!invocation_started_ || invocation_closed_) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                            "v3hook.no_open_invocation", "没有进行中的 invocation"};
    }
    nlohmann::json payload = nlohmann::json::object(
        {{"hookInvocationId", *current_invocation_id_}, {"hookId", *current_hook_id_}});
    if (decision.has_value()) {
        payload["decision"] = *decision;  // deny/rewrite 也是 completed(§4.22)
    }
    if (output_ref.has_value()) {
        payload["outputRef"] = std::move(*output_ref);
    }
    if (duration_ms.has_value()) {
        payload["durationMs"] = *duration_ms;
    }
    EventDraft draft = BaseDraft(EventKindV3::HookCompleted);
    draft.status = OpStatus::Done;
    draft.payload = std::move(payload);
    WriteReceipt receipt = writer.AppendEvent(std::move(draft), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        invocation_closed_ = true;
    }
    return receipt;
}

WriteReceipt HookDispatchSession::FailInvocation(V3Writer& writer, std::string error_code,
                                                 std::optional<std::uint64_t> duration_ms,
                                                 Durability durability) {
    if (!invocation_started_ || invocation_closed_) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                            "v3hook.no_open_invocation", "没有进行中的 invocation"};
    }
    nlohmann::json payload = nlohmann::json::object({{"error_code", std::move(error_code)}});
    if (current_invocation_id_.has_value()) {
        payload["hookInvocationId"] = *current_invocation_id_;
        payload["hookId"] = *current_hook_id_;
    }
    if (duration_ms.has_value()) {
        payload["durationMs"] = *duration_ms;
    }
    EventDraft draft = BaseDraft(EventKindV3::HookFailed);
    draft.status = OpStatus::Failed;
    draft.payload = std::move(payload);
    WriteReceipt receipt = writer.AppendEvent(std::move(draft), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        invocation_closed_ = true;
    }
    return receipt;
}

WriteReceipt HookDispatchSession::CancelInvocation(V3Writer& writer, std::string reason,
                                                   Durability durability) {
    if (!invocation_started_ || invocation_closed_) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                            "v3hook.no_open_invocation", "没有进行中的 invocation"};
    }
    nlohmann::json payload = nlohmann::json::object(
        {{"hookInvocationId", *current_invocation_id_}, {"hookId", *current_hook_id_},
         {"reason", std::move(reason)}});
    EventDraft draft = BaseDraft(EventKindV3::HookCancelled);
    draft.status = OpStatus::Cancelled;
    draft.payload = std::move(payload);
    WriteReceipt receipt = writer.AppendEvent(std::move(draft), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        invocation_closed_ = true;
    }
    return receipt;
}

WriteReceipt HookDispatchSession::MarkInvocationUnknown(V3Writer& writer, std::string reason,
                                                        Durability durability) {
    if (!invocation_started_ || invocation_closed_) {
        return WriteReceipt{WriteReceipt::Status::Rejected, "", 0, "",
                            "v3hook.no_open_invocation", "没有进行中的 invocation"};
    }
    nlohmann::json payload = nlohmann::json::object(
        {{"hookInvocationId", *current_invocation_id_}, {"hookId", *current_hook_id_},
         {"reason", std::move(reason)}});
    EventDraft draft = BaseDraft(EventKindV3::HookUnknown);
    draft.status = OpStatus::Unknown;
    draft.payload = std::move(payload);
    WriteReceipt receipt = writer.AppendEvent(std::move(draft), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        invocation_closed_ = true;
    }
    return receipt;
}

WriteReceipt HookDispatchSession::ApplyEffect(V3Writer& writer, std::string effect_type,
                                              std::optional<nlohmann::json> input_ref,
                                              std::optional<nlohmann::json> output_ref,
                                              std::optional<nlohmann::json> applied_value_ref,
                                              std::optional<std::string> validation,
                                              Durability durability) {
    nlohmann::json payload = nlohmann::json::object({{"effectType", std::move(effect_type)}});
    if (current_invocation_id_.has_value()) {
        payload["hookInvocationId"] = *current_invocation_id_;
    }
    if (input_ref.has_value()) {
        payload["inputRef"] = std::move(*input_ref);
    }
    if (output_ref.has_value()) {
        payload["outputRef"] = std::move(*output_ref);
    }
    if (applied_value_ref.has_value()) {
        payload["appliedValueRef"] = std::move(*applied_value_ref);
    }
    if (validation.has_value()) {
        payload["validation"] = *validation;  // 校验结果(通过/项明细)
    }
    EventDraft draft = BaseDraft(EventKindV3::HookEffectsApplied);
    draft.payload = std::move(payload);
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt HookDispatchSession::RejectEffect(V3Writer& writer, std::string effect_type,
                                               std::string reason, Durability durability) {
    nlohmann::json payload = nlohmann::json::object(
        {{"effectType", std::move(effect_type)}, {"reason", std::move(reason)}});
    if (current_invocation_id_.has_value()) {
        payload["hookInvocationId"] = *current_invocation_id_;
    }
    EventDraft draft = BaseDraft(EventKindV3::HookEffectsRejected);
    draft.payload = std::move(payload);
    return writer.AppendEvent(std::move(draft), durability);
}

ToolActionSession HookDispatchSession::BeginSubExecution(V3Writer& writer,
                                                         const ToolIdentity& identity,
                                                         std::string backend,
                                                         Durability durability) const {
    // §4.23:宿主派生执行——独立 actionId;payload 带 parentActionId/
    // hookInvocationId/来源与实际后端;不冒充模型新声明的 tool_call。
    nlohmann::json linkage = nlohmann::json::object(
        {{"parentActionId", action_id_.value_or(std::string())},
         {"hookDispatchId", dispatch_id_},
         {"logicalTool", identity.logical_name},
         {"backend", std::move(backend)}});
    if (current_invocation_id_.has_value()) {
        linkage["hookInvocationId"] = *current_invocation_id_;
    }
    return ToolActionSession::Admit(
        writer, turn_id_.value_or(std::string()), step_id_.value_or(std::string()),
        writer.NewActionId(), "hook_subexecution", std::nullopt, std::nullopt,
        std::move(linkage), durability);
}

HookDispatchSession::HookDispatchSession(std::string dispatch_id, std::string hook_point,
                                         std::optional<std::string> turn_id,
                                         std::optional<std::string> step_id,
                                         std::optional<std::string> action_id)
    : dispatch_id_(std::move(dispatch_id)),
      hook_point_(std::move(hook_point)),
      turn_id_(std::move(turn_id)),
      step_id_(std::move(step_id)),
      action_id_(std::move(action_id)) {}

EventDraft HookDispatchSession::BaseDraft(EventKindV3 kind) const {
    EventDraft draft;
    draft.kind = kind;
    draft.turn_id = turn_id_;
    draft.step_id = step_id_;
    draft.action_id = action_id_;
    draft.hook_dispatch_id = dispatch_id_;
    return draft;
}

// ---------------------------------------------------------------------------
// NestedHookDispatchSession(LuaHook 单 P0-B):洋葱嵌套版。事件合同与
// HookDispatchSession 同一套;invocation 面显式带 id,多枚可同时开张。
// ---------------------------------------------------------------------------

NestedHookDispatchSession NestedHookDispatchSession::Open(
    V3Writer& writer, std::string hook_dispatch_id, std::string hook_point,
    std::optional<std::string> turn_id, std::optional<std::string> step_id,
    std::optional<std::string> action_id, const std::vector<HookHandlerSpec>& handlers,
    std::optional<nlohmann::json> input_ref, Durability durability) {
    NestedHookDispatchSession session(std::move(hook_dispatch_id), std::move(hook_point),
                                      std::move(turn_id), std::move(step_id), std::move(action_id));
    nlohmann::json matched = nlohmann::json::array();
    for (const auto& handler : handlers) {
        matched.push_back(handler.ToJson());
    }
    nlohmann::json payload = nlohmann::json::object(
        {{"hookPoint", session.hook_point_}, {"matchedHandlers", std::move(matched)}});
    if (input_ref.has_value()) {
        payload["inputRef"] = std::move(*input_ref);
    }
    EventDraft draft = session.BaseDraft(EventKindV3::HookDispatchRequested);
    draft.payload = std::move(payload);
    writer.AppendEvent(std::move(draft), durability);
    return session;
}

WriteReceipt NestedHookDispatchSession::WriteSkip(V3Writer& writer, std::string hook_dispatch_id,
                                                  std::string hook_point, std::string reason,
                                                  std::optional<std::string> turn_id,
                                                  std::optional<std::string> step_id,
                                                  std::optional<std::string> action_id,
                                                  Durability durability) {
    NestedHookDispatchSession session(std::move(hook_dispatch_id), std::move(hook_point),
                                      std::move(turn_id), std::move(step_id), std::move(action_id));
    EventDraft draft = session.BaseDraft(EventKindV3::HookSkipped);
    draft.payload = nlohmann::json::object({{"hookPoint", session.hook_point_},
                                            {"reason", std::move(reason)}});
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt NestedHookDispatchSession::BeginInvocation(V3Writer& writer,
                                                        std::string hook_invocation_id,
                                                        const HookHandlerSpec& spec,
                                                        Durability durability) {
    EventDraft draft = BaseDraft(EventKindV3::HookStarted);
    draft.status = OpStatus::Running;
    draft.payload = nlohmann::json::object({{"hookInvocationId", std::move(hook_invocation_id)},
                                            {"hookId", spec.hook_id},
                                            {"handlerKind", spec.handler_kind},
                                            {"definitionHash", spec.definition_hash},
                                            {"definitionOrder", spec.definition_order},
                                            {"failurePolicy", spec.failure_policy}});
    return writer.AppendEvent(std::move(draft), durability);
}

namespace {

// invocation 事件的公共身份键(hookInvocationId)。started 之后的事件都带。
nlohmann::json InvocationPayload(const std::string& hook_invocation_id) {
    return nlohmann::json::object({{"hookInvocationId", hook_invocation_id}});
}

}  // namespace

WriteReceipt NestedHookDispatchSession::CompleteInvocation(V3Writer& writer,
                                                           const std::string& hook_invocation_id,
                                                           const std::string& hook_id,
                                                           std::optional<std::string> decision,
                                                           std::optional<nlohmann::json> output_ref,
                                                           std::optional<std::uint64_t> duration_ms,
                                                           Durability durability) {
    nlohmann::json payload = InvocationPayload(hook_invocation_id);
    payload["hookId"] = hook_id;
    if (decision.has_value()) {
        payload["decision"] = std::move(*decision);  // deny/rewrite 也是 completed(§4.22)
    }
    if (output_ref.has_value()) {
        payload["outputRef"] = std::move(*output_ref);
    }
    if (duration_ms.has_value()) {
        payload["durationMs"] = *duration_ms;
    }
    EventDraft draft = BaseDraft(EventKindV3::HookCompleted);
    draft.status = OpStatus::Done;
    draft.payload = std::move(payload);
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt NestedHookDispatchSession::FailInvocation(V3Writer& writer,
                                                       const std::string& hook_invocation_id,
                                                       std::string error_code,
                                                       std::optional<std::uint64_t> duration_ms,
                                                       Durability durability) {
    nlohmann::json payload = InvocationPayload(hook_invocation_id);
    payload["error_code"] = std::move(error_code);
    if (duration_ms.has_value()) {
        payload["durationMs"] = *duration_ms;
    }
    EventDraft draft = BaseDraft(EventKindV3::HookFailed);
    draft.status = OpStatus::Failed;
    draft.payload = std::move(payload);
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt NestedHookDispatchSession::CancelInvocation(V3Writer& writer,
                                                         const std::string& hook_invocation_id,
                                                         std::string reason, Durability durability) {
    nlohmann::json payload = InvocationPayload(hook_invocation_id);
    payload["reason"] = std::move(reason);
    EventDraft draft = BaseDraft(EventKindV3::HookCancelled);
    draft.status = OpStatus::Cancelled;
    draft.payload = std::move(payload);
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt NestedHookDispatchSession::MarkInvocationUnknown(V3Writer& writer,
                                                              const std::string& hook_invocation_id,
                                                              std::string reason,
                                                              Durability durability) {
    nlohmann::json payload = InvocationPayload(hook_invocation_id);
    payload["reason"] = std::move(reason);
    EventDraft draft = BaseDraft(EventKindV3::HookUnknown);
    draft.status = OpStatus::Unknown;
    draft.payload = std::move(payload);
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt NestedHookDispatchSession::ApplyEffect(V3Writer& writer,
                                                    const std::string& hook_invocation_id,
                                                    std::string effect_type,
                                                    std::optional<nlohmann::json> applied_value_ref,
                                                    Durability durability) {
    nlohmann::json payload = InvocationPayload(hook_invocation_id);
    payload["effectType"] = std::move(effect_type);
    if (applied_value_ref.has_value()) {
        payload["appliedValueRef"] = std::move(*applied_value_ref);
    }
    EventDraft draft = BaseDraft(EventKindV3::HookEffectsApplied);
    draft.payload = std::move(payload);
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt NestedHookDispatchSession::RejectEffect(V3Writer& writer,
                                                     const std::string& hook_invocation_id,
                                                     std::string effect_type, std::string reason,
                                                     Durability durability) {
    nlohmann::json payload = InvocationPayload(hook_invocation_id);
    payload["effectType"] = std::move(effect_type);
    payload["reason"] = std::move(reason);
    EventDraft draft = BaseDraft(EventKindV3::HookEffectsRejected);
    draft.payload = std::move(payload);
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt NestedHookDispatchSession::OutputProposed(V3Writer& writer,
                                                       const std::string& hook_invocation_id,
                                                       std::string phase, nlohmann::json candidate_ref,
                                                       Durability durability) {
    nlohmann::json payload = InvocationPayload(hook_invocation_id);
    payload["phase"] = std::move(phase);
    payload["candidate"] = std::move(candidate_ref);
    EventDraft draft = BaseDraft(EventKindV3::HookOutputProposed);
    draft.payload = std::move(payload);
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt NestedHookDispatchSession::ContinuationConsumed(V3Writer& writer,
                                                             const std::string& hook_invocation_id,
                                                             Durability durability) {
    EventDraft draft = BaseDraft(EventKindV3::HookContinuationConsumed);
    draft.payload = InvocationPayload(hook_invocation_id);
    return writer.AppendEvent(std::move(draft), durability);
}

NestedHookDispatchSession::NestedHookDispatchSession(std::string dispatch_id, std::string hook_point,
                                                     std::optional<std::string> turn_id,
                                                     std::optional<std::string> step_id,
                                                     std::optional<std::string> action_id)
    : dispatch_id_(std::move(dispatch_id)),
      hook_point_(std::move(hook_point)),
      turn_id_(std::move(turn_id)),
      step_id_(std::move(step_id)),
      action_id_(std::move(action_id)) {}

EventDraft NestedHookDispatchSession::BaseDraft(EventKindV3 kind) const {
    EventDraft draft;
    draft.kind = kind;
    draft.turn_id = turn_id_;
    draft.step_id = step_id_;
    draft.action_id = action_id_;
    draft.hook_dispatch_id = dispatch_id_;
    return draft;
}

}  // namespace lubancode::trajectory::v3
