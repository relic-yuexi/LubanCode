// AR-02(2026-09-21 架构审查)的合同册:监督线程与健康钩子派发线程
// 超时脱离(detach)后不得再访问宿主对象。旧病灶:两类线程都捕获
// [this],宿主析构有界等待(1s/1.5s)超时即 detach 放行;慢回调返回后
// 线程仍要写宿主的计数成员并重锁宿主的 mutex——对象已释放,UB。
//
// 修法合同(单子 AR-02):
//   - 线程只捕共享状态(shared_ptr 保命),永不捕宿主 this;
//   - 宿主析构断开台账访问后请求停止;跨截止时间存活的线程只摸共享状态;
//   - 停止后新 Publish 拒收并计入 dropped_events(账要看得见);
//   - 批内剩余事件照派发完(收线 = 跑完手头一批,原合同不松);
//   - 订阅重入/抛异常不锁死。
//
// 夹具纪律:交错用门闩(entered/gate_open)或"盖住一个监督周期"的
// 物理等待钉成确定次序,不赌线程调度;哨兵(lifetime_token_for_test)
// 过期 = 专职线程已退出、共享状态已析构——此后进程再无任何摸该状态
// 的代码。轮询等待沿 WaitForReceived 先例:死线内等条件,超时明败
// 不挂死。
//
// ASan 说明:CI 三腿是 Release,本册钉的是合同行为(哨兵、计数、不
// 派发已拒收事件);"释放后不再访问"由结构保证——线程 lambda 只捕
// shared_ptr,宿主指针不出现在线程世界。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "agent/agent_progress.hpp"
#include "runtime/agent_health_hooks.hpp"
#include "runtime/agent_supervisor.hpp"
#include "tools/task_ledger.hpp"

using namespace lubancode;

namespace {

using Clock = std::chrono::steady_clock;

std::shared_ptr<tools::TaskRecord> MakeTask(tools::TaskLedger& ledger, const std::string& title) {
    tools::AgentTaskSnapshot snapshot;
    snapshot.title = title;
    snapshot.prompt = "test";
    snapshot.start_time = Clock::now();
    return ledger.Register(std::move(snapshot));
}

agent::AgentSupervisionEvent MakeEvent(const std::string& reason) {
    agent::AgentSupervisionEvent event;
    event.kind = agent::AgentSupervisionEventKind::RecoveryStarted;
    event.task_id = 7;
    event.reason_code = reason;
    return event;
}

agent::SupervisionThresholds FastThresholds() {
    agent::SupervisionThresholds thresholds;
    thresholds.first_byte_soft_secs = 1;
    thresholds.streaming_soft_secs = 1;
    thresholds.tool_soft_secs = 1;
    thresholds.exec_idle_soft_secs = 1;
    thresholds.stale_notice_rounds = 2;
    thresholds.stale_fail_rounds = 3;
    return thresholds;
}

// 门闩:回调进门置 entered,随后原地等 gate_open——把"派发线程正在回调
// 里"钉成测试可见的确定交错。
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool open = false;

    void EnterAndWait() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            entered = true;
        }
        cv.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return open; });
    }
    void WaitEntered() {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(5), [this] { return entered; }));
    }
    void Open() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
};

// 哨兵过期等待:死线内等共享状态析构(专职线程退出、最后一个引用归零),
// 超时明败。按值收:weak_ptr 拷贝只动控制块计数,调用侧好传 const。
bool WaitTokenExpired(std::weak_ptr<const void> token) {
    for (int i = 0; i < 500; ++i) {
        if (token.expired()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return token.expired();
}

}  // namespace

TEST_CASE("钩子总线:长回调跨析构截止时间后返回,脱离线程只摸共享状态") {
    auto bus = std::make_unique<runtime::AgentHealthHookBus>();
    const auto token = bus->lifetime_token_for_test();
    Gate gate;
    std::atomic<int> hits{0};

    bus->Subscribe([&](const agent::AgentSupervisionEvent&) {
        if (hits.fetch_add(1, std::memory_order_acq_rel) == 0) {
            gate.EnterAndWait();  // 第一枚进门就闩上:派发线程持批卡在回调里
        }
    });
    bus->Publish(MakeEvent("first"));
    bus->Publish(MakeEvent("second"));
    gate.WaitEntered();  // 派发线程已吃进这批、正卡在门闩上

    // 析构在另一根线程上跑:门闩不开,派发线程跨过 1s 截止时间,
    // 析构必走 detach 分支放行——宿主就此释放,旧代码此刻已埋下 UB。
    std::thread destroyer([&] { bus.reset(); });
    destroyer.join();
    CHECK_FALSE(token.expired());  // 脱离线程还活着(只被共享状态保命)

    gate.Open();  // 放开长回调:线程回来只摸共享状态,跑完手头一批再退
    REQUIRE(WaitTokenExpired(token));
    CHECK(hits.load() == 2);  // 批内剩余照派发(收线 = 跑完手头一批)
}

TEST_CASE("钩子总线:停止后拒收新 Publish,拒收计入 dropped 账") {
    runtime::AgentHealthHookBus bus;
    std::atomic<int> hits{0};
    bus.Subscribe([&](const agent::AgentSupervisionEvent&) { hits.fetch_add(1); });

    bus.RequestStop();
    bus.Publish(MakeEvent("after-stop"));
    bus.DrainForTest();
    CHECK(bus.dropped_events() == 1);    // 拒收看得见账
    CHECK(bus.delivered_events() == 0);  // 没人收到
    CHECK(hits.load() == 0);             // 停止后不再起派发线程跑它
}

TEST_CASE("钩子总线:订阅重入不锁死,重入事件照常派发") {
    runtime::AgentHealthHookBus bus;
    std::atomic<int> hits{0};
    std::atomic<int> reentered{0};

    // 重入:回调里再 Publish。回调在锁外跑,入队只拿共享状态的小锁,
    // 不许锁死。深度门:重入回调里不再 Publish,防发散。
    bus.Subscribe([&](const agent::AgentSupervisionEvent&) {
        if (hits.fetch_add(1, std::memory_order_acq_rel) == 0) {
            reentered.fetch_add(1);
            bus.Publish(MakeEvent("reentrant"));
        }
    });
    bus.Publish(MakeEvent("seed"));
    bool done = false;
    for (int i = 0; i < 300; ++i) {
        if (hits.load(std::memory_order_acquire) >= 2) {
            done = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(done);  // 种子 + 重入两枚死线内都送达,没锁死
    CHECK(reentered.load() == 1);
}

TEST_CASE("监督器:健康拍要摸台账时宿主析构,脱离线程只摸共享状态") {
    tools::TaskLedger ledger;
    auto supervisor = std::make_unique<runtime::AgentSupervisor>(ledger);
    supervisor->SetThresholds(FastThresholds());
    const auto token = supervisor->lifetime_token_for_test();
    const auto task = MakeTask(ledger, "跨析构的健康拍");
    supervisor->WatchTask(task);
    supervisor->ArmWallClock(task, 1, 30);  // 近期限:让监督线程的循环留痕

    // 证据链第一步:期限落过锤(wall_clock_fired),监督线程确实在跑。
    bool fired = false;
    for (int i = 0; i < 500; ++i) {
        if (task->wall_clock_fired.load(std::memory_order_acquire)) {
            fired = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(fired);

    // 拿住台账锁,再盖一个监督周期(拍间隔 500ms,等 700ms):监督线程
    // 醒来必进健康拍、等在这把锁上。这不是赌调度,是周期被等待全盖。
    {
        std::unique_lock<std::mutex> ledger_lock(ledger.mutex);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));

        // 宿主析构:拔 sink(监督线程不在台账锁内,绝不正在 sink 里,无
        // 并发)-> 请求停止 -> 1.5s 截止时间。监督线程等在台账锁上,
        // notify 叫不醒,截止必超时 -> detach 放行。旧代码从此刻起,
        // 线程醒来后的每一步(重锁 mutex_、摸 ledger_、写 thread_exited_)
        // 全是摸已释放宿主的 UB。
        supervisor.reset();
        CHECK_FALSE(token.expired());  // 脱离的监督线程还活着

    }  // 放台账锁:监督线程跑完这一拍(摸台账+共享状态,都活着),
       // 回环见停止/断连即退,只摸共享状态。
    REQUIRE(WaitTokenExpired(token));

    // 台账自始至终没被释放(声明序在前),账面照常可读——"监督线程
    // 退出前 ledger 不释放"的正向面。
    CHECK_FALSE(ledger.TaskSettled(task->snapshot.id));
}

TEST_CASE("监督器:空闲收线走 join,析构返回即线程世界全退") {
    tools::TaskLedger ledger;
    std::weak_ptr<const void> token;
    {
        runtime::AgentSupervisor supervisor(ledger);
        supervisor.SetThresholds(FastThresholds());
        token = supervisor.lifetime_token_for_test();
        const auto task = MakeTask(ledger, "空闲收线");
        supervisor.WatchTask(task);
        CHECK(supervisor.supervisor_thread_count_for_test() == 1);
    }  // 空闲监督线程:notify 即退,有界窗内 join
    REQUIRE(WaitTokenExpired(token));
}

// LocalVariables:
// fill-column: 100
// End:
