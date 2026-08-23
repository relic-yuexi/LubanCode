// SessionWorkScheduler(loop 单第 1/5 期):公平泵实现。
//
// goal 单立的合同口在这里实装:
//   - WorkPriority:五档优先级数值(越小越先,同档 FIFO)。
//   - SessionWorkPump:一场 session 的取件总口。装配层把 user queue、
//     pending interaction、GoalWorkSource、due loop tick、maintenance 各
//     源喂进来;泵按优先级取一枚,FairnessCounter 管"goal 连跑三轮让
//     一枚到期 loop tick 插队"的防饿死账。
//
// 纯逻辑(单测钉):不碰线程、不碰磁盘;真源由装配层提供。

#include "runtime/session_work_scheduler.hpp"

#include <algorithm>

namespace lubancode::runtime {

int WorkPriority(WorkKind kind) {
    switch (kind) {
        case WorkKind::UserQueuedTurn: return 0;
        case WorkKind::PendingInteraction: return 0;
        case WorkKind::GoalContinuation: return 1;
        case WorkKind::LoopTick: return 1;
        case WorkKind::Maintenance: return 2;
    }
    return 2;
}

namespace {

// 同档内 FIFO:priority 相同时保 stable order(输入次序即到达次序)。
bool EarlierWork(const SessionWork& a, const SessionWork& b) {
    return WorkPriority(a.kind) < WorkPriority(b.kind);
}

}  // namespace

std::optional<SessionWork> PumpNextWork(const std::vector<SessionWork>& candidates,
                                        const FairnessCounter& fairness) {
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::vector<SessionWork> sorted = candidates;
    std::stable_sort(sorted.begin(), sorted.end(), EarlierWork);
    // 防饿死:goal 连跑够了(streak >= limit)且有一枚 due loop tick,
    // 让 loop 先(goal 与 loop 同档,这里凭 fairness 旗在两枚同档候选间
    // 挑 loop;没有 loop 候选时 goal 照走,streak 装配层在收口后清)。
    if (fairness.LoopShouldPreemptGoal()) {
        for (const SessionWork& work : sorted) {
            if (work.kind == WorkKind::LoopTick) {
                return work;
            }
        }
    }
    return sorted.front();
}

}  // namespace lubancode::runtime
