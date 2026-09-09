// compact 全链实现。
#include "trajectory/v3/compact.hpp"

#include "hooks/hash.hpp"
#include "trajectory/canonical_json.hpp"

namespace lubancode::trajectory::v3 {

namespace {

nlohmann::json RefsToJson(const std::vector<std::string>& refs) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& ref : refs) {
        array.push_back(ref);
    }
    return array;
}

// 上下文状态 hash(§3.2 定案):canonical({contextId, revision, refs}) 的
// sha256,算法版本 v3-refs-sha256 写进 applied payload。
std::string ComputeStateHash(const std::string& context_id, std::uint64_t revision,
                             const std::vector<ChainNode>& chain) {
    nlohmann::json refs = nlohmann::json::array();
    for (const auto& node : chain) {
        refs.push_back(node.message_ref);
    }
    nlohmann::json state = nlohmann::json::object(
        {{"contextId", context_id}, {"revision", revision}, {"messageRefs", refs}});
    auto canonical = CanonicalJsonDump(state);
    if (!canonical.has_value()) {
        return {};
    }
    return hooks::Sha256Hex(*canonical);
}

EventKindV3 FailKindToEvent(CompactSession::FailKind kind) {
    switch (kind) {
        case CompactSession::FailKind::Failed: return EventKindV3::CompactFailed;
        case CompactSession::FailKind::Cancelled: return EventKindV3::CompactCancelled;
        case CompactSession::FailKind::Rejected: return EventKindV3::CompactRejected;
    }
    return EventKindV3::CompactFailed;
}

}  // namespace

CompactSession::CompactSession(std::string compact_id, std::string turn_id,
                               std::optional<std::string> parent_turn_id,
                               std::uint64_t source_revision)
    : compact_id_(std::move(compact_id)),
      turn_id_(std::move(turn_id)),
      parent_turn_id_(std::move(parent_turn_id)),
      source_revision_(source_revision) {}

CompactSession::BeginOutcome CompactSession::Begin(V3Writer& writer, std::string_view trigger,
                                                    std::string_view reason,
                                                    std::optional<std::string> parent_turn_id,
                                                    nlohmann::json requirements_snapshot,
                                                    Durability durability) {
    BeginOutcome outcome;
    // 一次只运行一个 compact(§4.6);额外触发拒收并留原因。
    if (!writer.context().open_compact_ids.empty()) {
        outcome.info.error = "compact.busy: 已有进行中的 compact " +
                             writer.context().open_compact_ids.front();
        return outcome;
    }
    outcome.info.compact_id = writer.NewCompactId();
    outcome.info.turn_id = writer.NewTurnId();  // 独立内部回合(§4.6)
    EventDraft draft;
    draft.kind = EventKindV3::CompactRequested;
    draft.compact_id = outcome.info.compact_id;
    draft.turn_id = outcome.info.turn_id;
    draft.parent_turn_id = parent_turn_id;
    draft.payload = nlohmann::json::object(
        {{"trigger", std::string(trigger)},
         {"reason", std::string(reason)},
         {"requirementsSnapshot", std::move(requirements_snapshot)},
         {"sourceContextRevision", writer.context().revision}});
    outcome.info.requested = writer.AppendEvent(std::move(draft), durability);
    outcome.info.began = outcome.info.requested.status == WriteReceipt::Status::Committed;
    if (!outcome.info.began) {
        outcome.info.error =
            outcome.info.requested.error_code + " " + outcome.info.requested.error_message;
        return outcome;
    }
    // 直接 new(不用 make_unique):构造器是 private,make_unique 的模板
    // 上下文无权访问;new 表达式写在本成员函数体内,访问合法。
    outcome.session.reset(new CompactSession(outcome.info.compact_id, outcome.info.turn_id,
                                             std::move(parent_turn_id),
                                             writer.context().revision));
    outcome.session->trigger_ = std::string(trigger);
    return outcome;
}

CompactSession::FreezeResult CompactSession::Freeze(V3Writer& writer,
                                                    std::vector<std::string> removed_message_refs,
                                                    std::vector<std::string> retained_message_refs,
                                                    std::vector<std::string> protected_turn_ids,
                                                    Durability durability) {
    FreezeResult result;
    if (finished_) {
        result.eligible = false;
        result.error = "compact.finished";
        return result;
    }
    // 没有可摘要化历史:记录 no_eligible_history,不空调压缩模型(§4.9)。
    if (removed_message_refs.empty()) {
        EventDraft rejected;
        rejected.kind = EventKindV3::CompactRejected;
        rejected.status = OpStatus::Rejected;
        rejected.compact_id = compact_id_;
        rejected.turn_id = turn_id_;
        rejected.parent_turn_id = parent_turn_id_;
        rejected.payload = nlohmann::json::object(
            {{"reason", "no_eligible_history"},
             {"sourceContextRevision", writer.context().revision}});
        result.event = writer.AppendEvent(std::move(rejected), durability);
        result.eligible = false;
        finished_ = true;
        return result;
    }
    source_revision_ = writer.context().revision;  // 执行时冻结(§4.5 行1)
    removed_ = std::move(removed_message_refs);
    retained_ = std::move(retained_message_refs);
    protected_turns_ = std::move(protected_turn_ids);
    EventDraft draft;
    draft.kind = EventKindV3::CompactStarted;
    draft.status = OpStatus::Running;
    draft.compact_id = compact_id_;
    draft.turn_id = turn_id_;
    draft.parent_turn_id = parent_turn_id_;
    draft.payload = nlohmann::json::object(
        {{"sourceContextRevision", source_revision_},
         {"removedMessageRefs", RefsToJson(removed_)},
         {"retainedMessageRefs", RefsToJson(retained_)},
         {"protectedTurnIds", RefsToJson(protected_turns_)}});
    result.event = writer.AppendEvent(std::move(draft), durability);
    frozen_ = result.event.status == WriteReceipt::Status::Committed;
    return result;
}

WriteReceipt CompactSession::AppendPrompt(V3Writer& writer, nlohmann::json user_message,
                                          Durability durability) {
    MessageDraft draft;
    draft.turn_id = turn_id_;
    draft.parent_turn_id = parent_turn_id_;
    draft.compact_id = compact_id_;
    draft.purpose = MessagePurpose::Compact;
    draft.origin = MessageOrigin::CompactRuntime;  // runtime 拼装,不冒充真人输入
    draft.display = DisplayMode::Hidden;
    draft.message = std::move(user_message);
    return writer.AppendMessage(std::move(draft), durability);
}

WriteReceipt CompactSession::WriteSpecialSystem(V3Writer& writer,
                                                std::string_view system_content,
                                                Durability durability) {
    // 压缩专用 system(§4.3):另存实际内容,不替换会话 system。
    MessageDraft draft;
    draft.turn_id = std::nullopt;  // system 恒 null
    draft.compact_id = compact_id_;
    draft.purpose = MessagePurpose::Compact;
    draft.origin = MessageOrigin::CompactRuntime;
    draft.display = DisplayMode::Hidden;
    draft.system_meta = nlohmann::json::object(
        {{"cause", "compact_special"}, {"changeEventRef", nullptr}, {"systemChanged", false}});
    draft.message = nlohmann::json::object(
        {{"role", "system"}, {"content", std::string(system_content)}});
    return writer.AppendMessage(std::move(draft), durability);
}

WriteReceipt CompactSession::WriteCandidate(V3Writer& writer, nlohmann::json assistant_message,
                                            std::string_view request_id,
                                            std::string_view step_id, std::string_view provider,
                                            std::string_view wire, std::string_view model,
                                            nlohmann::json usage, Durability durability) {
    MessageDraft draft;
    draft.turn_id = turn_id_;
    draft.parent_turn_id = parent_turn_id_;
    draft.step_id = std::string(step_id);
    draft.request_id = std::string(request_id);
    draft.compact_id = compact_id_;
    draft.purpose = MessagePurpose::Compact;
    draft.origin = MessageOrigin::CompactRuntime;
    draft.display = DisplayMode::Hidden;
    draft.message = std::move(assistant_message);
    draft.provider = std::string(provider);
    draft.wire = std::string(wire);
    draft.model = std::string(model);
    draft.response_model = nlohmann::json(nullptr);
    draft.usage = std::move(usage);  // 压缩模型自己的 usage 归它,不充当主上下文数字
    WriteReceipt receipt = writer.AppendMessage(std::move(draft), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        candidate_message_id_ = receipt.id;
    }
    return receipt;
}

WriteReceipt CompactSession::StartValidation(V3Writer& writer, Durability durability) {
    EventDraft draft;
    draft.kind = EventKindV3::CompactValidationStarted;
    draft.status = OpStatus::Running;
    draft.compact_id = compact_id_;
    draft.turn_id = turn_id_;
    draft.parent_turn_id = parent_turn_id_;
    draft.payload = nlohmann::json::object(
        {{"candidateMessageRef", candidate_message_id_.value_or(std::string())}});
    return writer.AppendEvent(std::move(draft), durability);
}

WriteReceipt CompactSession::CompleteValidation(V3Writer& writer, bool passed,
                                                std::vector<nlohmann::json> checks,
                                                Durability durability) {
    nlohmann::json checks_json = nlohmann::json::array();
    for (auto& check : checks) {
        checks_json.push_back(std::move(check));
    }
    EventDraft draft;
    draft.kind = EventKindV3::CompactValidationCompleted;
    draft.status = OpStatus::Done;
    draft.compact_id = compact_id_;
    draft.turn_id = turn_id_;
    draft.parent_turn_id = parent_turn_id_;
    draft.payload = nlohmann::json::object(
        {{"passed", passed},
         {"validatorVersion", "compact-validator-1"},
         {"candidateMessageRef", candidate_message_id_.value_or(std::string())},
         {"checks", std::move(checks_json)}});
    WriteReceipt receipt = writer.AppendEvent(std::move(draft), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        validation_event_id_ = receipt.id;
    }
    return receipt;
}

CompactSession::ApplyResult CompactSession::Apply(V3Writer& writer,
                                                  std::string_view summary_content,
                                                  std::uint64_t context_tokens_before,
                                                  std::uint64_t context_tokens_after,
                                                  nlohmann::json token_metric,
                                                  Durability durability) {
    ApplyResult result;
    if (finished_ || !frozen_) {
        result.error = finished_ ? "compact.finished" : "compact.not_frozen";
        return result;
    }
    if (!candidate_message_id_.has_value() || !validation_event_id_.has_value()) {
        result.error = "compact.missing_candidate_or_validation";
        return result;
    }
    // 提交前再次核对源版本(§4.7"源未变化");变了记 conflict 拒收(§4.8)。
    if (writer.context().revision != source_revision_) {
        Fail(writer, FailKind::Rejected, "source_conflict", durability);
        result.error = "compact.source_conflict";
        return result;
    }
    // 新链 = 当前 system + 摘要 + 有序 retained(§4.30)。取快照拷贝,
    // 后续提交各走各的锁。
    const ContextView view = writer.context();
    std::vector<ChainNode> new_chain;
    new_chain.push_back(ChainNode{view.system_message_ref, std::nullopt});
    // 步骤 1:摘要消息先落稳(暂不生效,§4.5 行7)。
    {
        MessageDraft summary;
        summary.turn_id = std::nullopt;  // 摘要不属于真人回合(§4.6)
        summary.compact_id = compact_id_;
        summary.purpose = MessagePurpose::ContextSummary;
        summary.origin = MessageOrigin::CompactRuntime;
        summary.display = DisplayMode::Hidden;
        summary.source_message_ref = candidate_message_id_;
        summary.caused_by_event_ref = validation_event_id_;
        summary.message =
            nlohmann::json::object({{"role", "user"}, {"content", std::string(summary_content)}});
        result.summary = writer.AppendMessage(std::move(summary), durability);
        if (result.summary.status != WriteReceipt::Status::Committed) {
            result.error = result.summary.error_code + " " + result.summary.error_message;
            return result;
        }
    }
    new_chain.push_back(ChainNode{result.summary.id, view.system_message_ref});
    std::string prev = result.summary.id;
    for (const auto& ref : retained_) {
        new_chain.push_back(ChainNode{ref, prev});
        prev = ref;
    }
    // 步骤 2:compact.applied 原子提交(PowerLoss;§4.8 采用顺序)。
    const std::string old_state_hash = ComputeStateHash(view.context_id, source_revision_,
                                                         view.chain);
    const std::string new_state_hash =
        ComputeStateHash(view.context_id, source_revision_ + 1, new_chain);
    nlohmann::json chain_json = nlohmann::json::array();
    for (const auto& node : new_chain) {
        chain_json.push_back(node.ToJson());
    }
    EventDraft applied;
    applied.kind = EventKindV3::CompactApplied;
    applied.status = OpStatus::Done;
    applied.compact_id = compact_id_;
    applied.turn_id = turn_id_;
    applied.parent_turn_id = parent_turn_id_;
    applied.payload = nlohmann::json::object(
        {{"sourceContextRevision", source_revision_},
         {"newContextRevision", source_revision_ + 1},
         {"stateHashAlgorithm", "v3-refs-sha256"},
         {"oldStateHash", old_state_hash},
         {"newStateHash", new_state_hash},
         {"summaryMessageRef", result.summary.id},
         {"validationEventRef", *validation_event_id_},
         {"removedMessageRefs", RefsToJson(removed_)},
         {"retainedMessageRefs", RefsToJson(retained_)},
         {"protectedTurnIds", RefsToJson(protected_turns_)},
         {"contextId", view.context_id},
         {"contextChain", std::move(chain_json)},
         {"contextTokensBefore", context_tokens_before},
         {"contextTokensAfter", context_tokens_after},
         {"tokenMetric", std::move(token_metric)},
         {"trigger", trigger_}});
    // applied 的 trigger 冗余写 payload;信封 compactId 已贯穿。
    result.applied = writer.AppendEvent(std::move(applied), Durability::PowerLoss);
    if (result.applied.status != WriteReceipt::Status::Committed) {
        // applied 写盘失败:不得发布新内存视图,报告故障(§4.8);旧视图
        // 未动(writer 只在 applied 落稳后重放换账)。
        result.error = result.applied.error_code + " " + result.applied.error_message;
        return result;
    }
    finished_ = true;
    result.ok = true;
    return result;
}

WriteReceipt CompactSession::Fail(V3Writer& writer, FailKind kind, std::string_view reason,
                                  Durability durability) {
    EventDraft draft;
    draft.kind = FailKindToEvent(kind);
    draft.status = kind == FailKind::Failed     ? OpStatus::Failed
                   : kind == FailKind::Cancelled ? OpStatus::Cancelled
                                                  : OpStatus::Rejected;
    draft.compact_id = compact_id_;
    draft.turn_id = turn_id_;
    draft.parent_turn_id = parent_turn_id_;
    draft.payload = nlohmann::json::object(
        {{"reason", std::string(reason)},
         {"sourceContextRevision", source_revision_},
         {"candidateMessageRef", candidate_message_id_.value_or(std::string())}});
    WriteReceipt receipt = writer.AppendEvent(std::move(draft), durability);
    if (receipt.status == WriteReceipt::Status::Committed) {
        finished_ = true;
    }
    return receipt;
}

}  // namespace lubancode::trajectory::v3
