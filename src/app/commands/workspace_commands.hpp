// 工作区类 slash 命令的打印件:/tools 三态、/worktree 结果、/plugins、
// /mcp、/lsp。命令的交互接线(确认、取参数)仍在 InteractiveLoop。
// 路径杂务(PathToUtf8/SameFilesystemPath)两边都要用,一并住这,第八步
// 清债时再考虑挪 platform/paths。
//
// 搬家自 main.cpp,行为一字未改;依赖只认 cli/tools/lsp/mcp/app 装配层。

#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app/tool_runtime.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "cli/worktree.hpp"
#include "lsp/manager.hpp"
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
        std::cout << trf("cmd.tools.no_deferral", registry.All().size(),
                          threshold == 0 ? tr("cmd.tools.threshold_zero")
                                          : trf("cmd.tools.below_threshold", threshold))
                   << "\n";
        for (const auto& tool : registry.All()) {
            std::cout << "  - " << tool->name() << "\n";
        }
        return;
    }
    std::cout << trf("cmd.tools.enabled", threshold) << "\n";
    std::cout << trf("cmd.tools.core", core.size()) << "\n";
    for (const auto* tool : core) {
        std::cout << "  - " << tool->name() << "\n";
    }
    std::cout << trf("cmd.tools.loaded", loaded_deferred.size()) << "\n";
    for (const auto* tool : loaded_deferred) {
        std::cout << "  - " << tool->name() << "\n";
    }
    if (loaded_deferred.empty()) {
        std::cout << tr("cmd.tools.none_loaded") << "\n";
    }
    std::cout << trf("cmd.tools.pending", pending_deferred.size()) << "\n";
    for (const auto* tool : pending_deferred) {
        std::cout << "  - " << tool->name() << "\n";
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
            std::cout << trf("cmd.worktree.created", PathToUtf8(result.path), result.branch) << "\n";
            break;
        case worktree::WorktreeResultCode::Listed:
            std::cout << tr("cmd.worktree.list_header") << "\n";
            for (const auto& entry : result.entries) {
                const bool current = SameFilesystemPath(entry.path, result.path);
                std::cout << "  " << (current ? "* " : "- ") << PathToUtf8(entry.path);
                if (!entry.branch.empty()) {
                    std::cout << " [" << entry.branch << "]";
                } else if (entry.detached) {
                    std::cout << " " << tr("cmd.worktree.detached");
                }
                if (current) {
                    std::cout << " " << tr("cmd.worktree.current");
                }
                std::cout << "\n";
            }
            break;
        case worktree::WorktreeResultCode::Kept:
            std::cout << trf("cmd.worktree.kept", PathToUtf8(result.path)) << "\n";
            break;
        case worktree::WorktreeResultCode::Removed:
            std::cout << trf("cmd.worktree.removed", result.branch) << "\n";
            break;
        case worktree::WorktreeResultCode::NeedsRemoveConfirmation:
            std::cout << trf("cmd.worktree.dirty", PathToUtf8(result.path)) << "\n";
            break;
        case worktree::WorktreeResultCode::NeedsUserConfirmation:
            std::cout << trf("cmd.worktree.outside_confirm", PathToUtf8(result.path)) << "\n";
            break;
        case worktree::WorktreeResultCode::VerificationFailed:
            std::cout << trf("cmd.worktree.verify_failed", result.detail) << "\n";
            break;
        case worktree::WorktreeResultCode::NotRepository:
            std::cout << tr("cmd.worktree.not_repo") << "\n";
            break;
        case worktree::WorktreeResultCode::InvalidArgument:
            std::cout << tr("cmd.worktree.usage") << "\n";
            break;
        case worktree::WorktreeResultCode::InvalidName:
            std::cout << tr("cmd.worktree.invalid_name") << "\n";
            break;
        case worktree::WorktreeResultCode::AlreadyActive:
            std::cout << tr("cmd.worktree.already_active") << "\n";
            break;
        case worktree::WorktreeResultCode::NoActiveWorktree:
            std::cout << tr("cmd.worktree.no_active") << "\n";
            break;
        case worktree::WorktreeResultCode::GitError:
            std::cout << trf("cmd.worktree.git_failed", result.detail) << "\n";
            break;
        case worktree::WorktreeResultCode::FilesystemError:
            std::cout << trf("cmd.worktree.filesystem_failed", result.detail) << "\n";
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

// /plugins 命令:列已挂载的插件工具(完整工具名 + 类别)和启动时的加载
// 警告;一个都没有时打印目录约定,顺带说明两类插件各自怎么写。
void PrintPluginsCommand(const std::vector<PluginMountInfo>& mounted, const std::vector<std::string>& warnings) {
    if (mounted.empty() && warnings.empty()) {
        const auto home_dir = lubancode::config::HomeLubancodeDir();
        const std::string dir =
            (home_dir.has_value() ? *home_dir : tr("path.no_home") + "/.lubancode") + "/plugins";
        std::cout << trf("cmd.plugins.empty", dir) << "\n";
        return;
    }
    if (!mounted.empty()) {
        std::cout << trf("cmd.plugins.mounted", mounted.size()) << "\n";
        for (const auto& info : mounted) {
            std::cout << "  - " << info.tool_name << "  (" << info.kind << ")\n";
        }
    }
    if (!warnings.empty()) {
        std::cout << tr("cmd.plugins.warnings") << "\n";
        for (const auto& warning : warnings) {
            std::cout << "  - " << warning << "\n";
        }
    }
}

// /mcp 命令:每个服务器一行状态(运行中/已退出)+ 工具数,底下缩进列出
// 完整工具名(mcp__服务器名__工具名,跟模型实际看到的名字一致)。
void PrintMcpCommand(const std::vector<McpServerRuntime>& mcp_servers) {
    if (mcp_servers.empty()) {
        std::cout << tr("cmd.mcp.empty") << "\n";
        return;
    }
    for (const auto& runtime : mcp_servers) {
        const bool alive = runtime.client != nullptr && runtime.client->Alive();
        std::cout << trf("cmd.mcp.line", runtime.name, alive ? tr("mcp.state.alive") : tr("mcp.state.dead"),
                          runtime.tools.size())
                   << "\n";
        for (const auto& tool_info : runtime.tools) {
            std::cout << "      mcp__" << runtime.name << "__" << tool_info.name << "\n";
        }
    }
}

// /lsp 命令:每个配置了的语言一行状态(未启动/运行中/已闲置关停/已退出)。
// StatusList() 要顺手收割闲置进程(改内部状态),所以入参是可变引用,
// 不装 const。
void PrintLspCommand(std::optional<lubancode::lsp::Manager>& lsp_manager) {
    if (!lsp_manager.has_value()) {
        std::cout << tr("cmd.lsp.empty") << "\n";
        return;
    }
    const auto statuses = lsp_manager->StatusList();
    std::cout << trf("cmd.lsp.header", statuses.size()) << "\n";
    for (const auto& status : statuses) {
        std::cout << "  - " << status.language << " (" << status.command << "): " << status.state << "\n";
    }
}

}  // namespace lubancode::app
