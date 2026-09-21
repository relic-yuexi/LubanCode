// 命令分派注册制(会话终章):47 案 slash 分派 switch 换成注册表——
// 名字 → handler → 权限/补全元数据。各案 handler 归各域文件
// (commands/ 下按域接活,签名统一为 HandleSlashXxx);控制器只留路由
// 与门(feature 开关、pipe 拒绝的语义在各域 handler,装配材料在绑定期
// 由 session_command_bindings 折好)。
//
// 分层规矩:
//   - SlashCommandSpec 是注册行:command 是 cli::SlashCommand 枚举(与
//     cli::ParseSlashCommand 同源),handler 是域文件的入口。命令词汇
//     (名字/别名/展示标记)不在这层重写——统一看 cli::
//     SlashCommandDescriptors(),app 只按枚举绑 handler(HC-05);
//   - needs_console/needs_idle 是权限/补全元数据,供分组展示与后续门用;
//     现状拒绝语义仍在域 handler(如 loop 的非交互明拒、peer 组在管道下
//     由 handler 明说没起服务),不在表上另发明新门;
//   - HC-06(材料收窄)三小批已毕:各域 handler 吃自己的窄材料(窄
//     context 定义在各域头,绑定单元在装配期折好)。本头只剩路由表与
//     "FD-06 占件的过渡袋"——/insights 与 /prompt(audit 委托)两枚分派
//     位还在吃 SlashDispatchContext,那两域文件被在跑的 FD-06 占着;它
//     合入迁走后整束删除,本头同时退成纯路由(不再 include 任何材料头)。
//     此前这里是全命令域的总依赖容器(80 字段/26 直接入边),第三小批
//     把 65 枚字段退役进各域窄 context,重头依赖(agent/tool/channel/lsp/
//     memory/package/peers/telemetry/workflow/config)全部随迁。
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/commands/command_flow.hpp"
#include "cli/context_tracker.hpp"        // ContextTracker:过渡袋字段(prompt audit 的预算口径)
#include "cli/slash_commands.hpp"         // SlashCommand/ParsedSlashCommand(路由表)
#include "runtime/session_soul.hpp"       // SessionSoulSnapshot:过渡袋字段(prompt audit 的魂面)
#include "runtime/trajectory_session.hpp"  // TrajectorySessionLedger:过渡袋字段(insights/audit 的账本)

namespace lubancode::cli {
struct Theme;  // 过渡袋字段(定义在 cli/theme.hpp;域文件各自直递)
}  // namespace lubancode::cli
namespace lubancode::tools {
class ToolRegistry;  // 过渡袋字段(定义在 tools/registry.hpp)
}  // namespace lubancode::tools
namespace lubancode::agent {
struct PromptOptions;  // 过渡袋字段(定义在 agent/prompt_assembler.hpp)
}  // namespace lubancode::agent

namespace lubancode::app {

struct InteractiveSessionOptions;  // 过渡袋字段(定义在 app/interactive_session.hpp)

// 与底下 struct SlashDispatchContext 的定义同一写法(class/struct 混写是
// MSVC C4099,全仓前置声明与定义统一成 struct)。
struct SlashDispatchContext;

// HC-06(把Slash命令材料收窄到各自领域):执行器只接命令参数——各域材料
// 在绑定期由闭包捕获(session_command_bindings 的绑定单元构造),handler
// 不再统一摸整束 SlashDispatchContext。已收窄域吃自己的窄 context;尚未
// 收窄的域由绑定单元包一层过渡,后续批次逐域迁走。
using SlashCommandHandler =
    std::function<CommandFlow(const lubancode::cli::ParsedSlashCommand&)>;

// 一案一行。handler 为空 = 死案(Image 进不来分派、NotSlash 在上一层已
// 分流),表上留名只为 47 案对账齐整。
struct SlashCommandSpec {
    lubancode::cli::SlashCommand command;
    SlashCommandHandler handler;  // 域执行器;空 = 死案
    bool needs_console;           // 权限元数据:真控制台才有意义(peer 组等)
    bool needs_idle;              // 补全元数据:只在空闲 composer 生效(/plan)
};

// 注册表本体(命令注册制:案子按 switch 旧序登册,枚举可对)。HC-06:表
// 由绑定单元构造,这只"空材料版"作为对账钉子(案序/枚举/死案口径与真表
// 同源同构,测试与词汇面对账用)。
const std::vector<SlashCommandSpec>& SlashCommandTable();

// 会话控制器的路由入口:按枚举查表调 handler;查无(不可达)按 Continue
// 兜底,与旧 switch 的完备性兜底同语义。HC-06:路由只吃表——材料已在
// 绑定期闭包捕获,路由面不再摸 SlashDispatchContext。
CommandFlow DispatchSessionSlashCommand(const std::vector<SlashCommandSpec>& table,
                                        const lubancode::cli::ParsedSlashCommand& parsed);

// P0-2 TrajectoryCommandExecutor(§14.1/§15.7):terminal 与 app-server
// 命令入口的统一包装——flag 开的会话先 durable 落 control.command.
// requested(actor=user),handler 跑完落 completed。flag 关直接透传,
// 行为零变。effect class 按命令名粗分表(动作级细分随 P0-4 注册表
// 元数据落)。轨迹账本单独递参(HC-06:执行器包装不是域材料)。
CommandFlow ExecuteSessionCommand(const std::vector<SlashCommandSpec>& table,
                                  lubancode::runtime::TrajectorySessionLedger* trajectory,
                                  const lubancode::cli::ParsedSlashCommand& parsed);

// 过渡材料袋(HC-06 第三小批后的残余):只剩 /insights 与 /prompt(audit
// 委托)两枚分派位在吃——那两域文件被在跑的 FD-06 占着,合入迁走后本
// 结构整束删除。字段全借用:会话(构造它的 TerminalSessionController)与
// 组合根(装好的栈)在命令执行期间保证存活。
struct SlashDispatchContext {
    const InteractiveSessionOptions* opts = nullptr;  // law_source(/prompt 裸敲)
    const lubancode::cli::Theme* theme = nullptr;
    const std::optional<std::string>* home_lubancode = nullptr;  // /insights 的报告根
    const std::string* prompts_dir = nullptr;
    std::string* persona = nullptr;
    std::optional<std::string>* config_file_path = nullptr;  // /soul 的写回目标
    std::shared_ptr<std::string> current_model_instructions;  // prompt audit 的目录指令面
    std::shared_ptr<std::string> current_soul;
    std::string* current_soul_name = nullptr;
    lubancode::runtime::SessionSoulSnapshot* soul_session = nullptr;  // /soul 与 audit 的会话快照
    lubancode::cli::ContextTracker* context_tracker = nullptr;        // audit 的窗口预算口径
    lubancode::tools::ToolRegistry* registry = nullptr;               // audit 的工具面
    lubancode::agent::PromptOptions* prompt_options = nullptr;        // audit 的拼装现场
    lubancode::runtime::TrajectorySessionLedger* trajectory = nullptr;  // insights/audit 的账本
    // /soul 落定后的皮上刷新。
    std::function<void()> sync_request_policy;
};

}  // namespace lubancode::app
