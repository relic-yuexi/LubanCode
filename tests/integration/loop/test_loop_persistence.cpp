// /loop 单第 3 期集成:scheduler 全生命周期的事件账落 SessionStore、
// 行级 JSON 回放重建、崩溃各点的账面对齐。真磁盘(临时目录),不发网络。

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sessions/session_store.hpp"
#include "api/types.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/loop_types.hpp"
#include "tools/path_utils.hpp"  // PathToUtf8

using lubancode::agent::ParseSessionFile;
using lubancode::agent::ReadSessionFileBytes;
using lubancode::agent::SessionMeta;
using lubancode::agent::SessionStore;
using lubancode::runtime::loop::LoopPromptSource;
using lubancode::runtime::loop::LoopScheduler;
using lubancode::runtime::loop::LoopSchedulerEvent;
using lubancode::runtime::loop::LoopTaskState;
using lubancode::runtime::loop::LoopTickOutcome;

namespace {

constexpr std::chrono::seconds k5m{300};

struct ManualClock : lubancode::runtime::loop::LoopClock {
    std::int64_t now = 1000000;
    std::int64_t NowWallMs() const override { return now; }
};

// 落盘架:真 SessionStore(临时目录)+ 攒账事件行同步写。
struct StoreFixture {
    std::filesystem::path dir;
    std::unique_ptr<SessionStore> store;
    std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
    std::unique_ptr<LoopScheduler> sched;

    StoreFixture() {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path(ec) /
              ("lubancode_loop_store_" + std::to_string(::rand()));
        std::filesystem::create_directories(dir, ec);
        store = std::make_unique<SessionStore>(lubancode::tools::PathToUtf8(dir));
        SessionMeta meta;
        meta.started_at = "2026-08-23 00:00:00";
        meta.cwd = lubancode::tools::PathToUtf8(dir);
        meta.model = "test";
        REQUIRE(store->Begin(meta, "test-session"));
        LoopScheduler::Options options;
        options.enabled = true;
        sched = std::make_unique<LoopScheduler>(options, clock);
    }

    ~StoreFixture() {
        store.reset();  // 先关文件柄,再删目录(雷:先关柄再删)。
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // 攒账事件 -> JSONL 行(与 session 侧 FlushLoopEvents 同形状)。
    void Flush() {
        for (const auto& e : sched->TakeEvents()) {
            nlohmann::json line;
            line["type"] = e.family;
            line["event"] = e.event;
            line["task_id"] = e.task_id;
            if (!e.tick_id.empty()) {
                line["tick_id"] = e.tick_id;
            }
            line["payload"] = e.payload;
            line["timestamp_ms"] = e.timestamp_ms;
            REQUIRE(store->AppendRawLine(line.dump()));
        }
    }

    // 从档回放:按行解析 loop 事件,喂回一只新 scheduler。
    std::unique_ptr<LoopScheduler> Restore() {
        const auto bytes = ReadSessionFileBytes(store->file_path());
        REQUIRE(bytes.has_value());
        LoopScheduler::Options options;
        options.enabled = true;
        auto restored = std::make_unique<LoopScheduler>(options, clock);
        std::istringstream stream(*bytes);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.find("\"loop_task_v1\"") == std::string::npos &&
                line.find("\"loop_tick_v1\"") == std::string::npos) {
                continue;
            }
            const nlohmann::json j = nlohmann::json::parse(line);
            LoopSchedulerEvent event;
            event.family = j.value("type", std::string());
            event.event = j.value("event", std::string());
            event.task_id = j.value("task_id", std::string());
            event.tick_id = j.value("tick_id", std::string());
            event.payload = j.value("payload", nlohmann::json::object());
            event.timestamp_ms = j.value("timestamp_ms", static_cast<std::int64_t>(0));
            REQUIRE(restored->ReplayEvent(event));
        }
        return restored;
    }
};

}  // namespace

TEST_CASE("全生命周期:created→due→started→finished 落盘,回放重建") {
    StoreFixture fx;
    auto created = fx.sched->Create("检查 CI", k5m, fx.clock->now, "D:/proj", "s1",
                                    LoopPromptSource::Inline, "", [] { return "loop-1"; });
    REQUIRE(created.ok);
    fx.Flush();
    // 到点、消费、收口。
    fx.clock->now += 300000;
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "turn-1");
    REQUIRE(tick.has_value());
    fx.Flush();
    fx.sched->FinishTick("loop-1#1", LoopTickOutcome::Succeeded, fx.clock->now);
    fx.Flush();

    // 回放:账面对齐(state Active、run_count、next_due)。
    auto restored = fx.Restore();
    const auto view = restored->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Active);
    CHECK(view->task.run_count == 1);
    CHECK(view->task.tick_seq == 1);
    CHECK(view->task.next_due_at_ms == tick->task.next_due_at_ms);
    // 回放后的账继续走:第二拍到点照开。
    fx.clock->now = view->task.next_due_at_ms;
    auto second = restored->PumpDueTick(fx.clock->now, "turn-2");
    REQUIRE(second.has_value());
    CHECK(second->tick.tick_no == 2);
}

TEST_CASE("paused 恢复:不复活不自动烧 token") {
    StoreFixture fx;
    REQUIRE(fx.sched->Create("盯部署", k5m, fx.clock->now, "D:/proj", "s1",
                            LoopPromptSource::Inline, "", [] { return "loop-1"; })
                .ok);
    REQUIRE(fx.sched->Pause("loop-1", fx.clock->now, "user").ok);
    fx.Flush();

    auto restored = fx.Restore();
    const auto view = restored->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Paused);
    // paused 不消费:快进到点也空。
    fx.clock->now += 3000000;
    CHECK(restored->PumpDueTick(fx.clock->now, "t") == std::nullopt);
    // resume 接得上:从 now+interval 起。
    REQUIRE(restored->Resume("loop-1", fx.clock->now).ok);
    const auto after = restored->Find("loop-1", fx.clock->now);
    CHECK(after->task.next_due_at_ms == fx.clock->now + 300000);
}

TEST_CASE("terminal 恢复:stopped/completed/expired 不复活") {
    StoreFixture fx;
    // 三只:停、完成、过期。
    REQUIRE(fx.sched->Create("甲", k5m, fx.clock->now, "D:/proj", "s1",
                            LoopPromptSource::Inline, "", [] { return "loop-1"; })
                .ok);
    REQUIRE(fx.sched->Create("乙", k5m, fx.clock->now, "D:/proj", "s1",
                            LoopPromptSource::Inline, "", [] { return "loop-2"; })
                .ok);
    REQUIRE(fx.sched->Create("丙", k5m, fx.clock->now, "D:/proj", "s1",
                            LoopPromptSource::Inline, "", [] { return "loop-3"; })
                .ok);
    REQUIRE(fx.sched->Stop("loop-1", fx.clock->now, "user").ok);
    fx.clock->now += 300000;
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "t1");
    REQUIRE(tick.has_value());
    REQUIRE(fx.sched->Complete("loop-2", fx.clock->now, "部署完成").ok);
    fx.clock->now += 604800000;  // 7d:丙过期
    fx.sched->SweepExpiry(fx.clock->now);
    fx.Flush();

    auto restored = fx.Restore();
    CHECK(restored->Find("loop-1", fx.clock->now)->task.state == LoopTaskState::Cancelled);
    CHECK(restored->Find("loop-2", fx.clock->now)->task.state == LoopTaskState::Completed);
    CHECK(restored->Find("loop-3", fx.clock->now)->task.state == LoopTaskState::Expired);
    // 终态上一律不消费。
    CHECK(restored->PumpDueTick(fx.clock->now, "t") == std::nullopt);
}

TEST_CASE("崩在 started 与 finished 之间:恢复标停,不重复发 prompt") {
    StoreFixture fx;
    REQUIRE(fx.sched->Create("盯 CI", k5m, fx.clock->now, "D:/proj", "s1",
                            LoopPromptSource::Inline, "", [] { return "loop-1"; })
                .ok);
    fx.Flush();
    fx.clock->now += 300000;
    auto tick = fx.sched->PumpDueTick(fx.clock->now, "turn-1");
    REQUIRE(tick.has_value());
    fx.Flush();
    // 崩在这:turn 没收口。恢复侧见 started 无 finished——loop 的保守账:
    // Running 恢复成 Paused(session 侧 RestoreLoopFromArchive 的默认暂停
    // 路),不自动重发同一枚 prompt。

    auto restored = fx.Restore();
    const auto view = restored->Find("loop-1", fx.clock->now);
    REQUIRE(view.has_value());
    CHECK(view->task.state == LoopTaskState::Running);  // 账面如实在
    // session 装配层的默认暂停路在这照一遍:Pause 后不再消费。
    REQUIRE(restored->Pause("loop-1", fx.clock->now, "resumed_paused").ok);
    CHECK(restored->PumpDueTick(fx.clock->now + 300000, "t2") == std::nullopt);
    // 收口那枚迟到的 tick:恢复侧的账不带 current_tick,FinishTick 留账
    // 不动状态(迟到收口,不崩)。
    restored->FinishTick(tick->tick.tick_id, LoopTickOutcome::UnknownAfterStart, fx.clock->now,
                         "crash_recovery");
}

TEST_CASE("写盘失败熔断:AppendRawLine 拒后不再开新拍") {
    StoreFixture fx;
    REQUIRE(fx.sched->Create("盯 CI", k5m, fx.clock->now, "D:/proj", "s1",
                            LoopPromptSource::Inline, "", [] { return "loop-1"; })
                .ok);
    fx.Flush();
    // 模拟存档写盘失败:store 关柄后 AppendRawLine 返 false——session 侧
    // FlushLoopEvents 见 false 调 FailStore。这里直接调。
    fx.sched->FailStore("disk full");
    fx.clock->now += 300000;
    CHECK_FALSE(fx.sched->HasDueWork(fx.clock->now));
    CHECK(fx.sched->PumpDueTick(fx.clock->now, "t") == std::nullopt);
    // 建新任务也拒。
    CHECK_FALSE(fx.sched
                    ->Create("再建一只", k5m, fx.clock->now, "D:/proj", "s1",
                             LoopPromptSource::Inline, "", [] { return "loop-2"; })
                    .ok);
}

TEST_CASE("老 session 无 loop 事件:回放零条,不炸") {
    StoreFixture fx;
    // 只落一条普通消息行,没有 loop 事件。
    lubancode::api::Message msg;
    msg.role = lubancode::api::Role::User;
    msg.content.push_back(lubancode::api::TextBlock{"你好"});
    REQUIRE(fx.store->AppendMessage(msg));
    // 回放:Restore 里循环一行都没喂,零任务,不炸。
    auto restored = fx.Restore();
    CHECK(restored->Snapshot(fx.clock->now).empty());
}
