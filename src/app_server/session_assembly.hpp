// 会话级运行材料(工业化多协议接入单 P1)——冻结合同 §7 RuntimeBundle 的
// 最小形状:一场 thread 一份,同场多轮复用;先解析允许组件、再启动,不
// 留"先启动再过滤"。
//
// 这不是 LubanCore 单阶段 B 的中立 RuntimeAssembly 服务(§2.1 owner 账:
// 那套抽 SessionFactory/Theme 退役归 Core-B)。这里只服务 app-server 的
// 生产装配缺口(G01/G02):显式选择工具与 Agent 档案、复用同场资源、
// 缺授权/缺工具/依赖启动失败明拒。
//
// 装配序(冻结合同 §7.1 的最小落地;P2 起插三步):
//   0. 点名未接线组件(components.plugins)即拒:component_unavailable,
//      零副作用(应用Worker接入单 §7.2);
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
#include <vector>

#include "agent/agent.hpp"  // AgentProfile
#include "api/backend.hpp"
#include "app_server/agent_wiring.hpp"  // HarnessAgentPlan:P2 Agent/Skill 装配计划
#include "app_server/harness_profile.hpp"
#include "config/config.hpp"
#include "mcp/client.hpp"
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
    std::unique_ptr<lubancode::api::Backend> backend;
    lubancode::agent::AgentProfile agent_profile;
    // 装配降级账:可选组件(tools.allow 未引用)起失败被跳过的事实。
    // "可选降级必须写结果"(单子 P1):这份账进 thread/started 事件与
    // 诊断,不悄悄咽下。
    std::vector<std::string> degraded_components;
    std::vector<HeadlessMcpRuntime> mcp_servers;        // 拥有者:先于 registry
    std::unique_ptr<lubancode::tools::ToolRegistry> registry;  // 用户面:后声明
};

struct SessionAssemblyResult {
    std::unique_ptr<SessionAssembly> assembly;  // 空 = 装配失败(明拒)
    std::string error;                          // 失败人话(诊断与事件共用)
    // 稳定错误码(空 = 通用装配失败,server 落 assembly.failed)。在册值:
    //   component_unavailable —— 部署档点名当前 build 未接线的组件
    //   (单子 §7.2 Lua 插件;P5 接线后此码让位给真装载的失败码)。
    std::string error_code;
};

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
};

// 装配一场会话的运行材料。任何一步失败回空 assembly + 人话 error,
// 不出半成品(候选资源随栈析构清理,合同 §7.2)。
SessionAssemblyResult AssembleSession(SessionAssemblyRequest request);

}  // namespace lubancode::app_server
