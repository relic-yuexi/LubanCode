// 骨架拆解批五先行半批:预算闸与重试退避两只机制件。
//
// 机制钉死(不带各家语义——三外壳的语义各有自家测试钉):
//   - BudgetGate:三尺(次数/时长/token)、两种口径(Headroom 的
//     used>=limit 与 Overrun 的 used>limit 恰差一线)、合闸次序
//     count→elapsed→tokens、没账可对的尺跳过;
//   - StreakMeter:连撞 +1、成功/异因归零、到阈值 tripped;
//   - BackoffPolicy:Fixed/Exponential(翻倍 + 上限)/Ladder(越表即止),
//     抖动档默认关(开了改节奏,现状三家都没开);
//   - WaitBackoffCancellable:零等待直过、取消打断。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "runtime/budget_gate.hpp"
#include "runtime/retry_backoff.hpp"

using namespace lubancode::runtime;

// ---------------------------------------------------------------------------
// BudgetGate:口径与次序
// ---------------------------------------------------------------------------

TEST_CASE("Headroom 口径:used >= limit 即拦(下一轮装不下)") {
    BudgetGate gate(BudgetScales{
        .count = std::int64_t{3},
        .elapsed_ms = std::int64_t{1000},
        .tokens = std::int64_t{100},
    });
    // 全在帽下。
    CHECK(gate.CheckHeadroom(2, 999, 99) == BudgetStopReason::kNone);
    // 恰到帽:Headroom 拦(goal 的 iterations/elapsed/tokens、loop 的
    // expiry 边界都是这口径)。
    CHECK(gate.CheckHeadroom(3, 0, 0) == BudgetStopReason::kCount);
    CHECK(gate.CheckHeadroom(0, 1000, 0) == BudgetStopReason::kElapsed);
    CHECK(gate.CheckHeadroom(0, 0, 100) == BudgetStopReason::kTokens);
    // 越帽更拦。
    CHECK(gate.CheckHeadroom(4, 0, 0) == BudgetStopReason::kCount);
}

TEST_CASE("Overrun 口径:used > limit 才拦(已经越帽)") {
    BudgetGate gate(BudgetScales{
        .count = std::int64_t{3},
        .elapsed_ms = std::int64_t{1000},
        .tokens = std::int64_t{100},
    });
    // 恰到帽不拦(workflow 的 max_steps/tool_calls/tokens/timeout 是这口径)。
    CHECK(gate.CheckOverrun(3, 1000, 100) == BudgetStopReason::kNone);
    CHECK(gate.CheckOverrun(4, 0, 0) == BudgetStopReason::kCount);
    CHECK(gate.CheckOverrun(0, 1001, 0) == BudgetStopReason::kElapsed);
    CHECK(gate.CheckOverrun(0, 0, 101) == BudgetStopReason::kTokens);
}

TEST_CASE("合闸次序:count → elapsed → tokens,先拦先报") {
    BudgetGate gate(BudgetScales{
        .count = std::int64_t{1},
        .elapsed_ms = std::int64_t{1},
        .tokens = std::int64_t{1},
    });
    CHECK(gate.CheckHeadroom(1, 1, 1) == BudgetStopReason::kCount);
    CHECK(gate.CheckOverrun(2, 2, 2) == BudgetStopReason::kCount);
    // 单看后两把:elapsed 先于 tokens。
    BudgetGate back(BudgetScales{
        .elapsed_ms = std::int64_t{1},
        .tokens = std::int64_t{1},
    });
    CHECK(back.CheckHeadroom(0, 1, 1) == BudgetStopReason::kElapsed);
    CHECK(back.CheckOverrun(0, 2, 2) == BudgetStopReason::kElapsed);
}

TEST_CASE("没账可对的尺跳过:used 给 nullopt 不拦") {
    BudgetGate gate(BudgetScales{
        .count = std::int64_t{1},
        .elapsed_ms = std::int64_t{1},
        .tokens = std::int64_t{1},
    });
    // goal 的 usage_reported=false:token 尺没账,跳过;count 有账且到帽。
    CHECK(gate.CheckHeadroom(1, std::nullopt, std::nullopt) == BudgetStopReason::kCount);
    // 只剩 token 尺设限、没账:kNone(不拿 0 冒充)。
    BudgetGate tokens_only(BudgetScales{.tokens = std::int64_t{10}});
    CHECK(tokens_only.CheckHeadroom(std::nullopt, std::nullopt, std::nullopt) ==
          BudgetStopReason::kNone);
    // 全尺没设:怎么问都放行。
    BudgetGate bare;
    CHECK(bare.CheckHeadroom(999, 999, 999) == BudgetStopReason::kNone);
    CHECK(bare.CheckOverrun(999, 999, 999) == BudgetStopReason::kNone);
}

TEST_CASE("单尺口与合闸同判") {
    BudgetGate gate(BudgetScales{
        .count = std::int64_t{5},
        .elapsed_ms = std::int64_t{50},
        .tokens = std::int64_t{500},
    });
    CHECK(gate.HeadroomCount(5));
    CHECK_FALSE(gate.HeadroomCount(4));
    CHECK(gate.OverrunCount(6));
    CHECK_FALSE(gate.OverrunCount(5));
    CHECK(gate.HeadroomElapsed(50));
    CHECK_FALSE(gate.OverrunElapsed(50));
    CHECK(gate.OverrunElapsed(51));
    CHECK(gate.HeadroomTokens(500));
    CHECK(gate.OverrunTokens(501));
}

// ---------------------------------------------------------------------------
// StreakMeter:连撞计数
// ---------------------------------------------------------------------------

TEST_CASE("StreakMeter:连撞到阈值 tripped,成功归零") {
    StreakMeter meter{5, 0};
    for (int i = 1; i <= 4; ++i) {
        meter.NoteBad();
        CHECK_FALSE(meter.tripped());
        CHECK(meter.count == i);
    }
    meter.NoteBad();
    CHECK(meter.tripped());  // 五连败到顶(loop 的连败五拍)
    meter.NoteGood();
    CHECK(meter.count == 0);
    CHECK_FALSE(meter.tripped());
}

TEST_CASE("StreakMeter:阈值 0 不设;换因重数从 1 起") {
    StreakMeter off{0, 7};
    CHECK_FALSE(off.tripped());  // 不设限的连撞账只记账不拦
    StreakMeter blocker{3, 0};
    blocker.NoteGood();  // 换因:归零
    blocker.NoteBad();   // 新因第一撞
    CHECK(blocker.count == 1);  // goal 的"换 blocker 记 1"同一形状
}

// ---------------------------------------------------------------------------
// BackoffPolicy:阶梯三档
// ---------------------------------------------------------------------------

TEST_CASE("Exponential:initial * 2^(attempt-1),帽 max_ms") {
    BackoffPolicy policy;
    policy.kind = BackoffPolicy::Kind::Exponential;
    policy.initial_ms = 1000;
    policy.max_ms = 30000;
    CHECK(BackoffWaitMs(policy, 1)->count() == 1000);
    CHECK(BackoffWaitMs(policy, 2)->count() == 2000);
    CHECK(BackoffWaitMs(policy, 3)->count() == 4000);
    CHECK(BackoffWaitMs(policy, 6)->count() == 30000);  // 32000 撞帽
    CHECK(BackoffWaitMs(policy, 60)->count() == 30000);  // 翻到天也 30000
}

TEST_CASE("Fixed:每拍都等 initial_ms") {
    BackoffPolicy policy;
    policy.kind = BackoffPolicy::Kind::Fixed;
    policy.initial_ms = 500;
    CHECK(BackoffWaitMs(policy, 1)->count() == 500);
    CHECK(BackoffWaitMs(policy, 9)->count() == 500);
}

TEST_CASE("Ladder:逐拍对表,越表即止(loop 的 5s/15s)") {
    BackoffPolicy policy;
    policy.kind = BackoffPolicy::Kind::Ladder;
    policy.ladder_ms = {5000, 15000};
    REQUIRE(BackoffWaitMs(policy, 1).has_value());
    CHECK(BackoffWaitMs(policy, 1)->count() == 5000);
    REQUIRE(BackoffWaitMs(policy, 2).has_value());
    CHECK(BackoffWaitMs(policy, 2)->count() == 15000);
    CHECK_FALSE(BackoffWaitMs(policy, 3).has_value());  // 阶梯用完
}

TEST_CASE("抖动档:默认关,开档后仍在 ±25% 内") {
    BackoffPolicy policy;
    policy.kind = BackoffPolicy::Kind::Fixed;
    policy.initial_ms = 1000;
    for (int i = 0; i < 20; ++i) {
        REQUIRE(BackoffWaitMs(policy, 1).has_value());
        CHECK(BackoffWaitMs(policy, 1)->count() == 1000);  // 关档:恒定
    }
    policy.jitter = true;  // 开档:±250ms
    for (int i = 0; i < 50; ++i) {
        REQUIRE(BackoffWaitMs(policy, 1).has_value());
        const std::int64_t wait = BackoffWaitMs(policy, 1)->count();
        CHECK(wait >= 750);
        CHECK(wait <= 1250);
    }
}

// ---------------------------------------------------------------------------
// 可取消等待
// ---------------------------------------------------------------------------

TEST_CASE("WaitBackoffCancellable:零等待直过;取消打断;等满返回真") {
    CHECK(WaitBackoffCancellable(std::chrono::milliseconds(0), nullptr));
    std::atomic<bool> cancel{false};
    // 等一小段(不赌长时序):40ms 在 10ms 轮询的两拍内。
    CHECK(WaitBackoffCancellable(std::chrono::milliseconds(40), &cancel));
    cancel = true;
    CHECK_FALSE(WaitBackoffCancellable(std::chrono::milliseconds(0), &cancel));
    // 等待途中拉取消:立即断。
    std::atomic<bool> late{false};
    std::thread pull([&late]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        late = true;
    });
    CHECK_FALSE(WaitBackoffCancellable(std::chrono::milliseconds(5000), &late));
    pull.join();
}
