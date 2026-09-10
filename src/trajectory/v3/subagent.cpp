// subagent 独立账实现(§4.31-4.33)。
#include "trajectory/v3/subagent.hpp"

#include <system_error>

namespace lubancode::trajectory::v3 {

SubagentSpawn SubagentSpawn::Request(V3Writer& parent, std::string action_id,
                                     std::string turn_id, std::string step_id,
                                     std::string task_id, ChildSessionRef child,
                                     ParentActionRef parent_ref, nlohmann::json task_args,
                                     nlohmann::json config_snapshot,
                                     Durability durability) {
    EventDraft draft;
    draft.kind = EventKindV3::SubagentSpawnRequested;
    draft.turn_id = turn_id;
    draft.step_id = step_id;
    draft.action_id = action_id;
    draft.task_id = task_id;
    draft.payload = nlohmann::json::object(
        {{"taskId", task_id},
         {"childSessionRef", child.ToJson()},
         {"attempt", 1},
         {"parentActionRef", parent_ref.ToJson()},
         {"taskArgs", std::move(task_args)},
         {"configSnapshot", std::move(config_snapshot)},
         {"asyncStart", true}});  // 首版默认异步(§4.32)
    WriteReceipt receipt = parent.AppendEvent(std::move(draft), durability);
    SubagentSpawn spawn(std::move(action_id), std::move(turn_id), std::move(step_id),
                        std::move(task_id), std::move(child), std::move(parent_ref),
                        receipt);
    return spawn;
}

SubagentSpawn::BootstrapResult SubagentSpawn::BootstrapChild(
    const V3Writer& parent, std::string_view child_run_id,
    std::string_view child_system_content, std::string_view task_prompt,
    Durability durability) const {
    BootstrapResult outcome;
    // 子目录:父卷同层 subagents/<childSessionId>/(§4.31)。
    const std::filesystem::path parent_jsonl = parent.path();
    const std::filesystem::path session_root = parent_jsonl.parent_path();
    const std::filesystem::path child_dir =
        session_root / "subagents" / child_.session_id;
    std::error_code ec;
    std::filesystem::create_directories(child_dir, ec);
    if (ec) {
        outcome.error = "subagent.child_dir_failed: " + ec.message();
        return outcome;
    }
    const std::filesystem::path child_jsonl = child_dir / (child_.session_id + ".jsonl");
    // 派生来源进首行 system 的 systemMeta(§4.31:子账首行带派生来源);
    // spawnEventRef 五键含目标行 hash(§3.1 跨会话引用必带)。
    nlohmann::json spawn_ref = nlohmann::json::object(
        {{"sessionId", parent.session_id()},
         {"runId", parent.run_id()},
         {"seq", spawn_receipt_.seq},
         {"id", spawn_receipt_.id},
         {"hash", spawn_receipt_.line_hash}});
    nlohmann::json system_extra = nlohmann::json::object(
        {{"cause", "subagent_spawn"},
         {"parentActionRef", parent_ref_.ToJson()},
         {"taskId", task_id_},
         {"spawnEventRef", spawn_ref}});
    auto child = V3Writer::Start(child_jsonl, child_.session_id, child_run_id,
                                 child_system_content, std::move(system_extra),
                                 V3WriterOptions{});
    if (!child.has_value()) {
        outcome.error = "subagent.child_start_failed: " + child.error();
        return outcome;
    }
    // 委派任务 = 子账的真实 user 输入,origin=parent_agent(§4.31:
    // 不是子会话里的真人输入);先落稳再接纳。
    MessageDraft delegation;
    delegation.turn_id = child->NewTurnId();
    delegation.purpose = MessagePurpose::Conversation;
    delegation.origin = MessageOrigin::ParentAgent;
    delegation.message =
        nlohmann::json::object({{"role", "user"}, {"content", std::string(task_prompt)}});
    WriteReceipt delegation_receipt =
        child->AppendMessage(std::move(delegation), durability);
    if (delegation_receipt.status != WriteReceipt::Status::Committed) {
        outcome.error = "subagent.child_task_input_failed: " + delegation_receipt.error_code +
                        " " + delegation_receipt.error_message;
        return outcome;
    }
    WriteReceipt admit = child->AdmitMessages({delegation_receipt.id}, durability);
    if (admit.status != WriteReceipt::Status::Committed) {
        outcome.error = "subagent.child_admit_failed: " + admit.error_code + " " +
                        admit.error_message;
        return outcome;
    }
    // 任务开始记账在子账(taskId 贯穿,§4.32 步 3 前不执行副作用;
    // 真正执行由调用方在 Link 落稳后启动)。
    EventDraft started;
    started.kind = EventKindV3::TaskStarted;
    started.status = OpStatus::Running;
    started.task_id = task_id_;
    started.payload = nlohmann::json::object(
        {{"parentActionId", action_id_},
         {"taskId", task_id_},
         {"childSession", child_.ToJson()}});
    WriteReceipt started_receipt = child->AppendEvent(std::move(started), durability);
    if (started_receipt.status != WriteReceipt::Status::Committed) {
        outcome.error = "subagent.child_task_started_failed: " + started_receipt.error_code +
                        " " + started_receipt.error_message;
        return outcome;
    }
    outcome.checkpoint = ChildCheckpointRef{child_.session_id, std::string(child_run_id),
                                            child->next_seq() - 1, child->last_line_hash()};
    outcome.child_writer = std::move(*child);
    return outcome;
}

WriteReceipt SubagentSpawn::Link(V3Writer& parent, const ChildCheckpointRef& checkpoint,
                                 Durability durability) const {
    EventDraft draft;
    draft.kind = EventKindV3::SubagentLinked;
    draft.status = OpStatus::Done;
    draft.turn_id = turn_id_;
    draft.step_id = step_id_;
    draft.action_id = action_id_;
    draft.task_id = task_id_;
    draft.payload = nlohmann::json::object(
        {{"taskId", task_id_}, {"childCheckpointRef", checkpoint.ToJson()}});
    return parent.AppendEvent(std::move(draft), durability);
}

WriteReceipt SubagentSpawn::Observe(V3Writer& parent, const ChildCheckpointRef& checkpoint,
                                    nlohmann::json display_info,
                                    Durability durability) const {
    EventDraft draft;
    draft.kind = EventKindV3::SubagentObserved;
    draft.turn_id = turn_id_;
    draft.step_id = step_id_;
    draft.action_id = action_id_;
    draft.task_id = task_id_;
    draft.payload = nlohmann::json::object(
        {{"taskId", task_id_},
         {"childCheckpointRef", checkpoint.ToJson()},
         {"display", std::move(display_info)}});
    return parent.AppendEvent(std::move(draft), durability);
}

WriteReceipt SubagentSpawn::Fail(V3Writer& parent, std::string phase, std::string reason,
                                 Durability durability) const {
    EventDraft draft;
    draft.kind = EventKindV3::SubagentSpawnFailed;
    draft.status = OpStatus::Failed;
    draft.turn_id = turn_id_;
    draft.step_id = step_id_;
    draft.action_id = action_id_;
    draft.task_id = task_id_;
    draft.payload = nlohmann::json::object(
        {{"taskId", task_id_},
         {"phase", std::move(phase)},
         {"reason", std::move(reason)},
         {"childSessionRef", child_.ToJson()}});
    return parent.AppendEvent(std::move(draft), durability);
}

SubagentSpawn::SubagentSpawn(std::string action_id, std::string turn_id, std::string step_id,
                             std::string task_id, ChildSessionRef child,
                             ParentActionRef parent_ref, WriteReceipt spawn_receipt)
    : action_id_(std::move(action_id)),
      turn_id_(std::move(turn_id)),
      step_id_(std::move(step_id)),
      task_id_(std::move(task_id)),
      child_(std::move(child)),
      parent_ref_(std::move(parent_ref)),
      spawn_receipt_(std::move(spawn_receipt)) {}

}  // namespace lubancode::trajectory::v3
