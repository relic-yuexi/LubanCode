// LoopTickDriver 单测(骨架拆解反弹·问题 3):单拍执行的调度状态机自
// app/wirings/loop_session_wiring 下沉后,不必起整个会话就能钉住——
// scheduled message 拼装、prompt 源失源停账、loop_control 声明消费、
// 迟到收口拒账、审批旁听路由。全走注入 fake clock 与回调录音,零终端。

#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "runtime/loop_scheduler.hpp"
#include "runtime/loop_tick_driver.hpp"
#include "runtime/loop_types.hpp"

using lubancode::runtime::loop::LoopClock;
using lubancode::runtime::loop::LoopPromptSource;
using lubancode::runtime::loop::LoopScheduler;
using lubancode::runtime::loop::LoopTickDriver;
using lubancode::runtime::loop::LoopTickNotice;
using lubancode::runtime::loop::LoopTaskState;

namespace {

constexpr std::chrono::seconds k1m{60};
std::int64_t Ms(std::chrono::seconds s) { return s.count() * 1000; }

struct ManualClock : LoopClock {
    std::int64_t now = Ms(std::chrono::seconds(1000));
    std::int64_t NowWallMs() const override { return now; }
};

struct Fixture {
    std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
    std::unique_ptr<LoopScheduler> sched =
        std::make_unique<LoopScheduler>(LoopScheduler::Options{}, clock);
    std::shared_ptr<lubancode::tools::LoopControlState> control =
        std::make_shared<lubancode::tools::LoopControlState>();

    // 回调录音:start_turn 收的 message、notify 收的通知、flush 次数、
    // read_prompt 的应答。
    std::vector<std::string> messages;
    std::vector<LoopTickNotice> notices;
    int flushes = 0;
    LoopTickDriver::PromptSourceRead prompt_read;
    bool turn_failed = false;

    std::unique_ptr<LoopTickDriver> driver;

    Fixture() { prompt_read.text = "loop.md 现行正文"; }

    void EnsureDriver() {
        LoopTickDriver::Host host;
        host.scheduler = sched.get();
        host.read_prompt = [this]() { return prompt_read; };
        host.flush_events = [this]() { ++flushes; };
        host.start_turn = [this](const std::string& message, bool* failed) {
            messages.push_back(message);
            OnTurnRun();
            *failed = turn_failed;
        };
        host.notify = [this](const LoopTickNotice& notice) { notices.push_back(notice); };
        host.control_state = control;
        // 墙钟注入:与 scheduler 同一只 ManualClock——收口时刻不比建账
        // 时刻"晚七天",免得任务被误判 Expired。
        host.now = [this]() { return clock->now; };
        driver = std::make_unique<LoopTickDriver>(std::move(host));
    }

    // 开轮瞬间的钩子:子类场景在这儿立 loop_control 的旗。
    virtual void OnTurnRun() {}

    auto CreateInline(const std::string& prompt = "检查 CI") {
        int next_id = 0;
        return sched->Create(prompt, k1m, clock->now, "D:/proj", "s1", LoopPromptSource::Inline, "",
                             [&next_id]() { return "loop-" + std::to_string(++next_id); });
    }

    auto CreateProjectFile() {
        int next_id = 0;
        return sched->Create("", k1m, clock->now, "D:/proj", "s1", LoopPromptSource::ProjectFile,
                             "loop.md", [&next_id]() { return "loop-" + std::to_string(++next_id); });
    }
};

}  // namespace

TEST_CASE("单拍:scheduled message 拼装 + 收口 Succeeded + 通知文案") {
    Fixture fx;
    REQUIRE(fx.CreateInline("检查 CI").ok);
    fx.EnsureDriver();
    fx.clock->now += Ms(k1m);

    REQUIRE(fx.driver->PumpDueTick(fx.clock->now));
    REQUIRE(fx.messages.size() == 1);
    // scheduled message 的原文(模型须知道来源与时间,不伪装成用户正文)。
    CHECK(fx.messages[0].starts_with("[定时循环 tick]\ntask_id: loop-1\ntick: 1\n"));
    CHECK(fx.messages[0].find("原始任务:\n检查 CI") != std::string::npos);
    // 开拍通知(Info,纯文案不带色)。
    REQUIRE(fx.notices.size() == 1);
    CHECK(fx.notices[0].kind == LoopTickNotice::Kind::Info);
    CHECK(fx.notices[0].text == "[loop loop-1 第 1 拍]");
    // 事件账 flush:取件后一次、收口后一次。
    CHECK(fx.flushes >= 2);
    // 拍收口 Succeeded:下一拍排上,task 回 Active。
    const auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Active);
    CHECK(fx.driver->TickActive() == false);
}

TEST_CASE("失源:ProjectFile 解析不回来 -> 本拍 PromptSourceMissing,任务停") {
    Fixture fx;
    REQUIRE(fx.CreateProjectFile().ok);
    // 解析回来的是 Builtin(源没了),且无 error 文案——按"loop.md 没了"收。
    fx.prompt_read.source = LoopPromptSource::Builtin;
    fx.prompt_read.error = "";
    fx.EnsureDriver();
    fx.clock->now += Ms(k1m);

    REQUIRE(fx.driver->PumpDueTick(fx.clock->now));
    CHECK(fx.messages.empty());  // 没开 turn
    REQUIRE(fx.notices.size() == 1);
    CHECK(fx.notices[0].kind == LoopTickNotice::Kind::Error);
    CHECK(fx.notices[0].text == "loop loop-1 的 prompt 源读失败,任务已停: loop.md 没了");
    // 收口序:FinishTick(PromptSourceMissing) 回 Active,随后的 Stop 落
    // 终态——搬家前就是这个次序,这里钉的是搬家后一字不差。
    const auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Cancelled);
    CHECK(IsLoopTerminal(view->task.state));
}

TEST_CASE("失源(读报错):error 文案原样带进通知") {
    Fixture fx;
    REQUIRE(fx.CreateProjectFile().ok);
    fx.prompt_read.source = LoopPromptSource::ProjectFile;
    fx.prompt_read.error = "权限不足";
    fx.EnsureDriver();
    fx.clock->now += Ms(k1m);

    REQUIRE(fx.driver->PumpDueTick(fx.clock->now));
    REQUIRE(fx.notices.size() == 1);
    CHECK(fx.notices[0].text == "loop loop-1 的 prompt 源读失败,任务已停: 权限不足");
}

TEST_CASE("loop_control 声明 complete:任务落终态,下一拍不再排") {
    struct CompleteFixture : Fixture {
        void OnTurnRun() override {
            // 模型在 turn 里调了 loop_control(complete):工具只立旗,
            // 收口时由状态机消费。
            control->complete_requested = true;
        }
    };
    CompleteFixture fx;
    REQUIRE(fx.CreateInline().ok);
    fx.EnsureDriver();
    fx.clock->now += Ms(k1m);

    REQUIRE(fx.driver->PumpDueTick(fx.clock->now));
    const auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Completed);
    REQUIRE(fx.notices.size() == 2);  // 开拍 + 完成告示
    CHECK(fx.notices[1].text == "loop loop-1:模型声明完成,任务落终态(下一拍不再排)。");
    // 收口后 scope 清零:迟到的工具调用立即明拒。
    CHECK(fx.control->task_id.empty());
    CHECK_FALSE(fx.control->complete_requested);
}

TEST_CASE("loop_control 声明 pause:任务落 Paused,通知带续跑指引") {
    struct PauseFixture : Fixture {
        void OnTurnRun() override { control->pause_requested = true; }
    };
    PauseFixture fx;
    REQUIRE(fx.CreateInline().ok);
    fx.EnsureDriver();
    fx.clock->now += Ms(k1m);

    REQUIRE(fx.driver->PumpDueTick(fx.clock->now));
    const auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Paused);
    REQUIRE(fx.notices.size() == 2);
    CHECK(fx.notices[1].text == "loop loop-1:模型请求暂停(需要用户处理);续跑 /loop resume。");
}

TEST_CASE("turn 失败:本拍 ProviderError,task 仍活(退避账在 scheduler)") {
    Fixture fx;
    REQUIRE(fx.CreateInline().ok);
    fx.turn_failed = true;
    fx.EnsureDriver();
    fx.clock->now += Ms(k1m);

    REQUIRE(fx.driver->PumpDueTick(fx.clock->now));
    const auto view = fx.sched->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Active);
}

TEST_CASE("迟到收口:tick 号对不上时留账不动") {
    Fixture fx;
    REQUIRE(fx.CreateInline().ok);
    fx.EnsureDriver();
    fx.driver->FinishTick("不存在的-tick", /*turn_failed=*/true, /*cancelled=*/false);
    CHECK(fx.flushes == 0);  // 没碰 scheduler,没 flush
}

TEST_CASE("审批旁听:只在 loop 拍的 turn 里记账") {
    Fixture fx;
    REQUIRE(fx.CreateInline().ok);
    fx.EnsureDriver();
    // 不在拍上:旁听被拒。
    fx.driver->NotePermissionWait(/*asked=*/true, /*allowed=*/false);
    CHECK(fx.flushes == 0);
}
