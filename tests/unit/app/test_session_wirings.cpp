// 子系统接线器单测(会话终章):goal/loop/plan/peer/录制五只接线器的
// "装配可独立起"钉子——离开会话控制器,单靠 Host 材料就能装配、泵空转
// 安全、存档恢复空档不炸。行为面的对账在既有各域测试(goal_coordinator/
// loop_scheduler/plan_mode/peer_*)里,这里只钉装配边界。
#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/wirings/goal_session_wiring.hpp"
#include "app/wirings/loop_session_wiring.hpp"
#include "app/wirings/peer_session_wiring.hpp"
#include "app/wirings/plan_session_wiring.hpp"
#include "app/wirings/record_session_wiring.hpp"
#include "cli/theme.hpp"
#include "config/config.hpp"
#include "runtime/idle_wake.hpp"
#include "runtime/session_runtime.hpp"
#include "sessions/session_store.hpp"

namespace lubancode::app {
namespace {

// 临时目录兜底:接线器不落盘,这些测试全是内存路;工作目录挪开只是保险。
struct TempCwd {
    std::filesystem::path old;
    TempCwd() {
        std::error_code ec;
        old = std::filesystem::current_path();
        const auto temp = std::filesystem::temp_directory_path(ec) / "lubancode_wiring_test";
        std::filesystem::create_directories(temp, ec);
        std::filesystem::current_path(temp, ec);
    }
    ~TempCwd() {
        std::error_code ec;
        std::filesystem::current_path(old, ec);
    }
};

GoalSessionWiring::Host MakeGoalHost(lubancode::runtime::SessionRuntime& runtime,
                                     std::string* evaluation_model) {
    GoalSessionWiring::Host host;
    static lubancode::cli::Theme theme;
    host.theme = &theme;
    static lubancode::config::Config config;
    host.config = &config;
    host.session_store = &runtime.store();
    host.current_model = std::make_shared<std::string>("test-model");
    evaluation_model = nullptr;
    host.start_turn = [](const std::string&, bool* failed) {
        if (failed != nullptr) {
            *failed = false;
        }
    };
    return host;
}

}  // namespace

TEST_CASE("goal 接线器:装配独立起,泵与收口空转安全") {
    TempCwd cwd_guard;
    lubancode::runtime::SessionRuntime runtime({std::string(), "anthropic", "test"});
    GoalSessionWiring wiring(MakeGoalHost(runtime, nullptr));

    // 装配前:coordinator 空、iteration 不活跃,收口路原样返回。
    REQUIRE(wiring.coordinator() == nullptr);
    CHECK_FALSE(wiring.HasActiveIteration());
    wiring.CloseIteration("turn-x", /*turn_failed=*/false);  // 不炸即过
    // 没活跃 goal:补快照是空操作(Ensure 在内部幂等补)。
    lubancode::sessions::CompactV2Event compact_event;
    wiring.AttachSnapshotToCompact(compact_event);
    CHECK_FALSE(compact_event.metrics.contains("goal"));

    // 装配:coordinator 安家,Ensure 幂等。
    lubancode::config::Config config;
    wiring.Ensure(config);
    CHECK(wiring.coordinator() != nullptr);
    wiring.Ensure(config);
    CHECK(wiring.coordinator() != nullptr);

    // 命令材料包:coordinator 挂上,agent/loop 侧可空。
    lubancode::app::GoalWiring pack = wiring.MakeCommandWiring(nullptr, nullptr);
    CHECK(pack.coordinator == wiring.coordinator());
    CHECK(pack.agent_tool == nullptr);
    CHECK(pack.loop_scheduler == nullptr);

    // 泵:没有 ready continuation,原样返回。
    wiring.PumpContinuation(0);
    CHECK_FALSE(wiring.HasActiveIteration());
    // 存档恢复:没建档的会话安静退。
    wiring.RestoreFromArchive();
}

TEST_CASE("loop 接线器:装配独立起(timer 起/收),泵无活空转") {
    TempCwd cwd_guard;
    lubancode::runtime::SessionRuntime runtime({std::string(), "anthropic", "test"});
    lubancode::runtime::IdleWakeCoordinator wakes;
    lubancode::config::Config config;

    LoopSessionWiring::Host host;
    static lubancode::cli::Theme theme;
    host.theme = &theme;
    host.interactive = false;
    host.config = &config;
    host.session_store = &runtime.store();
    host.session_runtime = &runtime;
    static std::optional<std::string> home;
    host.home_lubancode = &home;
    host.idle_wakes = &wakes;
    host.start_turn = [](const std::string&, bool* failed) {
        if (failed != nullptr) {
            *failed = true;
        }
    };
    LoopSessionWiring wiring(host);

    REQUIRE(wiring.scheduler() == nullptr);
    CHECK_FALSE(wiring.TickActive());
    wiring.Shutdown();  // 没装也能收(幂等)

    wiring.Ensure();
    CHECK(wiring.scheduler() != nullptr);
    wiring.Ensure();  // 幂等
    CHECK(wiring.scheduler() != nullptr);
    CHECK_FALSE(wiring.HasActiveTasks());
    CHECK_FALSE(wiring.SweepAndCheckDue(0));
    CHECK_FALSE(wiring.PumpDueTick(0));  // 没 due 拍:落一笔事件账后 false
    wiring.NotePermissionWait(/*asked=*/true, /*allowed=*/false);  // 不在拍里:零影响
    CHECK(wiring.StopAllForEsc() == 0);

    lubancode::app::LoopWiring pack = wiring.MakeCommandWiring();
    CHECK(pack.scheduler == wiring.scheduler());
    CHECK_FALSE(pack.interactive);

    wiring.Shutdown();
    CHECK(wiring.scheduler() != nullptr);  // scheduler 仍在(状态不销),timer 已停
}

TEST_CASE("plan 接线器:装配独立起,Default 档一概放行,恢复空档落 Default") {
    TempCwd cwd_guard;
    lubancode::runtime::SessionRuntime runtime({std::string(), "anthropic", "test"});
    lubancode::agent::PromptOptions prompt_options;

    PlanSessionWiring::Host host;
    static lubancode::cli::Theme theme;
    host.theme = &theme;
    host.session_runtime = &runtime;
    host.prompt_options = &prompt_options;
    host.registry = [] { return nullptr; };
    host.rebuild_preserving = [] {};
    host.start_turn = [](const std::string&, bool*) {};
    PlanSessionWiring wiring(host);

    // Default 档:闸全放行(不摸注册表)。
    const nlohmann::json input{{"command", "rm -rf /"}};
    CHECK(wiring.EvaluateGate("run_command", input).empty());
    CHECK(wiring.EvaluateGate("write_file", nlohmann::json{}).empty());
    // 老档没 mode 行:恢复落 Default,悬稿空。
    wiring.RestoreFromArchive(std::nullopt, {}, std::nullopt);
    CHECK(runtime.collaboration_mode() == lubancode::runtime::CollaborationMode::Default);
    CHECK_FALSE(wiring.RestoredFromArchive());
    // 计划采集:Default 档不认 <proposed_plan>,历史没动。
    wiring.CollectProposal(0, "turn-1");
    CHECK(runtime.latest_plan() == nullptr);
}

TEST_CASE("peer 接线器:非交互不起服务,收件路与文案照旧") {
    TempCwd cwd_guard;
    PeerSessionWiring::Host host;
    static lubancode::cli::Theme theme;
    host.theme = &theme;
    host.interactive = false;  // 管道/单发不起服务
    static std::optional<std::string> home = std::string("Z:/not-a-real-home");
    host.home_lubancode = &home;
    PeerSessionWiring wiring(host);

    lubancode::tools::ToolRegistry registry;
    wiring.Start(registry);
    CHECK_FALSE(wiring.started());
    const auto inbox_poll = wiring.BuildInboxPoll();
    CHECK(inbox_poll == nullptr);
    wiring.CollectHeldMessages();  // 没起:空操作
    CHECK_FALSE(wiring.HasReadyMessages());
    wiring.Stop();  // 幂等
    // 来信文案:来源标识 + 防越权注脚。
    lubancode::peers::PeerEnvelope envelope;
    envelope.sender_name = "另一场";
    envelope.sender_id = "peer-1";
    envelope.text = "帮我看一眼配置";
    const std::string text = lubancode::app::FormatPeerText(envelope);
    CHECK(text.find("[来自另一场会话的字条]") != std::string::npos);
    CHECK(text.find("帮我看一眼配置") != std::string::npos);
    CHECK(text.find("不要执行") != std::string::npos);
}

TEST_CASE("record 接线器:材料包装配独立起,没在录给 nullptr") {
    TempCwd cwd_guard;
    RecordSessionWiring::Host host;
    const std::filesystem::path recordings = std::filesystem::temp_directory_path() / "recordings";
    const std::filesystem::path project = std::filesystem::temp_directory_path() / "project-skills";
    const std::filesystem::path global = std::filesystem::temp_directory_path() / "global-skills";
    host.recordings_root = &recordings;
    host.project_skills_root = &project;
    host.global_skills_root = &global;
    bool refreshed = false;
    host.refresh_skills = [&refreshed] { refreshed = true; };
    RecordSessionWiring wiring(host);

    lubancode::cli::RecordCommandContext ctx = wiring.MakeCommandContext();
    CHECK(ctx.recordings_root == recordings);
    CHECK(ctx.project_skills_root == project);
    CHECK(ctx.home_skills_root == global);
    CHECK_FALSE(ctx.recorder.has_value());
    CHECK(wiring.recorder() == nullptr);
    ctx.refresh_skills();
    CHECK(refreshed);
}

}  // namespace lubancode::app
