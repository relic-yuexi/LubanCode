// /workflow 命令的终端薄壳(自然语言编排单第 1 批起)。
//
// 分层规矩:业务在 src/workflow/(纯逻辑,单测钉);这里只做"拆子命令、
// 打印"。第 1 批覆盖 list/show/graph/validate/doctor;run/resume/cancel
// 随第 2/3 批接线时扩充,非法子命令打用法。
//
// 与另一工人的接缝:slash 分派在 InteractiveSession::DispatchSlashCommand,
// 这里只经 WorkflowCommandContext 拿材料(catalog 现扫,不占会话状态)。

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cli/theme.hpp"
#include "tools/registry.hpp"
#include "workflow/catalog.hpp"
#include "workflow/runtime.hpp"
#include "workflow/validator.hpp"

namespace lubancode::app {

// /workflow 命令的装配材料。project_root/user_root 是两级 workflows 目录
// 的锚点(catalog 自己拼 .lubancode/workflows);registry 可空(空则
// capability check 只数 core 工具,给 unknown_tool 警告)。
struct WorkflowCommandContext {
    std::optional<std::filesystem::path> project_root;  // 通常 cwd
    std::optional<std::filesystem::path> user_root;     // 通常 home
    std::optional<std::filesystem::path> home_lubancode;  // workflow-runs 落点
    const lubancode::tools::ToolRegistry* registry = nullptr;  // capability 快照
    std::vector<std::string> skill_names;               // 撞名检查用
    const lubancode::cli::Theme* theme = nullptr;       // 必填(指针免默认构造被删)
};

// 拆好的 /workflow 子命令。Invalid 时 usage 打印兜底。
enum class WorkflowCommandAction {
    Invalid,
    List,   // list [project|home|all]
    Show,   // show <id>
    Graph,  // graph <id> [ascii|mermaid|json]
    Validate,  // validate <id>
    Doctor,  // doctor:撞名/坏定义/缺失能力巡检
    Run,    // run <id> [参数...]
    Resume, // resume <run_id>
    Cancel, // cancel <run_id>(首版:标记请求,跑动中的取消经 ESC 通道)
    History,  // history <id> | history delete <run_id>
    Enable,   // enable|disable <id>
    Remove,   // remove <id>(要确认)
    Create,   // create <描述...>(第 5 批:自然语言向导)
    Alias,    // alias:列出直呼名与撞名账
};

struct ParsedWorkflowCommand {
    WorkflowCommandAction action = WorkflowCommandAction::Invalid;
    std::string id;      // show/graph/validate/run 的目标
    std::string format;  // graph 的格式(空 = ascii)
    std::string scope;   // list 的范围(空 = all)
    std::string rest;    // run 的剩余参数/create 的描述原文
    bool confirm = false;  // remove/delete 的确认词(remove yes / delete yes)
};

// 纯解析(单测钉)。
ParsedWorkflowCommand ParseWorkflowCommand(const std::string& args);

// 命令入口。返回 true = 交给会话层继续(恒 true;失败只打提示)。
bool HandleWorkflowCommand(const std::string& args, const WorkflowCommandContext& context);

// /workflow run 的执行装配:宿主(InteractiveSession)填执行器表;这里
// 只做编排。返回 run 摘要文本(给人看的一屏)。
std::string RunWorkflowById(const WorkflowCommandContext& context, const std::string& id,
                            const std::string& raw_args,
                            const std::map<lubancode::workflow::NodeKind,
                                           std::shared_ptr<lubancode::workflow::NodeExecutor>>& executors);

// 会话层给 alias 直呼用的查询:catalog 里有没有这个 alias;返回 workflow
// id(撞名禁用/不存在给空串)。第 5 批把这里换成 autocomplete 同源。
std::string ResolveWorkflowAlias(const WorkflowCommandContext& context, const std::string& alias);

}  // namespace lubancode::app
