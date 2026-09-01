// TurnBudgetAccount(turn 预算单 §13.2"纯函数与台账")的单测:准入状态机
// 的每一笔账——占额/发出/归还/完成四口,原子性、幂等性、终态拒绝、并发争
// 最后一枚。台账(TaskLedger)侧的同一台状态机走 Unlocked 系列,这里顺带
// 用台账口各钉一笔(冻结、终态拒绝、快照投影)。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "agent/turn_budget.hpp"
#include "tools/task_ledger.hpp"

using namespace lubancode;

TEST_CASE("TurnBudgetAccount:limit=2 连取两枚成功,第三枚稳定拒绝") {
    agent::TurnBudgetAccount account(2);
    auto first = account.TryReserveLock(true);
    REQUIRE(first.has_value());
    CHECK(account.CommitSentLock(*first).has_value());
    auto second = account.TryReserveLock(true);
    REQUIRE(second.has_value());
    CHECK(account.CommitSentLock(*second).has_value());
    // 第三枚:两条路都被拒——再占额与"终态后申请"同一个稳定码。
    const auto third = account.TryReserveLock(true);
    CHECK_FALSE(third.has_value());
    CHECK(third.error() == agent::kTurnBudgetDeniedExhausted);
    const agent::ModelTurnBudgetSnapshot snapshot = account.SnapshotLock();
    CHECK(snapshot.limit == 2);
    CHECK(snapshot.reserved == 0);
    CHECK(snapshot.attempted == 2);
    CHECK(snapshot.Exhausted());
}

TEST_CASE("TurnBudgetAccount:limit=0 连取不拒,task_turn_index 单调") {
    agent::TurnBudgetAccount account(0);
    int previous = 0;
    for (int i = 0; i < 5; ++i) {
        const auto permit = account.TryReserveLock(true);
        REQUIRE(permit.has_value());
        const auto index = account.CommitSentLock(*permit);
        REQUIRE(index.has_value());
        CHECK(*index == previous + 1);  // 从 1 起,单调
        previous = *index;
        CHECK_FALSE(account.SnapshotLock().Exhausted());
    }
}

TEST_CASE("TurnBudgetAccount:两线程争最后一枚,恰一枚成功,attempted 不超限") {
    agent::TurnBudgetAccount account(1);
    std::atomic<int> granted{0};
    std::atomic<int> denied{0};
    const auto race = [&]() {
        for (int i = 0; i < 64; ++i) {
            const auto permit = account.TryReserveLock(true);
            if (permit.has_value()) {
                // 拿到的那枚要翻成 attempted(占着不还,另一线程自然拿不到)。
                REQUIRE(account.CommitSentLock(*permit).has_value());
                granted.fetch_add(1);
            } else {
                denied.fetch_add(1);
            }
        }
    };
    std::thread a(race);
    std::thread b(race);
    a.join();
    b.join();
    CHECK(granted.load() == 1);
    CHECK(denied.load() == 127);
    CHECK(account.SnapshotLock().attempted == 1);  // 不超 limit
}

TEST_CASE("TurnBudgetAccount:reserve 后请求未发 abort,名额归还 reserved 回零") {
    agent::TurnBudgetAccount account(1);
    const auto permit = account.TryReserveLock(true);
    REQUIRE(permit.has_value());
    CHECK(account.SnapshotLock().reserved == 1);
    CHECK(account.AbortBeforeSendLock(*permit));
    const agent::ModelTurnBudgetSnapshot snapshot = account.SnapshotLock();
    CHECK(snapshot.reserved == 0);
    CHECK(snapshot.attempted == 0);  // abort 不碰 attempted
    // 名额还回来了:再取一枚应成功(同一枚限额)。
    const auto again = account.TryReserveLock(true);
    CHECK(again.has_value());
}

TEST_CASE("TurnBudgetAccount:同一 permit 不可 commit 两次;commit 后不可 abort") {
    agent::TurnBudgetAccount account(2);
    const auto permit = account.TryReserveLock(true);
    REQUIRE(permit.has_value());
    REQUIRE(account.CommitSentLock(*permit).has_value());
    // commit 第二次:稳定拒绝,attempted 不多加。
    const auto twice = account.CommitSentLock(*permit);
    CHECK_FALSE(twice.has_value());
    CHECK(twice.error() == agent::kTurnBudgetErrorStalePermit);
    CHECK(account.SnapshotLock().attempted == 1);
    // commit 之后 abort:稳定拒绝(请求已发,attempted 保留)。
    CHECK_FALSE(account.AbortBeforeSendLock(*permit));
    CHECK(account.SnapshotLock().attempted == 1);
    CHECK(account.SnapshotLock().reserved == 0);
}

TEST_CASE("TurnBudgetAccount:mark_completed 幂等,重复完成不加二次") {
    agent::TurnBudgetAccount account(3);
    const auto permit = account.TryReserveLock(true);
    REQUIRE(permit.has_value());
    REQUIRE(account.CommitSentLock(*permit).has_value());
    CHECK(account.MarkCompletedLock(*permit));
    CHECK(account.SnapshotLock().completed == 1);
    CHECK_FALSE(account.MarkCompletedLock(*permit));  // 重复:不加
    CHECK(account.SnapshotLock().completed == 1);
    // API 错误那枚(只 commit 没 complete)不占 completed:
    const auto failed = account.TryReserveLock(true);
    REQUIRE(failed.has_value());
    REQUIRE(account.CommitSentLock(*failed).has_value());
    const agent::ModelTurnBudgetSnapshot snapshot = account.SnapshotLock();
    CHECK(snapshot.attempted == 2);
    CHECK(snapshot.completed == 1);  // 失败请求可能花钱,attempted 保留,completed 不加
}

TEST_CASE("TurnBudgetAccount:任务不活态时占额稳定拒绝") {
    agent::TurnBudgetAccount account(5);
    const auto denied = account.TryReserveLock(false);
    CHECK_FALSE(denied.has_value());
    CHECK(denied.error() == agent::kTurnBudgetDeniedNotActive);
    CHECK(account.SnapshotLock().reserved == 0);
}

TEST_CASE("TurnBudgetAccount:将尽提示一任务恰一次(§9.2)") {
    agent::TurnBudgetAccount account(12);
    // remaining 从 12 往下走:12/11/.../4 都不触发,恰在 3(min(12,3))触发。
    auto step = [&account]() {
        const auto permit = account.TryReserveLock(true);
        REQUIRE(permit.has_value());
        REQUIRE(account.CommitSentLock(*permit).has_value());
        account.MarkCompletedLock(*permit);
    };
    CHECK_FALSE(account.ShouldNudgeOnceLock(3));
    for (int i = 0; i < 8; ++i) {
        step();
        CHECK_FALSE(account.ShouldNudgeOnceLock(3));
    }
    step();  // attempted=9,remaining=3:该提醒了
    CHECK(account.ShouldNudgeOnceLock(3));
    CHECK_FALSE(account.ShouldNudgeOnceLock(3));  // 只此一次,跨 Run 不重发
    step();
    step();
    step();
    CHECK_FALSE(account.ShouldNudgeOnceLock(3));
}

TEST_CASE("TurnBudgetAccount:limit=2 小预算第一步就提醒(min(limit,3)=2)") {
    agent::TurnBudgetAccount account(2);
    CHECK(account.ShouldNudgeOnceLock(3));
    CHECK_FALSE(account.ShouldNudgeOnceLock(3));
}

TEST_CASE("ShouldNudgeTurnLimit:纯函数与账户判定同一条(可单测)") {
    CHECK(agent::ShouldNudgeTurnLimit(12, 9, 3));
    CHECK_FALSE(agent::ShouldNudgeTurnLimit(12, 10, 3));
    CHECK(agent::ShouldNudgeTurnLimit(2, 0, 3));
    CHECK_FALSE(agent::ShouldNudgeTurnLimit(0, 100, 3));  // 不限:没有"将尽"
}

TEST_CASE("TaskLedger:turn_limit 随注册冻结,台账口驱动同一台状态机") {
    tools::TaskLedger ledger;
    tools::AgentTaskSnapshot snapshot;
    snapshot.turn_limit = 2;
    const auto task = ledger.Register(snapshot);
    CHECK(ledger.ModelTurnSnapshot(task).limit == 2);
    // 两枚照常走完,第三枚拒绝。
    for (int i = 0; i < 2; ++i) {
        const auto permit = ledger.TryReserveModelTurn(task);
        REQUIRE(permit.has_value());
        REQUIRE(ledger.CommitModelTurnSent(task, *permit).has_value());
        CHECK(ledger.MarkModelTurnCompleted(task, *permit));
    }
    CHECK_FALSE(ledger.TryReserveModelTurn(task).has_value());
    // 快照投影:Snapshots/Detail 读的是 turn_account 活账。
    const auto detail = ledger.Detail(task->snapshot.id);
    REQUIRE(detail.has_value());
    CHECK(detail->turn_limit == 2);
    CHECK(detail->turns_attempted == 2);
    CHECK(detail->turns_completed == 2);
    CHECK(detail->turns_reserved == 0);
}

TEST_CASE("TaskLedger:任务终态后再申请稳定拒绝,账面不动") {
    tools::TaskLedger ledger;
    tools::AgentTaskSnapshot snapshot;
    snapshot.turn_limit = 4;
    const auto task = ledger.Register(snapshot);
    // 直接翻终态(FinalizeFromToolResult 的常规路),再看占额。
    ledger.FinalizeFromToolResult(task, "done", false);
    const auto denied = ledger.TryReserveModelTurn(task);
    CHECK_FALSE(denied.has_value());
    CHECK(denied.error() == agent::kTurnBudgetDeniedNotActive);
    const agent::ModelTurnBudgetSnapshot account = ledger.ModelTurnSnapshot(task);
    CHECK(account.reserved == 0);
    CHECK(account.attempted == 0);
}
