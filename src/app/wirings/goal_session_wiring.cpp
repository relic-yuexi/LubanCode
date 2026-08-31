// /goal 子系统接线器的实现(会话终章):函数体原样自 interactive_session
// 大类搬来(RestoreGoalFromArchive/AttachGoalSnapshotToCompact/
// PumpGoalContinuation/CloseGoalIteration/MakeGoalWiring),材料换经 Host
// 递入,行为一字未改——注释一并随行。
//
// 骨架拆解反弹·问题 3:Ensure 里"事件类型分族 + ledger sink 搭建"抽去
// runtime::goal::MakeSessionLedgerSink(纯函数,单测钉);终端打印改产
// notify 回调(装配层决定怎么画),本文件不再有 TermOut。
#include "app/wirings/goal_session_wiring.hpp"

#include <chrono>
#include <utility>

#include "platform/paths.hpp"
#include "runtime/goal_compact.hpp"
#include "runtime/goal_context.hpp"
#include "runtime/goal_evidence.hpp"
#include "runtime/goal_evaluator.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "sessions/session_store.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"

namespace lubancode::app {

GoalSessionWiring::GoalSessionWiring(Host host) : host_(std::move(host)) {}

// 渲染事件出口(问题 3 第 2 条):is_error 定色,text 是纯文案——怎么画
// 由装配层(interactive_session_assembly 填的 notify)决定。
void GoalSessionWiring::Notify(bool is_error, const std::string& text) {
    if (host_.notify) {
        host_.notify(is_error, text);
    }
}

lubancode::runtime::goal::GoalCoordinator* GoalSessionWiring::coordinator() {
    return coordinator_.has_value() ? &*coordinator_ : nullptr;
}

bool GoalSessionWiring::ToolExposed() const {
    // 会话级条件(features.goals 正门 + env 总闸),与 GoalOptionsFromConfig
    // 给 coordinator 的 goals_enabled 同一条判式——暴露位与"真跑不跑"同源,
    // 不会出现"看得见工具却永远等不到轮次"之外的第三种状态。config 在
    // 会话启动定死,此值会话内恒定(动态工具 P2 的 ToolExposurePolicy)。
    return host_.config != nullptr && host_.config->features_goals && !lubancode::app::GoalsDisabledByEnv();
}

std::string GoalSessionWiring::ActiveGoalId() const {
    // checkpoint 工具账里的 goal_id 就是"当前在跑哪只 goal"的真值:Pump
    // 开轮前灌(goal iteration 的 id),收口后账面留着、下一次 Pump 重灌。
    // 能力段只在 HasActiveIteration 为真时才带这条注,收口后的旧值不会被
    // 念出来——判"在不在 goal 轮"永远以 HasActiveIteration 为准。
    return checkpoint_state_ != nullptr ? checkpoint_state_->goal_id : std::string();
}

void GoalSessionWiring::RegisterTools(lubancode::tools::ToolRegistry& registry) {
    if (!checkpoint_state_) {
        checkpoint_state_ = std::make_shared<lubancode::tools::GoalCheckpointState>();
    }
    registry.Register(std::make_unique<lubancode::tools::GoalCheckpointTool>(checkpoint_state_));
}

void GoalSessionWiring::Ensure(const lubancode::config::Config& config) {
    if (coordinator_.has_value()) return;
    auto options = lubancode::app::GoalOptionsFromConfig(config.features_goals, config.goals);
    coordinator_.emplace(std::move(options));
    // LedgerSink:goal 事件行 append+flush 进 session 存档(问题 3:事件
    // 类型分族与行折算在 runtime::goal::MakeSessionLedgerSink,纯函数)。
    coordinator_->SetLedgerSink(
        lubancode::runtime::goal::MakeSessionLedgerSink(*host_.session_store));
    // loop 单分流合流:coordinator 的 ready continuation 经 GoalWorkSource
    // 进泵(泵问 ProbeWork;选中后装配层 TakeReadyIteration 发 synthetic
    // turn)。trigger 各归各(evaluator 判终点 vs 时钟到点),泵共用。
    work_source_.SetProbe([this]() -> std::optional<lubancode::runtime::SessionWork> {
        if (!coordinator_.has_value() || !coordinator_->HasReadyContinuation()) {
            return std::nullopt;
        }
        lubancode::runtime::SessionWork work;
        work.kind = lubancode::runtime::WorkKind::GoalContinuation;
        work.id = coordinator_->ready_dedupe_key();
        work.payload["goal_id"] = coordinator_->task() != nullptr
                                      ? coordinator_->task()->id
                                      : std::string();
        return work;
    });
}

void GoalSessionWiring::RestoreFromArchive() {
    Ensure(*host_.config);
    if (!host_.session_store->active()) return;  // 没档可恢复
    const auto bytes = lubancode::sessions::ReadSessionFileBytes(host_.session_store->file_path());
    if (!bytes.has_value()) return;
    const auto loaded = lubancode::sessions::ParseSessionFile(*bytes);
    if (!loaded.has_value() || loaded->goal_events.empty()) return;
    const auto stats = coordinator_->RestoreFromArchive(loaded->goal_events);
    if (stats.replayed == 0 && stats.skipped == 0) return;
    std::string restored = "目标账已随会话恢复(" + std::to_string(stats.replayed) + " 条事件";
    if (stats.skipped > 0) {
        restored += "," + std::to_string(stats.skipped) + " 条坏行跳过";
    }
    restored += ")。默认暂停续跑;查看 /goal status,续跑 /goal resume。";
    Notify(/*is_error=*/false, restored);
    if (stats.suspended_by_policy) {
        Notify(/*is_error=*/false, "goals 功能当前未开启:目标挂起(SuspendedByPolicy),可查、可导出、可 clear,不自动跑。");
    }
}

lubancode::app::GoalWiring GoalSessionWiring::MakeCommandWiring(
    lubancode::tools::AgentTool* agent_tool, lubancode::runtime::loop::LoopScheduler* loop_scheduler) {
    lubancode::app::GoalWiring wiring;
    wiring.theme = host_.theme;
    wiring.coordinator = coordinator_.has_value() ? &*coordinator_ : nullptr;
    wiring.session_store = host_.session_store;
    wiring.agent_tool = agent_tool;
    wiring.checkpoint_state = checkpoint_state_.get();
    wiring.loop_scheduler = loop_scheduler;
    return wiring;
}

void GoalSessionWiring::AttachSnapshotToCompact(lubancode::sessions::CompactV2Event& event) {
    Ensure(*host_.config);
    const auto snapshot = lubancode::runtime::goal::BuildGoalSnapshot(*coordinator_);
    if (!snapshot.has_value()) return;  // 没 goal:不带,普通会话照旧
    nlohmann::json goal_metrics;
    goal_metrics["snapshot"] = snapshot->to_json();
    goal_metrics["conservation_sha256"] = lubancode::runtime::goal::GoalSnapshotConservationSha256(*snapshot);
    event.metrics["goal"] = std::move(goal_metrics);
}

void GoalSessionWiring::NoteSubagentCompletion() {
    lubancode::app::NoteSubagentCompletionForGoal(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr));
}

void GoalSessionWiring::PumpContinuation(std::int64_t now_ms) {
    // goal 的 ready continuation 开一轮 synthetic turn(单飞:与 loop 同泵,
    // 一场 session 同时只跑一枚主 turn)。TakeReadyIteration 落 started
    // 事件(带 turn_id/dedupe_key);失败(goal 单测过)静默返回,下一圈
    // 再问。
    if (!coordinator_.has_value() ||
        !coordinator_->HasReadyContinuation()) {
        return;
    }
    const auto started = coordinator_->TakeReadyIteration("goal-turn", /*before_fingerprint=*/"", now_ms);
    if (!started.ok) {
        return;
    }
    active_iteration_ = started.dedupe_key;
    fairness_.NoteGoalRan();
    // goal_checkpoint 工具的会话级状态:本轮 scope 灌好(空 goal_id = 工具
    // 明拒),上一轮的 entries 清零(候选只算本轮的)。
    if (checkpoint_state_ != nullptr) {
        checkpoint_state_->goal_id = started.iteration.goal_id;
        checkpoint_state_->iteration_id = started.iteration.id;
        checkpoint_state_->entries.clear();
    }
    Notify(/*is_error=*/false, "[goal " + started.iteration.goal_id + " iteration " +
                                   std::to_string(started.iteration.index) + "]");
    lubancode::app::EmitGoalHook(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
        lubancode::hooks::HookEvent::GoalIterationStart,
        nlohmann::json{{"goal_id", started.iteration.goal_id},
                       {"iteration_id", started.iteration.id},
                       {"iteration_index", started.iteration.index},
                       {"dedupe_key", started.dedupe_key}},
        /*match_value=*/std::string());
    bool turn_failed = false;
    host_.start_turn(started.synthetic_text, &turn_failed);
    // 收口:completion-driven 泵的真接线——采证/checkpoint/evaluator/
    // ApplyEvaluation/ScheduleNextIteration 都在主线程安全边界跑。
    CloseIteration("goal-turn", turn_failed);
    active_iteration_.clear();
    fairness_.NoteOtherWorkRan();
    lubancode::app::EmitGoalHook(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
        lubancode::hooks::HookEvent::GoalIterationEnd,
        nlohmann::json{{"goal_id", started.iteration.goal_id},
                       {"iteration_id", started.iteration.id},
                       {"iteration_index", started.iteration.index},
                       {"turn_failed", turn_failed}},
        /*match_value=*/std::string());
}

void GoalSessionWiring::CloseIteration(const std::string& turn_id, bool turn_failed) {
    if (!coordinator_.has_value() || active_iteration_.empty()) {
        return;  // 不在 goal 收口位(用户普通轮/迟到)
    }
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    const auto* task = coordinator_->task();
    if (task == nullptr || lubancode::runtime::goal::IsGoalTerminal(task->state)) {
        return;  // 已收账(收口前用户 clear 了):只留审计
    }

    // ---- 1) 采证:本轮 finished 的工具事件翻 GoalEvidence 喂账 ----
    if (host_.trace_hub != nullptr) {
        namespace goalns = lubancode::runtime::goal;
        goalns::GoalEvidenceContext ctx;
        ctx.goal_id = task->id;
        ctx.iteration_id = task->id + "/iter-" + std::to_string(task->counters.iterations_started);
        ctx.turn_id = turn_id;
        int evidence_seq = static_cast<int>(coordinator_->evidence_count());
        std::vector<std::string> fresh_ids;
        for (const auto& event : host_.trace_hub->FinishedEventsOfTurn(turn_id)) {
            const auto evidence =
                goalns::EvidenceFromToolTrace(event, ctx, "ev-" + std::to_string(++evidence_seq));
            if (!evidence.has_value()) {
                continue;
            }
            // 事件行(goal_evidence_v1)先落:证据账是 hard gate 的查表底。
            lubancode::sessions::GoalSessionEvent line;
            line.type = "goal_evidence_v1";
            line.event = "observed";
            line.goal_id = evidence->goal_id;
            line.iteration_id = evidence->iteration_id;
            line.revision = task->revision;
            nlohmann::json payload;
            payload["evidence"] = evidence->to_json();
            line.payload = std::move(payload);
            line.timestamp_ms = now_ms;
            if (host_.session_store->active()) {
                (void)host_.session_store->AppendGoalEvent(line);
            }
            coordinator_->RecordEvidence(*evidence);
            fresh_ids.push_back(evidence->id);
            // 写盘级工具落完:旧验证证据按分档翻 stale(单子"证据涉及改动
            // 后,旧 validation 要按影响范围翻 stale")。
            if (goalns::EvidenceStalesOnWrite(evidence->kind)) {
                for (const auto& id : coordinator_->EvidenceIds()) {
                    const auto* existing = coordinator_->FindEvidence(id);
                    if (existing != nullptr && goalns::EvidenceStalesOnWrite(existing->kind)) {
                        coordinator_->MarkEvidenceStale(id);
                    }
                }
            }
        }
        // 证据白名单喂给 checkpoint 工具状态:本轮采到的 id 是下一轮
        // goal_checkpoint 引用校验的白名单底(旧证据 id 引了报 unknown,
        // 单子:只能引用本 goal、本 iteration 已产生的 evidence id)。
        if (checkpoint_state_ != nullptr) {
            checkpoint_state_->valid_evidence_ids = std::move(fresh_ids);
        }
    }

    // ---- 2) checkpoint:工具调了取最后一枚,没调合成 missing ----
    lubancode::runtime::goal::GoalCheckpoint checkpoint;
    bool has_tool_checkpoint = false;
    if (checkpoint_state_ != nullptr && checkpoint_state_->HasCheckpoint() &&
        checkpoint_state_->goal_id == task->id) {
        const auto candidate = checkpoint_state_->Candidate();
        if (candidate.has_value()) {
            has_tool_checkpoint = true;
            checkpoint.version = 1;
            checkpoint.summary = candidate->summary;
            checkpoint.completed = candidate->completed;
            checkpoint.remaining = candidate->remaining;
            checkpoint.next_action = candidate->next_action;
            checkpoint.evidence_ids = candidate->evidence_ids;
            checkpoint.blocker_key = candidate->blocker_key;
            checkpoint.question = candidate->question;
            using GoalCheckpointStatus = lubancode::tools::GoalCheckpointStatus;
            checkpoint.synthesized = false;
            (void)GoalCheckpointStatus::Progress;  // 枚举仅对齐注释,不另存
        }
    }
    if (!has_tool_checkpoint) {
        checkpoint = coordinator_->MakeMissingCheckpoint();
    }
    const auto checkpoint_result = coordinator_->CheckpointReached(checkpoint, now_ms);
    if (!checkpoint_result.ok) {
        Notify(/*is_error=*/true, "goal checkpoint 落账失败: " + checkpoint_result.error_message);
        return;
    }
    // checkpoint 工具账清零:下一枚 iteration 从头攒(状态是会话级复用的)。
    if (checkpoint_state_ != nullptr) {
        checkpoint_state_->entries.clear();
    }

    // provider 账:turn 失败记连败(撞闸 coordinator 自己收 Paused)。
    coordinator_->NoteProviderOutcome(!turn_failed);

    // ---- 3) evaluator:独立无工具请求,判词不混 main history ----
    if (turn_failed) {
        // 请求都没成:evaluator 没材料可判,不烧这一趟。goal 留在原态,
        // 连败账已在上面记;下一圈泵再问(pause_requested/终态会拦)。
        return;
    }
    const auto* task_now = coordinator_->task();
    if (task_now == nullptr || task_now->state != lubancode::runtime::goal::GoalState::Evaluating) {
        return;  // 状态没走到 Evaluating(收口前 pause 了):留账等 resume
    }
    lubancode::runtime::goal::GoalEvaluationInput input;
    input.task = *task_now;
    input.checkpoint = checkpoint;
    for (const auto& id : checkpoint.evidence_ids) {
        const auto* evidence = coordinator_->FindEvidence(id);
        if (evidence != nullptr) {
            input.evidence.push_back(*evidence);
        }
    }
    if (input.evidence.empty()) {
        // checkpoint 引用的证据一枚都没有:evaluator 没有可判的材料,
        // 记 provider 连败同路的"无材料"分支——判 continue 只会空转。
        Notify(/*is_error=*/false, "goal 轮收口:checkpoint 没有可核证据,不烧 evaluator(下轮先产证据)。");
        const auto schedule = coordinator_->ScheduleNextIteration(now_ms);
        if (!schedule.ok) {
            Notify(/*is_error=*/false, "goal 停排下一轮: " + schedule.error_message);
        }
        return;
    }
    if (task_now->last_evaluation.has_value()) {
        input.previous = *task_now->last_evaluation;
    }
    input.workspace_summary = "cwd: " + lubancode::platform::CurrentDirUtf8();
    input.now_ms = now_ms;

    lubancode::runtime::goal::GoalEvaluatorOptions evaluator_options;
    evaluator_options.model = *host_.current_model;
    if (host_.model_router != nullptr) {
        const auto routed_info = host_.model_router->RouteInfo(lubancode::agent::TaskKind::GoalEvaluate);
        if (!routed_info.model.empty()) {
            evaluator_options.model = routed_info.model;
            evaluator_options.reasoning_effort = routed_info.effort;
        }
    }
    const auto evaluation =
        lubancode::runtime::goal::RunGoalEvaluation(*host_.evaluation_backend, evaluator_options, input, nullptr);
    if (!evaluation.has_value()) {
        // evaluator 两坏/请求失败:goal 进 Paused(evaluator_failed),
        // 不默认 achieved 也不盲开下一轮(单子"evaluator 失败")。
        Notify(/*is_error=*/true, "goal evaluator 失败: " + evaluation.error() + ";目标转暂停(/goal resume 续)。");
        (void)coordinator_->NoteEvaluatorFailed(evaluation.error(), now_ms);
        lubancode::app::EmitGoalHook(
            MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                              host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
            lubancode::hooks::HookEvent::GoalPaused,
            nlohmann::json{{"goal_id", task_now->id}, {"error", evaluation.error()}},
            /*match_value=*/"evaluator_failed");
        return;
    }
    coordinator_->AddUsage(evaluation->usage);

    // ---- 4) 判词落地:continue 排下一轮,terminal 收账 ----
    const auto applied = coordinator_->ApplyEvaluation(evaluation->evaluation, now_ms);
    if (!applied.ok) {
        Notify(/*is_error=*/true, "goal 判词落账失败: " + applied.error_message);
        return;
    }
    const std::string decision = applied.payload.value("decision", std::string());
    Notify(/*is_error=*/false, "[goal 判词: " + decision + "] " + evaluation->evaluation.summary);
    if (evaluation->evaluation.overridden_achieved) {
        Notify(/*is_error=*/false, "  (evaluator 判 achieved 被程序门槛改判 continue: " +
                                       evaluation->evaluation.override_reason + ")");
    }
    lubancode::app::EmitGoalHook(
        MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                          host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
        lubancode::hooks::HookEvent::GoalEvaluated,
        nlohmann::json{{"goal_id", task_now->id},
                       {"iteration_id", checkpoint_result.payload.value("iteration_id", std::string())},
                       {"summary", evaluation->evaluation.summary.substr(0, 600)},
                       {"confidence", evaluation->evaluation.confidence}},
        /*match_value=*/decision);
    const auto* after = coordinator_->task();
    if (after != nullptr && lubancode::runtime::goal::IsGoalTerminal(after->state)) {
        // terminal 事件已落,GoalCompleted 在其后跑(单子:它失败不把
        // Achieved 改回 Active——OutputCapabilities 的 can_block=false
        // 正是这条边界)。
        lubancode::app::EmitGoalHook(
            MakeCommandWiring(host_.agent_tool ? host_.agent_tool() : nullptr,
                              host_.loop_scheduler ? host_.loop_scheduler() : nullptr),
            lubancode::hooks::HookEvent::GoalCompleted,
            nlohmann::json{{"goal_id", after->id},
                           {"decision", decision},
                           {"iterations", after->counters.iterations_started}},
            /*match_value=*/lubancode::runtime::goal::ToString(after->state));
    }
    if (applied.payload.value("schedule_next", false)) {
        const auto schedule = coordinator_->ScheduleNextIteration(now_ms);
        if (!schedule.ok) {
            Notify(/*is_error=*/false, "goal 停排下一轮: " + schedule.error_message);
        }
    }
}

}  // namespace lubancode::app
