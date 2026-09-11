// 验收收口编排实现(轨迹 v3 §4.67 G2)。见 goal_evaluation_flow.hpp 的
// 分层说明;本件只做编排与折算,状态提交全数走 GoalService。

#include "runtime/goal_evaluation_flow.hpp"

#include <utility>
#include <set>
#include "hooks/hash.hpp"

namespace lubancode::runtime::goal {

namespace {

using trajectory::v3::Durability;
using trajectory::v3::EventDraft;
using trajectory::v3::EventKindV3;

// v3 快照 -> evaluator 材料(v1 GoalTask 形状;revision 用 contractRevision
// ——evaluator 永远按这版判,§4.67.5)。
GoalTask SnapshotToTask(const GoalStateSnapshot& snapshot) {
    GoalTask task;
    task.id = snapshot.goal_id;
    task.revision = static_cast<int>(snapshot.contract_revision);
    task.objective = snapshot.objective;
    task.contract = snapshot.contract;
    task.budget = snapshot.budget;
    task.counters = snapshot.counters;
    task.usage = snapshot.usage;
    return task;
}

// 续跑意图:去重键 (goalId, contractRevision, predecessorIterationId,
// continuationOrdinal=1)——每轮至多产一枚续跑,workItemId 与轮号对齐,
// 重复收口(重试)得到同一枚 id,不另发工作(§4.67.4)。
GoalPendingIntent NextContinuationIntent(const GoalStateSnapshot& snapshot) {
    GoalPendingIntent intent;
    intent.work_item_id = snapshot.goal_id + "/wi-" +
                          std::to_string(snapshot.counters.iterations_started);
    intent.contract_revision = snapshot.contract_revision;
    intent.predecessor_iteration_id =
        snapshot.iteration_id.value_or(std::string());
    intent.continuation_ordinal = 1;
    return intent;
}

void Fail(GoalCloseoutResult& result, std::string code, std::string message) {
    result.ok = false;
    result.error_code = std::move(code);
    result.error_message = std::move(message);
}

}  // namespace

GoalCloseoutResult CloseGoalIterationWithEvaluation(
    GoalService& service, trajectory::v3::V3Writer& writer, api::Backend& backend,
    const GoalEvaluationFlowOptions& options, const GoalCloseoutMaterial& material,
    const std::atomic<bool>* cancel) {
    GoalCloseoutResult result;
    const GoalStateSnapshot* snapshot = service.current();
    if (snapshot == nullptr) {
        Fail(result, kErrGoalNotFound, "没有 goal 可收口");
        return result;
    }
    if (snapshot->phase != GoalPhase::Running) {
        Fail(result, kErrGoalCandidateInvalid,
             "不在执行轮收口位(phase=" + ToString(snapshot->phase) + ")");
        return result;
    }
    if (!snapshot->iteration_id.has_value()) {
        Fail(result, kErrGoalCandidateInvalid, "执行轮缺 iterationId(开轮账不全)");
        return result;
    }
    const std::string iteration_id = *snapshot->iteration_id;
    const std::string evaluation_id = "eval-" + iteration_id;
    result.evaluation_id = evaluation_id;
    const nlohmann::json cause = nlohmann::json{{"source", "host"},
                                                {"iterationId", iteration_id}};

    // ---- 1) 证据入账:本轮新采逐枚落 goal.evidence.recorded 事实行 -----
    std::vector<GoalEvidenceRef> evidence_refs;
    for (const auto& evidence : material.fresh_evidence) {
        const GoalEvidenceRef ref =
            EvidenceRefFromTrace(evidence, writer.session_id(), writer.run_id(),
                                material.workspace_baseline);
        EventDraft recorded;
        recorded.kind = EventKindV3::GoalEvidenceRecorded;
        if (!material.parent_turn_id.empty()) {
            recorded.turn_id = material.parent_turn_id;
        }
        recorded.payload["goalId"] = snapshot->goal_id;
        recorded.payload["iterationId"] = iteration_id;
        recorded.payload["evidenceId"] = ref.id;
        recorded.payload["evidence"] = ref.ToJson();
        // v1 采证的 facts(命令/退出码/digest)一并留档:引用回指源账,
        // facts 让判材料可回溯(G3 从账投影旧证据时用)。
        recorded.payload["facts"] = evidence.facts;
        const auto receipt = writer.AppendEvent(std::move(recorded), Durability::ProcessCrash);
        if (receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
            Fail(result, kErrGoalStoreUnavailable,
                 "goal.evidence.recorded 落账失败(" + receipt.error_code + "): " +
                     receipt.error_message);
            return result;
        }
        evidence_refs.push_back(ref);
    }

    // ---- 2) checkpoint 入账:goal.checkpoint.recorded(不改活动 head) ---
    EventDraft checkpoint_event;
    checkpoint_event.kind = EventKindV3::GoalCheckpointRecorded;
    if (!material.parent_turn_id.empty()) {
        checkpoint_event.turn_id = material.parent_turn_id;
    }
    checkpoint_event.payload["goalId"] = snapshot->goal_id;
    checkpoint_event.payload["iterationId"] = iteration_id;
    checkpoint_event.payload["checkpoint"] = material.checkpoint.to_json();
    checkpoint_event.payload["synthesized"] = material.checkpoint.synthesized;
    const auto checkpoint_receipt =
        writer.AppendEvent(std::move(checkpoint_event), Durability::ProcessCrash);
    if (checkpoint_receipt.status != trajectory::v3::WriteReceipt::Status::Committed) {
        Fail(result, kErrGoalStoreUnavailable,
             "goal.checkpoint.recorded 落账失败(" + checkpoint_receipt.error_code + "): " +
                 checkpoint_receipt.error_message);
        return result;
    }

    // ---- 3) 排评估:phase -> evaluating(评估崩溃窗口的锚,§4.67.8) ----
    auto began = service.BeginEvaluation(snapshot->state_revision,
                                         checkpoint_receipt.id, std::move(evidence_refs),
                                         material.evidence_stale_ids, cause);
    if (!began.ok) {
        Fail(result, began.error_code, began.error_message);
        return result;
    }
    const GoalStateSnapshot* evaluating = service.current();
    if (evaluating == nullptr) {
        Fail(result, kErrGoalStoreUnavailable, "BeginEvaluation 后快照缺失");
        return result;
    }

    // ---- 4) 内部请求服务采判词(§4.67.5:经宿主调度并留账) -----------
    GoalEvaluationInput input;
    input.task = SnapshotToTask(*evaluating);
    input.checkpoint = material.checkpoint;
    input.evidence = material.material_evidence;
    input.previous = material.previous;
    input.workspace_summary = material.workspace_summary;
    input.wait_task_refs = material.wait_task_refs;
    input.now_ms = material.now_ms;

    GoalEvaluatorOptions evaluator_options;
    evaluator_options.model = options.model;
    evaluator_options.provider = options.provider;
    evaluator_options.wire = options.wire;
    evaluator_options.reasoning_effort = options.reasoning_effort;
    evaluator_options.timeout_secs = options.timeout_secs;
    evaluator_options.max_tokens = options.max_tokens;
    evaluator_options.ledger.writer = &writer;
    evaluator_options.ledger.goal_id = evaluating->goal_id;
    evaluator_options.ledger.iteration_id = iteration_id;
    evaluator_options.ledger.evaluation_id = evaluation_id;
    evaluator_options.ledger.parent_turn_id = material.parent_turn_id;

    const auto evaluation =
        RunGoalEvaluation(backend, evaluator_options, input, cancel);
    if (!evaluation.has_value()) {
        // evaluator 两坏/超时/请求失败:无效判词不采用——evaluator_failed
        // 暂停收口(§4.67.5),费用已在账(requested/failed/rejected 事实行),
        // 不默认 achieved,不盲排下一轮。
        EvaluationVerdict verdict;
        verdict.evaluation_id = evaluation_id;
        verdict.kind = GoalVerdictKind::EvaluatorFailed;
        verdict.stop_reason = "evaluator_failed: " + evaluation.error();
        const auto closed = service.CompleteIterationWithEvaluation(
            evaluating->state_revision, verdict, cause);
        result.decision = "evaluator_failed";
        result.summary = evaluation.error();
        result.ok = closed.ok;
        if (!closed.ok) {
            result.error_code = closed.error_code;
            result.error_message = closed.error_message;
        }
        return result;
    }
    result.usage = evaluation->usage;
    result.summary = evaluation->evaluation.summary;

    // ---- 5) 程序完成门槛:achieved 缺口改判 continue(§4.67.5) --------
    GoalEvaluation adopted = evaluation->evaluation;
    if (adopted.decision == GoalDecision::Achieved) {
        const GoalAchievementAudit audit = AuditAchievedDecision(input, adopted);
        if (!audit.eligible) {
            adopted.decision = GoalDecision::Continue;
            adopted.overridden_achieved = true;
            std::string joined;
            for (const auto& failure : audit.failures) {
                if (!joined.empty()) joined += "; ";
                joined += failure;
            }
            adopted.override_reason = joined;
            adopted.next_action = "补证据: " + joined;
            result.overridden_achieved = true;
            result.override_reason = joined;
        }
    }

    // ---- 6) 采用:判词与下一轮意图同一快照提交(§4.67.6) --------------
    EvaluationVerdict verdict;
    verdict.evaluation_id = evaluation_id;
    verdict.evaluation = adopted.to_json();
    verdict.usage_addition = evaluation->usage;
    // Hash material facts, not the evaluator's self-reported progress flag or
    // changing prose. New ids for repeated identical evidence do not reset it.
    std::set<std::string> evidence_facts;
    for (const auto& evidence : material.material_evidence) {
        evidence_facts.insert(nlohmann::json{{"kind", ToString(evidence.kind)},
            {"facts", evidence.facts}, {"fresh", evidence.fresh},
            {"truncated", evidence.truncated}}.dump());
    }
    nlohmann::json criterion_states = nlohmann::json::object();
    for (const auto& criterion : adopted.criteria)
        criterion_states[criterion.id] = criterion.status;
    verdict.progress_fingerprint = hooks::Sha256Hex(nlohmann::json{
        {"workspaceBaseline", material.workspace_baseline}, {"evidence", evidence_facts},
        {"criteria", criterion_states}}.dump());

    switch (adopted.decision) {
        case GoalDecision::Continue:
            verdict.kind = GoalVerdictKind::Continue;
            verdict.next_intent = NextContinuationIntent(*evaluating);
            result.next_work_item_id = verdict.next_intent->work_item_id;
            break;
        case GoalDecision::Achieved:
            verdict.kind = GoalVerdictKind::Achieved;
            break;
        case GoalDecision::Blocked:
            verdict.kind = GoalVerdictKind::Blocked;
            verdict.blocker_key = adopted.blocker_key.value_or(std::string());
            verdict.stop_reason = "blocked: " + adopted.blocker_key.value_or(std::string());
            break;
        case GoalDecision::NeedsUser:
            verdict.kind = GoalVerdictKind::NeedsUser;
            verdict.pending_question = adopted.question.value_or(std::string());
            verdict.stop_reason = "needs_user";
            break;
    }
    const auto closed =
        service.CompleteIterationWithEvaluation(evaluating->state_revision, verdict, cause);
    if (!closed.ok) {
        Fail(result, closed.error_code, closed.error_message);
        return result;
    }
    if (closed.payload.contains("parked")) result.next_work_item_id.clear();
    result.decision = ToString(adopted.decision);
    result.ok = true;
    return result;
}

}  // namespace lubancode::runtime::goal
