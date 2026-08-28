// /package 命令(统一 Package 封装单阶段 1):list/show/doctor 只读版。
// 只查只诊,不挂任何组件——trust/enable/disable/scaffold 等写动作是后
// 续阶段的事,这里不冒头。命令参数拆解是纯函数,单测直接钉;IO(扫描、
// 打印)在 cpp 的 handler 一头。
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
};

struct ParsedPackageCommand {
    PackageCommandAction action = PackageCommandAction::Invalid;
    std::optional<std::string> scope_filter;  // List 时:user/project/official/dev;nullopt = all
    std::string target;                       // Show/Doctor 的 id 或路径
    std::string bad_word;                     // Invalid 时第一词的原始拼写(提示用)
};

// 纯解析:拆出动作、过滤层、目标。认不得的子命令、show/doctor 缺目标一律
// Invalid,由 handler 统一打用法。
ParsedPackageCommand ParsePackageCommand(const std::string& args);

// /package 的分派位(命令注册表登册用)。
CommandFlow HandleSlashPackage(SlashDispatchContext& ctx,
                               const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
