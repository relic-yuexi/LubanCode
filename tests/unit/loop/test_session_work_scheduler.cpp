// SessionWorkScheduler 的公平泵与 FairnessCounter(loop 单:goal 分流合流)。
// goal 单立的合同口在这钉:五档优先级、同档 FIFO、goal 连跑三轮让一拍。

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "runtime/session_work_scheduler.hpp"

using lubancode::runtime::FairnessCounter;
using lubancode::runtime::GoalWorkSource;
using lubancode::runtime::PumpNextWork;
using lubancode::runtime::SessionWork;
using lubancode::runtime::WorkKind;
using lubancode::runtime::WorkPriority;

namespace {
SessionWork Make(WorkKind kind, const std::string& id) {
    SessionWork w;
    w.kind = kind;
    w.id = id;
    return w;
}
}  // namespace

TEST_CASE("优先级表:user/interaction 最高,goal≈loop 同档,maintenance 最低") {
    CHECK(WorkPriority(WorkKind::UserQueuedTurn) == 0);
    CHECK(WorkPriority(WorkKind::PendingInteraction) == 0);
    CHECK(WorkPriority(WorkKind::GoalContinuation) == 1);
    CHECK(WorkPriority(WorkKind::LoopTick) == 1);
    CHECK(WorkPriority(WorkKind::Maintenance) == 2);
}

TEST_CASE("泵:按优先级取,空给 nullopt") {
    CHECK(PumpNextWork({}, FairnessCounter{}) == std::nullopt);
    // user 压 goal/loop/maintenance。
    const auto picked = PumpNextWork(
        {Make(WorkKind::GoalContinuation, "g1"), Make(WorkKind::UserQueuedTurn, "u1"),
         Make(WorkKind::Maintenance, "m1"), Make(WorkKind::LoopTick, "l1")},
        FairnessCounter{});
    REQUIRE(picked.has_value());
    CHECK(picked->id == "u1");
    // 没 user 时 goal 与 loop 同档:到达序 FIFO(goal 先进先走)。
    const auto same = PumpNextWork({Make(WorkKind::GoalContinuation, "g1"),
                                    Make(WorkKind::LoopTick, "l1")},
                                   FairnessCounter{});
    REQUIRE(same.has_value());
    CHECK(same->id == "g1");
    // 反过来 loop 先进。
    const auto same2 = PumpNextWork({Make(WorkKind::LoopTick, "l1"),
                                     Make(WorkKind::GoalContinuation, "g1")},
                                    FairnessCounter{});
    REQUIRE(same2.has_value());
    CHECK(same2->id == "l1");
}

TEST_CASE("公平账:goal 连跑三轮,loop 插队") {
    FairnessCounter fairness;
    CHECK_FALSE(fairness.LoopShouldPreemptGoal());
    fairness.NoteGoalRan();
    CHECK_FALSE(fairness.LoopShouldPreemptGoal());
    fairness.NoteGoalRan();
    CHECK_FALSE(fairness.LoopShouldPreemptGoal());  // 两轮还不够
    fairness.NoteGoalRan();
    CHECK(fairness.LoopShouldPreemptGoal());  // 三轮,该让了

    // 让位真的发生:同档里 goal 先进,但 fairness 立着,loop 先走。
    const auto picked = PumpNextWork({Make(WorkKind::GoalContinuation, "g1"),
                                      Make(WorkKind::LoopTick, "l1")},
                                     fairness);
    REQUIRE(picked.has_value());
    CHECK(picked->id == "l1");

    // 别的工作跑了,streak 清零,goal 又能走。
    fairness.NoteOtherWorkRan();
    CHECK_FALSE(fairness.LoopShouldPreemptGoal());
    const auto back = PumpNextWork({Make(WorkKind::GoalContinuation, "g1"),
                                    Make(WorkKind::LoopTick, "l1")},
                                   fairness);
    REQUIRE(back.has_value());
    CHECK(back->id == "g1");

    // fairness 立着但没有 loop 候选:goal 照走(饿死账只在有得让时让)。
    FairnessCounter tripped;
    tripped.NoteGoalRan();
    tripped.NoteGoalRan();
    tripped.NoteGoalRan();
    const auto no_loop = PumpNextWork({Make(WorkKind::GoalContinuation, "g1"),
                                       Make(WorkKind::Maintenance, "m1")},
                                      tripped);
    REQUIRE(no_loop.has_value());
    CHECK(no_loop->id == "g1");
}

TEST_CASE("GoalWorkSource:SetProbe/ProbeWork 合同口") {
    GoalWorkSource source;
    CHECK(source.ProbeWork() == std::nullopt);  // 没 probe 给空
    source.SetProbe([]() -> std::optional<SessionWork> {
        SessionWork w;
        w.kind = WorkKind::GoalContinuation;
        w.id = "goal-3:r2:i8";
        w.text = "synthetic continuation";
        return w;
    });
    const auto probed = source.ProbeWork();
    REQUIRE(probed.has_value());
    CHECK(probed->kind == WorkKind::GoalContinuation);
    CHECK(probed->id == "goal-3:r2:i8");
    CHECK(probed->text == "synthetic continuation");
    // probe 说没有就没有。
    source.SetProbe([]() -> std::optional<SessionWork> { return std::nullopt; });
    CHECK(source.ProbeWork() == std::nullopt);
}
