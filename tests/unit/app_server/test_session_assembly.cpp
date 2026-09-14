// 会话级运行材料装配的单测(工业化多协议接入单 P1,G01/G02;应用Worker
// 接入单 P2 增 Skill/Agent/插件三路)。
//
// 钉的规矩(冻结合同 §7 最小形状 + 单子 P1):
//   - 注入路:backend/registry 工厂各调一次、零 MCP、档案显式;
//   - 无部署档 = 显式零工具默认档(空表,不照搬终端全部工具);
//   - 零工具档(zero-tools)零启动:config 配了 MCP 也不碰;
//   - 先解析再启动:必需服务缺上层配置即明拒,零进程启动;
//   - 必需服务起服失败(command 不存在)整场明拒,不出半成品;
//   - 可选服务起服失败降级记账,不冒充已挂;
//   - 步数闸与 system_prompt 原样进档案(装配不猜)。
//
// P2 增钉(应用Worker接入单 §五/§六/§7.2):
//   - skill 工具装配面 = features.skills ∧ tools 面点名 "skill";单根
//     显式扫描,清单段与工具面同进同退;开关单独不起工具;
//   - 点名未接线插件 → component_unavailable 整场明拒,零副作用;
//   - agent_plan 在场:系统提示由提示部件组合产出(能力段按实际注册表
//     面开合),步数闸与档案 runtime 取更严;
//   - allow 里的 "skill" 在复验里按已装对账(内置件,不冒"缺工具")。
//
// 真进程链(必需服务握手后工具复验)在 integration 册
// test_app_server_profile_smoke.cpp——python 夹具配真 exe 走。
#include <doctest/doctest.h>

#include <atomic>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/agent_definition.hpp"
#include "api/backend.hpp"
#include "app_server/agent_wiring.hpp"
#include "app_server/harness_profile.hpp"
#include "app_server/session_assembly.hpp"
#include "config/config.hpp"
#include "config/plugin_trust.hpp"
#include "platform/paths.hpp"
#include "runtime/plugin_contract.hpp"
#include "runtime/plugin_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"

namespace fs = std::filesystem;

using namespace lubancode;
using namespace lubancode::app_server;

namespace {

// 假 backend(test_app_server_service.cpp 同款思路):装配只要求工厂交
// 非空件,本册不发请求。
class StubBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(const api::Request&,
                                                const std::function<void(const api::StreamEvent&)>&,
                                                const std::atomic<bool>* = nullptr) override {
        return {};
    }
};

std::unique_ptr<api::Backend> MakeFakeBackend() {
    return std::make_unique<StubBackend>();
}

SessionAssemblyRequest BaseRequest() {
    SessionAssemblyRequest request;
    request.backend_factory = &MakeFakeBackend;
    request.system_prompt = "test prompt";
    request.max_steps_per_turn = 7;
    return request;
}

// 一枚 only 档的 harness(直接构造——JSON 解析路另有
// test_harness_profile.cpp 把关)。
HarnessProfile MakeOnlyProfile(std::vector<std::string> allow, std::vector<std::string> servers) {
    HarnessProfile profile;
    profile.name = "only";
    profile.tools.mode = HarnessToolPolicy::Mode::Only;
    profile.tools.allow = std::move(allow);
    profile.mcp_servers = std::move(servers);
    profile.features_enabled.insert("mcp");
    return profile;
}

config::Config MakeConfigWithServer(const std::string& name, const std::string& command) {
    config::Config config;
    config::McpServerConfig server;
    server.command = command;
    config.mcp_servers[name] = server;
    return config;
}

// P2:一枚放行 skills 并点名 skill 工具的 only 档。
HarnessProfile MakeSkillProfile(bool with_skill_in_allow) {
    HarnessProfile profile;
    profile.name = "skills";
    profile.tools.mode = HarnessToolPolicy::Mode::Only;
    if (with_skill_in_allow) {
        profile.tools.allow = {"skill"};
    } else {
        profile.tools.allow = {};  // 空表合法;skills 开关单独不起工具
    }
    profile.features_enabled.insert("skills");
    return profile;
}

// 临时 skills 根,种一份技能。
fs::path MakeSkillsRoot(const std::string& tag) {
    static int counter = 0;
    const fs::path root = fs::temp_directory_path() /
                          ("lubancode_assembly_skills_" + tag + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::create_directories(root / "greet", ec);
    {
        std::ofstream out(root / "greet" / "SKILL.md", std::ios::binary);
        out << "---\nname: greet\ndescription: 问候技能。\n---\nGREET-SKILL-BODY。\n";
    }
    return root;
}

// P5:临时插件发现根,种一只 v2 embedded-lua 插件(plugin.json + demo.lua,
// 夹具形状与 tests/unit/runtime/test_plugin_lua_manifest.cpp 同款)。返回
// 根路径;插件 id 是 demo-lua,工具 plugin__demo-lua__search。
void WriteLuaPlugin(const fs::path& dir, const std::string& lua_script) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream out(dir / "plugin.json", std::ios::binary);
        out << R"json({
  "manifest_version": 2,
  "id": "demo-lua",
  "version": "0.1.0",
  "language": "lua",
  "runtime": {"kind": "embedded-lua", "entry": "demo.lua"},
  "tools": [
    {
      "name": "search",
      "entry": "search",
      "description": "Demo search tool.",
      "input_schema": {
        "type": "object",
        "properties": {"query": {"type": "string"}},
        "required": ["query"],
        "additionalProperties": false
      }
    }
  ]
})json";
    }
    {
        std::ofstream out(dir / "demo.lua", std::ios::binary);
        out << lua_script;
    }
}

fs::path MakePluginsRoot(const std::string& tag) {
    static int counter = 0;
    const fs::path root = fs::temp_directory_path() /
                          ("lubancode_assembly_plugins_" + tag + "_" + std::to_string(counter++));
    WriteLuaPlugin(root / "demo-lua",
                   "return { search = function(input) return 'ok: ' .. tostring(input.query) end }\n");
    return root;
}

// 一只 v1 process 插件(kind 未接线的点名对象)。
void WriteProcessPlugin(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream out(dir / "plugin.json", std::ios::binary);
        out << R"json({
  "manifest_version": 1,
  "id": "v1proc",
  "version": "1.0.0",
  "language": "python",
  "runtime": {"kind": "process", "command": "python", "args": ["${plugin_dir}/runner.py"]},
  "tools": [{"name": "count", "description": "数词", "input_schema": {"type": "object"}}]
})json";
    }
    {
        std::ofstream out(dir / "runner.py", std::ios::binary);
        out << "print('{}')\n";
    }
}

// 发现根里全部插件记进一本纯内存信任账(装配消费账是只读面,测试自建)。
config::PluginTrustStore TrustAllIn(const fs::path& root) {
    auto [store, load_error] = config::PluginTrustStore::Load(std::optional<std::string>{});
    REQUIRE(load_error == std::nullopt);
    for (const auto& manifest : runtime::ScanPluginDirectories(root).manifests) {
        const auto hash = runtime::ComputePluginContentHash(manifest->plugin_dir);
        REQUIRE(hash.has_value());
        store.SetTrusted(platform::PathToUtf8(manifest->plugin_dir), *hash, "assembly test");
    }
    return store;
}

// 一份最小档案的冻结计划(persona 路;preload 可选)。
std::shared_ptr<HarnessAgentPlan> MakePlan(std::vector<std::string> preload = {}) {
    auto plan = std::make_shared<HarnessAgentPlan>();
    plan->agent = agent::AgentDefinition{};
    plan->agent->name = "research";
    plan->agent->description = "研究助理。";
    plan->agent->skills_preload = std::move(preload);
    plan->wire = "chat";
    plan->cwd = "/tmp/w";
    return plan;
}

}  // namespace

TEST_CASE("注入路:backend/registry 工厂各交一件,零 MCP,档案原样") {
    SessionAssemblyRequest request = BaseRequest();
    int registry_calls = 0;
    request.registry_factory = [&registry_calls]() {
        ++registry_calls;
        auto registry = std::make_unique<tools::ToolRegistry>();
        registry->Register(std::make_unique<tools::ReadFileTool>());
        return registry;
    };
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    CHECK(result.error.empty());
    CHECK(result.assembly->backend != nullptr);
    REQUIRE(result.assembly->registry != nullptr);
    CHECK(result.assembly->registry->Find("read_file") != nullptr);
    CHECK(result.assembly->mcp_servers.empty());
    CHECK(result.assembly->degraded_components.empty());
    CHECK(result.assembly->agent_profile.system_prompt == "test prompt");
    CHECK(result.assembly->agent_profile.runtime.max_steps_per_turn == 7);
    CHECK(registry_calls == 1);
}

TEST_CASE("无部署档 = 显式零工具默认档:空表,不照搬终端工具") {
    SessionAssemblyRequest request = BaseRequest();
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    REQUIRE(result.assembly->registry != nullptr);
    CHECK(result.assembly->registry->All().empty());  // 没有 run_command/write_file 一类
    CHECK(result.assembly->mcp_servers.empty());
}

TEST_CASE("零工具档零启动:config 配了 MCP 也不碰") {
    SessionAssemblyRequest request = BaseRequest();
    config::Config config = MakeConfigWithServer("tools-approved", "no-such-command");
    HarnessProfile harness;
    harness.name = "zero";
    harness.tools.mode = HarnessToolPolicy::Mode::None;
    request.config = &config;
    request.harness = &harness;
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    CHECK(result.assembly->mcp_servers.empty());  // 合同 §2.3:不启动 MCP 握手
    REQUIRE(result.assembly->registry != nullptr);
    CHECK(result.assembly->registry->All().empty());
}

TEST_CASE("先解析再启动:必需服务缺上层配置即明拒") {
    SessionAssemblyRequest request = BaseRequest();
    config::Config config;  // 空 config:mcp_servers 一枚没有
    HarnessProfile harness = MakeOnlyProfile({"mcp:tools-approved:echo"}, {"tools-approved"});
    request.config = &config;
    request.harness = &harness;
    const auto result = AssembleSession(std::move(request));
    CHECK(result.assembly == nullptr);  // 明拒,不出半成品
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("必需服务起服失败整场明拒(command 不存在)") {
    SessionAssemblyRequest request = BaseRequest();
    config::Config config = MakeConfigWithServer("tools-approved", "definitely-no-such-command-xyz");
    HarnessProfile harness = MakeOnlyProfile({"mcp:tools-approved:echo"}, {"tools-approved"});
    request.config = &config;
    request.harness = &harness;
    const auto result = AssembleSession(std::move(request));
    CHECK(result.assembly == nullptr);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("backend 工厂缺失或交空件:装配明拒") {
    SUBCASE("工厂缺失") {
        SessionAssemblyRequest request;
        request.system_prompt = "x";
        const auto result = AssembleSession(std::move(request));
        CHECK(result.assembly == nullptr);
        CHECK_FALSE(result.error.empty());
    }
    SUBCASE("交空件") {
        SessionAssemblyRequest request;
        request.backend_factory = []() { return nullptr; };
        const auto result = AssembleSession(std::move(request));
        CHECK(result.assembly == nullptr);
        CHECK_FALSE(result.error.empty());
    }
    SUBCASE("注入 registry 工厂交空件") {
        SessionAssemblyRequest request = BaseRequest();
        request.registry_factory = []() { return nullptr; };
        const auto result = AssembleSession(std::move(request));
        CHECK(result.assembly == nullptr);
        CHECK_FALSE(result.error.empty());
    }
}

// ---------------------------------------------------------------------------
// P2(应用Worker接入单):Skill 工具、插件点名、Agent 提示组合
// ---------------------------------------------------------------------------

TEST_CASE("P2 skill 工具:features 放行且 allow 点名才进面,清单段同进同退") {
    const fs::path skills_root = MakeSkillsRoot("on");

    SUBCASE("放行+点名:skill 工具进注册表,清单段进提示") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness = MakeSkillProfile(/*with_skill_in_allow=*/true);
        request.harness = &harness;
        request.skills_root = skills_root;
        request.agent_plan = MakePlan();
        const auto result = AssembleSession(std::move(request));
        REQUIRE(result.assembly != nullptr);
        REQUIRE(result.assembly->registry != nullptr);
        CHECK(result.assembly->registry->Find("skill") != nullptr);
        // 提示来自组合管线:清单段与预装(无)在场,业务 persona 在场。
        const std::string& prompt = result.assembly->agent_profile.system_prompt;
        CHECK(prompt.find("greet") != std::string::npos);
        CHECK(prompt.find("部署材料根的 skills/ 目录") != std::string::npos);
        CHECK(prompt.find("你是 research") != std::string::npos);
        // 装配不混入终端执行工具(AW-08:装技能不开 shell)。
        CHECK(result.assembly->registry->Find("run_command") == nullptr);
        CHECK(result.assembly->registry->Find("read_file") == nullptr);
    }
    SUBCASE("放行但 allow 未点名:不起工具、不注清单段") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness = MakeSkillProfile(/*with_skill_in_allow=*/false);
        request.harness = &harness;
        request.skills_root = skills_root;
        request.agent_plan = MakePlan();
        const auto result = AssembleSession(std::move(request));
        REQUIRE(result.assembly != nullptr);
        REQUIRE(result.assembly->registry != nullptr);
        CHECK(result.assembly->registry->Find("skill") == nullptr);
        CHECK(result.assembly->agent_profile.system_prompt.find("greet") == std::string::npos);
    }
    SUBCASE("features 未放行:即使 allow 误点名也零装配(解析层另有拦)") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness = MakeSkillProfile(/*with_skill_in_allow=*/true);
        harness.features_enabled.clear();  // 撤掉 skills 放行
        request.harness = &harness;
        request.skills_root = skills_root;
        const auto result = AssembleSession(std::move(request));
        REQUIRE(result.assembly != nullptr);
        REQUIRE(result.assembly->registry != nullptr);
        CHECK(result.assembly->registry->Find("skill") == nullptr);
    }
    SUBCASE("未递 skills_root:不扫描不装(显式来源,无隐式面)") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness = MakeSkillProfile(/*with_skill_in_allow=*/true);
        request.harness = &harness;
        // 本用例不带 agent_plan:system_prompt 走显式件,原样进档案。
        const auto result = AssembleSession(std::move(request));
        REQUIRE(result.assembly != nullptr);
        REQUIRE(result.assembly->registry != nullptr);
        CHECK(result.assembly->registry->Find("skill") != nullptr);  // 内置件照进面
        CHECK(result.assembly->agent_profile.system_prompt == "test prompt");
    }
}

// P5(应用Worker接入单 §7.2):点名插件真装载。装载/信任/HTTP·Secret/寿命
// 的整链用例在 test_plugin_assembly.cpp(单子 P5 勾选点名的三件测试);
// 这里只钉装配路与 tools 面的衔接。

TEST_CASE("P5 插件点名:未递发现根,plugin_missing 整场明拒") {
    SessionAssemblyRequest request = BaseRequest();
    HarnessProfile harness;
    harness.name = "lua";
    harness.features_enabled.insert("plugins");
    harness.plugins = {"demo.lua-tool"};
    request.harness = &harness;
    // 不递 plugins_root:点名件无处发现,明拒不降级(P2 时此路是
    // component_unavailable;P5 接线后让位给真装载的失败码)。
    const auto result = AssembleSession(std::move(request));
    CHECK(result.assembly == nullptr);
    CHECK(result.error_code == "plugin_missing");
    REQUIRE_FALSE(result.error.empty());
    CHECK(result.error.find("demo.lua-tool") != std::string::npos);
    CHECK(result.error.find("plugin_missing") != std::string::npos);
}

TEST_CASE("P5 插件装配:装载面与注册面分家——点名装载,allow 决定出面") {
    const fs::path root = MakePluginsRoot("face");

    SUBCASE("allow 点名插件工具:adapter 进注册表,挂载快照记账") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness;
        harness.name = "lua";
        harness.tools.mode = HarnessToolPolicy::Mode::Only;
        harness.tools.allow = {"plugin__demo-lua__search"};
        harness.features_enabled.insert("plugins");
        harness.plugins = {"demo-lua"};
        request.harness = &harness;
        request.plugins_root = root;
        config::PluginTrustStore trust = TrustAllIn(root);
        request.plugin_trust = &trust;
        request.plugin_data_root = root / "data";
        const auto result = AssembleSession(std::move(request));
        REQUIRE(result.assembly != nullptr);
        REQUIRE(result.assembly->registry != nullptr);
        CHECK(result.assembly->registry->Find("plugin__demo-lua__search") != nullptr);
        REQUIRE(result.assembly->manifest_lua != nullptr);
        REQUIRE(result.assembly->manifest_lua->plugins().size() == 1);
        REQUIRE(result.assembly->mounted_plugins.size() == 1);
        CHECK(result.assembly->mounted_plugins[0] == "demo-lua@0.1.0");
        // 统一工具闸:外部代码一律先问,不走旁路(§7.2/§十)。
        tools::Tool* tool = result.assembly->registry->Find("plugin__demo-lua__search");
        REQUIRE(tool != nullptr);
        CHECK(tool->needs_confirm());
        CHECK(tool->approval_class() == tools::ApprovalClass::External);
    }
    SUBCASE("allow 未点名:装载照旧(点名=部署意志),工具零出面") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness;
        harness.name = "lua";
        harness.tools.mode = HarnessToolPolicy::Mode::Only;
        harness.tools.allow = {};  // 空表=零工具面,合法
        harness.features_enabled.insert("plugins");
        harness.plugins = {"demo-lua"};
        request.harness = &harness;
        request.plugins_root = root;
        config::PluginTrustStore trust = TrustAllIn(root);
        request.plugin_trust = &trust;
        request.plugin_data_root = root / "data";
        const auto result = AssembleSession(std::move(request));
        REQUIRE(result.assembly != nullptr);
        REQUIRE(result.assembly->registry != nullptr);
        CHECK(result.assembly->registry->All().empty());
        REQUIRE(result.assembly->manifest_lua != nullptr);  // 装载照做
        CHECK(result.assembly->mounted_plugins.size() == 1);
    }
    SUBCASE("mode=none:点名装载,零工具空表(与 MCP 起服同构)") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness;
        harness.name = "lua";
        harness.tools.mode = HarnessToolPolicy::Mode::None;
        harness.features_enabled.insert("plugins");
        harness.plugins = {"demo-lua"};
        request.harness = &harness;
        request.plugins_root = root;
        config::PluginTrustStore trust = TrustAllIn(root);
        request.plugin_trust = &trust;
        request.plugin_data_root = root / "data";
        const auto result = AssembleSession(std::move(request));
        REQUIRE(result.assembly != nullptr);
        REQUIRE(result.assembly->registry != nullptr);
        CHECK(result.assembly->registry->All().empty());
        REQUIRE(result.assembly->manifest_lua != nullptr);
    }
    SUBCASE("allow 点名不存在的插件工具:缺工具明拒,不静默降级") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness;
        harness.name = "lua";
        harness.tools.mode = HarnessToolPolicy::Mode::Only;
        harness.tools.allow = {"plugin__demo-lua__no_such_tool"};
        harness.features_enabled.insert("plugins");
        harness.plugins = {"demo-lua"};
        request.harness = &harness;
        request.plugins_root = root;
        config::PluginTrustStore trust = TrustAllIn(root);
        request.plugin_trust = &trust;
        request.plugin_data_root = root / "data";
        const auto result = AssembleSession(std::move(request));
        CHECK(result.assembly == nullptr);
        REQUIRE_FALSE(result.error.empty());
        CHECK(result.error.find("plugin__demo-lua__no_such_tool") != std::string::npos);
    }
    SUBCASE("点名 process 件:kind 未接线,component_unavailable 明拒") {
        WriteProcessPlugin(root / "v1proc");
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness;
        harness.name = "proc";
        harness.features_enabled.insert("plugins");
        harness.plugins = {"v1proc"};
        request.harness = &harness;
        request.plugins_root = root;
        config::PluginTrustStore trust = TrustAllIn(root);
        request.plugin_trust = &trust;
        request.plugin_data_root = root / "data";
        const auto result = AssembleSession(std::move(request));
        CHECK(result.assembly == nullptr);
        CHECK(result.error_code == "component_unavailable");
        REQUIRE_FALSE(result.error.empty());
        CHECK(result.error.find("v1proc") != std::string::npos);
    }
}

TEST_CASE("P2 agent_plan:提示部件组合进档案,能力段按注册表实际面开合") {
    const fs::path skills_root = MakeSkillsRoot("cap");

    SUBCASE("有工具面:mcp 能力段按面注入") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness = MakeSkillProfile(true);
        request.harness = &harness;
        request.skills_root = skills_root;
        request.agent_plan = MakePlan();
        const auto result = AssembleSession(std::move(request));
        REQUIRE(result.assembly != nullptr);
        const std::string& prompt = result.assembly->agent_profile.system_prompt;
        CHECK(prompt.find("你是 research") != std::string::npos);        // 档案 persona
        CHECK(prompt.find("命令行 AI 编程助手") == std::string::npos);   // 默认身份让位
        CHECK(prompt.find("/tmp/w") != std::string::npos);               // 运行环境段(宿主)
        CHECK(prompt.find("test prompt") == std::string::npos);          // 显式件被组合覆盖
    }
    SUBCASE("步数闸:与档案 runtime 取更严(0=不限)") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness = MakeSkillProfile(true);
        request.harness = &harness;
        request.skills_root = skills_root;
        request.max_steps_per_turn = 7;
        auto plan = MakePlan();
        plan->agent->max_steps_per_turn = 3;
        request.agent_plan = plan;
        const auto result = AssembleSession(std::move(request));
        REQUIRE(result.assembly != nullptr);
        CHECK(result.assembly->agent_profile.runtime.max_steps_per_turn == 3);  // min(7,3)
    }
    SUBCASE("preload 缺名:组合失败整场明拒") {
        SessionAssemblyRequest request = BaseRequest();
        HarnessProfile harness = MakeSkillProfile(false);  // skill 不进面 → 零扫描
        request.harness = &harness;
        request.skills_root = skills_root;
        request.agent_plan = MakePlan({"no-such-skill"});
        const auto result = AssembleSession(std::move(request));
        CHECK(result.assembly == nullptr);
        REQUIRE_FALSE(result.error.empty());
        CHECK(result.error.find("no-such-skill") != std::string::npos);
    }
}

TEST_CASE("P2 复验:allow 点名 skill 视为已装(内置件),不冒缺工具") {
    SessionAssemblyRequest request = BaseRequest();
    HarnessProfile harness = MakeSkillProfile(/*with_skill_in_allow=*/true);
    request.harness = &harness;
    request.skills_root = MakeSkillsRoot("reverify");
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    REQUIRE(result.assembly->registry != nullptr);
    CHECK(result.assembly->registry->Find("skill") != nullptr);
    CHECK(result.error.empty());
}

// ---------------------------------------------------------------------------
// 应用Worker接入单 §六(本批):components.skills 声明消费 + §7.1 MCP 子进程环境
// ---------------------------------------------------------------------------

namespace {

// 声明了 skills 的档(required/optional/source_dir 可组合)。
HarnessProfile MakeDeclaredSkillsProfile(std::vector<std::string> required,
                                         std::vector<std::string> optional = {},
                                         std::string source_dir = std::string()) {
    HarnessProfile profile;
    profile.name = "skills-declared";
    profile.tools.mode = HarnessToolPolicy::Mode::Only;
    profile.tools.allow = {"skill"};
    profile.features_enabled.insert("skills");
    profile.skills_required = std::move(required);
    profile.skills_optional = std::move(optional);
    profile.skills_source_dir = std::move(source_dir);
    return profile;
}

// 临时 skills 根,种两份技能:helper(带依赖声明)与 extra。
fs::path MakeDeclaredSkillsRoot(const std::string& tag) {
    static int counter = 0;
    const fs::path root = fs::temp_directory_path() /
                          ("lubancode_assembly_declared_" + tag + "_" + std::to_string(counter++));
    std::error_code ec;
    fs::create_directories(root / "helper", ec);
    {
        std::ofstream out(root / "helper" / "SKILL.md", std::ios::binary);
        out << "---\nname: helper\ndescription: 助手技能。\nrequires-tools:\n  - run_command\n---\nHELPER-BODY。\n";
    }
    fs::create_directories(root / "extra", ec);
    {
        std::ofstream out(root / "extra" / "SKILL.md", std::ios::binary);
        out << "---\nname: extra\ndescription: 附加技能。\n---\nEXTRA-BODY。\n";
    }
    return root;
}

}  // namespace

TEST_CASE("skills 声明消费:required 缺件整场明拒 skill_missing,人话带扫描警告") {
    const fs::path root = MakeDeclaredSkillsRoot("required-missing");
    SessionAssemblyRequest request = BaseRequest();
    HarnessProfile harness = MakeDeclaredSkillsProfile({"no-such-skill"}, {"extra"});
    request.harness = &harness;
    request.skills_root = root;
    const auto result = AssembleSession(std::move(request));
    CHECK(result.assembly == nullptr);
    CHECK(result.error_code == "skill_missing");
    REQUIRE_FALSE(result.error.empty());
    CHECK(result.error.find("no-such-skill") != std::string::npos);
    CHECK(result.error.find("skill_missing") != std::string::npos);
}

TEST_CASE("skills 声明消费:required 坏格式(扫描跳过)同样明拒,警告进人话") {
    static int counter = 0;
    const fs::path root = fs::temp_directory_path() /
                          ("lubancode_assembly_declared_broken_" + std::to_string(counter++));
    std::error_code ec;
    fs::create_directories(root / "broken", ec);
    {
        std::ofstream out(root / "broken" / "SKILL.md", std::ios::binary);
        out << "---\nname: broken\ndescription: 没闭合的 frontmatter\n正文。\n";  // 无闭合 ---
    }
    SessionAssemblyRequest request = BaseRequest();
    HarnessProfile harness = MakeDeclaredSkillsProfile({"broken"});
    request.harness = &harness;
    request.skills_root = root;
    const auto result = AssembleSession(std::move(request));
    CHECK(result.assembly == nullptr);
    CHECK(result.error_code == "skill_missing");
    CHECK(result.error.find("broken") != std::string::npos);
    CHECK(result.error.find("扫描警告") != std::string::npos);
}

TEST_CASE("skills 声明消费:optional 缺件降级记账,冻结清单如实交代") {
    const fs::path root = MakeDeclaredSkillsRoot("optional-missing");
    SessionAssemblyRequest request = BaseRequest();
    HarnessProfile harness = MakeDeclaredSkillsProfile({"helper"}, {"no-such-skill", "extra"});
    request.harness = &harness;
    request.skills_root = root;
    request.agent_plan = MakePlan();
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    // optional 缺件:诊断进降级账。
    REQUIRE(result.assembly->degraded_components.size() == 1);
    CHECK(result.assembly->degraded_components[0].find("no-such-skill") != std::string::npos);
    // 冻结清单:required/optional/装载状态/依赖声明与缺口。
    REQUIRE(result.assembly->skills_manifest.size() == 3);
    CHECK(result.assembly->skills_manifest[0].name == "helper");
    CHECK(result.assembly->skills_manifest[0].required);
    CHECK(result.assembly->skills_manifest[0].loaded);
    REQUIRE(result.assembly->skills_manifest[0].requires_tools.size() == 1);
    CHECK(result.assembly->skills_manifest[0].requires_tools[0] == "run_command");
    REQUIRE(result.assembly->skills_manifest[0].missing_tools.size() == 1);
    CHECK(result.assembly->skills_manifest[0].missing_tools[0] == "run_command");
    CHECK(result.assembly->skills_manifest[1].name == "no-such-skill");
    CHECK_FALSE(result.assembly->skills_manifest[1].required);
    CHECK_FALSE(result.assembly->skills_manifest[1].loaded);
    CHECK(result.assembly->skills_manifest[2].name == "extra");
    CHECK(result.assembly->skills_manifest[2].loaded);
    // 获准面过滤:根内还有未声明的技能吗——本根只有 helper/extra,都被声明;
    // 再验清单段只列获准两枚(prompt 检查)。
    const std::string& prompt = result.assembly->agent_profile.system_prompt;
    CHECK(prompt.find("helper") != std::string::npos);
    CHECK(prompt.find("extra") != std::string::npos);
    // 声明的依赖在面上没有:skill 工具按需加载时回 capability_unavailable
    //(单册外的 test_skills.cpp 钉;这里钉清单把缺口交代出来)。
}

TEST_CASE("skills 声明消费:名单外的根内技能不进本场(声明即允许清单)") {
    const fs::path root = MakeDeclaredSkillsRoot("filter");
    SessionAssemblyRequest request = BaseRequest();
    HarnessProfile harness = MakeDeclaredSkillsProfile({"helper"});  // extra 未声明
    request.harness = &harness;
    request.skills_root = root;
    request.agent_plan = MakePlan();
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    const std::string& prompt = result.assembly->agent_profile.system_prompt;
    CHECK(prompt.find("helper") != std::string::npos);
    CHECK(prompt.find("extra") == std::string::npos);  // 未声明不进清单段
    REQUIRE(result.assembly->skills_manifest.size() == 1);
    CHECK(result.assembly->skills_manifest[0].name == "helper");
}

TEST_CASE("skills 声明消费:sourceDir 声明来源根,扫描落在子目录") {
    static int counter = 0;
    const fs::path root = fs::temp_directory_path() /
                          ("lubancode_assembly_declared_srcdir_" + std::to_string(counter++));
    std::error_code ec;
    fs::create_directories(root / "team" / "inner-skill", ec);
    {
        std::ofstream out(root / "team" / "inner-skill" / "SKILL.md", std::ios::binary);
        out << "---\nname: inner-skill\ndescription: 子目录技能。\n---\nINNER-BODY。\n";
    }
    SessionAssemblyRequest request = BaseRequest();
    HarnessProfile harness = MakeDeclaredSkillsProfile({"inner-skill"}, {}, "team");
    request.harness = &harness;
    request.skills_root = root;
    request.agent_plan = MakePlan();
    const auto result = AssembleSession(std::move(request));
    REQUIRE(result.assembly != nullptr);
    const std::string& prompt = result.assembly->agent_profile.system_prompt;
    CHECK(prompt.find("inner-skill") != std::string::npos);
    CHECK(result.error.empty());
}

TEST_CASE("MCP 子进程环境:base 集从宿主取,配置注入覆盖,密钥不递(§7.1)") {
    // 在宿主环境造一枚"模型密钥",ComposeMcpChildEnv 不得把它递给工具进程。
    struct EnvGuard {
        explicit EnvGuard(const char* name) : name_(name) {}
        ~EnvGuard() {
#ifdef _WIN32
            _putenv((std::string(name_) + "=").c_str());
#else
            unsetenv(name_);
#endif
        }
        void set(const std::string& value) {
#ifdef _WIN32
            _putenv((std::string(name_) + "=" + value).c_str());
#else
            setenv(name_, value.c_str(), 1);
#endif
        }
        const char* name_;
    } guard("LUBANCODE_TEST_MODEL_KEY");
    guard.set("sk-model-secret");

    const auto env = ComposeMcpChildEnv({{"LUBANCODE_TEST_TOOL_CRED", "tok-123"},
                                         {"PATH", "/cfg/override/path"}});
    const auto find = [&env](const std::string& key) -> const std::string* {
        for (const auto& [k, v] : env) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    };
    // 工具凭据(部署配置注入)在。
    const std::string* cred = find("LUBANCODE_TEST_TOOL_CRED");
    REQUIRE(cred != nullptr);
    CHECK(*cred == "tok-123");
    // base 集:PATH 在,且被配置同名覆盖。
    const std::string* path = find("PATH");
    REQUIRE(path != nullptr);
    CHECK(*path == "/cfg/override/path");
    // 模型密钥(宿主环境的其余变量)不递。
    CHECK(find("LUBANCODE_TEST_MODEL_KEY") == nullptr);
    CHECK(find("LUBAN_API_KEY") == nullptr);
}
