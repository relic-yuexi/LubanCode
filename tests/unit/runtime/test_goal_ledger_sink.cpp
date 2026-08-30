// MakeSessionLedgerSink 单测(骨架拆解反弹·问题 3):goal 事件账的存档
// sink 搭建自 app/wirings/goal_session_wiring 的 Ensure 抽成 runtime 纯函数
// 后,不必起接线器就能钉住——type 分族(iteration 级 goal_iteration_v1,
// 其余 goal_v1)、字段搬运、没建档的会话返回 true。

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/goal_coordinator.hpp"
#include "sessions/session_store.hpp"
#include "tools/path_utils.hpp"  // PathToUtf8

using lubancode::runtime::goal::GoalCoordinatorEvent;
using lubancode::sessions::GoalSessionEvent;
using lubancode::sessions::SessionStore;

namespace {

struct StoreFixture {
    std::filesystem::path dir;
    std::unique_ptr<SessionStore> store;

    StoreFixture() {
        std::error_code ec;
        dir = std::filesystem::temp_directory_path() /
              ("lubancode_goal_sink_" + std::to_string(::rand()));
        std::filesystem::create_directories(dir, ec);
        store = std::make_unique<SessionStore>(lubancode::tools::PathToUtf8(dir));
        lubancode::sessions::SessionMeta meta;
        meta.started_at = "2026-08-30 00:00:00";
        meta.cwd = lubancode::tools::PathToUtf8(dir);
        meta.model = "test";
        REQUIRE(store->Begin(meta, "test-session"));
    }

    ~StoreFixture() {
        store.reset();  // 先关文件柄,再删目录。
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

GoalCoordinatorEvent MakeEvent(const std::string& event, const std::string& goal_id,
                               const std::string& iteration_id, int revision) {
    GoalCoordinatorEvent e;
    e.event = event;
    e.goal_id = goal_id;
    e.iteration_id = iteration_id;
    e.revision = revision;
    e.payload = nlohmann::json{{"note", "x"}};
    e.timestamp_ms = 12345;
    return e;
}

}  // namespace

TEST_CASE("type 分族:iteration 级走 goal_iteration_v1,其余 goal_v1") {
    StoreFixture fx;
    auto sink = lubancode::runtime::goal::MakeSessionLedgerSink(*fx.store);

    CHECK(sink(MakeEvent("started", "goal-1", "goal-1/iter-1", 3)));
    CHECK(sink(MakeEvent("created", "goal-1", "", 1)));
    CHECK(sink(MakeEvent("paused", "goal-1", "", 2)));

    const auto bytes = lubancode::sessions::ReadSessionFileBytes(fx.store->file_path());
    REQUIRE(bytes.has_value());
    const auto loaded = lubancode::sessions::ParseSessionFile(*bytes);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->goal_events.size() == 3);

    const GoalSessionEvent& iteration_line = loaded->goal_events[0];
    CHECK(iteration_line.type == "goal_iteration_v1");
    CHECK(iteration_line.event == "started");
    CHECK(iteration_line.goal_id == "goal-1");
    CHECK(iteration_line.iteration_id == "goal-1/iter-1");
    CHECK(iteration_line.revision == 3);
    CHECK(iteration_line.payload.value("note", std::string()) == "x");
    CHECK(iteration_line.timestamp_ms == 12345);

    CHECK(loaded->goal_events[1].type == "goal_v1");
    CHECK(loaded->goal_events[1].event == "created");
    CHECK(loaded->goal_events[1].iteration_id.empty());
    CHECK(loaded->goal_events[2].type == "goal_v1");
}

TEST_CASE("store 没开张:返回 true,事件只进内存不落盘") {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("lubancode_goal_sink_idle_" + std::to_string(::rand()));
    std::filesystem::create_directories(dir, ec);
    SessionStore idle_store(lubancode::tools::PathToUtf8(dir));
    REQUIRE_FALSE(idle_store.active());
    auto sink = lubancode::runtime::goal::MakeSessionLedgerSink(idle_store);
    // 没建档的会话照常吃命令:事件吞进内存语义(不报错、不拦状态机)。
    CHECK(sink(MakeEvent("created", "goal-9", "", 1)));
    std::filesystem::remove_all(dir, ec);
}
