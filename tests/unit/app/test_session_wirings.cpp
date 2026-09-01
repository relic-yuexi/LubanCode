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
#include "tools/agent_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/skill_tool.hpp"
#include "tools/write_file.hpp"

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
    lubancode::runtime::SessionRuntime runtime({"anthropic", "test"});
    GoalSessionWiring wiring(MakeGoalHost(runtime, nullptr));

    // 装配前:coordinator 空、iteration 不活跃,收口路原样返回。
    REQUIRE(wiring.coordinator() == nullptr);
    CHECK_FALSE(wiring.HasActiveIteration());
    wiring.CloseIteration("turn-x", /*turn_failed=*/false);  // 不炸即过
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
    // 恢复口:P0-6 后是幂等空位(旧档路删,新账接入属后续波次)。
    wiring.RestoreFromArchive();
}

TEST_CASE("loop 接线器:装配独立起(timer 起/收),泵无活空转") {
    TempCwd cwd_guard;
    lubancode::runtime::SessionRuntime runtime({"anthropic", "test"});
    lubancode::runtime::IdleWakeCoordinator wakes;
    lubancode::config::Config config;

    LoopSessionWiring::Host host;
    static lubancode::cli::Theme theme;
    host.theme = &theme;
    host.interactive = false;
    host.config = &config;
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
    lubancode::runtime::SessionRuntime runtime({"anthropic", "test"});
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
    // 计划采集:Default 档不认 <proposed_plan>,历史没动。
    wiring.CollectProposal(0, "turn-1");
    CHECK(runtime.latest_plan() == nullptr);
}

// P2-3 的 Plan 闸接线钉子:真注册表(真工具的 effect_class 声明)+ 真
// agent 工具(挂自定义 Agent 解析口),钉 EvaluateGate 在 Plan 档按参数
// 判——skill 加载、只读 Explore、git ls-tree 放行;写盘件、general-purpose、
// 改状态命令拦,拦截回执带命中规则。
TEST_CASE("plan 接线器: Plan 档按参数放只读——skill/Explore/git ls-tree,拦写盘") {
    TempCwd cwd_guard;
    lubancode::runtime::SessionRuntime runtime({"anthropic", "test"});
    lubancode::agent::PromptOptions prompt_options;
    runtime.SetCollaborationMode(lubancode::runtime::CollaborationMode::Plan, "test", "confirm");

    // 真 agent 工具:send_stream 永不上路(闸测试不派真任务),解析口给
    // 一枚 tools.allow 全只读的自定义 Agent。
    struct IdleBackend final : public lubancode::api::Backend {
        std::expected<void, lubancode::api::Error> send_stream(
            const lubancode::api::Request&,
            const std::function<void(const lubancode::api::StreamEvent&)>&,
            const std::atomic<bool>*) override {
            return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Network, "idle"});
        }
    };
    static IdleBackend backend;
    static lubancode::tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    static lubancode::tools::AgentTool agent_tool(backend, sub_registry, "Z:/nowhere", std::string(), 0,
                                                  std::string());
    agent_tool.SetCustomAgentResolver(
        [](const std::string& name) -> std::optional<lubancode::tools::CustomAgentMaterial> {
            if (name != "library-reviewer") {
                return std::nullopt;
            }
            lubancode::tools::CustomAgentMaterial material;
            material.definition.name = name;
            material.definition.tools.allow = {"read_file", "search"};
            return material;
        });

    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    registry.Register(std::make_unique<lubancode::tools::SearchTool>());
    registry.Register(std::make_unique<lubancode::tools::WriteFileTool>());
    registry.Register(std::make_unique<lubancode::tools::SkillTool>(std::vector<lubancode::tools::SkillMeta>{}));
    registry.Register(std::make_unique<lubancode::tools::RunCommandTool>());
    // 闸查注册表要能按名查到 "agent"(转发壳名就是 agent,免重复 owning)。
    registry.Register(std::make_unique<lubancode::tools::AgentDispatchTool>(agent_tool));

    PlanSessionWiring::Host host;
    static lubancode::cli::Theme theme;
    host.theme = &theme;
    host.session_runtime = &runtime;
    host.prompt_options = &prompt_options;
    host.registry = [&registry] { return &registry; };
    host.agent_tool = [] { return &agent_tool; };  // 静态存储期,免捕引用
    host.rebuild_preserving = [] {};
    host.start_turn = [](const std::string&, bool*) {};
    PlanSessionWiring wiring(host);

    // 放行(P2-3):skill 加载、内置 Explore、tools.allow 全只读的自定义
    // Agent、git 只读子命令。
    CHECK(wiring.EvaluateGate("skill", {{"name", "x"}}).empty());
    CHECK(wiring.EvaluateGate("agent", {{"agent_type", "Explore"}}).empty());
    CHECK(wiring.EvaluateGate("agent", {{"agent_type", "library-reviewer"}}).empty());
    CHECK(wiring.EvaluateGate("run_command", {{"command", "git ls-tree -r --name-only HEAD"}, {"shell", "cmd"}})
              .empty());
    CHECK(wiring.EvaluateGate("run_command",
                              {{"command", "Get-ChildItem | Select-Object Name"}, {"shell", "powershell"}})
              .empty());

    // 拦截:写盘件、全工具面子代理、改状态命令;回执带稳定 code 与命中
    // 规则。
    const std::string write_denied = wiring.EvaluateGate("write_file", nlohmann::json{});
    CHECK(write_denied.find("mode.denied") == 0);
    const std::string general_denied =
        wiring.EvaluateGate("agent", {{"agent_type", "general-purpose"}});
    CHECK(general_denied.find("mode.denied.agent_role") == 0);
    const std::string push_denied =
        wiring.EvaluateGate("run_command", {{"command", "git push"}, {"shell", "cmd"}});
    CHECK(push_denied.find("mode.denied.shell") == 0);
    CHECK(push_denied.find("git 子命令 push") != std::string::npos);  // 命中的规则进回执
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

// ---------------------------------------------------------------------------
// 动态工具 PromptCache 守恒单 P2(条件工具也守恒·§8.2):goal/loop 窄工具
// 的暴露位自接线器出——只认会话级条件(features 正门 + env 总闸),与
// "本轮可不可用"(HasActiveIteration/TickActive,执行门那半边)分家。
// 暴露恒定,tools hash 才不随 goal 轮/loop 拍的进出抖。
// ---------------------------------------------------------------------------

TEST_CASE("P2 暴露位: goal_checkpoint 只认会话级条件,与轮次活跃账分家") {
    TempCwd cwd_guard;
    lubancode::runtime::SessionRuntime runtime({"anthropic", "test"});

    // features.goals 关:定义不进 tools(暴露位假)。
    {
        lubancode::config::Config config;
        config.features_goals = false;
        static lubancode::cli::Theme theme;
        GoalSessionWiring::Host host;
        host.theme = &theme;
        host.config = &config;
            GoalSessionWiring wiring(host);
        CHECK_FALSE(wiring.ToolExposed());
    }
    // features.goals 开:定义常驻(暴露位真)——但"本轮可不可用"另算,
    // 没在 goal 执行轮时 HasActiveIteration 照样假(执行门那半边)。
    {
        lubancode::config::Config config;
        config.features_goals = true;
        static lubancode::cli::Theme theme;
        GoalSessionWiring::Host host;
        host.theme = &theme;
        host.config = &config;
            GoalSessionWiring wiring(host);
        CHECK(wiring.ToolExposed());
        CHECK_FALSE(wiring.HasActiveIteration());
        CHECK(wiring.ActiveGoalId().empty());
    }
    // host 没配 config(防御):不给暴露,不给状态。
    {
        GoalSessionWiring wiring;
        CHECK_FALSE(wiring.ToolExposed());
        CHECK(wiring.ActiveGoalId().empty());
    }
}

TEST_CASE("P2 暴露位: loop_control 只认会话级条件,与拍活跃账分家") {
    TempCwd cwd_guard;
    lubancode::runtime::SessionRuntime runtime({"anthropic", "test"});
    lubancode::runtime::IdleWakeCoordinator wakes;

    // features.loop 关:暴露位假。
    {
        lubancode::config::Config config;
        config.features_loop = false;
        static lubancode::cli::Theme theme;
        LoopSessionWiring::Host host;
        host.theme = &theme;
        host.interactive = false;
        host.config = &config;
            host.idle_wakes = &wakes;
        LoopSessionWiring wiring(host);
        CHECK_FALSE(wiring.ToolExposed());
    }
    // features.loop 开:暴露位真;没在拍上时 TickActive 假、任务注空
    //(执行门那半边另算)。
    {
        lubancode::config::Config config;
        config.features_loop = true;
        static lubancode::cli::Theme theme;
        LoopSessionWiring::Host host;
        host.theme = &theme;
        host.interactive = false;
        host.config = &config;
            host.idle_wakes = &wakes;
        LoopSessionWiring wiring(host);
        CHECK(wiring.ToolExposed());
        CHECK_FALSE(wiring.TickActive());
        CHECK(wiring.ActiveLoopTaskId().empty());
        wiring.Shutdown();
    }
}

}  // namespace lubancode::app
