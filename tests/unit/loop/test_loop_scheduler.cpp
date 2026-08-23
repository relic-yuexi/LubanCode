// /loop 单第 1 期:内存 scheduler——准点一拍、忙时合并、single-flight、
// 多 task 排序、pause/resume、退避、expiry、熔断;IdleWakeCoordinator
// 的多路与 RAII 摘源。全部走注入 fake clock,不 sleep。

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "runtime/idle_wake.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/loop_types.hpp"

using lubancode::runtime::IdleWakeCoordinator;
using lubancode::runtime::loop::LoopClock;
using lubancode::runtime::loop::LoopDefaults;
using lubancode::runtime::loop::LoopPromptSource;
using lubancode::runtime::loop::LoopScheduler;
using lubancode::runtime::loop::LoopTaskState;
using lubancode::runtime::loop::LoopTickOutcome;

namespace {

constexpr std::chrono::seconds k1m{60};
constexpr std::chrono::seconds k5m{300};
constexpr std::int64_t kMs(const std::chrono::seconds s) {
    return s.count() * 1000;
}

// 手拨的墙钟:测试自己推。
struct ManualClock : LoopClock {
    std::int64_t now = kMs(std::chrono::seconds(1000));
    std::int64_t NowWallMs() const override { return now; }
};

struct Fixture {
    std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
    LoopScheduler::Options options;
    int next_id = 0;
    std::unique_ptr<LoopScheduler> sched;

    Fixture() : options{} {
        sched = std::make_unique<LoopScheduler>(options, clock);
    }

    auto Create5m(const std::string& prompt = "检查 CI") {
        return sched->Create(prompt, k5m, clock->now, "D:/proj", "s1",
                             LoopPromptSource::Inline, "", [this]() {
                                 return "loop-" + std::to_string(++next_id);
                             });
    }
};

}  // namespace

TEST_CASE("准点一拍:不到点不跑,到点开 turn") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    // next_due = now + 5m:还没到,泵空。
    CHECK_FALSE(fx.sched->HasDueWork(fx.clock->now));
    CHECK(fx.sched->PumpDueTick(fx.clock->now, "t1") == std::nullopt);
    // 过 5m:due。
    fx.clock->now += kMs(k5m);
    REQUIRE(fx.sched->HasDueWork(fx.clock->now));
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "turn-1");
    REQUIRE(tick.has_value());
    CHECK(tick->tick.tick_no == 1);
    CHECK(tick->tick.turn_id == "turn-1");
    CHECK(tick->task.state == LoopTaskState::Running);
    CHECK(tick->task.run_count == 1);
    CHECK(tick->text == "检查 CI");
    // 事件账:created/due/started 都在。
    const auto events = fx.sched->TakeEvents();
    REQUIRE(events.size() == 3);
    CHECK(events[0].event == "created");
    CHECK(events[1].event == "due");
    CHECK(events[1].family == "loop_tick_v1");
    CHECK(events[2].event == "started");
}

TEST_CASE("single-flight:Running 时到点不另开第二枚") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    fx.clock->now += kMs(k5m);
    REQUIRE(fx.sched->PumpDueTick(fx.clock->now, "turn-1").has_value());
    // 再过两个 interval,泵不该再取这只 task。
    fx.clock->now += kMs(k5m) * 2;
    CHECK(fx.sched->PumpDueTick(fx.clock->now, "turn-2") == std::nullopt);
    // HasDueWork 也静(Running 不算 due 活)。
    CHECK_FALSE(fx.sched->HasDueWork(fx.clock->now));
    // 收口后:cadence 已推进,未到新 slot 前 still 空。
    fx.sched->FinishTick("loop-1#1", LoopTickOutcome::Succeeded, fx.clock->now);
    CHECK(fx.sched->PumpDueTick(fx.clock->now, "turn-3") == std::nullopt);
}

TEST_CASE("休眠错过:只合并一拍,missed 记账") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    const std::int64_t due_at = fx.clock->now + kMs(k5m);
    // 睡两小时才来泵。
    fx.clock->now = due_at + kMs(std::chrono::seconds(7200));
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "turn-1");
    REQUIRE(tick.has_value());
    CHECK(tick->tick.missed_count == 24);
    CHECK(tick->tick.scheduled_at_ms == due_at);  // 排的点还是原 slot
    CHECK(tick->task.skipped_count == 24);
    // 收口后的下一拍在原时间轴 future slot,不追债。
    fx.sched->FinishTick("loop-1#1", LoopTickOutcome::Succeeded, fx.clock->now);
    const auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Active);
    CHECK(view->task.next_due_at_ms > fx.clock->now);
    // 事件账里 due 事件带 missed=24。
    bool saw_due = false;
    for (const auto& e : fx.sched->TakeEvents()) {
        if (e.event == "due") {
            saw_due = e.payload.value("missed", -1) == 24;
        }
    }
    CHECK(saw_due);
}

TEST_CASE("多 task 同刻:按 next_due,再按 creation seq") {
    Fixture fx;
    REQUIRE(fx.Create5m("甲").ok);  // loop-1,seq 早
    const std::string id2 = "loop-" + std::to_string(fx.next_id + 1);
    REQUIRE(fx.sched
                ->Create("乙", k1m, fx.clock->now, "D:/proj", "s1", LoopPromptSource::Inline,
                         "", [&id2]() { return id2; })
                .ok);
    fx.next_id += 1;
    // 甲 5m、乙 1m:过 5m,两只都到点,乙的 slot 早,先走。
    fx.clock->now += kMs(k5m);
    auto first = fx.sched->PumpDueTick(fx.clock->now, "t1");
    REQUIRE(first.has_value());
    CHECK(first->task.task_id == "loop-2");
    fx.sched->FinishTick("loop-2#1", LoopTickOutcome::Succeeded, fx.clock->now);
    auto second = fx.sched->PumpDueTick(fx.clock->now, "t2");
    REQUIRE(second.has_value());
    CHECK(second->task.task_id == "loop-1");
}

TEST_CASE("pause/resume:从 now+interval 重排,不补旧账") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    const std::int64_t original_due = fx.clock->now + kMs(k5m);
    REQUIRE(fx.sched->Pause("loop-1", fx.clock->now, "user").ok);
    // paused 期间过了原 due,泵不取。
    fx.clock->now = original_due + kMs(k1m);
    CHECK(fx.sched->PumpDueTick(fx.clock->now, "t") == std::nullopt);
    // resume:next_due = resume_now + 5m,原 slot 作废。
    REQUIRE(fx.sched->Resume("loop-1", fx.clock->now).ok);
    const auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.next_due_at_ms == fx.clock->now + kMs(k5m));
    CHECK(view->task.next_due_at_ms > original_due + kMs(k5m));
    // 幂等 pause。
    CHECK(fx.sched->Pause("loop-1", fx.clock->now, "user").ok);
}

TEST_CASE("stop/complete:终态不复活") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    REQUIRE(fx.sched->Stop("loop-1", fx.clock->now, "user").ok);
    auto stopped = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(stopped.has_value());
    CHECK(stopped->task.state == LoopTaskState::Cancelled);
    // 终态上 resume 报错,不复活。
    CHECK_FALSE(fx.sched->Resume("loop-1", fx.clock->now).ok);

    // complete 只从运行态。
    REQUIRE(fx.Create5m("乙").ok);
    fx.clock->now += kMs(k5m);
    CHECK_FALSE(fx.sched->Complete("loop-2", fx.clock->now, "done").ok);  // Active 不收
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "t1");
    REQUIRE(tick.has_value());
    REQUIRE(fx.sched->Complete("loop-2", fx.clock->now, "部署完成").ok);
    auto done = fx.sched->Find("loop-2", fx.clock->now);
    REQUIRE(done.has_value());
    CHECK(done->task.state == LoopTaskState::Completed);
    CHECK(fx.sched->PumpDueTick(fx.clock->now + kMs(k5m), "t2") == std::nullopt);
}

TEST_CASE("run now:立即补一拍,不改 cadence") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    const std::int64_t original_due = fx.clock->now + kMs(k5m);
    REQUIRE(fx.sched->RunNow("loop-1", fx.clock->now).ok);
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "t1");
    REQUIRE(tick.has_value());
    // 补拍:missed=0,scheduled_at 记"此刻"。
    CHECK(tick->tick.scheduled_at_ms == fx.clock->now);
    CHECK(tick->tick.missed_count == 0);
    fx.sched->FinishTick("loop-1#1", LoopTickOutcome::Succeeded, fx.clock->now);
    auto view = fx.sched->Find("loop-1", fx.clock->now);
    // cadence 不动:下一拍仍在原 slot(create 后排的 5m 点)。
    CHECK(view->task.next_due_at_ms == original_due);
    // 原 slot 到点,第二拍照常走。
    fx.clock->now = original_due;
    auto second = fx.sched->PumpDueTick(fx.clock->now, "t2");
    REQUIRE(second.has_value());
    CHECK(second->tick.tick_no == 2);
}

TEST_CASE("provider 连败五拍自动 Pause;成功清零") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    for (int i = 1; i <= 5; ++i) {
        fx.clock->now += kMs(k5m);
        auto tick = fx.sched->PumpDueTick(fx.clock->now, "t" + std::to_string(i));
        REQUIRE(tick.has_value());
        fx.sched->FinishTick(tick->tick.tick_id, LoopTickOutcome::ProviderError, fx.clock->now,
                             "provider.down");
        if (i < 5) {
            auto view = fx.sched->Find("loop-1", fx.clock->now);
            REQUIRE(view.has_value());
            CHECK(view->task.state == LoopTaskState::Active);
        }
    }
    auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Paused);
    CHECK(view->task.consecutive_failures == 5);
    // auto pause 的事件账里 reason=provider_failures。
    bool saw_auto = false;
    for (const auto& e : fx.sched->TakeEvents()) {
        if (e.event == "paused" && e.payload.value("reason", "") == "provider_failures") {
            saw_auto = true;
        }
    }
    CHECK(saw_auto);
    // resume 清败账:下一拍成功,failures 归零。
    REQUIRE(fx.sched->Resume("loop-1", fx.clock->now).ok);
    fx.clock->now += kMs(k5m);
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "t9");
    REQUIRE(tick.has_value());
    fx.sched->FinishTick(tick->tick.tick_id, LoopTickOutcome::Succeeded, fx.clock->now);
    auto after = fx.sched->Find("loop-1", fx.clock->now);
    CHECK(after->task.consecutive_failures == 0);
}

TEST_CASE("denied 连三拍自动 Pause;工具 is_error 不算失败") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    for (int i = 1; i <= 3; ++i) {
        fx.clock->now += kMs(k5m);
        auto tick = fx.sched->PumpDueTick(fx.clock->now, "t");
        REQUIRE(tick.has_value());
        fx.sched->FinishTick(tick->tick.tick_id, LoopTickOutcome::Declined, fx.clock->now);
    }
    auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Paused);
    CHECK(view->task.consecutive_denials == 3);

    // 工具 is_error(业务失败)本拍 failed,下一拍仍 Active。
    REQUIRE(fx.sched->Resume("loop-1", fx.clock->now).ok);
    fx.clock->now += kMs(k5m);
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "t");
    REQUIRE(tick.has_value());
    // succeeded 口径:正常工具 is_error 不停 task(装配层把整轮失败落
    // provider_error;单枚工具错不升格)。
    fx.sched->FinishTick(tick->tick.tick_id, LoopTickOutcome::Succeeded, fx.clock->now);
    auto after = fx.sched->Find("loop-1", fx.clock->now);
    CHECK(after->task.state == LoopTaskState::Active);
    CHECK(after->task.consecutive_failures == 0);
}

TEST_CASE("退避:rate limit 进 BackingOff,不双发") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    fx.clock->now += kMs(k5m);
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "t1");
    REQUIRE(tick.has_value());
    fx.sched->NoteRateLimited(tick->tick.tick_id, std::chrono::seconds(30), fx.clock->now);
    auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::BackingOff);
    // 退避里泵不取。
    CHECK(fx.sched->PumpDueTick(fx.clock->now + 1000, "t2") == std::nullopt);
    // cadence 挪到 backoff 结束之后。
    CHECK(view->task.next_due_at_ms >= fx.clock->now + 30000);
}

TEST_CASE("同 tick 退避重试:5s/15s,最多两次") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    fx.clock->now += kMs(k5m);
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "t1");
    REQUIRE(tick.has_value());
    auto first = fx.sched->RetryBackoffFor(tick->tick.tick_id, fx.clock->now);
    REQUIRE(first.has_value());
    CHECK(*first == std::chrono::milliseconds(5000));
    auto second = fx.sched->RetryBackoffFor(tick->tick.tick_id, fx.clock->now);
    REQUIRE(second.has_value());
    CHECK(*second == std::chrono::milliseconds(15000));
    // 第三次:到顶。
    CHECK(fx.sched->RetryBackoffFor(tick->tick.tick_id, fx.clock->now) == std::nullopt);
}

TEST_CASE("审批悬起:Waiting 不开新拍,答回继续") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    fx.clock->now += kMs(k5m);
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "t1");
    REQUIRE(tick.has_value());
    fx.sched->NotePermissionWait(tick->tick.tick_id, fx.clock->now);
    auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::WaitingPermission);
    // 悬着时再过几拍,泵不开新 turn。
    fx.clock->now += kMs(k5m) * 3;
    CHECK(fx.sched->PumpDueTick(fx.clock->now, "t2") == std::nullopt);
    // 用户回来答了:回 Running,本拍继续,收口正常。
    fx.sched->NotePermissionResolved(tick->tick.tick_id, fx.clock->now);
    view = fx.sched->Find("loop-1", fx.clock->now);
    CHECK(view->task.state == LoopTaskState::Running);
    fx.sched->FinishTick(tick->tick.tick_id, LoopTickOutcome::Succeeded, fx.clock->now);
    view = fx.sched->Find("loop-1", fx.clock->now);
    CHECK(view->task.state == LoopTaskState::Active);
}

TEST_CASE("expiry:七天到头落 expired,不默默消失") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    // 快进 7 天 + 1ms。
    fx.clock->now += kMs(LoopDefaults::kExpiryAge) + 1;
    fx.sched->SweepExpiry(fx.clock->now);
    auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Expired);
    CHECK(fx.sched->PumpDueTick(fx.clock->now, "t") == std::nullopt);
    bool saw_expired = false;
    for (const auto& e : fx.sched->TakeEvents()) {
        if (e.event == "expired") {
            saw_expired = true;
        }
    }
    CHECK(saw_expired);
}

TEST_CASE("cwd 移房:绑不上的 task Pause") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    fx.sched->NoteCwdChanged("D:/elsewhere", fx.clock->now);
    auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Paused);
    bool saw = false;
    for (const auto& e : fx.sched->TakeEvents()) {
        if (e.event == "paused" && e.payload.value("reason", "") == "cwd_moved") {
            saw = true;
        }
    }
    CHECK(saw);
}

TEST_CASE("上限与拒建") {
    Fixture fx;
    // 8 只到顶,第 9 只拒。
    for (int i = 0; i < LoopDefaults::kMaxActivePerSession; ++i) {
        REQUIRE(fx.Create5m().ok);
    }
    const auto ninth = fx.Create5m();
    CHECK_FALSE(ninth.ok);
    CHECK(ninth.error_code == lubancode::runtime::loop::kErrLoopTooManyActive);
    // interval 太小/太大。
    const auto tiny = fx.sched->Create("x", std::chrono::seconds(1), fx.clock->now, "d", "s");
    CHECK_FALSE(tiny.ok);
    // 空 inline prompt。
    const auto empty =
        fx.sched->Create("", k5m, fx.clock->now, "d", "s");
    CHECK_FALSE(empty.ok);
    // feature 关:全拒。
    LoopScheduler::Options off;
    off.enabled = false;
    LoopScheduler disabled(off, fx.clock);
    const auto gated = disabled.Create("x", k5m, 0, "d", "s");
    CHECK_FALSE(gated.ok);
    CHECK(gated.error_code == "loop.disabled");
}

TEST_CASE("熔断:存档写盘失败后不再开新 tick") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    fx.sched->FailStore("disk full");
    fx.clock->now += kMs(k5m);
    CHECK_FALSE(fx.sched->HasDueWork(fx.clock->now));
    CHECK(fx.sched->PumpDueTick(fx.clock->now, "t") == std::nullopt);
    CHECK_FALSE(fx.Create5m().ok);
    // 熔断事件在账上。
    bool saw = false;
    for (const auto& e : fx.sched->TakeEvents()) {
        if (e.event == "store_failed") {
            saw = true;
        }
    }
    CHECK(saw);
}

TEST_CASE("回放:created/started/finished 重建内存账") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    fx.clock->now += kMs(k5m);
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "turn-1");
    REQUIRE(tick.has_value());
    fx.sched->FinishTick("loop-1#1", LoopTickOutcome::Succeeded, fx.clock->now);
    const auto events = fx.sched->TakeEvents();

    // 新 scheduler 回放同一串账。
    ManualClock clock2;
    clock2.now = fx.clock->now + kMs(k1m);
    LoopScheduler restored({}, std::make_shared<ManualClock>(clock2));
    for (const auto& e : events) {
        CHECK(restored.ReplayEvent(e));
    }
    auto view = restored.Find("loop-1", clock2.now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Active);
    CHECK(view->task.run_count == 1);
    CHECK(view->task.next_due_at_ms == tick->task.next_due_at_ms);
    // 回放后的账继续走:到点开第二拍。
    clock2.now = view->task.next_due_at_ms;
    auto next = restored.PumpDueTick(clock2.now, "turn-2");
    REQUIRE(next.has_value());
    CHECK(next->tick.tick_no == 2);
}

TEST_CASE("迟到收口:不是当前拍,留账不动状态") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    fx.clock->now += kMs(k5m);
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "turn-1");
    REQUIRE(tick.has_value());
    fx.sched->FinishTick("loop-1#99", LoopTickOutcome::Succeeded, fx.clock->now);  // 不存在的拍
    auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Running);  // 没被动
    fx.sched->FinishTick(tick->tick.tick_id, LoopTickOutcome::Succeeded, fx.clock->now);
    view = fx.sched->Find("loop-1", fx.clock->now);
    CHECK(view->task.state == LoopTaskState::Active);
}

TEST_CASE("ResolveTaskId:裸数字别名与原样") {
    Fixture fx;
    REQUIRE(fx.Create5m().ok);
    CHECK(fx.sched->ResolveTaskId("loop-1") == "loop-1");
    CHECK(fx.sched->ResolveTaskId("1") == "loop-1");
    CHECK(fx.sched->ResolveTaskId("99") == "99");    // 没有,原样交调用方报错
    CHECK(fx.sched->ResolveTaskId("all") == "all");  // all 是保留字
}

TEST_CASE("IdleWakeCoordinator:多路并存与 RAII 摘源") {
    IdleWakeCoordinator coord;
    CHECK_FALSE(coord.AnyReady());
    CHECK(coord.SourceNames().empty());

    bool subagent_ready = false;
    bool loop_ready = false;
    {
        auto token_a = coord.AddSource("subagent", [&subagent_ready]() { return subagent_ready; });
        auto token_b = coord.AddSource("loop", [&loop_ready]() { return loop_ready; });
        REQUIRE(coord.SourceNames().size() == 2);
        // 都不 ready。
        CHECK_FALSE(coord.AnyReady());
        // loop 到点:让位(不弄丢子代理源)。
        loop_ready = true;
        CHECK(coord.AnyReady());
        loop_ready = false;
        subagent_ready = true;
        CHECK(coord.AnyReady());
        subagent_ready = false;
        CHECK_FALSE(coord.AnyReady());
        // token 出作用域自动摘。
    }
    CHECK(coord.SourceNames().empty());
    // 源已摘:回调不再被调(ready 变 true 也无人问)。
    subagent_ready = true;
    CHECK_FALSE(coord.AnyReady());
    subagent_ready = false;
}

TEST_CASE("IdleWakeCoordinator:重名摘一枚,不误伤同名的") {
    IdleWakeCoordinator coord;
    bool a = false;
    bool b = false;
    auto token_a = coord.AddSource("dual", [&a]() { return a; });
    {
        auto token_b = coord.AddSource("dual", [&b]() { return b; });
        REQUIRE(coord.SourceNames().size() == 2);
        b = true;
        CHECK(coord.AnyReady());
        b = false;
    }  // 摘 b 那枚
    REQUIRE(coord.SourceNames().size() == 1);
    CHECK_FALSE(coord.AnyReady());
    a = true;
    CHECK(coord.AnyReady());  // a 那枚还活着
    a = false;
}
