// session_assembly.hpp 的实现:app-server 生产装配(G01/G02 修复的
// 落点)。MCP 起服与注册复用 mcp::Client/McpTool 同一套底层件;与终端
// 路(app::StartMcpServers)的差别是显式的:终端宽容(起失败打一行警
// 告跳过),headless 生产按计划明拒——必需组件(tools.allow 引用)起
// 失败整场拒,可选组件降级记账。
#include "app_server/session_assembly.hpp"

#include <algorithm>
#include <utility>

#include "mcp/mcp_tool.hpp"

namespace lubancode::app_server {

namespace {

// tools.allow 的 canonical 名(mcp:<server>:<tool>)与握手清单对账用的
// 谓词:同一 server 下同名工具。
bool AllowNameMatches(const std::string& canonical, const std::string& server, const std::string& tool) {
    const std::string prefix = "mcp:" + server + ":";
    return canonical == prefix + tool;
}

}  // namespace

SessionAssemblyResult AssembleSession(SessionAssemblyRequest request) {
    SessionAssemblyResult result;
    if (!request.backend_factory) {
        result.error = "装配失败:backend 工厂缺失(headless 会话必须有显式 backend)";
        return result;
    }

    // ---- 步骤 1:计划解读(纯数据,零启动副作用)----
    // 无部署档 = 显式零工具默认档:不照搬终端全部工具,不拿空表冒充
    // 已接好(零工具是合法档,合同 §2.3)。注入路(registry_factory)
    // 的工具面由测试定,装配不碰 MCP。
    const bool injection_path = request.registry_factory != nullptr;
    const HarnessProfile* harness = request.harness;
    const bool plan_uses_mcp = !injection_path && harness != nullptr && harness->FeatureEnabled("mcp") &&
                               !harness->mcp_servers.empty();

    // ---- 步骤 2:依赖解释(tools.allow 引用的服务须已在上层配置)----
    std::vector<std::pair<std::string, const lubancode::config::McpServerConfig*>> allowed_servers;
    if (plan_uses_mcp && request.config != nullptr) {
        const std::set<std::string> referenced = harness->ReferencedMcpServers();
        for (const std::string& name : harness->mcp_servers) {
            const auto configured = request.config->mcp_servers.find(name);
            if (configured == request.config->mcp_servers.end()) {
                // 必需(被 tools.allow 引用)缺配置=明拒;可选=没得装,
                // 降级记账。两层都基于 Profile 的工具选择,不由装配猜。
                if (referenced.count(name) > 0) {
                    result.error = "装配失败:档点名必需的 MCP 服务未在上层配置获准: " + name;
                    return result;
                }
                // 走到这里的名字没进 config:降级账在步骤 3 统一记。
                continue;
            }
            allowed_servers.emplace_back(name, &configured->second);
        }
    }

    auto assembly = std::make_unique<SessionAssembly>();

    // ---- 步骤 3:backend ----
    assembly->backend = request.backend_factory();
    if (assembly->backend == nullptr) {
        result.error = "装配失败:backend 工厂交回空件";
        return result;
    }

    // ---- 步骤 4:按计划启动 MCP(只起点名的,不起 config 全量)----
    if (plan_uses_mcp && request.config != nullptr) {
        const std::set<std::string> referenced = harness->ReferencedMcpServers();
        std::set<std::string> mounted_names;
        for (auto& [name, server_config] : allowed_servers) {
            auto client = std::make_unique<lubancode::mcp::Client>(name);
            const auto start = client->StartProcess(server_config->command, server_config->args,
                                                    server_config->env);
            std::string reason;
            if (start.success) {
                const auto initialized = client->Initialize();
                if (!initialized.has_value()) {
                    reason = initialized.error();
                }
            } else {
                reason = start.error;
            }
            if (!reason.empty()) {
                if (referenced.count(name) > 0) {
                    result.error = "装配失败:必需 MCP 服务起服失败,整场拒绝: " + name + "(" + reason + ")";
                    return result;  // 候选资源随栈析构清理,不出半成品
                }
                assembly->degraded_components.push_back(name + ": " + reason);
                continue;  // 可选降级:记账继续,不冒充已挂
            }
            auto tools_result = client->ListTools();
            if (!tools_result.has_value()) {
                if (referenced.count(name) > 0) {
                    result.error =
                        "装配失败:必需 MCP 服务工具清单拉取失败,整场拒绝: " + name +
                        "(" + tools_result.error() + ")";
                    return result;
                }
                assembly->degraded_components.push_back(name + ": tools/list " + tools_result.error());
                continue;
            }
            HeadlessMcpRuntime runtime;
            runtime.name = name;
            runtime.tools = std::move(*tools_result);
            runtime.client = std::move(client);
            mounted_names.insert(name);
            assembly->mcp_servers.push_back(std::move(runtime));
        }
        // config 里有、档也点名了、但依赖解释步已跳过的(未获准)记降级账。
        for (const std::string& name : harness->mcp_servers) {
            if (request.config->mcp_servers.count(name) == 0 && mounted_names.count(name) == 0 &&
                referenced.count(name) == 0) {
                assembly->degraded_components.push_back(name + ": 上层配置未获准,未启动");
            }
        }
    }

    // ---- 步骤 5:注册表(一次性装齐;只装 allow 点名的工具)----
    if (injection_path) {
        assembly->registry = request.registry_factory();
        if (assembly->registry == nullptr) {
            result.error = "装配失败:注入的注册表工厂交回空件";
            return result;
        }
    } else if (harness != nullptr && harness->tools.mode == HarnessToolPolicy::Mode::Only &&
               !harness->tools.allow.empty()) {
        auto registry = std::make_unique<lubancode::tools::ToolRegistry>();
        for (HeadlessMcpRuntime& runtime : assembly->mcp_servers) {
            for (const auto& tool_info : runtime.tools) {
                const std::string canonical = "mcp:" + runtime.name + ":" + tool_info.name;
                const bool allowed = std::find(harness->tools.allow.begin(), harness->tools.allow.end(),
                                               canonical) != harness->tools.allow.end();
                if (!allowed) {
                    continue;  // 不装:发现面里就没有(deny 裁过的名单已在解析侧)
                }
                registry->Register(std::make_unique<lubancode::mcp::McpTool>(
                    *runtime.client, runtime.name, tool_info, std::string()));
            }
        }
        // 复验(步骤 4 的另一半):mode=only 的每枚 allow 名单必须真的装上
        // ——握手清单里没有就是"缺工具",明拒,不静默降级。
        for (const std::string& canonical : harness->tools.allow) {
            bool mounted = false;
            for (const HeadlessMcpRuntime& runtime : assembly->mcp_servers) {
                for (const auto& tool_info : runtime.tools) {
                    if (AllowNameMatches(canonical, runtime.name, tool_info.name)) {
                        mounted = true;
                        break;
                    }
                }
                if (mounted) {
                    break;
                }
            }
            if (!mounted) {
                result.error = "装配失败:档点名的工具在服务握手清单里不存在,整场拒绝: " + canonical;
                return result;
            }
        }
        assembly->registry = std::move(registry);
    } else {
        // 零工具面(none / only+空 allow / inherit / 无档):显式空表。
        // 合同 §2.3:不挂 tool_search/tool_invoke、不注提示段——这里
        // 本来就没装任何东西,空表如实空。
        assembly->registry = std::make_unique<lubancode::tools::ToolRegistry>();
    }

    // ---- 步骤 6:Agent 档案(显式材料,装配不猜)----
    assembly->agent_profile.system_prompt = request.system_prompt;
    assembly->agent_profile.runtime.max_steps_per_turn = request.max_steps_per_turn;

    result.assembly = std::move(assembly);
    return result;
}

}  // namespace lubancode::app_server
