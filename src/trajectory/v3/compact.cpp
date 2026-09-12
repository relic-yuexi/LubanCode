// compact 全链实现。
#include "trajectory/v3/compact.hpp"

#include <set>
#include "hooks/hash.hpp"
#include "trajectory/v3/reader.hpp"
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
    outcome.info.turn_id = writer.NewCompactTurnId();  // 独立内部回合,独立前缀不与主 turn 撞号(§4.6)
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
                                                    Durability durability,
                                                    nlohmann::json step_scope) {
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
    if (source_revision_ != writer.context().revision) {
        result.error = "source_conflict";
        result.eligible = false;
        result.event = Fail(writer, FailKind::Rejected, result.error);
        return result;
    }
    if (!step_scope.empty()) {
        auto ledger = ReadV3Ledger(writer.path());
        bool valid = ledger.has_value() && step_scope.is_object() &&
            step_scope.contains("turnId") && step_scope["turnId"].is_string() &&
            step_scope.contains("stepIds") && step_scope["stepIds"].is_array();
        std::set<std::string> expected_steps;
        if (valid) for (const auto& id : step_scope["stepIds"]) {
            if (!id.is_string() || id.get<std::string>().empty() ||
                !expected_steps.insert(id.get<std::string>()).second) valid = false;
        }
        if (valid) {
            const auto turn = step_scope["turnId"].get<std::string>();
            std::set<std::string> actual_steps;
            const std::set<std::string> removed(removed_message_refs.begin(), removed_message_refs.end());
            const std::set<std::string> retained(retained_message_refs.begin(), retained_message_refs.end());
            for (const auto& node : ledger->context.chain) {
                const auto* line = ledger->FindMessage(node.message_ref);
                if (!line || line->turn_id != turn) continue;
                if (removed.count(node.message_ref)) {
                    valid = valid && line->step_id.has_value() &&
                        line->message.value("role", std::string()) != "user";
                    if (line->step_id) actual_steps.insert(*line->step_id);
                } else {
                    valid = valid && retained.count(node.message_ref) > 0;
                    if (line->step_id && expected_steps.count(*line->step_id) &&
                        line->message.value("role", std::string()) != "user") valid = false;
                }
            }
            std::optional<std::string> latest_step;
            bool retained_step_seen = false;
            for (const auto& node : ledger->context.chain) {
                const auto* line = ledger->FindMessage(node.message_ref);
                if (!line || line->turn_id != turn || !line->step_id ||
                    line->message.value("role", std::string()) == "user") continue;
                latest_step = line->step_id;
                if (!removed.count(node.message_ref)) retained_step_seen = true;
                else if (retained_step_seen) valid = false; // selected steps must form an old prefix
            }
            if (latest_step && expected_steps.count(*latest_step)) valid = false;
            for (const auto& action : FoldToolActions(*ledger)) {
                if (action.turn_id != turn || !expected_steps.count(action.step_id)) continue;
                const bool terminal = action.folded_status == "done" || action.folded_status == "failed" ||
                    action.folded_status == "cancelled" || action.folded_status == "rejected";
                bool selected_result = false;
                for (const auto& version : action.message_versions) {
                    if (version.on_current_chain) {
                        selected_result = selected_result || removed.count(version.message_id) > 0;
                        if (retained.count(version.message_id)) valid = false;
                    }
                }
                valid = valid && terminal && selected_result && action.assistant_message_ref &&
                    removed.count(*action.assistant_message_ref) > 0;
            }
            valid = valid && !actual_steps.empty() && actual_steps == expected_steps;
        }
        if (!valid) {
            result.error = "invalid_step_scope";
            result.eligible = false;
            result.event = Fail(writer, FailKind::Rejected, result.error);
            return result;
        }
    }
    source_revision_ = writer.context().revision;  // 执行时冻结(§4.5 行1)
    removed_ = std::move(removed_message_refs);
    retained_ = std::move(retained_message_refs);
    protected_turns_ = std::move(protected_turn_ids);
    step_scope_ = std::move(step_scope);
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
         {"protectedTurnIds", RefsToJson(protected_turns_)},
         {"stepScope", step_scope_}});
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
                                            nlohmann::json usage, Durability durability,
                                            std::optional<CompletionStatus> completion_status) {
    // 流式三件套(D1 延伸,§4.43):started 预留 messageId+streamId →
    // 零批 delta(压缩客户端同步整收,非流式后端同款零批)→ completed
    // 定稿 + 完整 assistant 以预留 id 成行。与生产桥同一闭环语义,但
    // 不借 CompleteStreamResponse 整包:那一路把 origin 定死 session_
    // runtime 且不带 parentTurnId,会改掉候选行形状(内部回合纪律:
    // origin=compact_runtime、parentTurnId 挂主 turn);compact 内部
    // 回复也永不接纳进 main 链(§4.8),接纳半步本就用不上。
    const std::string stream_id = writer.NewStreamId();
    const std::string reserved_message_id = writer.NewMessageId();
    const WriteReceipt started =
        writer.BeginStreamResponse(request_id, stream_id, turn_id_, step_id,
                                   reserved_message_id, Durability::ProcessCrash);
    if (started.status != WriteReceipt::Status::Committed) {
        return started;  // started 记不住,候选不得成行(§4.4 同款栅栏)
    }
    {
        EventDraft done;
        done.kind = EventKindV3::ModelResponseCompleted;
        done.status = OpStatus::Done;
        done.turn_id = turn_id_;
        done.parent_turn_id = parent_turn_id_;
        done.step_id = std::string(step_id);
        done.request_id = std::string(request_id);
        done.compact_id = compact_id_;
        done.payload = nlohmann::json::object(
            {{"requestId", std::string(request_id)},
             {"streamId", stream_id},
             {"messageId", reserved_message_id},
             {"finishReason",
              completion_status == CompletionStatus::Truncated ? "length" : "end_turn"}});
        const WriteReceipt completed = writer.AppendEvent(std::move(done), durability);
        if (completed.status != WriteReceipt::Status::Committed) {
            return completed;  // 定稿记不住,assistant 不成行
        }
    }
    MessageDraft draft;
    draft.message_id_override = reserved_message_id;  // 预留 id 成行(§4.43)
    draft.turn_id = turn_id_;
    draft.parent_turn_id = parent_turn_id_;
    draft.step_id = std::string(step_id);
    draft.request_id = std::string(request_id);
    draft.compact_id = compact_id_;
    draft.purpose = MessagePurpose::Compact;
    draft.origin = MessageOrigin::CompactRuntime;
    draft.display = DisplayMode::Hidden;
    draft.completion_status = completion_status;
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
         {"stepScope", step_scope_},
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
