// GoalCoordinator(goal 单第 1 期):目标状态机的唯一写口。
//
// 单子的铁律:
//   - 状态转换集中 Apply(event);CLI、TUI、Hook、模型工具都不得直改
//     GoalTask.state。
//   - 一场 session 只许一只非终态 goal(active pointer);再 create 报
//     goal_already_active,不默默覆盖。
//   - id 一旦分配不复用;edit 只涨 revision;每枚 iteration 记自己吃的
//     revision,旧轮回来不可覆盖新目标。
//   - terminal 不自动复活;resume 只收 Paused/AwaitingUser/Blocked。
//   - evaluator 收口只产 decision event,coordinator 不在这里调
//     AgentLoop(单飞铁律:session pump 在安全边界取下一枚 continuation)。
//   - 命令幂等:重复 pause/resume/clear 返回当前状态,不添重复 terminal
//     event。
//
// 纯内存状态机 + 可注入的落账回调(持久化由装配层接 SessionStore 的
// goal 事件行,见 goal_session.hpp);不 include cli/app/agent。

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/goal_session.hpp"
#include "runtime/goal_types.hpp"

namespace lubancode::runtime::goal {

// coordinator 向外发的事件(装配层拿去落 session JSONL / 发 Runtime 事件)。
struct GoalCoordinatorEvent {
    std::string event;       // created/contract_ready/scheduled/started/...
    std::string goal_id;
    std::string iteration_id;  // 迭代类事件带
    int revision = 0;
    nlohmann::json payload = nlohmann::json::object();

    std::int64_t timestamp_ms = 0;  // 装配层落盘时填
};

// 合同前置检查的结果:合同能不能冻结。
enum class PreflightVerdict {
    Ready,      // criteria 齐,可 Active(合同冻结)
    NeedsUser,  // 缺产品选择/空话目标,进 AwaitingUser 问用户
    Reject,     // 目标要求永不停止等,拒绝自动长跑(报原因,不立 goal)
};

// 一枚命令的结果:ok=false 时 error_code 是稳定码。
struct GoalCommandResult {
    bool ok = false;
    std::string error_code;
    std::string error_message;         // 人话兜底
    nlohmann::json payload = nlohmann::json::object();
};

class GoalCoordinator {
public:
    // 落账回调:每个状态事件先过这里(装配层 append+flush 进 session
    // JSONL)。返回 false = 写盘失败 → coordinator fail closed(goal 进
    // Failed,不再排下一轮)。可空(单测/未接存档)。
    using LedgerSink = std::function<bool(const GoalCoordinatorEvent&)>;

    struct Options {
        std::int64_t default_max_elapsed_ms = 2 * 60 * 60 * 1000;  // 2h
        int default_max_iterations = 40;
        int default_max_no_progress_iterations = 3;
        int default_max_same_blocker_iterations = 3;
        int default_max_consecutive_provider_failures = 3;
        // achieved 的置信阈值:只可抬高门槛,不可盖过硬检查。
        double min_achieved_confidence = 0.0;
        // feature gate(总闸环境变量由装配层读进来折成这枚开关)。
        bool goals_enabled = false;
    };

    explicit GoalCoordinator(Options options);
    ~GoalCoordinator();

    GoalCoordinator(const GoalCoordinator&) = delete;
    GoalCoordinator& operator=(const GoalCoordinator&) = delete;

    void SetLedgerSink(LedgerSink sink) { ledger_ = std::move(sink); }

    // ---- 命令面 --------------------------------------------------------------
    // /goal <objective>:创建(Preparing)。已有非终态 goal 报 already_active;
    // feature 关报 store_unavailable 之外另有稳定码?——单子:功能关时命令
    // 面直接拒,这里用 payload.disabled 告知。发号口可注入(测试定死 id)。
    GoalCommandResult Create(const std::string& objective, const std::string& workspace_root,
                             const std::string& workspace_identity, std::int64_t now_ms,
                             std::function<std::string()> id_issuer = nullptr);

    // 裸 /goal 与 /goal status 的原材料:结构化全账,人话由前端拼。
    nlohmann::json Status(std::int64_t now_ms) const;

    // /goal edit <objective>:revision +1;Running 时先记 pending(等边界
    // pause),首版直接拒 busy(expected_revision CAS)。
    GoalCommandResult Edit(const std::string& objective, int expected_revision, std::int64_t now_ms);

    // pause:立刻态直落 Paused;Running/Evaluating 记 pause_requested(不
    // 从别的线程硬改 history)。幂等。
    GoalCommandResult Pause(std::int64_t now_ms);

    // resume:只收 Paused/AwaitingUser/Blocked;清 blocker/no-progress
    // streak,账不清零;terminal 报 goal_terminal。
    GoalCommandResult Resume(int expected_revision, std::int64_t now_ms);

    // clear:写 terminal event(Cleared),审计账不删;幂等(已 terminal 的
    // goal 重复 clear 返回当前状态,不添重复 terminal event)。
    GoalCommandResult Clear(std::int64_t now_ms);

    // ---- 合同 preflight(第 0 iteration) -----------------------------------
    // 模型/宿主拟出合同后交这里验:criteria 空、目标空话、要求永不停止
    // 都在此分档。Ready 时合同冻结(hash 记档),goal 进 Active。
    GoalCommandResult SubmitContract(const GoalContract& contract, std::int64_t now_ms);

    static PreflightVerdict PreflightContract(const GoalContract& contract, std::string* reason);

    // ---- iteration 生命周期 --------------------------------------------------
    // 排下一枚 iteration(evaluator=continue 或 resume 后)。预算先核
    // (headroom 不够报 budget_exhausted);落 scheduled 事件成功才排进
    // ready 队列。dedupe_key = "goal-N:rR:iI"。
    GoalCommandResult ScheduleNextIteration(std::int64_t now_ms);

    // 主泵取走 ready 的 iteration,发 synthetic turn 前调:落 started 事件
    // (带 turn_id/dedupe_key/before fingerprint)。没有 ready 的报 not_found。
    struct StartedIteration {
        bool ok = false;
        std::string error_code;
        GoalIteration iteration;
        std::string dedupe_key;          // goal-3:r2:i8
        std::string synthetic_text;      // 喂模型的文字(宿主拼,metadata 不在这)
    };
    StartedIteration TakeReadyIteration(const std::string& turn_id,
                                        const std::string& before_fingerprint, std::int64_t now_ms);

    // 执行轮收口:记 checkpoint(工具调了 goal_checkpoint 的最新一枚;没调
    // 就合成 missing checkpoint,仍送 evaluator,不谎成完成)。
    GoalCommandResult CheckpointReached(const GoalCheckpoint& checkpoint, std::int64_t now_ms);

    // 合成 missing checkpoint(模型没调工具便自然停)。
    static GoalCheckpoint MakeMissingCheckpoint();

    // evaluator 判词落地:硬检查(achieved 门槛)在这道口把关;continue
    // 只产 ready 事件,不开新轮(单飞铁律)。
    //   - achieved 证据不够 → 改判 continue + kErrGoalEvidenceInsufficient。
    //   - blocked 无 blocker_key → schema 拒(kErrGoalEvaluatorSchema)。
    //   - needs_user 无 question → schema 拒。
    GoalCommandResult ApplyEvaluation(const GoalEvaluation& evaluation, std::int64_t now_ms);

    // achieved 的程序硬门槛(单子"achieved 的硬门槛"全表;供 ApplyEvaluation
    // 与回放器共用)。evidence_lookup:ev-id → {fresh, hash 对得上}(装配层
    // 从证据账查;空表 = 无证据)。
    static bool HardGateAchieved(const GoalTask& task, const GoalEvaluation& evaluation,
                                 const std::function<std::optional<GoalEvidence>(const std::string&)>& evidence_lookup,
                                 std::string* failure_reason);

    // usage 记账(整轮 execution/evaluator/subagent 的账折进来)。
    void AddUsage(const GoalUsage& usage);

    // 无进展判定:宿主算 fingerprint,这里比上一轮;同则 streak+1,撞
    // max_no_progress → Paused(no_progress)。有进展清零。
    // 返回 {streak_after, tripped}。
    struct ProgressCheck {
        int streak_after = 0;
        bool tripped = false;
        bool progressed = false;
    };
    ProgressCheck NoteProgressFingerprint(const std::string& fingerprint, std::int64_t now_ms);

    // 同一 blocker 连续出现:撞 max_same_blocker → Blocked(resume 可解)。
    struct BlockerCheck {
        int streak_after = 0;
        bool tripped = false;
    };
    BlockerCheck NoteBlocker(const std::string& blocker_key, std::int64_t now_ms);

    // 预算硬闸:开新 iteration 前核 headroom(elapsed/iterations/token)。
    // 撞了返回 false + reason;coordinator 由 ScheduleNextIteration 自动收
    // BudgetExhausted。
    bool CheckBudgetHeadroom(std::int64_t now_ms, std::string* reason) const;

    // elapsed 闸:轮外也要能收口(休眠醒来直接 Exhausted)。
    bool ElapsedExceeded(std::int64_t now_ms, std::int64_t* over_by_ms) const;

    // provider 连败:撞 max_consecutive_provider_failures → Paused
    //(provider_failures)。成功一次清零。
    void NoteProviderOutcome(bool succeeded);

    // ---- 恢复(第 4 期回放器入口) -------------------------------------------
    // 从事件账重建(装配层 replay 出的事件序)。坏事件跳过,不废整场。
    void ReplayEvent(const GoalCoordinatorEvent& event);

    // 便捷口:/resume 从 LoadedSession::goal_events(存档行形状)整批回放。
    // 返回 {回放条数, 跳过条数};feature 关时读到 active goal 落
    // SuspendedByPolicy(不自动续跑,用户仍可查/导出/clear)。
    struct ReplayStats {
        int replayed = 0;
        int skipped = 0;
        bool suspended_by_policy = false;
    };
    ReplayStats RestoreFromArchive(const std::vector<lubancode::agent::GoalSessionEvent>& events);

    // 迟到事件(terminal 后到的旧 evaluator/子代理/Hook):只留账,不改
    // 状态。返回 true = 已吸收(留审计),false = 拒。
    bool AbsorbLateArrival(const GoalCoordinatorEvent& event);

    // ---- 查询 ----------------------------------------------------------------
    bool HasActiveGoal() const { return task_.has_value() && !IsGoalTerminal(task_->state); }
    bool has_goal() const { return task_.has_value(); }
    const GoalTask* task() const { return task_.has_value() ? &*task_ : nullptr; }
    bool goals_enabled() const { return options_.goals_enabled; }

    // ready 队列(主泵在安全边界问一句)。
    bool HasReadyContinuation() const { return ready_.has_value(); }
    const std::string& ready_dedupe_key() const { return ready_dedupe_; }

    // pause_requested(正在跑的轮在下一安全边界收口时看这枚旗)。
    bool pause_requested() const { return pause_requested_; }

    // ---- 证据账(hard gate 的查表口;装配层从 tool trace 采证后喂进来) ----
    // 只认本 goal 的证据;跨 goal/失效的查表即知。重复 id 后到的覆盖先到的
    //(append-only 账回放时同一 id 只该出现一次,回放侧天然满足)。
    void RecordEvidence(const GoalEvidence& evidence);
    const GoalEvidence* FindEvidence(const std::string& id) const;
    std::size_t evidence_count() const { return evidence_.size(); }
    // 证据失效翻 stale(相关改动后旧 validation 要按影响范围翻)。
    void MarkEvidenceStale(const std::string& id);

private:
    bool Emit(const GoalCoordinatorEvent& event);  // 落账失败 → FailClosed
    void FailClosed(std::int64_t now_ms, const std::string& reason);
    GoalCommandResult Fail(const char* code, const std::string& message);
    void RequireActiveOr(const char* absent_code);

    Options options_;
    LedgerSink ledger_;
    std::optional<GoalTask> task_;
    std::optional<GoalIteration> ready_;   // 已 scheduled 未 taken
    std::string ready_dedupe_;
    bool pause_requested_ = false;
    int next_iteration_index_ = 0;
    int goal_seq_ = 0;                     // session-local monotonic(goal-N)
    int evidence_seq_ = 0;                 // ev-N
    int eval_seq_ = 0;                     // eval-N
    std::vector<GoalCoordinatorEvent> late_arrivals_;  // 审计留账
    std::unordered_map<std::string, GoalEvidence> evidence_;  // 证据账(ev-id → 证据)
};

}  // namespace lubancode::runtime::goal
