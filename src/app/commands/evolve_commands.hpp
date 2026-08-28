// /evolve 命令(自进化闭环阶段 1):status/list/show 只读面。
// 只看观察账、只采观察——不生成 Package,不装任何东西(propose/approve 一类
// 写动作是后续阶段的事,这里不冒头)。参数拆解是纯函数,单测直接钉;IO
// (扫描、落账、打印)在 cpp 的 handler 一头。照 /package 的先例。
#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <string>

namespace lubancode::app {

class SlashDispatchContext;

enum class EvolveCommandAction {
    Invalid,
    Status,  // /evolve status(裸 /evolve 同义):扫五路账本采观察,报账面
    List,    // /evolve list [run|goal|recording|tooltrace|memory|all]:按指纹聚类列账
    Show,    // /evolve show <观察id>:看一条观察的全文与证据指回
};

struct ParsedEvolveCommand {
    EvolveCommandAction action = EvolveCommandAction::Invalid;
    std::string source_filter;  // List 时:run/goal/recording/tooltrace/memory;nullopt 语义用空串 = all
    std::string target;         // Show 的观察 id
    std::string bad_word;       // Invalid 时第一词的原始拼写(提示用)
};

// 纯解析:拆出动作、来源过滤、目标。认不得的子命令、show 缺 id 一律
// Invalid,由 handler 统一打用法。
ParsedEvolveCommand ParseEvolveCommand(const std::string& args);

// /evolve 的分派位(命令注册表登册用)。
CommandFlow HandleSlashEvolve(SlashDispatchContext& ctx,
                              const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
