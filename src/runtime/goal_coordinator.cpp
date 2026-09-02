// GoalCoordinator 实现:命令面、iteration 生命周期、预算闸、防空转、
// 硬门槛、恢复回放。纯内存 + LedgerSink 落账回调,不碰磁盘(问题 3 起
// 文件尾多一只 MakeSessionLedgerSink:把事件折进 SessionStore 的 sink 搭建,
// 递给装配层用,coordinator 本体仍零磁盘)。

#include "runtime/goal_coordinator.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#include "hooks/hash.hpp"
#include "platform/json_safe.hpp"  // DumpJsonSanitized:goal 事件落档的编码窄边界
#include "runtime/budget_gate.hpp"  // 三种宿主共用的预算闸与连撞计数
#include "runtime/replay.hpp"  // RestoreFromArchive 的次序、跳过与账面规矩

namespace lubancode::runtime::goal {

GoalCoordinator::GoalCoordinator(Options options) : options_(options) {}

GoalCoordinator::~GoalCoordinator() = default;

GoalCommandResult GoalCoordinator::Fail(const char* code, const std::string& message) {
    GoalCommandResult r;
    r.ok = false;
    r.error_code = code;
    r.error_message = message;
    return r;
}

bool GoalCoordinator::Emit(const GoalCoordinatorEvent& event) {
    if (!ledger_) return true;  // 没接存档(单测):事件照吃,只不落盘
    return ledger_(event);
}

void GoalCoordinator::FailClosed(std::int64_t now_ms, const std::string& reason) {
    if (!task_.has_value()) return;
    // 存档写不落:fail closed。goal 进 Failed(terminal),ready 清空,
    // 不排下一轮(单子"SessionStore append 失败后,fail closed")。
    ready_.reset();
    ready_dedupe_.clear();
    if (!IsGoalTerminal(task_->state)) {
        task_->state = GoalState::Failed;
        task_->terminal_at_ms = now_ms;
        task_->updated_at_ms = now_ms;
    }
    (void)reason;
}

// ---------------------------------------------------------------------------
// 命令面
// ---------------------------------------------------------------------------

GoalCommandResult GoalCoordinator::Create(const std::string& objective, const std::string& workspace_root,
                                          const std::string& workspace_identity, std::int64_t now_ms,
                                          std::function<std::string()> id_issuer) {
    return CreateWithLineage(objective, workspace_root, workspace_identity, /*parent_goal_id=*/std::string(),
                             now_ms, std::move(id_issuer));
}

GoalCommandResult GoalCoordinator::CreateWithLineage(const std::string& objective,
                                                     const std::string& workspace_root,
                                                     const std::string& workspace_identity,
                                                     const std::string& parent_goal_id, std::int64_t now_ms,
                                                     std::function<std::string()> id_issuer) {
    if (!options_.goals_enabled) {
        GoalCommandResult r = Fail(kErrGoalStoreUnavailable, "goals 功能未开启(features.goals)");
        r.payload["disabled"] = true;
        return r;
    }
    if (const std::string invalid = ValidateGoalObjective(objective); !invalid.empty()) {
        return Fail(invalid.c_str(),
                    invalid == kErrGoalObjectiveEmpty ? "objective 不能为空" : "objective 超过 4000 字符上限");
    }
    if (HasActiveGoal()) {
        return Fail(kErrGoalAlreadyActive, "已有一只活动目标;用 edit/clear,或另开 thread");
    }

    GoalTask t;
    // 发号(批五):goal-N 走 IdAuthority(与旧 goal_seq_ 同起算,1 起);
    // 注入口照旧(测试定死 id)。
    t.id = id_issuer ? id_issuer() : ids_.NextPrefixedId("goal");
    t.parent_goal_id = parent_goal_id;  // fork lineage:只记账,不构成复活/共享
    t.revision = 1;
    t.objective = objective;
    t.objective_sha256 = hooks::Sha256Hex(objective);
    t.state = GoalState::Preparing;
    t.budget.max_elapsed_ms = options_.default_max_elapsed_ms;
    t.budget.max_iterations = options_.default_max_iterations;
    t.budget.max_no_progress_iterations = options_.default_max_no_progress_iterations;
    t.budget.max_same_blocker_iterations = options_.default_max_same_blocker_iterations;
    t.budget.max_consecutive_provider_failures = options_.default_max_consecutive_provider_failures;
    t.workspace_root = workspace_root;
    t.workspace_identity = workspace_identity;
    t.created_at_ms = now_ms;
    t.updated_at_ms = now_ms;

    // 写盘栅栏 1:created flush 成功,才向用户报 goal 已立。
    GoalCoordinatorEvent ev;
    ev.event = "created";
    ev.goal_id = t.id;
    ev.revision = t.revision;
    ev.payload["objective"] = t.objective;
    ev.payload["objective_sha256"] = t.objective_sha256;
    ev.payload["budget"] = t.budget.to_json();
    ev.payload["workspace_root"] = t.workspace_root;
    ev.payload["workspace_identity"] = t.workspace_identity;
    if (!t.parent_goal_id.empty()) {
        ev.payload["parent_goal_id"] = t.parent_goal_id;  // lineage 随 created 落档
    }
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "created 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "goal 事件写盘失败,目标未立");
    }
    task_ = std::move(t);
    next_iteration_index_ = 0;

    GoalCommandResult r;
    r.ok = true;
    r.payload["goal_id"] = task_->id;
    r.payload["state"] = ToString(task_->state);
    if (!task_->parent_goal_id.empty()) {
        r.payload["parent_goal_id"] = task_->parent_goal_id;
    }
    return r;
}

GoalCommandResult GoalCoordinator::ForkFrom(const GoalTask& source, const std::string& workspace_root,
                                            const std::string& workspace_identity, std::int64_t now_ms,
                                            std::function<std::string()> id_issuer) {
    // fork 单子的三条边界(单子"crash、resume、fork、clear thread"节):
    //   - fork 复制 goal snapshot,新分支状态设 Paused(不默认两边一起续跑);
    //   - 原 thread 与 fork 不共享同一 active goal id(新 id 由发号口出,
    //     lineage 记 source id);
    //   - 复制的是合同/预算/计数器/最近 checkpoint 的账面;usage 从零起
    //     (新 thread 的消耗自己记),evidence 不搬(证据钉在旧 iteration,
    //     跨 thread 引用查无对账)。
    // terminal 的源也收——fork 不是复活,是把账面抄去新处;源的状态不追改。
    GoalCommandResult r = CreateWithLineage(source.objective, workspace_root, workspace_identity, source.id,
                                            now_ms, std::move(id_issuer));
    if (!r.ok) {
        return r;
    }
    // 抄账面:合同(冻结态与 hash 一并)、预算、计数器、checkpoint。
    task_->contract = source.contract;
    task_->contract_frozen = source.contract_frozen;
    task_->contract_sha256 = source.contract_sha256;
    task_->budget = source.budget;
    task_->counters = source.counters;
    task_->counters.last_progress_fingerprint.clear();  // 新处起算防空转误判
    task_->checkpoint = source.checkpoint;
    task_->state = GoalState::Paused;  // 用户明确 resume 才跑
    GoalCoordinatorEvent ev;
    ev.event = "forked";
    ev.goal_id = task_->id;
    ev.revision = task_->revision;
    ev.payload["parent_goal_id"] = source.id;
    ev.payload["source_state"] = ToString(source.state);
    ev.payload["state"] = ToString(task_->state);
    // 抄去的账面随事件落档(回放侧凭这一份重建 fork 的 snapshot,不必
    // 去源 thread 的存档翻)。
    ev.payload["contract"] = task_->contract.to_json();
    ev.payload["contract_frozen"] = task_->contract_frozen;
    ev.payload["contract_sha256"] = task_->contract_sha256;
    ev.payload["budget"] = task_->budget.to_json();
    ev.payload["checkpoint"] = task_->checkpoint.to_json();
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "forked 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "forked 事件写盘失败");
    }
    r.payload["state"] = ToString(task_->state);
    r.payload["parent_goal_id"] = source.id;
    return r;
}

nlohmann::json GoalCoordinator::Status(std::int64_t now_ms) const {
    nlohmann::json j = nlohmann::json::object();
    j["goals_enabled"] = options_.goals_enabled;
    if (!task_.has_value()) {
        j["has_goal"] = false;
        return j;
    }
    j["has_goal"] = true;
    j["goal"] = task_->to_json();
    j["state_label"] = ToString(task_->state);
    // 预算消耗快照(状态栏那块):
    nlohmann::json spent;
    spent["iterations"] = task_->counters.iterations_started;
    if (task_->budget.max_iterations.has_value()) {
        spent["iterations_limit"] = *task_->budget.max_iterations;
    }
    if (task_->started_at_ms.has_value() && task_->budget.max_elapsed_ms.has_value()) {
        const std::int64_t elapsed = std::max<std::int64_t>(0, now_ms - *task_->started_at_ms);
        spent["elapsed_ms"] = elapsed;
        spent["elapsed_limit_ms"] = *task_->budget.max_elapsed_ms;
    }
    spent["usage"] = task_->usage.to_json();
    j["spent"] = std::move(spent);
    j["no_progress_streak"] = task_->counters.no_progress_streak;
    j["same_blocker_streak"] = task_->counters.same_blocker_streak;
    j["pause_requested"] = pause_requested_;
    j["has_ready_continuation"] = ready_.has_value();
    if (task_->last_evaluation.has_value()) {
        j["last_decision"] = ToString(task_->last_evaluation->decision);
    }
    return j;
}

GoalCommandResult GoalCoordinator::Edit(const std::string& objective, int expected_revision,
                                        std::int64_t now_ms) {
    if (!task_.has_value()) return Fail(kErrGoalNotFound, "没有活动目标");
    if (IsGoalTerminal(task_->state)) return Fail(kErrGoalTerminal, "目标已收账,edit 不再受理");
    if (expected_revision != 0 && expected_revision != task_->revision) {
        GoalCommandResult r = Fail(kErrGoalRevisionConflict, "revision 冲突:目标已被改过");
        r.payload["current_revision"] = task_->revision;
        return r;
    }
    if (const std::string invalid = ValidateGoalObjective(objective); !invalid.empty()) {
        return Fail(invalid.c_str(), "objective 非法");
    }
    // Running:先在安全边界 pause;首版直接拒 busy(单子"当前 Running 时先
    // pause at boundary"由装配层在边界重放 edit;这里报 busy 让前端等)。
    if (task_->state == GoalState::Running || task_->state == GoalState::Evaluating ||
        task_->state == GoalState::Pausing) {
        return Fail(kErrGoalBusy, "目标正在跑;edit 等安全边界(先 pause)");
    }

    const int new_revision = task_->revision + 1;
    GoalCoordinatorEvent ev;
    ev.event = "edited";
    ev.goal_id = task_->id;
    ev.revision = new_revision;
    ev.payload["objective"] = objective;
    ev.payload["objective_sha256"] = hooks::Sha256Hex(objective);
    ev.payload["previous_revision"] = task_->revision;
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "edit 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "edit 事件写盘失败");
    }
    task_->objective = objective;
    task_->objective_sha256 = hooks::Sha256Hex(objective);
    task_->revision = new_revision;
    // edit 后 no-progress/blocker streak 清零;usage 不清零。合同回 Preparing
    // 重拟(单子:先存新 objective,revision+1,重跑 contract preflight)。
    task_->state = GoalState::Preparing;
    task_->contract_frozen = false;
    task_->contract_sha256.clear();
    task_->contract = GoalContract{};
    task_->counters.no_progress_streak = 0;
    task_->counters.same_blocker_streak = 0;
    task_->counters.last_blocker_key.clear();
    task_->updated_at_ms = now_ms;
    ready_.reset();
    ready_dedupe_.clear();

    GoalCommandResult r;
    r.ok = true;
    r.payload["revision"] = task_->revision;
    return r;
}

GoalCommandResult GoalCoordinator::Pause(std::int64_t now_ms) {
    if (!task_.has_value()) return Fail(kErrGoalNotFound, "没有活动目标");
    const PauseOutcome p = PauseTransition(task_->state);
    if (!p.allowed) {
        return Fail(kErrGoalTerminal, "目标已收账,pause 无意义");
    }
    // 幂等:已 Paused/Pausing 再 pause 返回当前状态,不添事件。
    if (task_->state == GoalState::Paused || task_->state == GoalState::Pausing) {
        GoalCommandResult r;
        r.ok = true;
        r.payload["state"] = ToString(task_->state);
        r.payload["idempotent"] = true;
        return r;
    }
    pause_requested_ = true;
    GoalCoordinatorEvent ev;
    ev.event = "pause_requested";
    ev.goal_id = task_->id;
    ev.revision = task_->revision;
    ev.payload["target_state"] = ToString(p.target);
    ev.payload["immediate"] = p.immediate;
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "pause 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "pause 事件写盘失败");
    }
    if (p.immediate) {
        task_->state = p.target;
        task_->updated_at_ms = now_ms;
        ready_.reset();
        ready_dedupe_.clear();
    } else {
        task_->state = GoalState::Pausing;
        task_->updated_at_ms = now_ms;
    }
    GoalCommandResult r;
    r.ok = true;
    r.payload["state"] = ToString(task_->state);
    r.payload["immediate"] = p.immediate;
    return r;
}

GoalCommandResult GoalCoordinator::Resume(int expected_revision, std::int64_t now_ms) {
    if (!task_.has_value()) return Fail(kErrGoalNotFound, "没有活动目标");
    if (!options_.goals_enabled) {
        GoalCommandResult r = Fail(kErrGoalStoreUnavailable, "goals 功能未开启");
        r.payload["disabled"] = true;
        return r;
    }
    if (IsGoalTerminal(task_->state)) {
        // 幂等角:Cleared 后 resume 报 terminal,不复活。
        return Fail(kErrGoalTerminal, "目标已收账;Achieved 后要再跑,新建目标");
    }
    if (!IsGoalResumable(task_->state)) {
        return Fail(kErrGoalInvalidTransition,
                    "当前状态(" + ToString(task_->state) + ")不可 resume");
    }
    if (expected_revision != 0 && expected_revision != task_->revision) {
        GoalCommandResult r = Fail(kErrGoalRevisionConflict, "revision 冲突");
        r.payload["current_revision"] = task_->revision;
        return r;
    }
    // resume preflight:预算未尽(workspace/root 对账由装配层在调用前验,
    // 这里管预算)。
    std::string budget_reason;
    if (!CheckBudgetHeadroom(now_ms, &budget_reason)) {
        return Fail(kErrGoalBudgetExhausted, "预算已尽:" + budget_reason);
    }
    // Preparing(合同未冻结)的 resume 退回 preflight;这里统一进 Active
    // 留给 SubmitContract,若合同未冻结则状态回 Preparing。
    const GoalState target =
        task_->contract_frozen ? GoalState::Active : GoalState::Preparing;
    GoalCoordinatorEvent ev;
    ev.event = "resumed";
    ev.goal_id = task_->id;
    ev.revision = task_->revision;
    ev.payload["from_state"] = ToString(task_->state);
    ev.payload["to_state"] = ToString(target);
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "resume 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "resume 事件写盘失败");
    }
    task_->state = target;
    // resume 清连续 blocker 计数,保留旧账(usage/iteration 数不清零)。
    task_->counters.same_blocker_streak = 0;
    task_->counters.no_progress_streak = 0;
    task_->counters.last_blocker_key.clear();
    pause_requested_ = false;
    task_->updated_at_ms = now_ms;
    GoalCommandResult r;
    r.ok = true;
    r.payload["state"] = ToString(task_->state);
    return r;
}

GoalCommandResult GoalCoordinator::Clear(std::int64_t now_ms) {
    if (!task_.has_value()) return Fail(kErrGoalNotFound, "没有活动目标");
    if (IsGoalTerminal(task_->state)) {
        // 幂等:terminal 后重复 clear 返回当前状态,不添重复 terminal event。
        GoalCommandResult r;
        r.ok = true;
        r.payload["state"] = ToString(task_->state);
        r.payload["idempotent"] = true;
        return r;
    }
    GoalCoordinatorEvent ev;
    ev.event = "cleared";
    ev.goal_id = task_->id;
    ev.revision = task_->revision;
    ev.payload["from_state"] = ToString(task_->state);
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "clear 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "clear 事件写盘失败");
    }
    task_->state = GoalState::Cleared;
    task_->terminal_at_ms = now_ms;
    task_->updated_at_ms = now_ms;
    ready_.reset();
    ready_dedupe_.clear();
    pause_requested_ = false;
    GoalCommandResult r;
    r.ok = true;
    r.payload["state"] = ToString(task_->state);
    r.payload["note"] = "审计账保留;clear 不撤销已改文件";
    return r;
}

// ---------------------------------------------------------------------------
// 合同 preflight
// ---------------------------------------------------------------------------

GoalCommandResult GoalCoordinator::SubmitContract(const GoalContract& contract, std::int64_t now_ms) {
    if (!task_.has_value()) return Fail(kErrGoalNotFound, "没有活动目标");
    if (IsGoalTerminal(task_->state)) return Fail(kErrGoalTerminal, "目标已收账");
    if (task_->state != GoalState::Preparing) {
        return Fail(kErrGoalInvalidTransition, "合同只在 Preparing 期冻结;要改用 /goal edit");
    }
    std::string reason;
    const PreflightVerdict verdict = PreflightContract(contract, &reason);
    if (verdict == PreflightVerdict::Reject) {
        return Fail("goal.contract_rejected", reason);
    }
    if (verdict == PreflightVerdict::NeedsUser) {
        // 空话目标/缺产品选择:进 AwaitingUser 问一句,不自动长跑。
        GoalCoordinatorEvent ev;
        ev.event = "awaiting_user";
        ev.goal_id = task_->id;
        ev.revision = task_->revision;
        ev.payload["reason"] = reason;
        ev.timestamp_ms = now_ms;
        if (!Emit(ev)) {
            FailClosed(now_ms, "awaiting_user 写盘失败");
            return Fail(kErrGoalStoreUnavailable, "事件写盘失败");
        }
        task_->state = GoalState::AwaitingUser;
        task_->updated_at_ms = now_ms;
        GoalCommandResult r;
        r.ok = true;
        r.payload["state"] = ToString(task_->state);
        r.payload["reason"] = reason;
        return r;
    }

    // 写盘栅栏 2:contract_ready 成功,才开首轮有副作用工具。
    // contract 的 objective/描述是用户键入与模型产出,坏串窄边界:dump
    // 不许抛 316(裸 dump 遇坏串直接穿透,顶层就是"未预料的异常")。
    const std::string contract_json = platform::DumpJsonSanitized(contract.to_json());
    GoalCoordinatorEvent ev;
    ev.event = "contract_ready";
    ev.goal_id = task_->id;
    ev.revision = task_->revision;
    ev.payload["contract"] = contract.to_json();
    ev.payload["contract_sha256"] = hooks::Sha256Hex(contract_json);
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "contract_ready 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "contract 事件写盘失败");
    }
    task_->contract = contract;
    task_->contract_frozen = true;
    task_->contract_sha256 = hooks::Sha256Hex(contract_json);
    task_->state = GoalState::Active;
    if (!task_->started_at_ms.has_value()) task_->started_at_ms = now_ms;
    task_->updated_at_ms = now_ms;
    GoalCommandResult r;
    r.ok = true;
    r.payload["state"] = ToString(task_->state);
    return r;
}

PreflightVerdict GoalCoordinator::PreflightContract(const GoalContract& contract, std::string* reason) {
    const auto set_reason = [reason](const std::string& text) {
        if (reason != nullptr) *reason = text;
    };
    if (contract.objective.empty()) {
        set_reason("合同缺 objective");
        return PreflightVerdict::Reject;
    }
    bool has_required = false;
    for (const auto& c : contract.criteria) {
        if (c.required && !c.text.empty()) {
            has_required = true;
            break;
        }
    }
    // 单子 corner case 钉死:"目标要求永不停止:preflight 拒绝"——比空话
    // 目标(一般性 NeedsUser)更硬,先判。
    const std::string obj = contract.objective;
    const bool open_ended = obj.find("永不停止") != std::string::npos ||
                            obj.find("永远不要停") != std::string::npos;
    if (open_ended && !has_required) {
        set_reason("目标要求永不停止:必须改成可验证终点");
        return PreflightVerdict::Reject;
    }
    if (!has_required) {
        set_reason("必需 criteria 为空:终点判不了");
        return PreflightVerdict::NeedsUser;
    }
    return PreflightVerdict::Ready;
}

// ---------------------------------------------------------------------------
// iteration 生命周期
// ---------------------------------------------------------------------------

GoalCommandResult GoalCoordinator::ScheduleNextIteration(std::int64_t now_ms) {
    if (!task_.has_value()) return Fail(kErrGoalNotFound, "没有活动目标");
    if (IsGoalTerminal(task_->state)) return Fail(kErrGoalTerminal, "目标已收账");
    if (!task_->contract_frozen) {
        return Fail(kErrGoalInvalidTransition, "合同未冻结,先过 preflight");
    }
    if (task_->state != GoalState::Active && task_->state != GoalState::Pausing) {
        return Fail(kErrGoalInvalidTransition,
                    "只有 Active 才排下一轮(当前 " + ToString(task_->state) + ")");
    }
    if (pause_requested_) {
        // pause 与 ready 同时发生,pause 胜(单子调度测试)。
        return Fail(kErrGoalBusy, "pause 已请求;不排新 iteration");
    }
    std::string budget_reason;
    if (!CheckBudgetHeadroom(now_ms, &budget_reason)) {
        // 落 terminal 事件再收状态。
        GoalCoordinatorEvent ev;
        ev.event = "budget_exhausted";
        ev.goal_id = task_->id;
        ev.revision = task_->revision;
        ev.payload["reason"] = budget_reason;
        ev.timestamp_ms = now_ms;
        Emit(ev);
        task_->state = GoalState::BudgetExhausted;
        task_->terminal_at_ms = now_ms;
        task_->updated_at_ms = now_ms;
        return Fail(kErrGoalBudgetExhausted, budget_reason);
    }
    if (ready_.has_value()) {
        // duplicate ready/dedupe key 不开两轮:已有排程,幂等返回。
        GoalCommandResult r;
        r.ok = true;
        r.payload["idempotent"] = true;
        r.payload["iteration_id"] = ready_->id;
        return r;
    }

    ++next_iteration_index_;
    GoalIteration it;
    it.id = task_->id + "/iter-" + std::to_string(next_iteration_index_);
    it.goal_id = task_->id;
    it.index = next_iteration_index_;
    it.goal_revision = task_->revision;  // 这枚 iteration 吃的 revision
    it.phase = GoalIterationPhase::Scheduled;
    const std::string dedupe = task_->id + ":r" + std::to_string(task_->revision) + ":i" +
                               std::to_string(next_iteration_index_);

    // 写盘栅栏 3:iteration scheduled 先落,main pump 才能取。
    GoalCoordinatorEvent ev;
    ev.event = "scheduled";
    ev.goal_id = task_->id;
    ev.iteration_id = it.id;
    ev.revision = task_->revision;
    ev.payload["index"] = it.index;
    ev.payload["dedupe_key"] = dedupe;
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "scheduled 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "scheduled 事件写盘失败");
    }
    ready_ = std::move(it);
    ready_dedupe_ = dedupe;
    GoalCommandResult r;
    r.ok = true;
    r.payload["iteration_id"] = ready_->id;
    r.payload["dedupe_key"] = ready_dedupe_;
    return r;
}

GoalCoordinator::StartedIteration GoalCoordinator::TakeReadyIteration(const std::string& turn_id,
                                                                      const std::string& before_fingerprint,
                                                                      std::int64_t now_ms) {
    StartedIteration out;
    if (!task_.has_value() || !ready_.has_value()) {
        out.error_code = kErrGoalNotFound;
        return out;
    }
    if (IsGoalTerminal(task_->state)) {
        out.error_code = kErrGoalTerminal;
        return out;
    }
    // 写盘栅栏 4:started 先落,再发 synthetic turn,dedupe key 同时写。
    GoalCoordinatorEvent ev;
    ev.event = "started";
    ev.goal_id = task_->id;
    ev.iteration_id = ready_->id;
    ev.revision = ready_->goal_revision;
    ev.payload["turn_id"] = turn_id;
    ev.payload["dedupe_key"] = ready_dedupe_;
    ev.payload["before_fingerprint"] = before_fingerprint;
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "started 写盘失败");
        out.error_code = kErrGoalStoreUnavailable;
        return out;
    }
    ready_->turn_id = turn_id;
    ready_->phase = GoalIterationPhase::Running;
    ready_->started_at_ms = now_ms;
    ready_->before_workspace_fingerprint = before_fingerprint;
    task_->state = GoalState::Running;
    task_->counters.iterations_started += 1;
    task_->updated_at_ms = now_ms;

    out.ok = true;
    out.iteration = *ready_;
    out.dedupe_key = ready_dedupe_;
    // synthetic 文字:供模型看;metadata(source/goal_id/revision/iteration/
    // dedupe_key)由装配层塞进消息 metadata,不在这拼 JSON(单子:文字供
    // 模型看,metadata 供宿主认身份)。
    out.synthetic_text =
        "[goal " + task_->id + " r" + std::to_string(ready_->goal_revision) + " iteration " +
        std::to_string(ready_->index) + "]\n目标:" + task_->objective +
        "\n请继续推进当前目标;先读上方 GoalContext,收口前调用 goal_checkpoint 写检查点。";
    return out;
}

GoalCommandResult GoalCoordinator::CheckpointReached(const GoalCheckpoint& checkpoint, std::int64_t now_ms) {
    if (!task_.has_value() || !ready_.has_value()) {
        return Fail(kErrGoalNotFound, "没有进行中的 iteration");
    }
    if (task_->state != GoalState::Running && task_->state != GoalState::Pausing) {
        return Fail(kErrGoalInvalidTransition, "当前不在执行轮收口位");
    }
    // 写盘栅栏 6(前半):checkpoint 落账才开 evaluator。
    GoalCoordinatorEvent ev;
    ev.event = "checkpoint";
    ev.goal_id = task_->id;
    ev.iteration_id = ready_->id;
    ev.revision = ready_->goal_revision;
    ev.payload["checkpoint"] = checkpoint.to_json();
    ev.payload["sha256"] = hooks::Sha256Hex(platform::DumpJsonSanitized(checkpoint.to_json()));
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "checkpoint 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "checkpoint 事件写盘失败");
    }
    ready_->checkpoint = checkpoint;
    ready_->phase = GoalIterationPhase::Checkpointed;
    task_->checkpoint = checkpoint;
    // checkpoint 已落,收口待判:goal 状态面进 Evaluating(Checkpointed 是
    // iteration 相位,不是 GoalState)。
    task_->state = GoalState::Evaluating;
    task_->updated_at_ms = now_ms;
    GoalCommandResult r;
    r.ok = true;
    r.payload["iteration_id"] = ready_->id;
    return r;
}

GoalCheckpoint GoalCoordinator::MakeMissingCheckpoint() {
    GoalCheckpoint c;
    c.synthesized = true;
    c.summary = "执行轮收口时未调用 goal_checkpoint;宿主合成 missing checkpoint。";
    c.next_action = "重读 GoalContext 与合同 criteria,补一枚明确 checkpoint 再收口。";
    return c;
}

bool GoalCoordinator::HardGateAchieved(
    const GoalTask& task, const GoalEvaluation& evaluation,
    const std::function<std::optional<GoalEvidence>(const std::string&)>& evidence_lookup,
    std::string* failure_reason) {
    const auto fail = [failure_reason](const std::string& text) {
        if (failure_reason != nullptr) *failure_reason = text;
        return false;
    };
    if (evaluation.decision != GoalDecision::Achieved) {
        fail("evaluator 判词不是 achieved");
        return false;
    }
    // 1. 每条 required criterion 都 pass。
    for (const auto& criterion : task.contract.criteria) {
        if (!criterion.required) continue;
        const auto* verdict = [&]() -> const CriterionVerdict* {
            for (const auto& v : evaluation.criteria) {
                if (v.id == criterion.id) return &v;
            }
            return nullptr;
        }();
        if (verdict == nullptr) {
            fail("criterion " + criterion.id + " 缺判词");
            return false;
        }
        if (verdict->status != "pass") {
            fail("criterion " + criterion.id + " 状态是 " + verdict->status);
            return false;
        }
        // 2. 每条 pass 至少指一枚有效 evidence id。
        bool has_valid_evidence = false;
        for (const auto& ev_id : verdict->evidence_ids) {
            if (!evidence_lookup) break;
            const auto evidence = evidence_lookup(ev_id);
            if (evidence.has_value() && evidence->fresh && evidence->goal_id == task.id) {
                has_valid_evidence = true;
                break;
            }
        }
        if (!has_valid_evidence) {
            fail("criterion " + criterion.id + " 没有有效新鲜证据");
            return false;
        }
    }
    // 3. 没有 remaining critical item(checkpoint.remaining 非空时拒)。
    if (!task.checkpoint.remaining.empty()) {
        fail("checkpoint 还有未完成项(" + std::to_string(task.checkpoint.remaining.size()) + " 条)");
        return false;
    }
    return true;
}

GoalCommandResult GoalCoordinator::ApplyEvaluation(const GoalEvaluation& evaluation, std::int64_t now_ms) {
    if (!task_.has_value()) return Fail(kErrGoalNotFound, "没有活动目标");
    if (IsGoalTerminal(task_->state)) {
        // terminal 后迟到判词:只留审计,不改状态。
        GoalCoordinatorEvent late;
        late.event = "late_evaluation";
        late.goal_id = task_->id;
        late.revision = task_->revision;
        late.payload["evaluation"] = evaluation.to_json();
        late.timestamp_ms = now_ms;
        late_arrivals_.push_back(late);
        return Fail(kErrGoalTerminal, "目标已收账,判词只留审计");
    }
    if (!ready_.has_value()) return Fail(kErrGoalNotFound, "没有进行中的 iteration");
    if (task_->state != GoalState::Evaluating && task_->state != GoalState::Pausing &&
        task_->state != GoalState::Running) {
        return Fail(kErrGoalInvalidTransition, "当前不在 evaluator 收口位");
    }
    // schema 门槛:blocked 必有 blocker_key,needs_user 必有 question。
    if (evaluation.decision == GoalDecision::Blocked && !evaluation.blocker_key.has_value()) {
        return Fail(kErrGoalEvaluatorSchema, "blocked 判词缺 blocker_key");
    }
    if (evaluation.decision == GoalDecision::NeedsUser && !evaluation.question.has_value()) {
        return Fail(kErrGoalEvaluatorSchema, "needs_user 判词缺 question");
    }

    GoalEvaluation final_eval = evaluation;
    if (evaluation.decision == GoalDecision::Achieved) {
        std::string gate_reason;
        const bool gate_ok = HardGateAchieved(
            *task_, evaluation,
            [this](const std::string& ev_id) -> std::optional<GoalEvidence> {
                const auto it = evidence_.find(ev_id);
                if (it == evidence_.end()) return std::nullopt;
                return it->second;
            },
            &gate_reason);
        if (!gate_ok) {
            // evaluator 说 achieved,程序门槛不够:改判 continue,缺证据项
            // 写成下一步(单子原文)。
            final_eval.decision = GoalDecision::Continue;
            final_eval.overridden_achieved = true;
            final_eval.override_reason = gate_reason;
            final_eval.next_action = "补证据:" + gate_reason;
        }
    }

    // 写盘栅栏 7:evaluation 先落,coordinator 再改内存 state。
    GoalCoordinatorEvent ev;
    ev.event = "evaluated";
    ev.goal_id = task_->id;
    ev.iteration_id = ready_->id;
    ev.revision = ready_->goal_revision;
    ev.payload["evaluation"] = final_eval.to_json();
    ev.payload["evidence_sha256"] = hooks::Sha256Hex(platform::DumpJsonSanitized(final_eval.to_json()));
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "evaluation 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "evaluation 事件写盘失败");
    }
    task_->last_evaluation = final_eval;
    ready_->phase = GoalIterationPhase::Finished;
    ready_->finished_at_ms = now_ms;
    task_->updated_at_ms = now_ms;

    // 判词分路 → 状态转移 + 是否排下一轮。
    switch (final_eval.decision) {
        case GoalDecision::Continue: {
            // 无进展/blocker 防空转账(宿主 fingerprint 已由装配层喂进来;
            // 这里若 checkpoint 带指纹则对账)。
            task_->state = GoalState::Active;
            GoalCommandResult r;
            r.ok = true;
            r.payload["decision"] = "continue";
            r.payload["schedule_next"] = true;
            r.payload["iteration_id"] = ready_->id;
            return r;
        }
        case GoalDecision::Achieved: {
            task_->state = GoalState::Achieved;
            task_->terminal_at_ms = now_ms;
            ready_.reset();
            ready_dedupe_.clear();
            GoalCommandResult r;
            r.ok = true;
            r.payload["decision"] = "achieved";
            return r;
        }
        case GoalDecision::Blocked: {
            // 同 blocker 连续且无实质进展才 Blocked;单轮碰墙由装配层判
            // 可换路(evaluator 单说 blocked 而轮数不足:状态留 Active,
            // 由装配层决定送 needs_user 还是继续)。这里按单子:进 Blocked
            // 的门槛在 NoteBlocker(三轮);evaluator 直说 blocked 而 streak
            // 不足时退 continue 换路。
            if (task_->counters.same_blocker_streak + 1 >=
                task_->budget.max_same_blocker_iterations) {
                GoalCoordinatorEvent blocked_ev;
                blocked_ev.event = "blocked";
                blocked_ev.goal_id = task_->id;
                blocked_ev.revision = task_->revision;
                blocked_ev.payload["blocker_key"] = final_eval.blocker_key.value_or(std::string());
                blocked_ev.timestamp_ms = now_ms;
                Emit(blocked_ev);
                task_->state = GoalState::Blocked;
                task_->terminal_at_ms = now_ms;
                ready_.reset();
                ready_dedupe_.clear();
            } else {
                task_->state = GoalState::Active;
            }
            GoalCommandResult r;
            r.ok = true;
            r.payload["decision"] = ToString(final_eval.decision);
            r.payload["schedule_next"] = task_->state == GoalState::Active;
            return r;
        }
        case GoalDecision::NeedsUser: {
            task_->state = GoalState::AwaitingUser;
            ready_.reset();
            ready_dedupe_.clear();
            GoalCommandResult r;
            r.ok = true;
            r.payload["decision"] = "needs_user";
            r.payload["question"] = final_eval.question.value_or(std::string());
            return r;
        }
    }
    return Fail(kErrGoalInvalidTransition, "判词分路落空");
}

void GoalCoordinator::AddUsage(const GoalUsage& usage) {
    if (task_.has_value()) task_->usage.Add(usage);
}

GoalCoordinator::ProgressCheck GoalCoordinator::NoteProgressFingerprint(const std::string& fingerprint,
                                                                        std::int64_t now_ms) {
    ProgressCheck out;
    if (!task_.has_value()) return out;
    // 防空转(批五):连撞计数走 StreakMeter,计数仍落在 counters 域字段
    // 上(存档要);阈值是 goal 自己的尺(max_no_progress_iterations)。
    StreakMeter meter{task_->budget.max_no_progress_iterations,
                      task_->counters.no_progress_streak};
    out.progressed = task_->counters.last_progress_fingerprint != fingerprint;
    if (out.progressed) {
        meter.NoteGood();
        task_->counters.last_progress_fingerprint = fingerprint;
    } else {
        meter.NoteBad();
    }
    task_->counters.no_progress_streak = meter.count;
    out.streak_after = task_->counters.no_progress_streak;
    if (meter.tripped() && !IsGoalTerminal(task_->state)) {
        GoalCoordinatorEvent ev;
        ev.event = "paused";
        ev.goal_id = task_->id;
        ev.revision = task_->revision;
        ev.payload["reason"] = "no_progress";
        ev.payload["streak"] = task_->counters.no_progress_streak;
        ev.timestamp_ms = now_ms;
        Emit(ev);
        task_->state = GoalState::Paused;
        task_->updated_at_ms = now_ms;
        ready_.reset();
        ready_dedupe_.clear();
        out.tripped = true;
    }
    return out;
}

GoalCoordinator::BlockerCheck GoalCoordinator::NoteBlocker(const std::string& blocker_key, std::int64_t now_ms) {
    BlockerCheck out;
    if (!task_.has_value()) return out;
    // 同 blocker 反复(批五):换 key 归零重数(Reset + NoteBad = 从 1 起),
    // 与旧"换 key 记 1"同账。
    StreakMeter meter{task_->budget.max_same_blocker_iterations,
                      task_->counters.same_blocker_streak};
    if (blocker_key == task_->counters.last_blocker_key) {
        meter.NoteBad();
    } else {
        meter.NoteGood();
        meter.NoteBad();
        task_->counters.last_blocker_key = blocker_key;
    }
    task_->counters.same_blocker_streak = meter.count;
    out.streak_after = task_->counters.same_blocker_streak;
    if (meter.tripped() && !IsGoalTerminal(task_->state)) {
        GoalCoordinatorEvent ev;
        ev.event = "blocked";
        ev.goal_id = task_->id;
        ev.revision = task_->revision;
        ev.payload["blocker_key"] = blocker_key;
        ev.timestamp_ms = now_ms;
        Emit(ev);
        task_->state = GoalState::Blocked;
        task_->terminal_at_ms = now_ms;
        ready_.reset();
        ready_dedupe_.clear();
        out.tripped = true;
    }
    return out;
}

bool GoalCoordinator::CheckBudgetHeadroom(std::int64_t now_ms, std::string* reason) const {
    if (!task_.has_value()) {
        if (reason != nullptr) *reason = "没有目标";
        return false;
    }
    // 预算闸(批五):三尺声明进公共件,Headroom 口径(开新轮前问,
    // "下一轮装不下"即拦);没账可对的尺(started 未落、usage 未报)跳过,
    // 与旧逐尺短路同序同文。
    BudgetScales scales;
    if (task_->budget.max_iterations.has_value()) {
        scales.count = static_cast<std::int64_t>(*task_->budget.max_iterations);
    }
    scales.elapsed_ms = task_->budget.max_elapsed_ms;
    scales.tokens = task_->budget.max_total_tokens;
    const BudgetGate gate(scales);
    const bool started = task_->started_at_ms.has_value();
    const std::int64_t elapsed = started ? now_ms - *task_->started_at_ms : 0;
    // token 未回报不能拿 0 冒充没花:usage_reported=false 时跳过 token 闸
    // (time/iteration 仍能收口)。
    const std::optional<std::int64_t> tokens_used = [this]() -> std::optional<std::int64_t> {
        if (!task_->budget.max_total_tokens.has_value() || !task_->usage.usage_reported) {
            return std::nullopt;
        }
        return task_->usage.input_tokens + task_->usage.output_tokens +
               task_->usage.cache_read_tokens + task_->usage.cache_creation_tokens;
    }();
    switch (gate.CheckHeadroom(task_->counters.iterations_started,
                               started ? std::optional<std::int64_t>(elapsed) : std::nullopt,
                               tokens_used)) {
        case BudgetStopReason::kCount:
            if (reason != nullptr) {
                *reason =
                    "iteration 上限 " + std::to_string(*task_->budget.max_iterations) + " 已用满";
            }
            return false;
        case BudgetStopReason::kElapsed:
            if (reason != nullptr) {
                *reason = "elapsed 上限已到(用了 " + std::to_string(elapsed / 1000) + "s)";
            }
            return false;
        case BudgetStopReason::kTokens:
            if (reason != nullptr) *reason = "token 上限已到";
            return false;
        case BudgetStopReason::kNone:
            break;
    }
    return true;
}

bool GoalCoordinator::ElapsedExceeded(std::int64_t now_ms, std::int64_t* over_by_ms) const {
    if (!task_.has_value() || !task_->budget.max_elapsed_ms.has_value() ||
        !task_->started_at_ms.has_value()) {
        return false;
    }
    const std::int64_t elapsed = now_ms - *task_->started_at_ms;
    // 轮外收口是"已经越线"口径:Overrun(严格 >)。与 Headroom 的 >= 差
    // 一线——休眠醒来恰等于上限不算越线,开新轮才算到顶。
    if (BudgetGate(BudgetScales{.elapsed_ms = task_->budget.max_elapsed_ms}).OverrunElapsed(elapsed)) {
        if (over_by_ms != nullptr) *over_by_ms = elapsed - *task_->budget.max_elapsed_ms;
        return true;
    }
    return false;
}

void GoalCoordinator::NoteProviderOutcome(bool succeeded) {
    if (!task_.has_value()) return;
    // provider 连败(批五):连撞计数走 StreakMeter,计数落 counters 域字段。
    StreakMeter meter{task_->budget.max_consecutive_provider_failures,
                      task_->counters.consecutive_provider_failures};
    if (succeeded) {
        meter.NoteGood();
        task_->counters.consecutive_provider_failures = meter.count;
        return;
    }
    meter.NoteBad();
    task_->counters.consecutive_provider_failures = meter.count;
    if (meter.tripped() && !IsGoalTerminal(task_->state)) {
        GoalCoordinatorEvent ev;
        ev.event = "paused";
        ev.goal_id = task_->id;
        ev.revision = task_->revision;
        ev.payload["reason"] = "provider_failures";
        ev.payload["streak"] = task_->counters.consecutive_provider_failures;
        ev.timestamp_ms = 0;  // 装配层补
        Emit(ev);
        task_->state = GoalState::Paused;
        ready_.reset();
        ready_dedupe_.clear();
    }
}

GoalCommandResult GoalCoordinator::NoteEvaluatorFailed(const std::string& error, std::int64_t now_ms) {
    if (!task_.has_value()) return Fail(kErrGoalNotFound, "没有活动目标");
    if (IsGoalTerminal(task_->state)) {
        return Fail(kErrGoalTerminal, "目标已收账,evaluator 失败只留审计");
    }
    GoalCoordinatorEvent ev;
    ev.event = "paused";
    ev.goal_id = task_->id;
    ev.revision = task_->revision;
    ev.payload["reason"] = "evaluator_failed";
    ev.payload["error"] = error;
    ev.timestamp_ms = now_ms;
    if (!Emit(ev)) {
        FailClosed(now_ms, "evaluator_failed 写盘失败");
        return Fail(kErrGoalStoreUnavailable, "evaluator_failed 事件写盘失败");
    }
    task_->state = GoalState::Paused;
    task_->updated_at_ms = now_ms;
    ready_.reset();
    ready_dedupe_.clear();
    GoalCommandResult r;
    r.ok = true;
    r.payload["state"] = ToString(task_->state);
    r.payload["reason"] = "evaluator_failed";
    return r;
}

// ---------------------------------------------------------------------------
// 恢复回放
// ---------------------------------------------------------------------------

void GoalCoordinator::ReplayEvent(const GoalCoordinatorEvent& event) {
    // 回放按 append-only 事件重建,不信最后一条 summary(单子回放器)。
    if (event.event == "created") {
        GoalTask t;
        t.id = event.goal_id;
        t.revision = event.revision;
        if (event.payload.contains("objective")) {
            t.objective = event.payload.at("objective").get<std::string>();
        }
        if (event.payload.contains("objective_sha256")) {
            t.objective_sha256 = event.payload.at("objective_sha256").get<std::string>();
        }
        if (event.payload.contains("parent_goal_id")) {
            t.parent_goal_id = event.payload.at("parent_goal_id").get<std::string>();
        }
        if (event.payload.contains("budget")) t.budget = GoalBudget::from_json(event.payload.at("budget"));
        if (event.payload.contains("workspace_root")) {
            t.workspace_root = event.payload.at("workspace_root").get<std::string>();
        }
        if (event.payload.contains("workspace_identity")) {
            t.workspace_identity = event.payload.at("workspace_identity").get<std::string>();
        }
        t.created_at_ms = event.timestamp_ms;
        t.updated_at_ms = event.timestamp_ms;
        t.state = GoalState::Preparing;
        task_ = std::move(t);
        next_iteration_index_ = 0;
        return;
    }
    if (!task_.has_value()) return;  // 坏序(无 created):跳过不废
    task_->updated_at_ms = event.timestamp_ms;
    if (event.event == "edited") {
        if (event.payload.contains("objective")) {
            task_->objective = event.payload.at("objective").get<std::string>();
        }
        task_->revision = event.revision;
        task_->state = GoalState::Preparing;
        task_->contract_frozen = false;
        task_->counters.no_progress_streak = 0;
        task_->counters.same_blocker_streak = 0;
        return;
    }
    if (event.event == "contract_ready") {
        if (event.payload.contains("contract")) {
            task_->contract = GoalContract::from_json(event.payload.at("contract"));
        }
        if (event.payload.contains("contract_sha256")) {
            task_->contract_sha256 = event.payload.at("contract_sha256").get<std::string>();
        }
        task_->contract_frozen = true;
        if (IsGoalTerminal(task_->state)) return;
        task_->state = GoalState::Active;
        if (!task_->started_at_ms.has_value()) task_->started_at_ms = event.timestamp_ms;
        return;
    }
    if (event.event == "scheduled") {
        if (IsGoalTerminal(task_->state)) return;
        GoalIteration it;
        it.id = event.iteration_id;
        it.goal_id = event.goal_id;
        it.index = event.payload.value("index", 0);
        it.goal_revision = event.revision;
        it.phase = GoalIterationPhase::Scheduled;
        ready_ = it;
        if (event.payload.contains("dedupe_key")) {
            ready_dedupe_ = event.payload.at("dedupe_key").get<std::string>();
        }
        if (it.index > next_iteration_index_) next_iteration_index_ = it.index;
        return;
    }
    if (event.event == "started") {
        if (IsGoalTerminal(task_->state)) return;
        if (ready_.has_value()) {
            ready_->phase = GoalIterationPhase::Running;
            if (event.payload.contains("turn_id")) {
                ready_->turn_id = event.payload.at("turn_id").get<std::string>();
            }
            ready_->started_at_ms = event.timestamp_ms;
            if (event.payload.contains("before_fingerprint")) {
                ready_->before_workspace_fingerprint =
                    event.payload.at("before_fingerprint").get<std::string>();
            }
        }
        task_->state = GoalState::Running;
        task_->counters.iterations_started += 1;
        return;
    }
    if (event.event == "checkpoint") {
        if (IsGoalTerminal(task_->state)) return;
        if (event.payload.contains("checkpoint") && ready_.has_value()) {
            ready_->checkpoint = GoalCheckpoint::from_json(event.payload.at("checkpoint"));
            task_->checkpoint = *ready_->checkpoint;
            ready_->phase = GoalIterationPhase::Checkpointed;
        }
        return;
    }
    if (event.event == "evaluated") {
        if (IsGoalTerminal(task_->state)) return;
        if (event.payload.contains("evaluation")) {
            task_->last_evaluation = GoalEvaluation::from_json(event.payload.at("evaluation"));
        }
        if (ready_.has_value()) {
            ready_->phase = GoalIterationPhase::Finished;
            ready_->finished_at_ms = event.timestamp_ms;
            // iteration 已收账:ready 位清空,恢复侧 ScheduleNextIteration
            // 才能补下一枚(否则幂等分支把旧 iteration 当在排)。
            ready_.reset();
            ready_dedupe_.clear();
        }
        if (task_->last_evaluation.has_value()) {
            switch (task_->last_evaluation->decision) {
                case GoalDecision::Continue:
                    task_->state = GoalState::Active;
                    break;
                case GoalDecision::Achieved:
                    task_->state = GoalState::Achieved;
                    task_->terminal_at_ms = event.timestamp_ms;
                    ready_.reset();
                    ready_dedupe_.clear();
                    break;
                case GoalDecision::Blocked:
                    task_->state = GoalState::Blocked;
                    task_->terminal_at_ms = event.timestamp_ms;
                    ready_.reset();
                    ready_dedupe_.clear();
                    break;
                case GoalDecision::NeedsUser:
                    task_->state = GoalState::AwaitingUser;
                    ready_.reset();
                    ready_dedupe_.clear();
                    break;
            }
        }
        return;
    }
    if (event.event == "forked") {
        // fork 账回放:lineage、Paused 落点与抄来的账面(created 已建 task;
        // 抄去的合同/预算/checkpoint 在 forked 事件的 payload 里)。
        if (event.payload.contains("parent_goal_id")) {
            task_->parent_goal_id = event.payload.at("parent_goal_id").get<std::string>();
        }
        if (event.payload.contains("contract")) {
            task_->contract = GoalContract::from_json(event.payload.at("contract"));
        }
        if (event.payload.contains("contract_frozen")) {
            task_->contract_frozen = event.payload.at("contract_frozen").get<bool>();
        }
        if (event.payload.contains("contract_sha256")) {
            task_->contract_sha256 = event.payload.at("contract_sha256").get<std::string>();
        }
        if (event.payload.contains("budget")) {
            task_->budget = GoalBudget::from_json(event.payload.at("budget"));
        }
        if (event.payload.contains("checkpoint")) {
            task_->checkpoint = GoalCheckpoint::from_json(event.payload.at("checkpoint"));
        }
        if (!IsGoalTerminal(task_->state)) task_->state = GoalState::Paused;
        ready_.reset();
        ready_dedupe_.clear();
        return;
    }
    if (event.event == "pause_requested" || event.event == "paused") {
        if (IsGoalTerminal(task_->state)) return;
        if (event.payload.contains("reason")) {
            const std::string reason = event.payload.at("reason").get<std::string>();
            if (reason == "no_progress" || reason == "evaluator_failed" ||
                reason == "provider_failures" || reason == "store_unavailable") {
                // Paused(原因)是可恢复暂停,保持 Paused(非 terminal)。
            }
        }
        task_->state = GoalState::Paused;
        pause_requested_ = event.event == "pause_requested";
        ready_.reset();
        ready_dedupe_.clear();
        return;
    }
    if (event.event == "resumed") {
        if (IsGoalTerminal(task_->state)) return;
        task_->state = task_->contract_frozen ? GoalState::Active : GoalState::Preparing;
        task_->counters.same_blocker_streak = 0;
        task_->counters.no_progress_streak = 0;
        pause_requested_ = false;
        return;
    }
    if (event.event == "cleared") {
        task_->state = GoalState::Cleared;
        task_->terminal_at_ms = event.timestamp_ms;
        ready_.reset();
        ready_dedupe_.clear();
        return;
    }
    if (event.event == "budget_exhausted") {
        task_->state = GoalState::BudgetExhausted;
        task_->terminal_at_ms = event.timestamp_ms;
        ready_.reset();
        ready_dedupe_.clear();
        return;
    }
    if (event.event == "blocked") {
        task_->state = GoalState::Blocked;
        task_->terminal_at_ms = event.timestamp_ms;
        ready_.reset();
        ready_dedupe_.clear();
        return;
    }
    if (event.event == "awaiting_user") {
        if (!IsGoalTerminal(task_->state)) task_->state = GoalState::AwaitingUser;
        return;
    }
    if (event.event == "achieved") {
        task_->state = GoalState::Achieved;
        task_->terminal_at_ms = event.timestamp_ms;
        ready_.reset();
        ready_dedupe_.clear();
        return;
    }
    if (event.event == "failed") {
        task_->state = GoalState::Failed;
        task_->terminal_at_ms = event.timestamp_ms;
        ready_.reset();
        ready_dedupe_.clear();
        return;
    }
    // 未知未来事件:跳过(兼容)。
}

GoalCoordinator::ReplayStats GoalCoordinator::RestoreFromArchive(
    const std::vector<lubancode::sessions::GoalSessionEvent>& events) {
    // goal 行的中立解析留在
    // sessions 层(ParseGoalEvent,不反向依赖 runtime 的老规矩),这里
    // 解析后的域事件折成信封、经同一条次序/坏行跳过/账面规矩交
    // ReplayEvent。goal_id/iteration_id/revision 是域字段,进 payload
    // 原样过境。
    std::vector<replay::Envelope> envelopes;
    envelopes.reserve(events.size());
    for (const auto& row : events) {
        replay::Envelope envelope;
        envelope.family = row.type;
        envelope.event = row.event;
        envelope.timestamp_ms = row.timestamp_ms;
        nlohmann::json payload = nlohmann::json::object();
        payload["goal_id"] = row.goal_id;
        payload["iteration_id"] = row.iteration_id;
        payload["revision"] = row.revision;
        payload["payload"] = row.payload;
        envelope.payload = std::move(payload);
        envelopes.push_back(std::move(envelope));
    }
    const auto replayed = replay::ReplayEnvelopes(
        std::move(envelopes), [this](const replay::Envelope& envelope) {
            GoalCoordinatorEvent event;
            event.event = envelope.event;
            event.goal_id = envelope.payload.value("goal_id", std::string());
            event.iteration_id = envelope.payload.value("iteration_id", std::string());
            event.revision = envelope.payload.value("revision", 0);
            event.payload = envelope.payload.value("payload", nlohmann::json::object());
            event.timestamp_ms = envelope.timestamp_ms;
            ReplayEvent(event);
            return true;  // 坏载荷由 ReplayEvent 内部/折叠口兜底折成跳过
        });
    ReplayStats stats;
    stats.replayed = replayed.replayed;
    stats.skipped = replayed.skipped;
    // feature 关:resume 读到非终态 goal 落 SuspendedByPolicy(可查/导出/
    // clear,不自动续跑)。
    if (!options_.goals_enabled && task_.has_value() && !IsGoalTerminal(task_->state)) {
        task_->state = GoalState::SuspendedByPolicy;
        task_->terminal_at_ms = 0;
        task_->updated_at_ms = 0;
        ready_.reset();
        ready_dedupe_.clear();
        stats.suspended_by_policy = true;
    }
    return stats;
}

bool GoalCoordinator::AbsorbLateArrival(const GoalCoordinatorEvent& event) {
    if (!task_.has_value() || !IsGoalTerminal(task_->state)) return false;
    late_arrivals_.push_back(event);
    return true;
}

// ---------------------------------------------------------------------------
// 证据账
// ---------------------------------------------------------------------------

void GoalCoordinator::RecordEvidence(const GoalEvidence& evidence) {
    // 只认本 goal 的证据(跨 goal id 不收;checkpoint 工具引了别的 goal 的
    // evidence id 会在 HardGateAchieved 的查表里露馅)。
    if (!task_.has_value() || evidence.goal_id != task_->id) return;
    evidence_[evidence.id] = evidence;
}

const GoalEvidence* GoalCoordinator::FindEvidence(const std::string& id) const {
    const auto it = evidence_.find(id);
    return it == evidence_.end() ? nullptr : &it->second;
}

void GoalCoordinator::MarkEvidenceStale(const std::string& id) {
    const auto it = evidence_.find(id);
    if (it != evidence_.end()) it->second.fresh = false;
}

std::vector<std::string> GoalCoordinator::EvidenceIds() const {
    std::vector<std::string> ids;
    ids.reserve(evidence_.size());
    for (const auto& [id, evidence] : evidence_) {
        (void)evidence;
        ids.push_back(id);
    }
    return ids;
}

}  // namespace lubancode::runtime::goal
