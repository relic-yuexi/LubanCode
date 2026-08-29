// /package 命令(统一封装单阶段 1/4/6):list/show/doctor 只读版 +
// trust/untrust 信任门(阶段 4:批的是整包内容哈希,重启生效)+ enable/
// disable 启停账与 reload 重折快照(阶段 6:启停在包外,生效在下回装配)。
// scaffold 是后续阶段的事。命令参数拆解是纯函数,单测直接钉;IO(扫描、
// 打印、信任账、启停账)在 cpp 的 handler 一头。
#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <optional>
#include <string>

namespace lubancode::app {

// 定义在 command_registry.hpp(struct;前置声明与定义统一,防 MSVC C4099)。
struct SlashDispatchContext;

enum class PackageCommandAction {
    Invalid,
    List,    // /package list [all|user|project|official|dev]
    Show,    // /package show <id>
    Doctor,  // /package doctor <id|路径>
    Trust,   // /package trust <id>(批准整包内容哈希,重启生效)
    Untrust, // /package untrust <id>(销账)
    Enable,  // /package enable <id>(复启;下回装配生效)
    Disable, // /package disable <id>(停用;挂载一律跳过,下回装配生效)
    Reload,  // /package reload(重扫五路折新快照,折好才换)
};

struct ParsedPackageCommand {
    PackageCommandAction action = PackageCommandAction::Invalid;
    std::optional<std::string> scope_filter;  // List 时:user/project/official/dev;nullopt = all
    std::string target;                       // Show/Doctor/Trust/Enable/Disable 的 id 或路径
    std::string bad_word;                     // Invalid 时第一词的原始拼写(提示用)
};

// 纯解析:拆出动作、过滤层、目标。认不得的子命令、show/doctor 缺目标一律
// Invalid,由 handler 统一打用法。
ParsedPackageCommand ParsePackageCommand(const std::string& args);

// /package 的分派位(命令注册表登册用)。
CommandFlow HandleSlashPackage(SlashDispatchContext& ctx,
                               const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
