// session_assembly.hpp 的实现:app-server 生产装配(G01/G02 修复的
// 落点)。MCP 起服与注册复用 mcp::Client/McpTool 同一套底层件;与终端
// 路(app::StartMcpServers)的差别是显式的:终端宽容(起失败打一行警
// 告跳过),headless 生产按计划明拒——必需组件(tools.allow 引用)起
// 失败整场拒,可选组件降级记账。
// P5(应用Worker接入单 §7.2):components.plugins 点名的 v2 embedded-lua
// 插件走真装载——复用 ManifestLuaRuntime/LuaHostState/PluginTrustStore
// 既有件(发现面、manifest 格式、信任账、HTTP/Secret 执法都不另造第二
// 套),装载序见步骤 0 的 PluginMounter。
#include "app_server/session_assembly.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include "mcp/mcp_tool.hpp"
#include "platform/paths.hpp"
#include "runtime/plugin_tool.hpp"  // ScanPluginDirectories/ComputePluginContentHash
#include "runtime/secret_resolver.hpp"  // StandalonePluginDataDir
#include "tools/skill_loader.hpp"

namespace lubancode::app_server {

namespace {

// tools.allow 的 canonical 名(mcp:<server>:<tool>)与握手清单对账用的
// 谓词:同一 server 下同名工具。
bool AllowNameMatches(const std::string& canonical, const std::string& server, const std::string& tool) {
    const std::string prefix = "mcp:" + server + ":";
    return canonical == prefix + tool;
}

// P5:点名插件的装载序(发现 -> 对账 -> 信任 -> 挂载)。任何一步失败
// 整场明拒(稳定码见 SessionAssemblyResult::error_code),不出半成品。
// 复用 ManifestLuaRuntime/LuaHostState 既有宿主件——HTTP/Secret 的调用期
// 执法(网络账对账、Secret 只在调用作用域解析)都在那套件里,这里不另
// 立第二套权限面。
//
// 语义钉子(与 MCP 的差异要说清):components.plugins 点名是部署者的
// 显式意志,不是目录隐式发现——点名即必需件,四路失败(缺件/未信任/
// kind 未接线/装载坏)都整场拒,没有"可选降级"。MCP 的可选性来自
// tools.allow 未引用的档内组合;插件没有这种隐式发现面,部署档点了名
// 就是要它在场。
class PluginMounter {
public:
    PluginMounter(const SessionAssemblyRequest& request, SessionAssemblyResult& result)
        : request_(request), result_(result) {}

    // 装配步骤 0 的执行体。装配成功且有点名件时回非空 owner(调用方
    // 持进 SessionAssembly);零点名件/未放行/注入路回 nullptr 且不写错;
    // 失败已写 result_(人话+稳定码)并回 nullptr——调用方按 error 非空
    // 收口整场拒。
    std::unique_ptr<lubancode::runtime::ManifestLuaRuntime> Mount() {
        const HarnessProfile* harness = request_.harness;
        const bool plan_uses_plugins = request_.registry_factory == nullptr && harness != nullptr &&
                                       !harness->plugins.empty() && harness->FeatureEnabled("plugins");
        if (!plan_uses_plugins) {
            return nullptr;  // 注入路工具面由测试定;无点名件=无插件面
        }
        if (!request_.plugins_root.has_value()) {
            Fail("plugin_missing", "部署档点名插件但装配未递发现根(材料根 plugins/),无处发现: " +
                                        JoinNames(*harness));
            return nullptr;
        }
        // 发现:与终端同一扫描面(一插件一目录 plugin.json),不另造清单。
        const lubancode::runtime::PluginScanResult scanned =
            lubancode::runtime::ScanPluginDirectories(*request_.plugins_root);
        std::map<std::string, const std::shared_ptr<const lubancode::runtime::PluginManifest>*> by_id;
        for (const auto& manifest : scanned.manifests) {
            by_id.emplace(manifest->id, &manifest);
        }
        auto owner = std::make_unique<lubancode::runtime::ManifestLuaRuntime>();
        for (const std::string& id : harness->plugins) {
            if (!mounted_ids_.insert(id).second) {
                Fail("plugin_missing", "点名名单重复(同一 id 两遍会装出两份 state、工具撞名): " + id);
                return nullptr;
            }
            const auto found = by_id.find(id);
            if (found == by_id.end()) {
                // manifest 坏的件在扫描期已被剔出账(警告带目录名)——点名
                // 它同样按缺件拒,人话带扫描警告摘要帮诊断。
                std::string detail = "点名插件不在发现账: " + id;
                if (!scanned.warnings.empty()) {
                    detail += "(扫描警告: ";
                    for (const std::string& warning : scanned.warnings) {
                        detail += warning + "; ";
                    }
                    detail += ")";
                }
                Fail("plugin_missing", std::move(detail));
                return nullptr;
            }
            const lubancode::runtime::PluginManifest& manifest = **found->second;
            // kind 门:P5 只接 v2 embedded-lua;process/native 未接线
            // app-server 装配,照旧 component_unavailable 明拒(§7.2
            // "不支持即拒绝"收窄到 kind 面,不冒充已装载)。
            if (manifest.kind != lubancode::runtime::RuntimeKind::EmbeddedLua) {
                Fail("component_unavailable",
                     "点名插件 " + id + " 的 runtime kind(" +
                         std::string(lubancode::runtime::RuntimeKindName(manifest.kind)) +
                         ")未接线 app-server 装配(component_unavailable);P5 已接:embedded-lua");
                return nullptr;
            }
            // 信任门:托管信任来自预先部署的 hash 与策略(§7.2),装配
            // 只读账不批信任。disabled 也算信任面不过(人话点明)。
            const auto content_hash = lubancode::runtime::ComputePluginContentHash(manifest.plugin_dir);
            if (!content_hash.has_value()) {
                Fail("plugin_untrusted",
                     "点名插件 " + id + " 内容指纹算不出,无从验信任: " + content_hash.error());
                return nullptr;
            }
            const std::string dir_utf8 = lubancode::platform::PathToUtf8(manifest.plugin_dir);
            if (request_.plugin_trust != nullptr &&
                request_.plugin_trust->IsDisabled(dir_utf8, *content_hash)) {
                Fail("plugin_untrusted", "点名插件 " + id + " 已在信任账里标了 disable,整场拒绝");
                return nullptr;
            }
            if (request_.plugin_trust == nullptr ||
                !request_.plugin_trust->IsTrusted(dir_utf8, *content_hash)) {
                Fail("plugin_untrusted",
                     "点名插件 " + id + " 未过信任账(项目目录里的插件是外来代码,放进目录就是执行"
                         "代码;批准:/plugin trust " + id + ",或预置信任账后重启)");
                return nullptr;
            }
            // 挂载:读 entry -> 顶层零副作用加载 -> handler 对账。测试注
            // 入口(transport/resolver)优先;生产走 EnvDotEnv + Cpr 受控
            // 传输,与终端同一套生产件。
            lubancode::runtime::ManifestLuaLoadOptions options;
            options.plugin_data_dir =
                request_.plugin_data_root.has_value()
                    ? *request_.plugin_data_root /
                          lubancode::platform::Utf8ToPath(manifest.id)
                    : lubancode::runtime::StandalonePluginDataDir(manifest.id);
            if (request_.plugin_transport_factory != nullptr) {
                options.transport = request_.plugin_transport_factory(manifest);
            }
            if (request_.plugin_resolver_factory != nullptr) {
                options.resolver = request_.plugin_resolver_factory(manifest);
            }
            auto plugin = lubancode::runtime::LoadManifestLuaPlugin(*found->second, std::move(options));
            if (!plugin.has_value()) {
                Fail("plugin_load_failed",
                     "点名插件 " + id + " Lua 挂载失败: " + plugin.error());
                return nullptr;
            }
            owner->Adopt(std::move(*plugin));
            mounted_.push_back(manifest.id + "@" + manifest.version);
        }
        return owner;
    }

    const std::vector<std::string>& mounted() const { return mounted_; }

private:
    void Fail(const char* code, std::string message) {
        result_.error_code = code;
        result_.error = "部署档点名插件组件,装配整场拒绝(" + std::string(code) + "): " +
                        std::move(message);
    }

    static std::string JoinNames(const HarnessProfile& harness) {
        std::string names;
        for (const std::string& plugin : harness.plugins) {
            if (!names.empty()) {
                names += ", ";
            }
            names += plugin;
        }
        return names;
    }

    const SessionAssemblyRequest& request_;
    SessionAssemblyResult& result_;
    std::vector<std::string> mounted_;
    std::set<std::string> mounted_ids_;
};

}  // namespace

SessionAssemblyResult AssembleSession(SessionAssemblyRequest request) {
    SessionAssemblyResult result;
    if (!request.backend_factory) {
        result.error = "装配失败:backend 工厂缺失(headless 会话必须有显式 backend)";
        return result;
    }

    // ---- 步骤 0:P5,点名插件真装载(发现->对账->信任->挂载)----
    // P2 时整层未接线(component_unavailable 一律拒);P5 起 v2
    // embedded-lua 真装载,四路失败(缺件/未信任/kind 未接线/装载坏)
    // 整场明拒、零副作用,错误码冻结见 docs/reference/capability-contract.md
    // §13.4。装载序在一切启动副作用之前(backend/MCP 都还没起),失败
    // 零清理负担。注入路(registry_factory)不碰插件——与不碰 MCP 同一条
    // 规矩,工具面由测试定;features 未放行 plugins 时零装载零拒绝
    //(解析层已把"点名但未放行"当配置错误拒了,这里只是防御,与 skill
    // 的兜底同款)。
    const bool injection_path = request.registry_factory != nullptr;
    PluginMounter mounter(request, result);
    auto manifest_lua = mounter.Mount();
    if (!result.error.empty()) {
        return result;  // 装载失败:人话+稳定码已写,整场拒
    }

    // ---- 步骤 1:计划解读(纯数据,零启动副作用)----
    // 无部署档 = 显式零工具默认档:不照搬终端全部工具,不拿空表冒充
    // 已接好(零工具是合法档,合同 §2.3)。注入路(registry_factory)
    // 的工具面由测试定,装配不碰 MCP。
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
    // P5:插件 owner 与挂载快照移交本场材料。成员序=寿命序(hpp):owner
    // 在 registry 之前声明,析构反序——registry 里的 adapter 先亡,owner
    // 后关 Lua state,裸指针不悬垂。每场装配现造一份,state 不跨会话。
    assembly->manifest_lua = std::move(manifest_lua);
    assembly->mounted_plugins = mounter.mounted();

    // ---- 步骤 2.5:P2,Skill 材料——单根显式扫描,不搬终端五层合并 ----
    // 装配面 = features.skills 放行 ∧ tools 面点名 "skill"(mode=only 的
    // allow 名单;inherit/none 无内置面可继承,开关单独不起工具)。清单、
    // 工具、提示段三面同进同退(§六"清单、正文加载结果、实际工具面必须
    // 一致")。skill 工具只加载 SKILL.md 正文——脚本/CLI/MCP 需求仅是
    // 依赖声明,装它不自动授予任何执行工具(§六"SKILL.md 正文与脚本分开
    // 授权";本场注册表里本就只有档点名的那几枚工具)。
    const bool skill_exposed =
        !injection_path && harness != nullptr && harness->FeatureEnabled("skills") &&
        harness->tools.mode == HarnessToolPolicy::Mode::Only &&
        std::find(harness->tools.allow.begin(), harness->tools.allow.end(), std::string("skill")) !=
            harness->tools.allow.end();
    std::vector<lubancode::tools::SkillMeta> session_skills;
    if (skill_exposed && request.skills_root.has_value()) {
        session_skills = lubancode::tools::ScanSkillsDir(*request.skills_root, "材料根级");
    }

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
        // P2:内置 skill 工具(受控单根清单,与扫描件同一份——发现面、
        // 提示清单段、SkillTool 构造三处同源,不各扫各的)。
        if (skill_exposed) {
            registry->Register(std::make_unique<lubancode::tools::SkillTool>(session_skills));
        }
        // P5:点名 Lua 插件的工具(只装 allow 点名的;装载面≠注册面,与
        // MCP 同一条规矩——components 点名=装载,tools.allow=出面)。adapter
        // 走统一工具闸:needs_confirm 恒真、ApprovalClass::External,模型
        // 调用与内置工具过同一条审批/轨迹面,不旁路。
        if (assembly->manifest_lua != nullptr) {
            for (const auto& plugin : assembly->manifest_lua->plugins()) {
                for (const auto& tool : plugin->manifest->tools) {
                    const std::string wire_name = plugin->ToolWireName(tool.name);
                    if (std::find(harness->tools.allow.begin(), harness->tools.allow.end(), wire_name) ==
                        harness->tools.allow.end()) {
                        continue;  // 不在 allow:装载了也不出面(点名面由档定)
                    }
                    registry->Register(std::make_unique<lubancode::runtime::ManifestLuaToolAdapter>(
                        plugin.get(), &tool));
                }
            }
        }
        // 复验(步骤 4 的另一半):mode=only 的每枚 allow 名单必须真的装上
        // ——握手清单里没有就是"缺工具",明拒,不静默降级。"skill" 是内置
        // 件,上面 skill_exposed 为真即已装。
        for (const std::string& canonical : harness->tools.allow) {
            if (canonical == "skill") {
                continue;  // 内置件:装不装由 features/allow 交集定,装了就在
            }
            if (canonical.rfind("plugin__", 0) == 0) {
                // P5:插件工具对装载件的 manifest 清单精确对账(解析层的
                // id 段粗拆在这里补上全名对账)——缺工具明拒,不静默降级。
                bool mounted = false;
                if (assembly->manifest_lua != nullptr) {
                    for (const auto& plugin : assembly->manifest_lua->plugins()) {
                        for (const auto& tool : plugin->manifest->tools) {
                            if (plugin->ToolWireName(tool.name) == canonical) {
                                mounted = true;
                                break;
                            }
                        }
                        if (mounted) {
                            break;
                        }
                    }
                }
                if (!mounted) {
                    result.error = "装配失败:档点名的插件工具在插件 manifest 清单里不存在,整场拒绝: " +
                                   canonical;
                    return result;
                }
                continue;
            }
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
    // P2:计划在场(生产递了部署档)时,系统提示由提示部件组合产出
    // (prompt_assembler 既有管线;业务正文来自档案,宿主段由本场实际
    // 工具面与 wire 现拼,盖不掉)。计划缺席(无档默认路/测试注入路)
    // 沿用调用方显式给的 system_prompt。
    if (request.agent_plan != nullptr) {
        HarnessPromptInput prompt_input;
        prompt_input.plan = request.agent_plan.get();
        prompt_input.skills = &session_skills;
        prompt_input.face_names.reserve(assembly->registry->All().size());
        for (const auto& tool : assembly->registry->All()) {
            prompt_input.face_names.push_back(tool->name());
        }
        const HarnessPromptResult composed = ComposeHarnessSystemPrompt(prompt_input);
        if (!composed.error.empty()) {
            result.error = "装配失败:Agent 提示部件组合失败,整场拒绝: " + composed.error;
            return result;
        }
        assembly->agent_profile.system_prompt = std::move(composed.text);
    } else {
        assembly->agent_profile.system_prompt = request.system_prompt;
    }
    // 步数闸:宿主/档收窄值再与档案 runtime.max_steps_per_turn 取更严
    // (§5.2 预算行:交集/更严限制,不放宽;0 = 不限,不限 ∩ N = N)。
    int planned_steps = request.max_steps_per_turn;
    if (request.agent_plan != nullptr && request.agent_plan->agent.has_value() &&
        request.agent_plan->agent->max_steps_per_turn.has_value()) {
        const int agent_steps = *request.agent_plan->agent->max_steps_per_turn;
        if (planned_steps <= 0) {
            planned_steps = agent_steps;
        } else if (agent_steps > 0) {
            planned_steps = std::min(planned_steps, agent_steps);
        }
    }
    assembly->agent_profile.runtime.max_steps_per_turn = planned_steps;

    result.assembly = std::move(assembly);
    return result;
}

}  // namespace lubancode::app_server
