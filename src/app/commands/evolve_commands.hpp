// /evolve 命令(自进化闭环阶段 1/2/3/4):status/list/show 只读面,阶段 2 的
// propose/diff/reject,阶段 3 的 test,阶段 4 的 approve/use/promote/rollback
// ——批准页、点名 canary、晋升与回滚。候选状态机的写笔全在
// EvolutionCoordinator(契约铁律),命令层只递材料、只打印,不自写迁移。
#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "app/cli_options.hpp"            // EvolveTestArgs(CI 子命令)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <string>

namespace lubancode::app {

// 定义在 command_registry.hpp(struct;前置声明与定义统一,防 MSVC C4099)。
struct SlashDispatchContext;

enum class EvolveCommandAction {
    Invalid,
    Status,   // /evolve status(裸 /evolve 同义):扫五路账本采观察,报账面
    List,     // /evolve list [run|goal|recording|tooltrace|memory|all]:按指纹聚类列账 + 候选区
    Show,     // /evolve show <观察id|候选id>:看一条观察或一只候选,指回来源
    Propose,  // /evolve propose <recording-id|observation-id>:起草并落候选(阶段 2)
    Diff,     // /evolve diff <candidate-id>:与父版或空对照(阶段 2)
    Reject,   // /evolve reject <candidate-id> [reason]:落 rejected,指纹进拒绝账(阶段 2)
    Test,     // /evolve test <candidate-id>:跑评测五道门,账只追加(阶段 3)
    Approve,  // /evolve approve <candidate-id>:出批准页并装 store(阶段 4)
    Use,      // /evolve use <candidate-id>:点名 canary(阶段 4)
    Promote,  // /evolve promote <candidate-id>:canary -> active(阶段 4)
    Rollback, // /evolve rollback <package-id> [version]:切回父版或指定版(阶段 4)
};

struct ParsedEvolveCommand {
    EvolveCommandAction action = EvolveCommandAction::Invalid;
    std::string source_filter;  // List 时:run/goal/recording/tooltrace/memory;nullopt 语义用空串 = all
    std::string target;         // Show/Propose/Diff/Reject/Test/Approve/Use/Promote/Rollback 的目标 id
    std::string target_extra;   // Rollback 的可选版本号
    std::string reason;         // Reject 的理由(可省)
    std::string bad_word;       // Invalid 时第一词的原始拼写(提示用)
};

// 纯解析:拆出动作、来源过滤、目标。认不得的子命令、show 缺 id 一律
// Invalid,由 handler 统一打用法。
ParsedEvolveCommand ParseEvolveCommand(const std::string& args);

// /evolve 的分派位(命令注册表登册用)。
CommandFlow HandleSlashEvolve(SlashDispatchContext& ctx,
                              const lubancode::cli::ParsedSlashCommand& parsed);

// CI 非交互入口:luban evolve test <candidate-dir> [--baseline <package-dir>]
// [--json]。人话或 JSON 打到 stdout,退出码按结果(全过 0/有 fail 1/夹具
// 缺失 2)。评测引擎与 /evolve test 同一枚 EvolutionCoordinator::TestDir。
int RunEvolveTestCommand(const EvolveTestArgs& args);

}  // namespace lubancode::app
