// tool_runtime.hpp 的实现:工具表装配、MCP 起服注册、插件挂载与
// ToolRuntime 构造的全套函数体,具体工具与 i18n 的依赖都在这只
// translation unit 里,不往公开头漏。

#include "app/tool_runtime.hpp"

#include "tools/undo_file_edit.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>

#include "app/version.hpp"
#include "app/commands/agent_commands.hpp"  // ComputeAgentScanRoots:自定义 Agent 目录三层的根
#include "cli/i18n.hpp"
#include "cli/console_input.hpp"  // CurrentConfirmMode:父会话权限档(阶段 3 解析环境)
#include "memory/memory_tool.hpp"
#include "mcp/mcp_tool.hpp"
#include "ptc/profile.hpp"
#include "agent/agent_catalog.hpp"  // LoadAgentCatalog:派发时按名解析自定义 Agent(P2-2)
#include "agent/agent_profile_resolver.hpp"  // AgentProfileResolveEnvironment:阶段 3 解析环境
#include "tools/agent_message_tool.hpp"
#include "tools/background_output.hpp"
#include "tools/edit_file.hpp"
#include "tools/lua_tool.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/path_utils.hpp"
#include "tools/read_file.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/search_ripgrep.hpp"  // BundledRipgrepRunner:SearchTool 的 P0-2 装配注入口
#include "tools/skill_tool.hpp"
#include "tools/tool_search.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"

namespace lubancode::app {

namespace {

// 沙箱豁免开关:POSIX 只有 rlimit(资源上限,无文件系统/网络隔离),按
// 规格"没有可靠 sandbox 的平台默认禁 PTC"执行;开发者/测试要用环境
// 变量 LUBANCODE_PTC_ALLOW_NO_SANDBOX 显式豁免,风险自担。
bool PosixSandboxExempted() {
#ifndef _WIN32
    const char* raw = std::getenv("LUBANCODE_PTC_ALLOW_NO_SANDBOX");
    return raw != nullptr && raw[0] != '\0';
#else
    return true;  // Windows 有 Job Object + 受限 token,天然豁免
#endif
}

}  // namespace

// i18n:装配函数里到处用 tr/trf,拉进来省得每处全限定。
using lubancode::cli::tr;
using lubancode::cli::trf;

lubancode::memory::Options MemoryOptionsFromConfig(const lubancode::config::MemoryConfig& config) {
    lubancode::memory::Options options;
    // config.memory.enabled 只能由用户全局配置打开(config merge 层守过),
    // 到了运行对象就当 global_allowed 用;本场总开关起手与全局授权同步。
    options.global_allowed = config.enabled;
    options.enabled = config.enabled;
    options.use = config.use;
    options.user_enabled = config.user_enabled;
    // learn 档位与上限都来自合并后的配置;本场 set_learn 只能降到上限以内。
    options.learn = lubancode::memory::ParseLearnMode(config.learn).value_or(lubancode::memory::LearnMode::Off);
    options.learn_ceiling = options.learn;
    options.max_index_bytes = config.max_index_bytes;
    options.max_retrieval_bytes = config.max_retrieval_bytes;
    options.max_results = config.max_results;
    return options;
}

lubancode::tools::ToolRegistry BuildBaseToolRegistry(const std::vector<lubancode::tools::SkillMeta>& skills,
                                                     const lubancode::config::SearchConfig& search_config) {
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    registry.Register(std::make_unique<lubancode::tools::RunCommandTool>());
    // 后台命令三件套:run_command 起后台(background_tasks 登记 task_id + watcher
    // 探活),background_output 查状态/读输出,stop_background 收尾。两个新工具
    // 是纯进程内单例查询/控制,无外部依赖,直接进基础表(子代理也能用)。
    registry.Register(std::make_unique<lubancode::tools::BackgroundOutputTool>());
    registry.Register(std::make_unique<lubancode::tools::StopBackgroundTool>());
    registry.Register(std::make_unique<lubancode::tools::WriteFileTool>());
    registry.Register(std::make_unique<lubancode::tools::EditFileTool>());
    // ripgrep 迁移单 P0-2:装配层注入默认 runner(定位只认 exe-dir/libexec;
    // 构造零动作,smoke 懒做)。P0-5 切主路之前 SearchTool::execute 仍走
    // 内置 std::regex 内核——这里只是把注入口接到生产装配上。
    registry.Register(std::make_unique<lubancode::tools::SearchTool>(
        std::make_shared<lubancode::tools::BundledRipgrepRunner>()));
    registry.Register(std::make_unique<lubancode::tools::SkillTool>(skills));
    registry.Register(std::make_unique<lubancode::tools::WebFetchTool>("lubancode/" + std::string(kVersion)));
    if (search_config.Configured()) {
        registry.Register(std::make_unique<lubancode::tools::WebSearchTool>(search_config));
    }
    return registry;
}

lubancode::tools::ToolRegistry BuildExploreToolRegistry(const lubancode::config::SearchConfig& search_config) {
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<lubancode::tools::ReadFileTool>());
    // 同基础表:Explore 表的 search 也注入同一款默认 runner。
    registry.Register(std::make_unique<lubancode::tools::SearchTool>(
        std::make_shared<lubancode::tools::BundledRipgrepRunner>()));
    registry.Register(std::make_unique<lubancode::tools::WebFetchTool>("lubancode/" + std::string(kVersion)));
    if (search_config.Configured()) {
        registry.Register(std::make_unique<lubancode::tools::WebSearchTool>(search_config));
    }
    return registry;
}

std::vector<McpServerRuntime> StartMcpServers(
    const std::map<std::string, lubancode::config::McpServerConfig>& configs, const lubancode::cli::Theme& theme) {
    std::vector<McpServerRuntime> out;
    for (const auto& [name, server_config] : configs) {
        auto client = std::make_unique<lubancode::mcp::Client>(name);
        const auto start_result = client->StartProcess(server_config.command, server_config.args, server_config.env);
        if (!start_result.success) {
            std::cout << theme.error << trf("mcp.start_failed", name, start_result.error) << theme.reset << "\n";
            continue;
        }
        const auto init_result = client->Initialize();
        if (!init_result.has_value()) {
            std::cout << theme.error << trf("mcp.init_failed", name, init_result.error()) << theme.reset << "\n";
            continue;
        }
        auto tools_result = client->ListTools();
        if (!tools_result.has_value()) {
            std::cout << theme.error << trf("mcp.list_failed", name, tools_result.error()) << theme.reset << "\n";
            continue;
        }

        McpServerRuntime runtime;
        runtime.name = name;
        runtime.tools = std::move(*tools_result);
        std::cout << trf("mcp.mounted", name, runtime.tools.size()) << "\n";
        runtime.client = std::move(client);
        out.push_back(std::move(runtime));
    }
    return out;
}

void RegisterMcpTools(std::vector<McpServerRuntime>& mcp_servers, lubancode::tools::ToolRegistry& registry) {
    for (auto& runtime : mcp_servers) {
        // 每只 MCP 的 tools 按 qualified name 排序后注册:服务端 ListTools
        // 若乱序,LubanCode 也跟着乱——跨进程 /resume 时 schema 内容虽一样,
        // tools 数组次序换了,请求前缀照样对不上(前缀缓存守恒单第七期)。
        std::sort(runtime.tools.begin(), runtime.tools.end(),
                  [](const auto& left, const auto& right) { return left.name < right.name; });
        for (const auto& tool_info : runtime.tools) {
            // tool_search:MCP 工具裹一层 DeferredTool 标成延迟挂载(mcp/
            // 目录不动,没法直接在 McpTool 上加 override)。阈值没超时延迟
            // 机制整个不启用,这层包装只是纯转发,行为不变。
            // 阶段 5:packaged MCP 注册名用 wire 服务段(%2E 编码,契约
            // packages.md §6.1),说明前缀换带点 canonical 名;来源账进
            // 注册元数据(/tools、/mcp 显示用)。
            std::string display_server;
            if (runtime.package_origin.has_value()) {
                const auto& origin = *runtime.package_origin;
                display_server =
                    origin.package_id + "." + origin.component_id.substr(origin.component_id.find(':') + 1);
            }
            lubancode::tools::ToolRegistration registration;
            registration.tool = std::make_unique<lubancode::tools::DeferredTool>(
                std::make_unique<lubancode::mcp::McpTool>(*runtime.client, runtime.name, tool_info,
                                                          std::move(display_server)));
            if (runtime.package_origin.has_value()) {
                registration.source_kind = lubancode::tools::ToolSourceKind::Mcp;
                registration.source_instance = runtime.package_origin->component_id;
                registration.package_origin = runtime.package_origin;
            }
            registry.Register(std::move(registration));
        }
    }
}

void PublishPackagedPlugins(const lubancode::package::PackageCodeMountResult& staged,
                            lubancode::tools::ToolRegistry& registry, std::vector<PluginMountInfo>& mounted,
                            bool report) {
    for (const auto& plugin : staged.plugins) {
        for (const auto& tool : plugin.manifest->tools) {
            const std::string local_id =
                plugin.canonical_id.substr(plugin.canonical_id.find(':') + 1);
            const std::string wire_name = lubancode::runtime::BuildPackagedToolWireName(
                "plugin", plugin.package_id, local_id, tool.name);
            const std::string display_name = lubancode::runtime::BuildPackagedToolDisplayName(
                "plugin", plugin.package_id, local_id, tool.name);
            lubancode::tools::ToolRegistration registration;
            registration.tool = std::make_unique<lubancode::runtime::PluginToolAdapter>(
                plugin.manifest, &tool, wire_name);
            registration.source_instance = plugin.canonical_id;
            registration.package_origin = lubancode::tools::ToolOrigin{
                plugin.package_id, plugin.package_version, plugin.canonical_id};
            registry.Register(std::move(registration));
            if (report) {
                PluginMountInfo info;
                info.tool_name = display_name;  // 展示名带点(canonical 段)
                info.kind = "package-process";
                info.package_origin = lubancode::tools::ToolOrigin{
                    plugin.package_id, plugin.package_version, plugin.canonical_id};
                mounted.push_back(std::move(info));
            }
        }
    }
}

void PublishPackagedLuaPlugins(const std::vector<lubancode::runtime::ManifestLuaPlugin*>& plugins,
                               lubancode::tools::ToolRegistry& registry, std::vector<PluginMountInfo>& mounted,
                               bool report) {
    for (lubancode::runtime::ManifestLuaPlugin* const plugin : plugins) {
        for (const auto& tool : plugin->manifest->tools) {
            const std::string wire_name = lubancode::runtime::BuildPackagedToolWireName(
                "plugin", plugin->package_id, plugin->local_id, tool.name);
            const std::string display_name = lubancode::runtime::BuildPackagedToolDisplayName(
                "plugin", plugin->package_id, plugin->local_id, tool.name);
            lubancode::tools::ToolRegistration registration;
            registration.tool = std::make_unique<lubancode::runtime::ManifestLuaToolAdapter>(plugin, &tool);
            registration.source_kind = lubancode::tools::ToolSourceKind::PluginLua;
            registration.source_instance = plugin->manifest->id;
            registration.package_origin = lubancode::tools::ToolOrigin{
                plugin->package_id, plugin->package_version,
                plugin->package_id + ":" + plugin->local_id};
            registry.Register(std::move(registration));
            if (report) {
                PluginMountInfo info;
                info.tool_name = display_name;
                info.kind = "package-embedded-lua";
                info.package_origin = registration.package_origin;
                mounted.push_back(std::move(info));
            }
        }
    }
}

void MountPlugins(lubancode::tools::PluginHost& plugin_host, lubancode::runtime::EmbeddedLuaRuntime& lua_runtime,
                  lubancode::runtime::ManifestLuaRuntime& manifest_lua_runtime,
                  lubancode::tools::ToolRegistry& registry, const lubancode::cli::Theme& theme,
                  std::vector<PluginMountInfo>& mounted, std::vector<std::string>& warnings, bool report,
                  std::vector<std::shared_ptr<const lubancode::runtime::PluginManifest>>& process_manifests,
                  std::vector<std::string>& process_warnings, const std::string& project_root_utf8,
                  const lubancode::config::PluginTrustStore* project_trust) {
    const auto home_dir = lubancode::config::HomeLubancodeDir();
    if (!home_dir.has_value()) {
        return;  // 找不到主目录,也就没有插件目录可扫
    }
    const std::filesystem::path plugins_dir =
        std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(home_dir->data()), home_dir->size())) /
        "plugins";

    // C ABI DLL 插件
    std::vector<std::string> new_warnings = plugin_host.LoadDirectory(plugins_dir);
    auto wrapped_plugins = plugin_host.WrapTools(new_warnings);
    if (report) {
        for (auto& warning : new_warnings) {
            std::cout << theme.error << warning << theme.reset << "\n";
            warnings.push_back(std::move(warning));
        }
    }
    for (auto& wrapped : wrapped_plugins) {
        if (report) {
            std::cout << trf("plugin.mounted_line", wrapped.stem, wrapped.tools.size()) << "\n";
        }
        for (auto& tool : wrapped.tools) {
            if (report) {
                mounted.push_back({tool->name(), "DLL"});
            }
            registry.Register(std::move(tool));
        }
    }

    // Lua 插件:第 4 步起走 EmbeddedLuaRuntime(引擎不变:每文件一 state、
    // mutex 串行、文件名稳定排序;新:profile 分级、指令预算、内存帽、
    // 取消链)。第二遍调用(main/sub 各一遍)只造轻 adapter,不重扫。
    std::vector<std::string> lua_warnings = lua_runtime.LoadDirectory(plugins_dir);
    if (report) {
        for (auto& warning : lua_warnings) {
            std::cout << theme.error << warning << theme.reset << "\n";
            warnings.push_back(std::move(warning));
        }
        for (const auto& record : lua_runtime.records()) {
            std::cout << trf("plugin.mounted_line", record.id, 1) << "\n";
            mounted.push_back({record.tool_name, "lua"});
        }
    }
    for (auto& adapter : lua_runtime.MakeAdapters()) {
        registry.Register(std::move(adapter));
    }

    // process 插件(plugin.json 一插件一目录,plugins 单第 7 步挂进):Scan
    // 一次(manifest 钉 shared_ptr),每张 registry 各造一枚 adapter。挂载
    // 行/警告只在 report 遍打;manifests 由调用方持有,两张表共享同一批。
    // 第 8 步:项目级 <cwd>/.lubancode/plugins/ 一并扫——先过信任门(内容
    // hash 审批),未信任的跳过并警告(manifest-backed Lua 同一道门:未信任
    // 的 v2 件连 Lua state 都不建,chunk 一字不跑)。
    if (process_manifests.empty()) {
        const auto scan = lubancode::runtime::ScanPluginDirectories(plugins_dir);
        process_manifests = scan.manifests;
        process_warnings.insert(process_warnings.end(), scan.warnings.begin(), scan.warnings.end());
        const std::filesystem::path project_dir = lubancode::tools::Utf8ToPath(project_root_utf8);
        const auto project_scan = lubancode::runtime::ScanProjectPluginDirectories(project_dir, project_trust);
        // 主目录与项目级重名:项目级让位(先到先得,主目录是用户亲手放的)。
        std::set<std::string> home_ids;
        for (const auto& m : process_manifests) {
            home_ids.insert(m->id);
        }
        for (const auto& m : project_scan.manifests) {
            if (home_ids.count(m->id) != 0) {
                process_warnings.push_back("[plugin] " + m->id +
                                           ": 与用户主目录插件重名,项目级让位,跳过");
                continue;
            }
            process_manifests.push_back(m);
        }
        process_warnings.insert(process_warnings.end(), project_scan.warnings.begin(),
                                project_scan.warnings.end());
    }
    // manifest-backed Lua(阶段 4):扫描账里的 v2 embedded-lua 件挂进
    // ManifestLuaRuntime(幂等——第二遍只造 adapter)。顶层零副作用加载与
    // handler 对账在 Load 里;单件坏一条警告跳过,不连累其余。
    {
        const std::vector<std::string> lua_manifest_warnings =
            manifest_lua_runtime.LoadFromManifests(process_manifests);
        if (report) {
            for (const std::string& warning : lua_manifest_warnings) {
                std::cout << theme.error << warning << theme.reset << "\n";
                warnings.push_back(warning);
            }
            for (const auto& plugin : manifest_lua_runtime.plugins()) {
                if (!plugin->package_id.empty()) {
                    continue;  // packaged 件的挂载行走 Package 事务那边,不在此打
                }
                std::cout << trf("plugin.mounted_line", plugin->manifest->id,
                                 plugin->manifest->tools.size())
                          << "\n";
                for (const auto& tool : plugin->manifest->tools) {
                    mounted.push_back({plugin->ToolWireName(tool.name), "embedded-lua"});
                }
            }
        }
    }
    for (auto& adapter : manifest_lua_runtime.MakeAdapters()) {
        // 只收 standalone 件(packaged 件由 PublishPackagedLuaPlugins 发布,
        // 两边各注册会撞名);MakeAdapters 内部已按 package_id 过滤。
        registry.Register(std::move(adapter));
    }
    if (report) {
        for (const auto& warning : process_warnings) {
            std::cout << theme.error << warning << theme.reset << "\n";
            warnings.push_back(warning);
        }
        for (const auto& manifest : process_manifests) {
            if (manifest->kind == lubancode::runtime::RuntimeKind::EmbeddedLua) {
                continue;  // v2 件的账在 manifest_lua_runtime 那边记过了
            }
            std::cout << trf("plugin.mounted_line", manifest->id, manifest->tools.size()) << "\n";
            for (const auto& tool : manifest->tools) {
                mounted.push_back(
                    {tool.full_name, std::string(lubancode::runtime::RuntimeKindName(manifest->kind))});
            }
        }
    }
    for (const auto& manifest : process_manifests) {
        if (manifest->kind == lubancode::runtime::RuntimeKind::EmbeddedLua) {
            continue;  // v2 embedded-lua 走 ManifestLuaToolAdapter(上面已注册)
        }
        for (const auto& tool : manifest->tools) {
            registry.Register(std::make_unique<lubancode::runtime::PluginToolAdapter>(manifest, &tool));
        }
    }
}

// 自定义 Agent 的解析体(阶段 6 自 resolver lambda 折出,两路共用):
// Catalog 现扫(包层成品件从给定快照折),预装技能正文包内件从快照读、
// standalone 件从盘上现读。快照为空 = 从前的无包行为。
std::optional<lubancode::tools::CustomAgentMaterial> ResolveCustomAgentMaterial(
    const std::vector<lubancode::tools::SkillMeta>& skills,
    const lubancode::package::PackageSnapshot* snapshot, const std::string& name) {
    const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(
        ComputeAgentScanRoots(snapshot != nullptr
                                  ? lubancode::package::MountAgentEntries(snapshot->mount())
                                  : std::vector<lubancode::agent::PackagedAgentEntry>{}));
    const lubancode::agent::AgentCatalogEntry* entry = catalog.Find(name);
    if (entry == nullptr || !entry->available || !entry->definition.has_value()) {
        return std::nullopt;
    }
    lubancode::tools::CustomAgentMaterial material;
    material.definition = *entry->definition;
    material.builtin = entry->layer == lubancode::agent::AgentSourceLayer::Builtin && entry->file == "(builtin)";
    for (const std::string& skill_name : material.definition.skills_preload) {
        std::string body;
        bool have_body = false;
        // 包内技能优先从快照读:正文在折快照时已进 records,盘中删包/改
        // SKILL 都不影响钉住这份的在跑引用(阶段 6 验收线)。
        if (snapshot != nullptr) {
            if (const auto pinned = snapshot->SkillBody(skill_name); pinned.has_value()) {
                body = *pinned;
                have_body = true;
            }
        }
        // standalone 技能照旧从盘上现读;名单里没有的留给 doctor,只降级。
        if (!have_body) {
            const auto meta = std::find_if(skills.begin(), skills.end(),
                                           [&](const lubancode::tools::SkillMeta& candidate) {
                                               return candidate.name == skill_name;
                                           });
            if (meta != skills.end()) {
                if (const auto text = lubancode::tools::ReadSkillBody(*meta); text.has_value()) {
                    body = *text;
                }
            }
        }
        material.preloaded_skills.push_back(std::move(body));
    }
    return material;
}

ToolRuntime::ToolRuntime(const lubancode::config::Config& config, const lubancode::cli::Theme& theme,
                         lubancode::api::Backend& agent_backend, const std::vector<lubancode::tools::SkillMeta>& skills,
                         const std::string& skills_segment, const std::string& cwd_utf8, Options options) {
    // M8:起服务器打出 "[mcp] xxx: N 个工具已挂载" 行,紧跟着调用方
    // 刚打完的横幅;单个服务器出岔子只打警告跳过,不阻塞会话。
    mcp_servers_ = StartMcpServers(config.mcp_servers, theme);
    // 统一 Package 封装单阶段 5:packaged Plugin/MCP 走挂载事务(整包成
    // 整包败)。暂存在事务里完成——plugin 探针进程过协议、MCP 起服握手列
    // 工具;全件起得来才把 MCP client 并进 mcp_servers_(发布),坏一件
    // 整包回滚(杀已起进程),诊断指到坏件。必须在 mcp_servers_ 定型后、
    // resolve_env_static_ 折账前跑:packaged MCP 的 canonical 名要进 Agent
    // 解析环境的名单。plugin adapter 的注册晚一步(registry 建好后
    // PublishPackagedPlugins),那步不失败,不破坏原子性。
    if (options.package_snapshot != nullptr) {
        lubancode::package::PackageCodeMountOptions code_options;
        code_options.cwd_utf8 = cwd_utf8;
        if (const auto home_lubancode = lubancode::config::HomeLubancodeDir();
            home_lubancode.has_value()) {
            const std::filesystem::path data_root =
                lubancode::tools::Utf8ToPath(*home_lubancode) / "package-data";
            std::error_code ec;
            std::filesystem::create_directories(data_root, ec);  // 拿不到/建不动都照旧
            code_options.package_data_root = data_root;
        }
        // 挂载事务吃启动这一折的快照(阶段 6):code 组件只在会话启动跑,
        // reload 不热插也不热卸——须新会话。现行快照经供应商口取,装配
        // 次序上 reload 尚不存在,取到的必是启动折。
        const std::shared_ptr<const lubancode::package::PackageSnapshot> startup_snapshot =
            options.package_snapshot();
        package_code_ = lubancode::package::MountPackageCode(startup_snapshot->mount(), code_options);
        for (auto& staged : package_code_.mcp_servers) {
            McpServerRuntime runtime;
            runtime.name = staged.wire_server_name;
            runtime.tools = std::move(staged.tools);
            runtime.package_origin = lubancode::tools::ToolOrigin{
                staged.package_id, staged.package_version, staged.canonical_id};
            std::cout << trf("package.code.mounted_mcp", staged.canonical_id, runtime.tools.size(),
                             staged.package_id + " " + staged.package_version)
                      << "\n";
            runtime.client = std::move(staged.client);
            mcp_servers_.push_back(std::move(runtime));
        }
        for (auto& staged : package_code_.plugins) {
            const bool is_lua = staged.lua != nullptr;
            if (is_lua) {
                // v2 embedded-lua(阶段 4):暂存 state 移交 owner(Adopt),
                // 发布段造 wire adapter——回滚路的 state 根本到不了这里。
                packaged_lua_plugins_.push_back(manifest_lua_runtime_.Adopt(std::move(staged.lua)));
            }
            std::cout << trf(is_lua ? "package.code.mounted_plugin_lua" : "package.code.mounted_plugin",
                             staged.canonical_id, staged.manifest->tools.size(),
                             staged.package_id + " " + staged.package_version)
                      << "\n";
        }
        for (const auto& note : package_code_.notes) {
            std::cout << theme.stats << "[package] " << note << theme.reset << "\n";
        }
        for (const auto& diagnostic : package_code_.diagnostics) {
            const std::string line = diagnostic.Format();
            std::cout << theme.error << line << theme.reset << "\n";
            plugin_warnings_.push_back(std::move(line));
        }
        package_code_.mcp_servers.clear();  // client 已移交,清掉防双重持有
    }
    // LSP:配了 lsp 段才构造;构造本身不起进程(懒启动,首次用到某语言
    // 才拉),析构时把还活着的服务器按 shutdown/exit + 2s 兜底全关。
    if (!config.lsp_servers.empty()) {
        lsp_manager_.emplace(config.lsp_servers, cwd_utf8);
    }
    // 三份表:sub_registry 与 main_registry 同为"基础 + MCP + LSP"的全能力
    // 表(子代理与 main 同级,规格"产品不变量");agent 委托工具两边都有
    // ——子表挂的是转发壳(AgentDispatchTool),递归不再靠拿掉工具防,改由
    // AgentTool 的全局并发槽 + 显式深度上限治理(SetDispatchGovernance)。
    // explore 只读硬边界:独立一张只读表,不含 agent/todo/写入类。
    if (options.with_explore) {
        explore_registry_.emplace(BuildExploreToolRegistry(config.search));
    }
    sub_registry_ = BuildBaseToolRegistry(skills, config.search);
    main_registry_ = BuildBaseToolRegistry(skills, config.search);
    // MCP 工具进主表 + 子代理表(不进 explore);两份独立 McpTool 实例,
    // 底下同一个 mcp::Client&。
    RegisterMcpTools(mcp_servers_, sub_registry_);
    RegisterMcpTools(mcp_servers_, main_registry_);
    // lsp 工具同样进主表 + 子代理表,有 explore 也一并;两份(三份)
    // LspTool 共享同一个 Manager。
    if (lsp_manager_.has_value()) {
        if (explore_registry_.has_value()) {
            explore_registry_->Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager_));
        }
        sub_registry_.Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager_));
        main_registry_.Register(std::make_unique<lubancode::tools::LspTool>(*lsp_manager_));
    }
    // 子代理步数预算从配置来(规格"现场四"):首选 subagent 段的预算,未设
    // 继承主代理的;0 的语义全路一致(不限步)。旧版这里先后写死
    // 过 40、构造器默认 15——两处暗闸都拆掉,不再有魔数。
    const int subagent_default_steps_per_turn = config.subagent.max_steps_per_turn.value_or(config.max_steps_per_turn);
    main_registry_.Register(std::make_unique<lubancode::tools::AgentTool>(
        agent_backend, sub_registry_, cwd_utf8, config.model, subagent_default_steps_per_turn,
        skills_segment));
    agent_tool_ = dynamic_cast<lubancode::tools::AgentTool*>(main_registry_.Find("agent"));
    if (agent_tool_ != nullptr) {
        // 阶段 3:解析环境(AgentProfileResolver 的父会话活材料账)。静态
        // 半份(技能/MCP 名单、角色路由、思考档表)构造时定格;权限档是
        // 会话活账(Shift+Tab 随时切),供应商回调里现读——每笔派发拿到的
        // 都是当下值。角色路由照 BuildRoleSpecs 的优先级:高级段
        // model_roles.<role>.model 非空优先,回落 shorthand 三字段。
        resolve_env_static_.skill_names.reserve(skills.size());
        for (const auto& skill : skills) {
            resolve_env_static_.skill_names.push_back(skill.name);
        }
        resolve_env_static_.mcp_server_names.reserve(mcp_servers_.size());
        for (const auto& server : mcp_servers_) {
            resolve_env_static_.mcp_server_names.push_back(server.name);
        }
        const auto route_of = [](const config::ModelRoleRouteConfig& advanced,
                                 const std::string& shorthand) -> lubancode::agent::AgentRoleRoute {
            if (!advanced.model.empty()) {
                return {advanced.provider, advanced.model};
            }
            if (!shorthand.empty()) {
                return {std::string(), shorthand};
            }
            return {};
        };
        resolve_env_static_.role_normal = route_of(config.model_roles.normal, config.normal_model);
        resolve_env_static_.role_cheap = route_of(config.model_roles.cheap, config.cheap_model);
        resolve_env_static_.role_lao = route_of(config.model_roles.lao, config.lao_model);
        resolve_env_static_.supported_efforts = config.provider_think_levels;
        agent_tool_->SetResolveEnvironment([this]() {
            lubancode::agent::AgentProfileResolveEnvironment env = resolve_env_static_;
            switch (lubancode::cli::CurrentConfirmMode()) {
                case lubancode::cli::ConfirmMode::Auto:
                    env.parent_permission = lubancode::agent::AgentPermissionMode::Auto;
                    break;
                case lubancode::cli::ConfirmMode::Yolo:
                    env.parent_permission = lubancode::agent::AgentPermissionMode::Yolo;
                    break;
                case lubancode::cli::ConfirmMode::Confirm:
                default:
                    env.parent_permission = lubancode::agent::AgentPermissionMode::Confirm;
                    break;
            }
            return env;
        });
        // 自定义 Agent 解析口(真机实测 P2-1/P2-2;阶段 4 起是 agent_type 的
        // 唯一派发校验):按名查 AgentCatalog,查得到即可派——码内内置两枚
        // 标 builtin(工具层走内置快路),user/project 层覆盖内置名的按定义
        // 走自定义路。现扫不缓存——用户改了 YAML,下一次派发即生效,不必
        // 重启会话。预装技能的正文:包内件从快照读(在跑引用不回盘),名
        // 单里没有的技能留给 doctor 诊断,这里只降级(登记名字不注正文)。
        // 统一 Package 封装单阶段 6:每派发经供应商口取现行快照——reload
        // 换档后的下一次装配即见新账,在跑引用各自钉着旧折照旧跑完。
        const std::function<std::shared_ptr<const lubancode::package::PackageSnapshot>()>
            snapshot_provider = options.package_snapshot;
        agent_tool_->SetCustomAgentResolver(
            [skills, snapshot_provider](const std::string& name) -> std::optional<lubancode::tools::CustomAgentMaterial> {
                if (snapshot_provider == nullptr) {
                    return ResolveCustomAgentMaterial(skills, nullptr, name);
                }
                return ResolveCustomAgentMaterial(skills, snapshot_provider().get(), name);
            });
        // agent 类型清单源(阶段 4·动态 schema):schema 的 agent_type 说明
        // 列"当前可派的类型"(可用条目:内置+自定义,各带一句 description)。
        // 现扫现列与派发口同款——AgentTool 在回合边界(SetHooks)翻新缓存,
        // 一回合至多扫一遍盘,不是每请求一遍。
        agent_tool_->SetAgentTypesProvider([]() -> std::vector<lubancode::tools::AgentTypeInfo> {
            std::vector<lubancode::tools::AgentTypeInfo> types;
            const lubancode::agent::AgentCatalog catalog =
                lubancode::agent::LoadAgentCatalog(ComputeAgentScanRoots());
            for (const lubancode::agent::AgentCatalogEntry* entry : catalog.Available()) {
                types.push_back(lubancode::tools::AgentTypeInfo{entry->name, entry->definition->description});
            }
            return types;
        });
        // 长任务 compact:子代理复用主 compact,窗口从配置来(0 = 未知不评估)。
        agent_tool_->SetContextWindowTokens(config.context_window_tokens);
        // 派工治理(规格"递归派工不能再靠拿掉工具解决"):并发槽与深度上限
        // 都从配置来,没写用公开默认值(config.hpp)。
        agent_tool_->SetDispatchGovernance(
            config.subagent.max_active.value_or(lubancode::config::kDefaultSubagentMaxActive),
            config.subagent.max_depth.value_or(lubancode::config::kDefaultSubagentMaxDepth),
            config.subagent.max_children_per_task.value_or(0), config.subagent.max_tree_nodes.value_or(0));
    }
    // 同级派工:子表也挂 agent(转发壳,目标是上面那只 AgentTool)。后台
    // 的独立注册表(BuildDetachedRegistry)不挂——后台线程不能同步跑前台
    // 任务,那条路要另立单子接(见 AgentDispatchTool 注释)。
    if (agent_tool_ != nullptr) {
        sub_registry_.Register(std::make_unique<lubancode::tools::AgentDispatchTool>(*agent_tool_));
    }
    // agent_message:main 挂 caller_task_id=0 的无限定实例——可投任意存活
    // 任务(规格 §9.3 末两行)。子代理挂的是绑定各自 task_id 的窄实例,由
    // AgentTool::RunTask 的第二段随 scoped agent 一并现挂(同一道资格门,
    // P1-1),不在 sub_registry_/detached 源表里预置——每只任务的窄实例都
    // 是运行期现造,不能共享一份。execute 只调 AgentTool::SendTaskMessage,
    // 与查看态传话、排队转投共用同一本 TaskRecord::inbox。
    if (agent_tool_ != nullptr) {
        main_registry_.Register(std::make_unique<lubancode::tools::AgentMessageTool>(agent_tool_));
    }
    if (agent_tool_ != nullptr && explore_registry_.has_value()) {
        agent_tool_->SetExploreRegistry(&*explore_registry_);
    }
    // todo_write:主表挂会话级待办(/todos、RunTurn 渲染读同一份 state);
    // 子表也挂(同级能力),但 AgentTool::RunTask 会把每只任务的 todo_write
    // 换成该任务独占的实例——子代理有自己的私有 todo,不乱写 main 的
    // 待办,也不与别只子代理共用一块板。
    todo_state_ = std::make_shared<lubancode::tools::TodoListState>();
    main_registry_.Register(std::make_unique<lubancode::tools::TodoWriteTool>(todo_state_));
    sub_todo_state_ = std::make_shared<lubancode::tools::TodoListState>();
    sub_registry_.Register(std::make_unique<lubancode::tools::TodoWriteTool>(sub_todo_state_));
    if (options.with_ask_user) {
        main_registry_.Register(std::make_unique<lubancode::tools::AskUserTool>(options.ask_user_handler));
    }
    // memory_save 同级(规格"同级能力审计"memory 写入行):同授权、默认写
    // 候选——子表也挂,与主表同一个 ProjectMemory 引擎。
    if (options.memory != nullptr && options.memory->generate_enabled()) {
        main_registry_.Register(std::make_unique<lubancode::memory::MemorySaveTool>(options.memory));
        sub_registry_.Register(std::make_unique<lubancode::memory::MemorySaveTool>(options.memory));
    }
    // 模型侧 worktree 工具:只挂主表(子代理不起房,带 isolation 的
    // 子代理由 agent_tool 另走 base_dir 包装那条路)。
    if (options.worktree_session != nullptr) {
        main_registry_.Register(std::make_unique<lubancode::tools::WorktreeTool>(
            *options.worktree_session, options.worktree_confirm, options.on_worktree_moved));
    }
    // 插件工具主表 + 子表都挂(子代理与 main 同能力,独立任务 agent 默认
    // 完成后退出,不是低配跑腿),挂载行紧跟 [mcp] 那几行。process 插件
    // (plugin.json)的 adapter 挂进各表后灌项目根(第 7 步:进程 cwd 缺省
    // 项目根);取消链/LogSink 由 turn_runner 每轮灌(SetPluginCancel 等)。
    // 项目插件信任账(plugins 单第 8 步):启动装载一次;读不动的警告当
    // 空账(全部项目插件按未信任跳过,不带着一本读不动的账放行)。项目根
    // 一并存下(信任流 UI 的 /plugin trust|untrust 要按同一份路径口径重扫
    // 才算得出对得上的账本键)。
    project_root_utf8_ = cwd_utf8;
    if (const auto path = lubancode::config::PluginTrustStore::DefaultStorePath(); path.has_value()) {
        auto [store, load_error] = lubancode::config::PluginTrustStore::Load(path);
        if (load_error.has_value()) {
            plugin_warnings_.push_back(*load_error);
        }
        project_plugin_trust_ = std::move(store);
    }
    MountPlugins(plugin_host_, lua_runtime_, manifest_lua_runtime_, main_registry_, theme, plugin_mounted_,
                 plugin_warnings_,
                 /*report=*/true, process_manifests_, process_plugin_warnings_, cwd_utf8,
                 project_plugin_trust_.has_value() ? &*project_plugin_trust_ : nullptr);
    MountPlugins(plugin_host_, lua_runtime_, manifest_lua_runtime_, sub_registry_, theme, plugin_mounted_,
                 plugin_warnings_,
                 /*report=*/false, process_manifests_, process_plugin_warnings_, cwd_utf8,
                 project_plugin_trust_.has_value() ? &*project_plugin_trust_ : nullptr);
    // 阶段 5 发布段 plugin 半边:事务暂存的 plugin 造 adapter(wire 覆盖名
    // + 来源账)注册进两表;MCP 半边早在 mcp_servers_ 定型时一并进了
    // RegisterMcpTools。放在 SetCwd 循环前,packaged adapter 同吃项目根。
    PublishPackagedPlugins(package_code_, main_registry_, plugin_mounted_, /*report=*/true);
    PublishPackagedPlugins(package_code_, sub_registry_, plugin_mounted_, /*report=*/false);
    // 阶段 4:packaged v2 embedded-lua 的发布半边(wire 名 + 来源账)。
    PublishPackagedLuaPlugins(packaged_lua_plugins_, main_registry_, plugin_mounted_, /*report=*/true);
    PublishPackagedLuaPlugins(packaged_lua_plugins_, sub_registry_, plugin_mounted_, /*report=*/false);
    for (const auto& adapter : main_registry_.All()) {
        if (auto* plugin_adapter = dynamic_cast<lubancode::runtime::PluginToolAdapter*>(adapter.get());
            plugin_adapter != nullptr) {
            plugin_adapter->SetCwd(cwd_utf8);
        }
    }
    for (const auto& adapter : sub_registry_.All()) {
        if (auto* plugin_adapter = dynamic_cast<lubancode::runtime::PluginToolAdapter*>(adapter.get());
            plugin_adapter != nullptr) {
            plugin_adapter->SetCwd(cwd_utf8);
        }
    }

    // tool_search(延迟挂载):全部工具(MCP/插件/LSP/agent/todo)都注册
    // 完了才数总数、定启停。loaded 集合是会话级的(/clear 不清),主会话
    // 与子代理共享同一份;主表/子表各自按各自的总数判定,同一阈值。
    //
    // 动态工具 P1:deferred_tool_mode 决定命中之后怎么走——legacy_expand
    //(默认)沿用 loaded 扩写路,行为与从前一字不差;proxy_reference 换
    // 代理引用路:tool_search 铸 ref 不写 loaded,tool_invoke 常驻顶层,
    // 延迟工具永远不进顶层 tools(单子 §5.1);disabled 连延迟都关掉。
    // 先 opt-in:配置不写就是现状。
    const int tool_search_threshold = config.tool_search_threshold;
    const auto configured_mode =
        lubancode::tools::ParseDeferredToolMode(config.deferred_tool_mode);
    const lubancode::tools::DeferredToolMode mode =
        configured_mode.value_or(lubancode::tools::DeferredToolMode::LegacyExpand);
    main_mode_ = mode;
    sub_mode_ = mode;
    main_deferral_ = lubancode::tools::DeferralEnabled(main_registry_.All().size(), tool_search_threshold);
    sub_deferral_ = lubancode::tools::DeferralEnabled(sub_registry_.All().size(), tool_search_threshold);
    if (main_mode_ == lubancode::tools::DeferredToolMode::Disabled) {
        main_deferral_ = false;
    }
    if (sub_mode_ == lubancode::tools::DeferredToolMode::Disabled) {
        sub_deferral_ = false;
    }
    main_proxy_ = main_deferral_ && main_mode_ == lubancode::tools::DeferredToolMode::ProxyReference;
    sub_proxy_ = sub_deferral_ && sub_mode_ == lubancode::tools::DeferredToolMode::ProxyReference;
    if (main_proxy_) {
        main_resolver_ = std::make_shared<lubancode::tools::DeferredToolResolver>("main");
    }
    if (sub_proxy_) {
        sub_resolver_ = std::make_shared<lubancode::tools::DeferredToolResolver>("sub");
    }
    // 注册次序钉死:tool_search 之后紧跟 tool_invoke——同一会话内顶层
    // tools 数组的数量与次序都不许动(单子 §5.1)。
    if (main_deferral_) {
        if (main_proxy_) {
            main_registry_.Register(
                std::make_unique<lubancode::tools::ToolSearchTool>(main_registry_, main_resolver_));
            main_registry_.Register(std::make_unique<lubancode::tools::ToolInvokeTool>());
        } else {
            main_registry_.Register(
                std::make_unique<lubancode::tools::ToolSearchTool>(main_registry_, loaded_tools_));
        }
    }
    if (sub_deferral_) {
        if (sub_proxy_) {
            sub_registry_.Register(
                std::make_unique<lubancode::tools::ToolSearchTool>(sub_registry_, sub_resolver_));
            sub_registry_.Register(std::make_unique<lubancode::tools::ToolInvokeTool>());
        } else {
            sub_registry_.Register(
                std::make_unique<lubancode::tools::ToolSearchTool>(sub_registry_, loaded_tools_));
        }
    }
    // 暴露策略(动态工具 P2·§8.2 的 ToolExposurePolicy):只认注册表与
    // 延迟挂载账,不再现查运行档。memory gate 的清账在此——旧路把
    // `memory->generate_enabled()`(随 /memory on|off、/memory learn 翻)
    // 现查进过滤谓词,用户一关学习,memory_save 的定义就从 tools 数组里
    // 消失,tools hash 白断一次。P2 起:注册了就常驻(注册本身只认构造时
    // 或 /memory on 补挂那两处能力变化,变了 hash 断得有名有姓),运行档
    // 交给执行侧——直名调用被 MemorySaveTool::execute 拒("本场记忆写入
    // 未开启"),proxy 路被下面的 main_execution_policy_ 拒
    //(proxy.tool_not_allowed),两道都是稳定错,模型看得懂。
    main_tool_filter_ = [loaded = loaded_tools_, deferral = main_deferral_](const lubancode::tools::Tool& tool) {
        return !deferral || !tool.deferred() || loaded->count(tool.name()) != 0;
    };
    sub_tool_filter_ = [loaded = loaded_tools_, deferral = sub_deferral_](const lubancode::tools::Tool& tool) {
        return !deferral || !tool.deferred() || loaded->count(tool.name()) != 0;
    };
    // proxy 模式的执行资格(单子 §5.5):只作用于经 tool_invoke 解引用来的
    // 调用。exposure 过滤(上面的 tool_filter)不动——延迟工具照旧不进顶层
    // tools、直接按名调用照旧被拦(发现不等于授权)。main 侧带 memory 的
    // 运行档闸(P2 清账:暴露只认注册,运行档全在执行侧——这道闸与
    // MemorySaveTool::execute 的自拒是同一状态的两道口);sub 侧与子过滤同
    // 一口径,无额外闸。denial 走 "稳定码|人话" 两截(RunOneTool 的解析
    // 口径),报 proxy.tool_not_allowed,模型不得重试同一调用。
    if (main_proxy_) {
        main_execution_policy_ = [memory = options.memory](const lubancode::tools::Tool& tool) {
            if (tool.name() == "memory_save") {
                return memory != nullptr && memory->generate_enabled();
            }
            return true;
        };
        main_execution_denial_ = std::string(lubancode::tools::kErrToolRefNotAllowed) +
                                 "|该工具不在当前会话的执行策略内(角色/权限限制),不得重试同一调用。";
    }
    if (sub_proxy_) {
        sub_execution_policy_ = [](const lubancode::tools::Tool&) { return true; };
        sub_execution_denial_ = std::string(lubancode::tools::kErrToolRefNotAllowed) +
                                "|该工具不在当前会话的执行策略内(角色/权限限制),不得重试同一调用。";
    }

    // ---- PTC 装配(tool_calling 配置档 + 五条硬条件 + auto 门槛)----
    // json(默认):什么都不挂,行为与从前逐字节一致。
    // programmatic:五条硬条件齐(Python 探测在 PtcTool 构造里做)才挂;
    // 不齐明报回落 json。auto:画像 verified + 门槛过才选 ptc——首版没有
    // verified 画像,恒落 json(ResolveToolCalling 照规格实现,不放宽)。
    ptc_resolution_ = "json";
    if (config.tool_calling != lubancode::config::ToolCallingMode::Json) {
        lubancode::ptc::PtcHardConditions hard;
#ifdef _WIN32
        hard.sandbox_reliable = true;  // Job Object + 受限 token
#else
        hard.sandbox_reliable = PosixSandboxExempted();  // rlimit 不算可靠沙箱
#endif
        // 构造 PtcTool(顺带探测 Python)。条件齐才注册进主表。
        auto tool = std::make_unique<lubancode::ptc::PtcTool>(main_registry_, main_tool_filter_,
                                                              [&] {
                                                                  lubancode::ptc::PtcTool::Config ptc_config;
                                                                  ptc_config.python_cmd = config.ptc.python;
                                                                  ptc_config.limits.wall_clock_ms =
                                                                      config.ptc.wall_clock_ms;
                                                                  ptc_config.limits.cpu_ms = config.ptc.cpu_ms;
                                                                  ptc_config.limits.memory_bytes =
                                                                      config.ptc.memory_bytes;
                                                                  ptc_config.limits.output_bytes =
                                                                      config.ptc.output_bytes;
                                                                  ptc_config.limits.max_calls =
                                                                      config.ptc.max_calls;
                                                                  ptc_config.limits.max_concurrency =
                                                                      config.ptc.max_concurrency;
                                                                  ptc_config.restricted_token =
                                                                      config.ptc.restricted_token;
                                                                  ptc_config.eligible_tools = config.ptc.tools;
                                                                  return ptc_config;
                                                              }());
        const bool python_ok = tool->available();
        hard.python_version_ok = python_ok;
        hard.tools_wired = python_ok;  // 入选集(默认 read_file/search)都接 RPC/权限/hooks/取消链
        hard.context_fits_stubs = true;
        hard.model_free_code = true;

        // 画像指纹:provider + endpoint + model + wire + python + harness。
        // 存档里 harness 版本对不上 = 画像过期,按 unknown 算。
        const std::string store_path = lubancode::ptc::DefaultProfileStorePath();
        lubancode::ptc::PtcStatus profile_status = lubancode::ptc::PtcStatus::Unknown;
        if (!store_path.empty()) {
            const std::string fingerprint = lubancode::ptc::BuildPtcFingerprint(
                lubancode::config::BoundProviderName(config, config.active_provider), config.base_url, config.model,
                lubancode::config::ProviderWireName(config.wire), tool->available() ? "py" : "",
                lubancode::ptc::kPtcHarnessRevision);
            lubancode::ptc::PtcProfileStore store(store_path);
            const auto profile = store.Find(fingerprint);
            if (profile.has_value() && profile->harness_revision == lubancode::ptc::kPtcHarnessRevision) {
                profile_status = profile->status;
            }
        }

        lubancode::ptc::PtcTool* registered = nullptr;
        if (config.tool_calling == lubancode::config::ToolCallingMode::Programmatic) {
            // 强制档:硬条件不齐(POSIX 无沙箱未豁免/没有 Python)就明报
            // 回落,不留一只空壳工具让模型白调。
            if (hard.AllMet()) {
                registered = tool.get();
                main_registry_.Register(std::move(tool));
                ptc_resolution_ = "ptc";
            } else {
                const auto failures = hard.FailureTexts();
                ptc_resolution_ = "ptc→json(" + (failures.empty() ? std::string("条件不齐") : failures.front()) + ")";
                std::cout << theme.error << trf("ptc.fallback_line", ptc_resolution_) << theme.reset << "\n";
            }
        } else {
            // auto:门槛判定(首版无 verified 画像,恒 json)。
            lubancode::ptc::PtcAutoGates gates;
            gates.profile_status = profile_status;
            gates.hard_conditions_met = hard.AllMet();
            gates.estimated_chain_depth = 0;  // 预估器未建:保守零
            gates.estimated_fanout = 0;
            const auto decision = lubancode::ptc::ResolveToolCalling(gates);
            if (decision == lubancode::ptc::ToolCallingDecision::Programmatic) {
                registered = tool.get();
                main_registry_.Register(std::move(tool));
                ptc_resolution_ = "auto→ptc";
            } else {
                ptc_resolution_ = "auto→json";
                if (!python_ok && !config.ptc.python.empty()) {
                    std::cout << theme.error << trf("ptc.probe_failed", tool->unavailability_reason())
                              << theme.reset << "\n";
                }
            }
        }
        ptc_tool_ = registered;
    }
}

void ToolRuntime::SetPluginCancel(const std::atomic<bool>* cancel) {
    // 三路插件都要:process(adapter 的进程超时/取消同一落锤路)、Lua(hook
    // 里查这面旗掐死循环)、manifest-backed Lua(owner 灌给 state 的
    // instruction hook 与受控 HTTP 回调——同一枚旗,§8.4)。turn_runner 每轮
    // 灌(plugins 单第 7 步的 ESC 链)。run_command 同链(进程生命线单 P1:
    // 前台命令的取消通道)。
    lua_runtime_.SetCancel(cancel);
    manifest_lua_runtime_.SetCancel(cancel);
    for (const auto& adapter : main_registry_.All()) {
        if (auto* plugin_adapter = dynamic_cast<lubancode::runtime::PluginToolAdapter*>(adapter.get());
            plugin_adapter != nullptr) {
            plugin_adapter->SetCancel(cancel);
        }
        if (auto* run_command = dynamic_cast<lubancode::tools::RunCommandTool*>(adapter.get());
            run_command != nullptr) {
            run_command->SetCancel(cancel);
        }
    }
    for (const auto& adapter : sub_registry_.All()) {
        if (auto* plugin_adapter = dynamic_cast<lubancode::runtime::PluginToolAdapter*>(adapter.get());
            plugin_adapter != nullptr) {
            plugin_adapter->SetCancel(cancel);
        }
        if (auto* run_command = dynamic_cast<lubancode::tools::RunCommandTool*>(adapter.get());
            run_command != nullptr) {
            run_command->SetCancel(cancel);
        }
    }
}

void ToolRuntime::SetPluginCwd(std::string cwd_utf8) {
    for (const auto& adapter : main_registry_.All()) {
        if (auto* plugin_adapter = dynamic_cast<lubancode::runtime::PluginToolAdapter*>(adapter.get());
            plugin_adapter != nullptr) {
            plugin_adapter->SetCwd(cwd_utf8);
        }
    }
    for (const auto& adapter : sub_registry_.All()) {
        if (auto* plugin_adapter = dynamic_cast<lubancode::runtime::PluginToolAdapter*>(adapter.get());
            plugin_adapter != nullptr) {
            plugin_adapter->SetCwd(cwd_utf8);
        }
    }
}

void ToolRuntime::SetPluginLogSink(lubancode::runtime::PluginLogSink sink) {
    for (const auto& adapter : main_registry_.All()) {
        if (auto* plugin_adapter = dynamic_cast<lubancode::runtime::PluginToolAdapter*>(adapter.get());
            plugin_adapter != nullptr) {
            plugin_adapter->SetLogSink(sink);
        }
    }
    for (const auto& adapter : sub_registry_.All()) {
        if (auto* plugin_adapter = dynamic_cast<lubancode::runtime::PluginToolAdapter*>(adapter.get());
            plugin_adapter != nullptr) {
            plugin_adapter->SetLogSink(sink);
        }
    }
}

void ToolRuntime::AttachMemoryTool(std::shared_ptr<lubancode::memory::ProjectMemory> memory) {
    // capability gate:全局未授权时连工具都不注册(规格"授权边界"节)。
    if (memory != nullptr && memory->generate_enabled() &&
        main_registry_.Find("memory_save") == nullptr) {
        main_registry_.Register(std::make_unique<lubancode::memory::MemorySaveTool>(std::move(memory)));
    }
}

void ToolRuntime::AttachUndoTool(lubancode::runtime::ToolTraceHub* trace_hub) {
    // 条件式撤销的执行侧(逐枚追踪单第四期):主表与子表都挂——undo 是
    // 写操作,needs_confirm 恒真,与 write/edit 同一道确认门,子代理调它
    // 一样要过自己的确认链。explore 只读表不挂。hub 为空 = 会话没装
    // trace,工具挂了也查不到凭据,干脆不挂。
    if (trace_hub == nullptr) {
        return;
    }
    tools::UndoTokenLookup lookup;
    lookup.find = [trace_hub](const std::string& execution_id) {
        return trace_hub->FindUndoToken(execution_id);
    };
    lookup.owner_of = [trace_hub](const std::string& execution_id) {
        return trace_hub->OwnerOfExecution(execution_id);
    };
    if (main_registry_.Find("undo_file_edit") == nullptr) {
        main_registry_.Register(std::make_unique<lubancode::tools::UndoFileEditTool>(std::move(lookup)));
    }
    if (sub_registry_.Find("undo_file_edit") == nullptr) {
        sub_registry_.Register(std::make_unique<lubancode::tools::UndoFileEditTool>(std::move(lookup)));
    }
}

std::string ToolRuntime::LastCompensatesOf(const std::string& tool_use_id) const {
    // undo 工具的 last_compensates 在 execute 里查;这里按工具实例取
    // 最近一次的报账(补偿关系边随 finished 落账,时序上 execute 完成后
    // 立即被问,不会串到下一枚)。
    (void)tool_use_id;
    if (const auto* tool = main_registry_.Find("undo_file_edit");
        tool != nullptr) {
        if (const auto* undo_tool = dynamic_cast<const lubancode::tools::UndoFileEditTool*>(tool);
            undo_tool != nullptr) {
            return undo_tool->last_compensates();
        }
    }
    return std::string();
}

}  // namespace lubancode::app
