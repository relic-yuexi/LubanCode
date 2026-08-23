// SessionWorkScheduler(持久目标单第 8 期立的合同口;loop 单第 0/1 期来接):
// 一场 session 的自动工作公平调度总口。
//
// 单子的定案(goal 单"与 Loop/Hook/Subagent 合流"节 + loop 单"goal 分流"):
//   - /goal continuation 与 /loop tick 不共用 trigger(evaluator 判终点 vs
//     时钟到点),却共用一只 session work pump(单飞铁律:一场 session 只
//     跑一枚 main turn,主泵在安全边界取下一枚)。
//   - 优先级表(user queued > pending interaction > goal continuation ≈
//     due loop tick > maintenance);goal 连跑三轮后让一枚到期 loop tick
//     插队,防饿死。
//   - goal 内不得创建零间隔 loop 旁路预算;loop prompt 不得 /goal 新建
//     第二只 active goal(两侧装配层各拦,这里只管排序)。
//
// 本头只立合同(work kind、优先级、取件接口与 goal 喂件口);真正的 timer/
// 泵实现在 loop 单落地。goal 侧的接法:装配层把 coordinator 的 ready
// continuation 喂进 GoalWorkSource,主泵问 TakeNextWork。

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace lubancode::runtime {

// 一类自动工作。
enum class WorkKind {
    UserQueuedTurn,   // 用户排队消息(最高)
    PendingInteraction, // 审批/问答(最高)
    GoalContinuation, // evaluator=continue 的下一轮 goal iteration
    LoopTick,         // 到点的 loop 拍子
    Maintenance,      // compact/摘要一类后台活(低)
};

// 优先级数值越小越先(同档按 FIFO)。goal 与 loop 同档,防饿死靠
// FairnessCounter 的"goal 连跑三轮让一拍"。
int WorkPriority(WorkKind kind);

// 一枚待取的工作。
struct SessionWork {
    WorkKind kind = WorkKind::GoalContinuation;
    std::string id;      // goal: dedupe_key(goal-3:r2:i8);loop: tick id
    std::string text;    // goal: synthetic continuation;loop: tick prompt
    nlohmann::json payload;  // 其余结构字段(goal_id/iteration_id/loop task id…)
};

// goal 侧的喂件口(装配层持一只,coordinator 的 ready continuation 经它
// 进泵;主泵取走后装配层再 TakeReadyIteration 发 synthetic turn)。
class GoalWorkSource {
public:
    // 泵问一句:goal 有没有 ready continuation。text/payload 由装配层现拼
    // (GoalContext 走 SetTurnContext,不进这)。
    using Probe = std::function<std::optional<SessionWork>()>;
    void SetProbe(Probe probe) { probe_ = std::move(probe); }
    std::optional<SessionWork> ProbeWork() const { return probe_ ? probe_() : std::nullopt; }

private:
    Probe probe_;
};

// 公平账:goal 连跑 N 轮(默认 3)后,同档的 due loop tick 插一次队。
// 纯逻辑,单测钉;泵在每轮收口后调 NoteGoalRan/Reset。
class FairnessCounter {
public:
    explicit FairnessCounter(int goal_streak_limit = 3) : goal_streak_limit_(goal_streak_limit) {}

    void NoteGoalRan() { ++goal_streak_; }
    void NoteOtherWorkRan() { goal_streak_ = 0; }
    void Reset() { goal_streak_ = 0; }

    // 这一拍该不该让 loop tick 先(goal 连跑够了)。
    bool LoopShouldPreemptGoal() const { return goal_streak_ >= goal_streak_limit_; }
    int goal_streak() const { return goal_streak_; }

private:
    int goal_streak_ = 0;
    int goal_streak_limit_ = 3;
};

// 公平泵(loop 单实装):从一组候选里取下一枚工作。
//   - 按 WorkPriority 升序,同档保序(stable,即到达次序 FIFO)。
//   - fairness.LoopShouldPreemptGoal() 立着且候选里有 due loop tick 时,
//     loop 先(防 goal 饿死同档的 loop)。
// 装配层每圈把各源现拼的候选传进来,取走一枚、跑完回来再问下一圈。
std::optional<SessionWork> PumpNextWork(const std::vector<SessionWork>& candidates,
                                        const FairnessCounter& fairness);

}  // namespace lubancode::runtime
