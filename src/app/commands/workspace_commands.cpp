// workspace_commands.hpp 的实现:工具清单/插件/MCP/LSP/worktree 命令的函数体。
#include "app/commands/workspace_commands.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

using lubancode::cli::TermOut;
using lubancode::cli::TermErr;

#include <iostream>

#include "cli/console_input.hpp"
#include "tools/background_tasks.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app/tool_runtime.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "cli/worktree.hpp"
#include "lsp/manager.hpp"
#include "platform/process.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/plugin_contract.hpp"
#include "tools/path_utils.hpp"
#include "tools/registry.hpp"

namespace lubancode::app {


using lubancode::cli::tr;
using lubancode::cli::trf;

// 各带计数。没启用延迟机制(总数没超阈值,或阈值是 0)时说明一句,不摆
// 三态的空架子。
void PrintToolsCommand(const lubancode::tools::ToolRegistry& registry, const std::set<std::string>& loaded,
                        bool deferral_enabled, int threshold) {
    std::vector<const lubancode::tools::Tool*> core;
    std::vector<const lubancode::tools::Tool*> loaded_deferred;
    std::vector<const lubancode::tools::Tool*> pending_deferred;
    for (const auto& tool : registry.All()) {
        if (!tool->deferred()) {
            core.push_back(tool.get());
        } else if (loaded.count(tool->name()) != 0) {
            loaded_deferred.push_back(tool.get());
        } else {
            pending_deferred.push_back(tool.get());
        }
    }
    if (!deferral_enabled) {
        TermOut() << trf("cmd.tools.no_deferral", registry.All().size(),
                          threshold == 0 ? tr("cmd.tools.threshold_zero")
                                          : trf("cmd.tools.below_threshold", threshold))
                   << "\n";
        for (const auto& tool : registry.All()) {
            TermOut() << "  - " << tool->name() << "\n";
        }
        return;
    }
    TermOut() << trf("cmd.tools.enabled", threshold) << "\n";
    TermOut() << trf("cmd.tools.core", core.size()) << "\n";
    for (const auto* tool : core) {
        TermOut() << "  - " << tool->name() << "\n";
    }
    TermOut() << trf("cmd.tools.loaded", loaded_deferred.size()) << "\n";
    for (const auto* tool : loaded_deferred) {
        TermOut() << "  - " << tool->name() << "\n";
    }
    if (loaded_deferred.empty()) {
        TermOut() << tr("cmd.tools.none_loaded") << "\n";
    }
    TermOut() << trf("cmd.tools.pending", pending_deferred.size()) << "\n";
    for (const auto* tool : pending_deferred) {
        TermOut() << "  - " << tool->name() << "\n";
    }
}

std::string PathToUtf8(const std::filesystem::path& path);
bool SameFilesystemPath(const std::filesystem::path& left, const std::filesystem::path& right);

// /worktree 的显示层只拿 i18n 键说话。Git 调用和目录状态都在 cli/worktree
// 里，main 只管给交互会话报结果、在脏树删除前收一声确认。
void PrintWorktreeResult(const lubancode::cli::WorktreeResult& result) {
    namespace worktree = lubancode::cli;
    switch (result.code) {
        case worktree::WorktreeResultCode::Created:
            TermOut() << trf("cmd.worktree.created", PathToUtf8(result.path), result.branch) << "\n";
            break;
        case worktree::WorktreeResultCode::Listed:
            TermOut() << tr("cmd.worktree.list_header") << "\n";
            for (const auto& entry : result.entries) {
                const bool current = SameFilesystemPath(entry.path, result.path);
                TermOut() << "  " << (current ? "* " : "- ") << PathToUtf8(entry.path);
                if (!entry.branch.empty()) {
                    TermOut() << " [" << entry.branch << "]";
                } else if (entry.detached) {
                    TermOut() << " " << tr("cmd.worktree.detached");
                }
                if (current) {
                    TermOut() << " " << tr("cmd.worktree.current");
                }
                TermOut() << "\n";
            }
            break;
        case worktree::WorktreeResultCode::Kept:
            TermOut() << trf("cmd.worktree.kept", PathToUtf8(result.path)) << "\n";
            break;
        case worktree::WorktreeResultCode::Removed:
            TermOut() << trf("cmd.worktree.removed", result.branch) << "\n";
            break;
        case worktree::WorktreeResultCode::NeedsRemoveConfirmation:
            TermOut() << trf("cmd.worktree.dirty", PathToUtf8(result.path)) << "\n";
            break;
        case worktree::WorktreeResultCode::NeedsUserConfirmation:
            TermOut() << trf("cmd.worktree.outside_confirm", PathToUtf8(result.path)) << "\n";
            break;
        case worktree::WorktreeResultCode::VerificationFailed:
            TermOut() << trf("cmd.worktree.verify_failed", result.detail) << "\n";
            break;
        case worktree::WorktreeResultCode::NotRepository:
            TermOut() << tr("cmd.worktree.not_repo") << "\n";
            break;
        case worktree::WorktreeResultCode::InvalidArgument:
            TermOut() << tr("cmd.worktree.usage") << "\n";
            break;
        case worktree::WorktreeResultCode::InvalidName:
            TermOut() << tr("cmd.worktree.invalid_name") << "\n";
            break;
        case worktree::WorktreeResultCode::AlreadyActive:
            TermOut() << tr("cmd.worktree.already_active") << "\n";
            break;
        case worktree::WorktreeResultCode::NoActiveWorktree:
            TermOut() << tr("cmd.worktree.no_active") << "\n";
            break;
        case worktree::WorktreeResultCode::GitError:
            TermOut() << trf("cmd.worktree.git_failed", result.detail) << "\n";
            break;
        case worktree::WorktreeResultCode::FilesystemError:
            TermOut() << trf("cmd.worktree.filesystem_failed", result.detail) << "\n";
            break;
    }
}

// 当前工作目录,转成 UTF-8 字符串(拼进系统提示词里给模型看)。
std::string PathToUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}
bool SameFilesystemPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code ec;
    return std::filesystem::equivalent(left, right, ec) && !ec;
}

// 打不打,这个函数本身不做 is_console 判断。

// /plugins 命令:列已挂载的插件工具(完整工具名 + runtime)和启动时的加载
// 警告;一个都没有时打印目录约定,顺带说明三路插件各自怎么写(plugins 单
// 第 8 步:不再是"只列 Lua/DLL",process 插件一并入账)。
void PrintPluginsCommand(const std::vector<PluginMountInfo>& mounted, const std::vector<std::string>& warnings) {
    if (mounted.empty() && warnings.empty()) {
        const auto home_dir = lubancode::config::HomeLubancodeDir();
        const std::string dir =
            (home_dir.has_value() ? *home_dir : tr("path.no_home") + "/.lubancode") + "/plugins";
        TermOut() << trf("cmd.plugins.empty", dir) << "\n";
        return;
    }
    if (!mounted.empty()) {
        TermOut() << trf("cmd.plugins.mounted", mounted.size()) << "\n";
        for (const auto& info : mounted) {
            TermOut() << "  - " << info.tool_name << "  (" << info.kind << ")\n";
        }
    }
    if (!warnings.empty()) {
        TermOut() << tr("cmd.plugins.warnings") << "\n";
        for (const auto& warning : warnings) {
            TermOut() << "  - " << warning << "\n";
        }
    }
}

// /plugin 子命令(plugins 单第 8 步)的实现。三路插件的账都从调用方递
// 进来:process 走 manifests,native/Lua 走 mounted(完整工具名前缀对
// 得上插件 id)。doctor 只探环境不执行 tool;test 走与模型调用同一条
// 执行链(这里给 v1 的最短版:经 ToolRuntime 的 registry 由调用方真跑,
// 命令层只出说明——真跑要确认流,硬造一条免确认的捷径正是单子禁的)。
void HandlePluginCommand(const std::string& args,
                         const std::vector<PluginMountInfo>& mounted,
                         const std::vector<std::shared_ptr<const lubancode::runtime::PluginManifest>>& manifests) {
    // 拆子命令与目标 id。
    std::string sub = args;
    std::string rest;
    const std::size_t space = args.find_first_of(" \t");
    if (space != std::string::npos) {
        sub = args.substr(0, space);
        rest = args.substr(space + 1);
    }
    // 去掉 rest 两端空白。
    const auto begin = rest.find_first_not_of(" \t");
    rest = begin == std::string::npos ? std::string() : rest.substr(begin);
    const auto end = rest.find_last_not_of(" \t");
    if (end != std::string::npos) {
        rest = rest.substr(0, end + 1);
    }

    if (sub.empty()) {
        TermOut() << tr("cmd.plugin.usage") << "\n";
        return;
    }

    // 形态一:/plugin inspect <id>(sub 是子动词,target 在 rest)。
    // 形态二:/plugin <id>(裸 id,视同 inspect)。
    const bool sub_is_verb =
        sub == "inspect" || sub == "doctor" || sub == "reload" || sub == "enable" || sub == "disable" || sub == "test";
    const std::string target_id = sub_is_verb ? rest : sub;
    const std::string action = sub_is_verb ? sub : std::string("inspect");
    const lubancode::runtime::PluginManifest* manifest = nullptr;
    if (!target_id.empty()) {
        for (const auto& m : manifests) {
            if (m->id == target_id) {
                manifest = m.get();
            }
        }
    }

    if (action == "inspect") {
        if (manifest != nullptr) {
            TermOut() << trf("cmd.plugin.inspect.header", manifest->id, manifest->version,
                             std::string(lubancode::runtime::RuntimeKindName(manifest->kind)),
                             manifest->language.empty() ? std::string("-") : manifest->language)
                      << "\n";
            TermOut() << trf("cmd.plugin.inspect.dir", lubancode::tools::PathToUtf8(manifest->plugin_dir)) << "\n";
            if (manifest->kind == lubancode::runtime::RuntimeKind::Process) {
                std::string argv_text;
                for (const auto& a : manifest->argv) {
                    argv_text += argv_text.empty() ? a : (" " + a);
                }
                TermOut() << trf("cmd.plugin.inspect.argv", argv_text) << "\n";
                TermOut() << trf("cmd.plugin.inspect.timeout", manifest->timeout_ms) << "\n";
                if (!manifest->env_allowlist.empty()) {
                    std::string env_names;
                    for (const auto& name : manifest->env_allowlist) {
                        env_names += env_names.empty() ? name : (", " + name);
                    }
                    TermOut() << trf("cmd.plugin.inspect.env", env_names) << "\n";
                }
            }
            TermOut() << trf("cmd.plugin.inspect.tools", manifest->tools.size()) << "\n";
            for (const auto& tool : manifest->tools) {
                TermOut() << "  - " << tool.full_name << "\n";
            }
            return;
        }
        // native/Lua 的 inspect:mounted 里按前缀找。
        const std::string prefix = "plugin__" + target_id + "__";
        bool found = false;
        for (const auto& info : mounted) {
            if (info.tool_name.rfind(prefix, 0) == 0) {
                if (!found) {
                    TermOut() << trf("cmd.plugin.inspect.legacy_header", target_id, info.kind) << "\n";
                    found = true;
                }
                TermOut() << "  - " << info.tool_name << "\n";
            }
        }
        if (found) {
            return;
        }
        TermOut() << trf("cmd.plugin.not_found", target_id) << "\n";
        return;
    }

    if (action == "doctor") {
        // doctor:process 查解释器起不起得来;native/Lua 只报在不在账上。
        if (manifest != nullptr) {
            if (manifest->kind == lubancode::runtime::RuntimeKind::Process) {
                const auto result = lubancode::platform::RunProcess({manifest->argv[0], "--version"}, 15000);
                if (result.spawn_failed || result.exit_code != 0) {
                    TermOut() << trf("cmd.plugin.doctor.command_bad", manifest->argv[0],
                                     result.spawn_failed ? result.spawn_error : std::to_string(result.exit_code))
                              << "\n";
                } else {
                    std::string version = result.output;
                    if (version.size() > 80) {
                        version = version.substr(0, 80) + "...";
                    }
                    TermOut() << trf("cmd.plugin.doctor.command_ok", manifest->argv[0], version) << "\n";
                }
            } else {
                TermOut() << tr("cmd.plugin.doctor.not_process") << "\n";
            }
            return;
        }
        for (const auto& info : mounted) {
            const std::string prefix = "plugin__" + target_id + "__";
            if (info.tool_name.rfind(prefix, 0) == 0) {
                TermOut() << trf("cmd.plugin.doctor.legacy_ok", info.kind) << "\n";
                return;
            }
        }
        TermOut() << trf("cmd.plugin.not_found", target_id) << "\n";
        return;
    }

    if (action == "test") {
        // test 的口径:与模型调用同一条链(schema 验参、确认、timeout)。
        // 确认流在交互层,命令层不另开无防护捷径——这里指路,真跑让模型
        // 调(或用 scaffold 生成的 test_runner.py 离线自测)。
        TermOut() << tr("cmd.plugin.test.hint") << "\n";
        return;
    }

    if (action == "reload") {
        TermOut() << tr("cmd.plugin.reload.hint") << "\n";
        return;
    }
    if (action == "enable" || action == "disable") {
        TermOut() << tr("cmd.plugin.toggle.hint") << "\n";
        return;
    }

    TermOut() << trf("cmd.plugin.unknown_sub", sub) << "\n";
    TermOut() << tr("cmd.plugin.usage") << "\n";
}

// /mcp 命令:每个服务器一行状态(运行中/已退出)+ 工具数,底下缩进列出
// 完整工具名(mcp__服务器名__工具名,跟模型实际看到的名字一致)。
void PrintMcpCommand(const std::vector<McpServerRuntime>& mcp_servers) {
    if (mcp_servers.empty()) {
        TermOut() << tr("cmd.mcp.empty") << "\n";
        return;
    }
    for (const auto& runtime : mcp_servers) {
        const bool alive = runtime.client != nullptr && runtime.client->Alive();
        TermOut() << trf("cmd.mcp.line", runtime.name, alive ? tr("mcp.state.alive") : tr("mcp.state.dead"),
                          runtime.tools.size())
                   << "\n";
        for (const auto& tool_info : runtime.tools) {
            TermOut() << "      mcp__" << runtime.name << "__" << tool_info.name << "\n";
        }
    }
}

// /lsp 命令:每个配置了的语言一行状态(未启动/运行中/已闲置关停/已退出)。
// StatusList() 要顺手收割闲置进程(改内部状态),所以入参是可变引用,
// 不装 const。
void PrintLspCommand(std::optional<lubancode::lsp::Manager>& lsp_manager) {
    if (!lsp_manager.has_value()) {
        TermOut() << tr("cmd.lsp.empty") << "\n";
        return;
    }
    const auto statuses = lsp_manager->StatusList();
    TermOut() << trf("cmd.lsp.header", statuses.size()) << "\n";
    for (const auto& status : statuses) {
        TermOut() << "  - " << status.language << " (" << status.command << "): " << status.state << "\n";
    }
}

// ---------------------------------------------------------------------------
// 工作区命令 handler:原样搬自会话主循环的 slash case,行为一字未改。
// ---------------------------------------------------------------------------

CommandFlow HandleWorktreeCommand(WorkspaceCommandState& state, const std::string& args,
                                  const lubancode::cli::Theme& theme) {
    const lubancode::cli::ParsedWorktreeCommand command = lubancode::cli::ParseWorktreeCommand(args);
    lubancode::cli::WorktreeResult result;
    switch (command.action) {
        case lubancode::cli::WorktreeAction::New:
            result = state.worktree.Create(command.name);
            break;
        case lubancode::cli::WorktreeAction::List:
            result = state.worktree.List();
            break;
        case lubancode::cli::WorktreeAction::Exit:
            result = state.worktree.Exit(command.exit_mode);
            break;
        case lubancode::cli::WorktreeAction::Invalid:
            result.code = lubancode::cli::WorktreeResultCode::InvalidArgument;
            break;
    }
    PrintWorktreeResult(result);
    if (result.code == lubancode::cli::WorktreeResultCode::NeedsRemoveConfirmation) {
        const std::optional<std::string> answer = lubancode::cli::ReadLine(
            theme.confirm + tr("cmd.worktree.remove_confirm") + theme.reset, theme,
            /*esc_rejects=*/true);
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            result = state.worktree.ConfirmRemove();
            PrintWorktreeResult(result);
        } else {
            TermOut() << tr("cmd.worktree.remove_cancelled") << "\n";
        }
    }
    // /worktree new 撞上园子外的已有房:跟模型工具同一道硬确认。
    if (result.code == lubancode::cli::WorktreeResultCode::NeedsUserConfirmation) {
        const std::optional<std::string> answer = lubancode::cli::ReadLine(
            theme.confirm + tr("cmd.worktree.outside_prompt") + theme.reset, theme,
            /*esc_rejects=*/true);
        if (answer.has_value() && (*answer == "y" || *answer == "Y")) {
            result = state.worktree.Enter(command.name, /*base=*/"head", /*confirmed_outside=*/true);
            PrintWorktreeResult(result);
        }
    }
    // std::filesystem::current_path 是工具层共同的相对路径基准。同步提示词
    // 和子代理那份 cwd,历史则原样保留。
    if (state.sync_worktree_directory) {
        state.sync_worktree_directory();
    }
    return CommandFlow::Continue;
}

CommandFlow HandleBackgroundCommand(const lubancode::cli::Theme& theme) {
    // /background:列后台命令任务清单。文案直接用字面量(跟
    // background_output 工具的返回文本一个路数),不经 i18n——这条命令是
    // 给开发者的运维视图,新功能先不铺多语言。
    // 0.30.x 第四批起每项补三行尾巴:最近三行非空输出、启动时间、已跑
    // 时长(仿 Codex /ps 的省眼力手法);日志被删/非法 UTF-8/进程已死都
    // 只是那一项少几行,菜单不带崩。
    const auto tasks = lubancode::tools::BackgroundTaskRegistry::Instance().List();
    if (tasks.empty()) {
        TermOut() << "当前没有后台任务。" << "\n";
        return CommandFlow::Continue;
    }
    TermOut() << "后台任务共 " << tasks.size() << " 个:" << "\n" << "\n";
    for (const auto& t : tasks) {
        const char* label = "未知";
        switch (t.status) {
            case lubancode::tools::BackgroundTaskStatus::Running: label = "运行中"; break;
            case lubancode::tools::BackgroundTaskStatus::Stopping: label = "停止中"; break;
            case lubancode::tools::BackgroundTaskStatus::Completed: label = "完成"; break;
            case lubancode::tools::BackgroundTaskStatus::Failed: label = "失败"; break;
            case lubancode::tools::BackgroundTaskStatus::Stopped: label = "已停止"; break;
            case lubancode::tools::BackgroundTaskStatus::StopFailed: label = "停止失败"; break;
        }
        // 时长:运行中 = now - start;终态 = finish - start(拿不到按 0)。
        const auto span_end = t.status == lubancode::tools::BackgroundTaskStatus::Running ||
                                      t.status == lubancode::tools::BackgroundTaskStatus::Stopping
                                  ? std::chrono::system_clock::now()
                                  : t.finish_time;
        const long long secs =
            span_end > t.start_time
                ? std::chrono::duration_cast<std::chrono::seconds>(span_end - t.start_time).count()
                : 0;
        TermOut() << theme.tool_line << "[#" << t.task_id << "] " << label;
        if (t.status != lubancode::tools::BackgroundTaskStatus::Running &&
            t.status != lubancode::tools::BackgroundTaskStatus::Stopping) {
            TermOut() << " (exit "
                      << (t.exit.exit_code.has_value() ? std::to_string(*t.exit.exit_code) : "unknown") << ")";
        }
        TermOut() << theme.reset << "  PID=" << t.pid << "  已跑 " << secs << "s" << "\n"
                  << theme.stats << "  命令: " << t.command << theme.reset << "\n"
                  << theme.stats << "  日志: " << t.log_path << theme.reset << "\n";
        // 尾巴:尾部若干行里挑最近三行非空(输出正增长时看得到在动)。
        if (const std::string tail =
                lubancode::tools::BackgroundTaskRegistry::Instance().ReadOutput(t.task_id, 24);
            !tail.empty()) {
            std::string safe = lubancode::platform::SanitizeExternalText(tail);
            std::vector<std::string> non_empty;
            std::size_t line_start = 0;
            while (line_start <= safe.size()) {
                const std::size_t line_end = safe.find('\n', line_start);
                std::string line = safe.substr(
                    line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                    line.pop_back();
                }
                if (!line.empty()) {
                    non_empty.push_back(std::move(line));
                }
                if (line_end == std::string::npos) {
                    break;
                }
                line_start = line_end + 1;
            }
            if (non_empty.size() > 3) {
                non_empty.erase(non_empty.begin(),
                                non_empty.end() - 3);  // 只要最近三行非空
            }
            for (const std::string& line : non_empty) {
                TermOut() << theme.stats << "  ⎿ " << line << theme.reset << "\n";
            }
        }
        TermOut() << "\n";
    }
    return CommandFlow::Continue;
}

}  // namespace lubancode::app
