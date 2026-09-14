// 会话级运行材料(工业化多协议接入单 P1)——冻结合同 §7 RuntimeBundle 的
// 最小形状:一场 thread 一份,同场多轮复用;先解析允许组件、再启动,不
// 留"先启动再过滤"。
//
// 这不是 LubanCore 单阶段 B 的中立 RuntimeAssembly 服务(§2.1 owner 账:
// 那套抽 SessionFactory/Theme 退役归 Core-B)。这里只服务 app-server 的
// 生产装配缺口(G01/G02):显式选择工具与 Agent 档案、复用同场资源、
// 缺授权/缺工具/依赖启动失败明拒。
//
// 装配序(冻结合同 §7.1 的最小落地;P2 起插三步;P5 再接插件):
//   0. components.plugins 点名的插件进入真装载(P5,应用Worker接入单
//      §7.2):发现根扫描 -> 点名对账 -> 信任账 -> Lua 挂载(v2
//      embedded-lua;ManifestLuaRuntime,不另造 manifest 格式)。缺件
//      plugin_missing、信任账不过 plugin_untrusted、Lua 装载坏
//      plugin_load_failed、点名 process/native 件 component_unavailable
//      ——四路都整场明拒,零副作用不降级;
//   1. 计划已解析(HarnessProfile 纯数据,解析在 harness_profile.hpp,
//      零外部启动副作用);
//   2. 依赖解释:tools.allow 引用的每个 MCP 服务须在 config.mcp_servers
//      里配置(上层获准的现行判法)——缺即明拒,不起任何进程;
//   2.5. Skill 材料:features.skills 放行且 tools 面点名 "skill" 才按
//      显式单根(skills_root)扫描(§六;GAP-03 不搬终端五层合并);
//   3. 只启动计划点名的 MCP 服务(config.mcp_servers ∩ components.
//      mcpServers):tools.allow 引用(必需)的起失败=整场明拒;未被
//      引用(可选)的起失败=降级记账、继续(Profile 的工具选择决定该
//      组件能否降级);
//   4. 握手 tools/list 后复验:mode=only 的每枚 allow 名单必须在握手
//      清单里——缺即明拒("缺工具"不许静默降级);
//   5. 注册表只装 allow 点名的工具(内置 skill 工具同规矩);mode=none/
//      inherit 落零工具空表(合同 §2.3 无工具会话:不启动 MCP、不挂发现器);
//   6. Agent 档案显式:agent_plan 在场时系统提示由提示部件组合产出
//      (agent_wiring.hpp,prompt_assembler 既有管线),步数闸与档案
//      runtime 取更严;缺席时 system_prompt 与步数闸由调用方显式给。
//
// 寿命规矩(§7.2/§7.3 的最小落地):MCP 子进程持有者先声明(后析构),
// 注册表后声明(先析构)——McpTool 持 Client&。同场多轮共用,不每轮
// 重建注册表把旧 Tool 指针留给后台任务。
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"  // AgentProfile
#include "api/backend.hpp"
#include "app_server/agent_wiring.hpp"  // HarnessAgentPlan:P2 Agent/Skill 装配计划
#include "app_server/harness_profile.hpp"
#include "config/config.hpp"
#include "config/plugin_trust.hpp"  // PluginTrustStore:P5 插件信任账
#include "mcp/client.hpp"
#include "runtime/plugin_contract.hpp"     // PluginManifest(P5 装载件)
#include "runtime/plugin_http.hpp"         // BoundedHttpTransport(P5 注入缝)
#include "runtime/plugin_lua_manifest.hpp"  // ManifestLuaRuntime:P5 Lua owner
#include "runtime/secret_resolver.hpp"     // SecretResolver(P5 注入缝)
#include "tools/registry.hpp"
#include "tools/skill_tool.hpp"

namespace lubancode::app_server {

// 一枚已起服的 MCP 服务(headless 持有形状;与 app::McpServerRuntime
// 同构,app_server 不引 app 层的 tool_runtime——那套拖终端 Theme)。
struct HeadlessMcpRuntime {
    std::string name;
    std::unique_ptr<lubancode::mcp::Client> client;  // 拥有者:McpTool 持它的引用
    std::vector<lubancode::mcp::ToolInfo> tools;
};

// 一场 thread 的运行材料。成员序=寿命序:拥有者(backend、MCP 子进程)
// 在前,注册表在后——析构反序,注册表里的 McpTool 先亡,Client 引用
// 不悬垂。
struct SessionAssembly {
    // 冻结技能清单的一条(§六:来源声明与依赖状态供客户端检查)。只在
    // 部署档声明了 components.skills 时填——缺省(P2 约定:材料根全量)
    // 不出清单,不冒充声明过。
    struct SkillManifestEntry {
        std::string name;         // canonical ID
        bool required = false;    // required(否则 optional)
        bool loaded = false;      // 扫到并进本场获准清单(optional 缺件 false)
        std::vector<std::string> requires_tools;      // frontmatter 依赖声明
        std::vector<std::string> missing_tools;       // 声明了但本场面上没有的
    };
    std::unique_ptr<lubancode::api::Backend> backend;
    lubancode::agent::AgentProfile agent_profile;
    // 装配降级账:可选组件(tools.allow 未引用)起失败被跳过的事实。
    // "可选降级必须写结果"(单子 P1):这份账进 thread/started 事件与
    // 诊断,不悄悄咽下。
    std::vector<std::string> degraded_components;
    std::vector<HeadlessMcpRuntime> mcp_servers;        // 拥有者:先于 registry
    // P5:点名 Lua 插件的挂载 owner(v2 manifest-backed embedded-lua)。
    // 每场装配现造一份——Lua state 不跨会话,随本对象析构关闭。成员序=
    // 寿命序:在 registry 之前声明(先析构),registry 里的 adapter 持
    // ManifestLuaPlugin 裸指针,注册表先撤、owner 后收口,不悬垂。
    std::unique_ptr<lubancode::runtime::ManifestLuaRuntime> manifest_lua;
    // 挂载快照:本场真装上的插件("<id>@<version>" 一件一条)。点名=部署
    // 者显式意志,装上与否要看得见(§7.2),不悄悄咽下。
    std::vector<std::string> mounted_plugins;
    // §六 冻结技能清单(components.skills 声明时才有;见 SkillManifestEntry)。
    std::vector<SkillManifestEntry> skills_manifest;
    // §五 提示组合的可追溯记录(prompt.composition.applied 的 payload:
    // 组合次序/各段 hash/来源/最终快照 ID)。agent_plan 组合路才填;
    // server 在 v3 场把它落进会话账(内存件只是搬运,不再自造账)。
    std::optional<nlohmann::json> prompt_composition;
    std::unique_ptr<lubancode::tools::ToolRegistry> registry;  // 用户面:后声明
};

struct SessionAssemblyResult {
    std::unique_ptr<SessionAssembly> assembly;  // 空 = 装配失败(明拒)
    std::string error;                          // 失败人话(诊断与事件共用)
    // 稳定错误码(空 = 通用装配失败,server 落 assembly.failed)。在册值:
    //   component_unavailable —— 点名的插件是当前 build 未接线的 runtime
    //     kind(process/native;P2 时整层未接线,P5 起 v2 embedded-lua 真装
    //     载,此码收窄到 kind 面)
    //   plugin_missing —— 点名件不在发现账(根缺席/目录没有/manifest 坏
    //     被扫描剔除;人话带扫描警告摘要)
    //   plugin_untrusted —— 信任账不过(未信任/被禁用/内容指纹算不出)
    //   plugin_load_failed —— Lua 挂载失败(entry 读不到/编译坏/handler
    //     对账不过)
    //   skill_missing —— components.skills 声明的 required 技能不在扫描
    //     账(缺文件/坏格式被扫描跳过;人话带扫描警告摘要,§六)
    // 经 thread/start 错误信封带出(message 含码,data.code additive,
    // 与 component_unavailable 同一条 P2 约定)。
    std::string error_code;
};

// MCP 子进程环境(§7.1:凭据分开传,不递 Worker 全环境)。base 集 =
// 进程基件(PATH/系统变量/临时目录,plugin_process 同一张合同的口径)
// 从宿主环境取值;server_env 是部署配置(config.json 的 mcpServers.env,
// 工具自己的凭据走这里)注入,同名覆盖 base。装配方以 EnvMode::Replace
// 落锤——宿主环境的其余变量(含模型 API key)一概不递。单测可注入:
// 只依赖 GetEnvVar,无进程副作用。
std::vector<std::pair<std::string, std::string>> ComposeMcpChildEnv(
    const std::vector<std::pair<std::string, std::string>>& server_env);

struct SessionAssemblyRequest {
    // 生产配置(mcp_servers 的上层获准来源);空 = 测试注入路(无 MCP)。
    const lubancode::config::Config* config = nullptr;
    // 部署档计划(纯数据,已解析);空 = 显式零工具默认档(合同 §2.3:
    // 生产未递档不照搬终端全部工具,也不拿空工厂充当已接好)。
    const HarnessProfile* harness = nullptr;
    // backend 工厂(生产 BuildBackend/测试假件)。空 = 装配失败。
    std::function<std::unique_ptr<lubancode::api::Backend>()> backend_factory;
    // 显式注册表(测试注入假工具表);生产留空=按计划装配(只装 allow
    // 点名的 MCP 工具)。给了它就不再碰 MCP(注入路的工具面由测试定)。
    std::function<std::unique_ptr<lubancode::tools::ToolRegistry>()> registry_factory;
    // 显式 Agent 档案材料:system_prompt 与步数闸(调用方定,G02 的
    // "明确的 Agent 档案"不在装配里猜)。
    std::string system_prompt;
    int max_steps_per_turn = 0;
    // ---- P2(应用Worker接入单 §五/§六)----
    // 显式 Skill 来源根(材料根 skills/;GAP-03)。features.skills 放行
    // 且 tools 面点名 "skill" 时才扫描、装 skill 工具、注清单段——单根
    // 显式扫描,不搬终端五层合并。空 = 不装。
    std::optional<std::filesystem::path> skills_root;
    // 启动冻结的 Agent 装配计划(RunAppServerMode 解析 agentRef 的结果,
    // GAP-01/02)。非空时 system_prompt 由提示部件组合产出(prompt_assembler
    // 既有管线,能力段按本场注册表实际面开合),步数闸再与档案 runtime
    // 取更严;空时沿用上面的 system_prompt 显式件(无档默认路)。
    std::shared_ptr<const HarnessAgentPlan> agent_plan;
    // ---- P5(应用Worker接入单 §7.2)----
    // 插件发现根(材料根 plugins/;ScanPluginDirectories 一插件一目录的
    // plugin.json 扫描账,与终端同一发现面,不另造清单)。components.
    // plugins 点名 ∧ 根在场才装载;根缺席时点名件无处发现,plugin_missing
    // 明拒(不静默降级)。信任账为 nullptr 时全按未信任处理——点名即拒,
    // 同 plugin_tool.hpp 的既有语义。
    std::optional<std::filesystem::path> plugins_root;
    const lubancode::config::PluginTrustStore* plugin_trust = nullptr;
    // 插件数据根(.env 的家;<root>/<id> 一件一目录)。生产递数据根的
    // plugin-data;缺省走 StandalonePluginDataDir(状态根)。测试递临时
    // 目录,不碰真实家目录。
    std::optional<std::filesystem::path> plugin_data_root;
    // 插件宿主件的测试注入口(生产留空 = EnvDotEnv resolver + Cpr 受控
    // 传输,与终端 ManifestLuaRuntime 同一套生产件,不另立第二套 seam)。
    std::function<std::unique_ptr<lubancode::runtime::BoundedHttpTransport>(
        const lubancode::runtime::PluginManifest&)>
        plugin_transport_factory;
    std::function<std::unique_ptr<lubancode::runtime::SecretResolver>(
        const lubancode::runtime::PluginManifest&)>
        plugin_resolver_factory;
};

// 装配一场会话的运行材料。任何一步失败回空 assembly + 人话 error,
// 不出半成品(候选资源随栈析构清理,合同 §7.2)。
SessionAssemblyResult AssembleSession(SessionAssemblyRequest request);

}  // namespace lubancode::app_server
