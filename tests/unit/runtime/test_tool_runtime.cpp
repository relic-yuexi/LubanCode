// ToolRuntime 的装配与寿命性状测试:真构造、真查询、真析构(空配置下
// 不起 MCP 子进程、不配 LSP;ToolAssemblyPlan 显式给出各场插件与数据根,
// 插件三路扫描静默空,工具数只剩内置那批)。MCP/DLL/LSP 的真 fixture 见
// test_mcp_*、test_plugins、test_lsp_*,这边只钉装配结构:哪张表有哪些
// 工具、agent 工具抓的引用、Explore 硬边界、过滤与补挂。
// 全局插件真装上时 deferral 该触发,是产品行为——那笔对账归集成册
// integration/plugins/test_tool_runtime_deferral.cpp,这边只管隔离。
#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "app/tool_runtime.hpp"
#include "app/cli_tool_assembly.hpp"
#include "config/plugin_trust.hpp"
#include "memory/project_memory.hpp"  // P2 memory gate 清账:真 ProjectMemory 翻档
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "runtime/plugin_tool.hpp"
#include "tool_assembly_fixture.hpp"

namespace {

class NullBackend : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)request;
        (void)on_event;
        (void)cancel;
        return {};
    }
};

lubancode::config::Config EmptyConfig() {
    lubancode::config::Config config;
    config.mcp_servers.clear();
    config.lsp_servers.clear();
    config.search = lubancode::config::SearchConfig();
    return config;
}

const std::vector<lubancode::tools::SkillMeta>& NoSkills() {
    static const std::vector<lubancode::tools::SkillMeta> skills;
    return skills;
}

using lubancode::test_support::ToolAssemblyFixture;

struct CaptureOutput {
    std::ostringstream output, error;
    std::streambuf* previous_output = std::cout.rdbuf(output.rdbuf());
    std::streambuf* previous_error = std::cerr.rdbuf(error.rdbuf());
    void Restore() {
        if (!previous_output) return;
        std::cout.rdbuf(previous_output);
        std::cerr.rdbuf(previous_error);
        previous_output = nullptr;
    }
    ~CaptureOutput() { Restore(); }
};

void InstallMarkerPlugin(const std::filesystem::path& plugins, const std::string& marker) {
    std::filesystem::create_directories(plugins / "isolated");
    nlohmann::json tool;
    tool["name"] = "inspect";
    tool["description"] = marker;
    tool["input_schema"] = {{"type", "object"}};
    nlohmann::json manifest;
    manifest["manifest_version"] = 1;
    manifest["id"] = "isolated";
    manifest["version"] = "1.0.0";
    manifest["language"] = "shell";
    manifest["runtime"] = {{"kind", "process"}, {"command", "echo"}};
    manifest["tools"] = nlohmann::json::array({tool});
    std::ofstream(plugins / "isolated" / "plugin.json", std::ios::binary) << manifest.dump();
}

void WriteAgent(const std::filesystem::path& directory, const std::string& name) {
    std::filesystem::create_directories(directory);
    std::ofstream(directory / (name + ".yaml"), std::ios::binary)
        << "schema: 1\nname: " << name << "\ndescription: " << name
        << " marker\nskills:\n  preload:\n    - startup-skill\n";
}

}  // namespace

using namespace lubancode::app;

TEST_CASE("默认装配:主表有 agent/todo_write/基础工具,子表同级(含 agent 转发壳与 todo)") {
    ToolAssemblyFixture fixture;  // 显式插件根为空,不读也不修改真实 HOME。
    lubancode::config::Config config = EmptyConfig();
    NullBackend backend;
    ToolRuntime runtime(config, backend, NoSkills(),
                        /*skills_segment=*/"", fixture.Plan(), ToolRuntime::Options{});

    CHECK(runtime.main_registry().Find("agent") != nullptr);
    CHECK(runtime.main_registry().Find("todo_write") != nullptr);
    CHECK(runtime.main_registry().Find("read_file") != nullptr);
    CHECK(runtime.main_registry().Find("run_command") != nullptr);
    CHECK(runtime.main_registry().Find("ask_user") == nullptr);  // 交互独有,默认不挂
    // 同级能力(规格"产品不变量"):子表也挂 agent(AgentDispatchTool 转发壳)
    // 与 todo_write(RunTask 给每只任务换独占实例);递归治理靠 AgentTool 的
    // 深度账,不靠"子表没有 agent"。
    CHECK(runtime.sub_registry().Find("agent") != nullptr);
    CHECK(runtime.sub_registry().Find("agent")->name() == "agent");
    CHECK(runtime.sub_registry().Find("todo_write") != nullptr);
    CHECK(runtime.sub_registry().Find("read_file") != nullptr);
    // 子表 todo 板与主表各是各的:子代理不写 main 的待办。
    CHECK(runtime.sub_todo_state() != nullptr);
    CHECK(runtime.sub_todo_state() != runtime.todo_state());
    CHECK(runtime.explore_registry() == nullptr);  // 单发/默认无 Explore
    CHECK(runtime.agent_tool() != nullptr);
    CHECK(runtime.todo_state() != nullptr);
    CHECK(runtime.loaded_tools() != nullptr);
    // 空配置 + 零插件(显式目录钉死):两张表只剩内置工具(主 12、子
    // 11,均低于默认阈值 20),口径直接钉数字——总数严格大于阈值才启用
    // (DeferralEnabled 的合同),所以 deferral 必关、tool_search 不挂、
    // 过滤直通。真机上用户装多少插件都进不来,这几条在谁的家目录下跑都
    // 是同一个数。
    CHECK(runtime.main_registry().All().size() <
          static_cast<std::size_t>(config.tool_search_threshold));
    CHECK(runtime.sub_registry().All().size() <
          static_cast<std::size_t>(config.tool_search_threshold));
    CHECK(runtime.main_deferral() == false);
    CHECK(runtime.sub_deferral() == false);
    CHECK(runtime.main_tool_filter()(*runtime.main_registry().Find("read_file")));
    CHECK(runtime.sub_tool_filter()(*runtime.sub_registry().Find("read_file")));
    runtime.AttachMemoryTool(nullptr);  // 空指针安全
}

TEST_CASE("with_explore:Explore 只读硬边界,并挂到 agent 工具") {
    ToolAssemblyFixture fixture;  // 不读用户家目录:Explore 断言不吃全局插件
    lubancode::config::Config config = EmptyConfig();
    NullBackend backend;
    ToolRuntime::Options options;
    options.with_explore = true;
    ToolRuntime runtime(config, backend, NoSkills(),
                        /*skills_segment=*/"", fixture.Plan(), std::move(options));

    lubancode::tools::ToolRegistry* explore = runtime.explore_registry();
    REQUIRE(explore != nullptr);
    CHECK(explore->Find("read_file") != nullptr);
    CHECK(explore->Find("search") != nullptr);
    CHECK(explore->Find("write_file") == nullptr);   // 只读边界:无写入
    CHECK(explore->Find("run_command") == nullptr);  // 无命令
    CHECK(explore->Find("agent") == nullptr);        // 只读角色不派工(角色限制)
    CHECK(explore->Find("todo_write") == nullptr);   // 只读角色无 todo
    // Explore 表不进 MCP(空配置下无从验),但 agent 工具确实拿到了它。
    CHECK(runtime.agent_tool() != nullptr);
}

TEST_CASE("寿命:构造-查询-析构全程不崩,表地址稳定") {
    ToolAssemblyFixture fixture;  // 不读用户家目录:析构册也不碰真机 DLL
    lubancode::config::Config config = EmptyConfig();
    auto backend = std::make_unique<NullBackend>();
    const std::vector<lubancode::tools::SkillMeta> no_skills;
    auto runtime = std::make_unique<ToolRuntime>(config, *backend,
                                                 no_skills, /*skills_segment=*/"", fixture.Plan(),
                                                 ToolRuntime::Options{});
    lubancode::tools::ToolRegistry* main_before = &runtime->main_registry();
    lubancode::tools::ToolRegistry* sub_before = &runtime->sub_registry();
    CHECK(runtime->main_registry().Find("agent") != nullptr);
    CHECK(&runtime->main_registry() == main_before);
    CHECK(&runtime->sub_registry() == sub_before);
    runtime.reset();  // 真析构:表先亡、拥有者后亡
}

// 动态工具 PromptCache 守恒单 P2(§十三 P2 第四条:memory gate 清账):
// memory_save 的暴露只认注册(能力变化,hash 断得有名有姓),不再现查
// 运行档——/memory off、/memory learn off 翻档后定义照旧常驻,tools hash
// 不白断;运行档全在执行侧(MemorySaveTool::execute 自拒 + proxy 路的
// main_execution_policy_)。册里拿真 ProjectMemory 翻档对账。
TEST_CASE("P2 清账: memory_save 暴露只认注册,运行档翻面不收定义") {
    ToolAssemblyFixture fixture;
    lubancode::config::Config config = EmptyConfig();
    NullBackend backend;

    lubancode::memory::ProjectIdentity identity;
    identity.project_root = fixture.Project();
    identity.workspace_dir = identity.project_root;
    identity.workspace_key = "p2-memory-test";
    lubancode::memory::Options memory_options;
    memory_options.global_allowed = true;
    memory_options.enabled = true;
    memory_options.learn = lubancode::memory::LearnMode::Review;
    memory_options.learn_ceiling = lubancode::memory::LearnMode::Review;
    auto memory = std::make_shared<lubancode::memory::ProjectMemory>(
        identity, fixture.root / "memory", memory_options);

    ToolRuntime::Options options;
    options.memory = memory;
    ToolRuntime runtime(config, backend, NoSkills(),
                        /*skills_segment=*/"", fixture.Plan(), std::move(options));

    lubancode::tools::Tool* memory_save = runtime.main_registry().Find("memory_save");
    REQUIRE(memory_save != nullptr);
    CHECK(memory->generate_enabled());
    CHECK(runtime.main_tool_filter()(*memory_save));  // 注册即常驻

    // 用户翻档:/memory off → 本场总开关关。旧路这里定义会从 tools 里
    // 消失(hash 白断);P2 起照旧常驻,执行侧自拒。
    REQUIRE(memory->set_enabled(false).has_value());
    CHECK_FALSE(memory->generate_enabled());
    CHECK(runtime.main_tool_filter()(*memory_save));  // 定义不收

    // 再翻:/memory learn off(档位语义同效)。
    REQUIRE(memory->set_enabled(true).has_value());
    REQUIRE(memory->set_learn(lubancode::memory::LearnMode::Off).has_value());
    CHECK_FALSE(memory->generate_enabled());
    CHECK(runtime.main_tool_filter()(*memory_save));  // 定义还是不收

    // 执行侧真拦:直名调用被工具自己的运行时判定拒掉,稳定文案,不装成功。
    const auto refused = memory_save->execute(nlohmann::json{{"kind", "fact"}, {"topic", "x"}, {"content", "y"}});
    CHECK(refused.is_error);
    CHECK(refused.content.find("未开启") != std::string::npos);
}

TEST_CASE("ToolAssemblyPlan: invalid cwd or roots reject before presenting assembly diagnostics") {
    ToolAssemblyFixture fixture;
    auto plan = fixture.Plan();
    SUBCASE("empty cwd") { plan.cwd_utf8.clear(); }
    SUBCASE("relative cwd") { plan.cwd_utf8 = "relative project"; }
    SUBCASE("invalid UTF-8 cwd") { plan.cwd_utf8 = std::string(1, '\xff'); }
    SUBCASE("relative plugin root") { plan.user_plugins_dir = "relative plugins"; }
    SUBCASE("relative state root") { plan.package_data_root = "relative state"; }
    auto config = EmptyConfig();
    config.mcp_servers["must-not-start"].command = "missing-plan-fixture-command";
    NullBackend backend;
    int reports = 0;
    CHECK_THROWS_AS(ToolRuntime(config, backend, NoSkills(), "", plan, ToolRuntime::Options{},
        [&](const ToolAssemblyDiagnostic&) { ++reports; }), std::invalid_argument);
    CHECK(reports == 0);
}

TEST_CASE("ToolAssemblyPlan: diagnostics remain ordered and silent without a presentation sink") {
    ToolAssemblyFixture fixture;
    auto config = EmptyConfig();
    for (const auto& name : {"alpha", "beta"}) {
        config.mcp_servers[name].command = "missing-tool-assembly-fixture-command-42";
    }
    NullBackend backend;
    CaptureOutput captured;
    ToolRuntime runtime(config, backend, NoSkills(), "", fixture.Plan(), ToolRuntime::Options{});
    captured.Restore();
    CHECK(captured.output.str().empty());
    CHECK(captured.error.str().empty());
    const auto& diagnostics = runtime.diagnostics();
    REQUIRE(diagnostics.size() >= 2);
    for (std::size_t index = 0; index != 2; ++index) {
        CHECK(diagnostics[index].code == "mcp.start_failed");
        CHECK(diagnostics[index].severity == ToolAssemblyDiagnosticSeverity::Warning);
        CHECK(diagnostics[index].component == (index == 0 ? "alpha" : "beta"));
        CHECK(diagnostics[index].scope == ToolAssemblyDiagnosticScope::Shared);
        CHECK_FALSE(diagnostics[index].arguments.empty());
    }
}

TEST_CASE("ToolAssemblyPlan: same project can bind separate same-name plugins and diagnostic owners") {
    ToolAssemblyFixture first, second;
    InstallMarkerPlugin(first.Plugins(), "FIRST_OWNER_MARKER");
    InstallMarkerPlugin(second.Plugins(), "SECOND_OWNER_MARKER");
    auto first_plan = first.Plan();
    auto second_plan = second.Plan();
    second_plan.cwd_utf8 = first_plan.cwd_utf8;
    auto config = EmptyConfig();
    NullBackend backend;
    std::vector<ToolAssemblyDiagnostic> presented;
    auto presentation_owner = std::make_shared<int>(42);
    std::weak_ptr<int> presentation_weak = presentation_owner;
    CaptureOutput captured;
    ToolRuntime one(config, backend, NoSkills(), "", first_plan, ToolRuntime::Options{},
        [presentation_owner, &presented](const ToolAssemblyDiagnostic& diagnostic) {
            presented.push_back(diagnostic);
        });
    presentation_owner.reset();
    const bool presentation_released = presentation_weak.expired();
    ToolRuntime two(config, backend, NoSkills(), "", second_plan, ToolRuntime::Options{});
    captured.Restore();
    CHECK(presentation_released);
    CHECK(captured.output.str().empty());
    CHECK(captured.error.str().empty());
    for (auto* registry : {&one.main_registry(), &one.sub_registry()}) {
        const auto* tool = registry->Find("plugin__isolated__inspect");
        REQUIRE(tool != nullptr);
        CHECK(tool->description().find("FIRST_OWNER_MARKER") != std::string::npos);
        CHECK(tool->description().find("SECOND_OWNER_MARKER") == std::string::npos);
    }
    for (auto* registry : {&two.main_registry(), &two.sub_registry()}) {
        const auto* tool = registry->Find("plugin__isolated__inspect");
        REQUIRE(tool != nullptr);
        CHECK(tool->description().find("SECOND_OWNER_MARKER") != std::string::npos);
        CHECK(tool->description().find("FIRST_OWNER_MARKER") == std::string::npos);
    }
    int main_mounts = 0, sub_mounts = 0, shown_mounts = 0;
    for (const auto& diagnostic : one.diagnostics()) {
        if (diagnostic.code != "plugin.mounted_line") continue;
        if (diagnostic.scope == ToolAssemblyDiagnosticScope::Main) ++main_mounts;
        if (diagnostic.scope == ToolAssemblyDiagnosticScope::Sub) ++sub_mounts;
    }
    for (const auto& diagnostic : presented) {
        CHECK(diagnostic.scope != ToolAssemblyDiagnosticScope::Sub);
        if (diagnostic.code == "plugin.mounted_line") ++shown_mounts;
    }
    CHECK(main_mounts == 1);
    CHECK(sub_mounts == 1);
    CHECK(shown_mounts == 1);
}

TEST_CASE("ToolAssemblyPlan: CLI presentation uses only its explicit destination and copied theme") {
    ToolAssemblyFixture fixture;
    auto config = EmptyConfig();
    config.mcp_servers["explicit-diagnostic"].command = "missing-tool-assembly-fixture-command-42";
    NullBackend backend;
    std::ostringstream destination;
    auto sink = [&] {
        auto theme = lubancode::cli::BuiltinTheme("plain");
        theme.error = "ERROR_BEGIN";
        theme.reset = "ERROR_END";
        return MakeCliToolAssemblyDiagnosticSink(theme, destination);
    }();
    CaptureOutput captured;
    ToolRuntime runtime(config, backend, NoSkills(), "", fixture.Plan(), ToolRuntime::Options{}, sink);
    captured.Restore();
    CHECK(captured.output.str().empty());
    CHECK(captured.error.str().empty());
    CHECK(destination.str().find("ERROR_BEGIN") != std::string::npos);
    CHECK(destination.str().find("ERROR_END") != std::string::npos);
    CHECK(destination.str().find("explicit-diagnostic") != std::string::npos);
    REQUIRE_FALSE(runtime.diagnostics().empty());
    CHECK(runtime.diagnostics()[0].component == "explicit-diagnostic");
}

TEST_CASE("ToolAssemblyPlan: agent roots and permission stay late-bound while startup skills are owned") {
    ToolAssemblyFixture fixture;
    const auto first_roots = fixture.root / "agents first";
    const auto second_roots = fixture.root / "agents second";
    WriteAgent(first_roots, "first-agent");
    WriteAgent(second_roots, "second-agent");
    const auto skill_path = fixture.root / "skill with spaces";
    std::filesystem::create_directories(skill_path);
    std::ofstream(skill_path / "SKILL.md", std::ios::binary)
        << "---\nname: startup-skill\ndescription: fixture\n---\nOWNED_STARTUP_SKILL_BODY\n";
    const auto package_layer = fixture.root / "dev packages";
    const auto package_root = package_layer / "content";
    std::filesystem::create_directories(package_root / "skills" / "startup-skill");
    std::ofstream(package_root / "package.yaml", std::ios::binary)
        << "schema: 1\nid: fixture.content\nversion: 0.1.0\nname: Fixture\ndescription: fixture\n";
    WriteAgent(package_root / "agents", "pack-agent");
    const auto package_skill = package_root / "skills" / "startup-skill" / "SKILL.md";
    std::ofstream(package_skill, std::ios::binary)
        << "---\nname: startup-skill\ndescription: fixture\n---\nPINNED_PACKAGE_SKILL_BODY\n";
    lubancode::package::PackageMountInput mount;
    mount.scan.dev_roots.push_back(package_layer);
    auto snapshot = lubancode::package::BuildPackageSnapshot(mount, 1);
    REQUIRE_FALSE(snapshot->empty());
    auto roots = std::make_shared<lubancode::agent::AgentCatalogScanRoots>();
    roots->user_dir = first_roots;
    auto permission = std::make_shared<lubancode::ApprovalMode>(lubancode::ApprovalMode::Default);
    auto plan = fixture.Plan();
    plan.agent_scan_roots = [roots] { return *roots; };
    plan.parent_permission = [permission] { return *permission; };
    auto config = EmptyConfig();
    NullBackend backend;
    std::unique_ptr<ToolRuntime> runtime;
    ToolRuntime::Options options;
    options.package_snapshot = [snapshot] { return snapshot; };
    {
        lubancode::tools::SkillMeta skill;
        skill.name = "startup-skill";
        skill.description = "fixture";
        skill.dir_path = lubancode::platform::PathToUtf8(skill_path);
        std::vector<lubancode::tools::SkillMeta> skills{skill};
        runtime = std::make_unique<ToolRuntime>(config, backend, skills, "", plan, options);
    }
    auto* agent = runtime->agent_tool();
    REQUIRE(agent != nullptr);
    const auto check_skill = [&](const char* name) {
        auto material = agent->custom_agent_resolver()(name);
        REQUIRE(material.has_value());
        REQUIRE(material->preloaded_skills.size() == 1);
        CHECK(material->preloaded_skills[0].find("OWNED_STARTUP_SKILL_BODY") != std::string::npos);
    };
    check_skill("first-agent");
    CHECK(agent->input_schema().dump().find("first-agent") != std::string::npos);
    CHECK(agent->resolve_environment_provider()().parent_permission == lubancode::ApprovalMode::Default);
    roots->user_dir = second_roots;
    *permission = lubancode::ApprovalMode::DontAsk;
    std::ofstream(package_skill, std::ios::binary) << "Changed after the snapshot was pinned.";
    agent->SetHooks({});  // The real turn-boundary refresh invalidates the type cache.
    check_skill("second-agent");
    CHECK_FALSE(agent->custom_agent_resolver()("first-agent").has_value());
    const auto schema = agent->input_schema().dump();
    CHECK(schema.find("second-agent") != std::string::npos);
    CHECK(schema.find("first-agent") == std::string::npos);
    CHECK(schema.find("fixture.content:pack-agent") != std::string::npos);
    const auto packaged = agent->custom_agent_resolver()("fixture.content:pack-agent");
    REQUIRE(packaged.has_value());
    REQUIRE(packaged->preloaded_skills.size() == 1);
    CHECK(packaged->preloaded_skills[0].find("PINNED_PACKAGE_SKILL_BODY") != std::string::npos);
    CHECK(packaged->preloaded_skills[0].find("Changed after") == std::string::npos);
    CHECK(agent->resolve_environment_provider()().parent_permission == lubancode::ApprovalMode::DontAsk);
    roots->user_dir = "relative agents";
    CHECK_THROWS_AS(agent->custom_agent_resolver()("second-agent"), std::invalid_argument);
    CHECK_THROWS_AS(agent->SetHooks({}), std::invalid_argument);
    roots->user_dir = second_roots;
    agent->SetHooks({});
    CHECK(agent->input_schema().dump().find("second-agent") != std::string::npos);
}

TEST_CASE("ToolAssemblyPlan: project trust stays with the explicit store even in one shared cwd") {
    ToolAssemblyFixture first, second;
    const auto project_plugins = first.Project() / ".lubancode" / "plugins";
    InstallMarkerPlugin(project_plugins, "TRUSTED_PROJECT_MARKER");
    const auto plugin_dir = project_plugins / "isolated";
    const auto hash = lubancode::runtime::ComputePluginContentHash(plugin_dir);
    REQUIRE(hash.has_value());
    auto first_plan = first.Plan();
    auto second_plan = second.Plan();
    second_plan.cwd_utf8 = first_plan.cwd_utf8;
    auto [trust, error] = lubancode::config::PluginTrustStore::Load(
        lubancode::platform::PathToUtf8(*first_plan.plugin_trust_path));
    REQUIRE_FALSE(error.has_value());
    REQUIRE(trust.SetTrusted(lubancode::platform::PathToUtf8(
        std::filesystem::weakly_canonical(plugin_dir)), *hash, "fixture"));
    REQUIRE_FALSE(trust.Save().has_value());
    auto config = EmptyConfig();
    NullBackend backend;
    ToolRuntime accepted(config, backend, NoSkills(), "", first_plan, ToolRuntime::Options{});
    ToolRuntime refused(config, backend, NoSkills(), "", second_plan, ToolRuntime::Options{});
    REQUIRE(accepted.main_registry().Find("plugin__isolated__inspect") != nullptr);
    REQUIRE(accepted.sub_registry().Find("plugin__isolated__inspect") != nullptr);
    CHECK(refused.main_registry().Find("plugin__isolated__inspect") == nullptr);
    CHECK(refused.sub_registry().Find("plugin__isolated__inspect") == nullptr);
    CHECK(accepted.process_manifests().size() == 1);
    CHECK(refused.process_manifests().empty());
    CHECK_FALSE(refused.diagnostics().empty());
    CHECK_FALSE(std::filesystem::exists(*second_plan.plugin_trust_path));
}

TEST_CASE("ToolAssemblyPlan: real MCP processes use explicit cwd without changing the host directory") {
    ToolAssemblyFixture first, second;
    const auto host_cwd = std::filesystem::current_path();
#ifdef _WIN32
    const char* python = "python";
#else
    const char* python = "python3";
#endif
    const auto located = lubancode::platform::RunProcess(
        {python, "-c", "import json,sys; print(json.dumps(sys.executable))"}, 10000);
    REQUIRE_FALSE(located.spawn_failed);
    REQUIRE_FALSE(located.timed_out);
    REQUIRE(located.exit_code == 0);
    auto config = EmptyConfig();
    config.mcp_servers["location"].command = nlohmann::json::parse(located.output).get<std::string>();
    config.mcp_servers["location"].args = {
        std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/session_resources_mcp.py"};
    NullBackend backend;
    auto one = std::make_unique<ToolRuntime>(config, backend, NoSkills(), "", first.Plan(), ToolRuntime::Options{});
    auto two = std::make_unique<ToolRuntime>(config, backend, NoSkills(), "", second.Plan(), ToolRuntime::Options{});
    const auto inspect = [&](ToolRuntime& runtime, const std::filesystem::path& expected) {
        auto* tool = runtime.main_registry().Find("mcp__location__where");
        REQUIRE(tool != nullptr);
        const auto result = tool->execute(nlohmann::json::object());
        REQUIRE_FALSE(result.is_error);
        const auto body = nlohmann::json::parse(result.content);
        CHECK(std::filesystem::equivalent(
            lubancode::platform::Utf8ToPath(body.at("cwd").get<std::string>()), expected));
        CHECK(std::filesystem::equivalent(host_cwd, std::filesystem::current_path()));
        return body.at("pid").get<unsigned long>();
    };
    const auto first_pid = inspect(*one, first.Project());
    const auto second_pid = inspect(*two, second.Project());
    CHECK(first_pid != second_pid);
    one.reset();
    CHECK_FALSE(lubancode::platform::IsProcessAlive(first_pid));
    CHECK(inspect(*two, second.Project()) == second_pid);
    two.reset();
    CHECK_FALSE(lubancode::platform::IsProcessAlive(second_pid));
}
