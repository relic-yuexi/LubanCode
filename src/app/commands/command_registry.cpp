// 命令分派注册制(会话终章):路由入口与对账钉子。表本体在命令绑定
// 单元(session_command_bindings)构造——HC-06 起各域 handler 的材料在
// 绑定期闭包捕获,这里只查表调执行器;旧 47 案 switch 的路由职责收于
// 此,案序照旧 switch 登册(枚举可对)。
#include "app/commands/command_registry.hpp"

#include <set>

#include "app/commands/session_command_bindings.hpp"  // BuildSessionSlashCommandTable(HC-06)

namespace lubancode::app {

const std::vector<SlashCommandSpec>& SlashCommandTable() {
    // HC-06:真表由绑定单元从会话材料构造(控制器持有);这只"空材料版"
    // 是对账钉子——案序/枚举/死案口径与真表同源同构(同一个构造函数),
    // 测试与词汇面对账用,不用于真分派。
    static const SlashDispatchContext empty_dispatch{};
    static const std::vector<SlashCommandSpec> table =
        BuildSessionSlashCommandTable(SessionCommandMaterials{&empty_dispatch});
    return table;
}

CommandFlow DispatchSessionSlashCommand(const std::vector<SlashCommandSpec>& table,
                                        const lubancode::cli::ParsedSlashCommand& parsed) {
    for (const SlashCommandSpec& spec : table) {
        if (spec.command != parsed.command) {
            continue;
        }
        if (!spec.handler) {
            break;  // 死案(Image/NotSlash):旧 switch 的 break 同语义
        }
        return spec.handler(parsed);
    }
    return CommandFlow::Continue;  // switch 完备性兜底同款
}

// ---------------------------------------------------------------------------
// P0-2 TrajectoryCommandExecutor(§14.1/§15.7)
// ---------------------------------------------------------------------------

namespace {

// effect class 的粗分表(§14.2 的类别列;动作级细分——/context 裸敲与
// /context 256k 之别——随 P0-4 的注册表元数据落,P0-2 按命令名粗分)。
const char* CoarseEffectClass(const std::string& name) {
    static const std::set<std::string> kSessionState = {
        "model",     "provider", "think",  "context", "plan",       "soul",     "prompt",
        "language",  "title",    "keymap", "init",    "worktree",   "config",   "hooks",
        "compact",   "record",   "memory", "todos",   "instructions",
        "context-window"};
    static const std::set<std::string> kExternalWrite = {"skill", "plugin", "package", "send", "peerperm",
                                                         "evolve"};
    static const std::set<std::string> kSpawnRun = {"agent", "workflow", "goal", "loop", "background",
                                                    "doctor"};
    static const std::set<std::string> kSessionBoundary = {"clear", "resume", "exit", "archive", "delete"};
    // Token 账本单 A5:/insights 写派生摘要与报告(§11.2 effect=derived_write);
    // status 是只读面,粗分按命令名给执行档,报告路径的细分留给后续注册表
    // 元数据。
    if (name == "insights") {
        return "derived_write";
    }
    if (kSessionBoundary.count(name) != 0) {
        return name == "delete" ? "destructive" : "session_boundary";
    }
    if (kSpawnRun.count(name) != 0) {
        return "spawn_run";
    }
    if (kExternalWrite.count(name) != 0) {
        return "external_write";
    }
    if (kSessionState.count(name) != 0) {
        return "session_state";
    }
    return "read_only";  // help/skills/mcp/lsp/agents/tools/peers/sessions/trace/export/copy/unknown...
}

}  // namespace

CommandFlow ExecuteSessionCommand(const std::vector<SlashCommandSpec>& table,
                                  lubancode::runtime::TrajectorySessionLedger* trajectory,
                                  const lubancode::cli::ParsedSlashCommand& parsed) {
    lubancode::runtime::TrajectorySessionLedger* ledger = trajectory;
    if (ledger == nullptr) {
        return DispatchSessionSlashCommand(table, parsed);  // flag 关:零变透传
    }
    // 轨迹账的命令名从词汇表主名取(HC-05:app 不再自带一份名字)。Unknown/
    // NotSlash 不是用户词汇,表上没有,落回这两个审计名。
    std::string spec_name =
        parsed.command == lubancode::cli::SlashCommand::NotSlash ? "notslash" : "unknown";
    if (const lubancode::cli::SlashCommandDescriptor* descriptor =
            lubancode::cli::FindSlashCommandDescriptor(parsed.command);
        descriptor != nullptr) {
        spec_name = descriptor->word;
    }
    // clear/resume 是跨 session 例外(§14.1):requested 落旧 main、terminal
    // 由新 main 的换账事务写(P0-3 的 SessionManager 八步/七步掌管),不走
    // 这只"同一 main stream 内 requested/terminal"的通用环。
    if (spec_name == "clear" || spec_name == "resume") {
        return DispatchSessionSlashCommand(table, parsed);
    }
    // requested 先 durable(§14.1:有外部写入/派生执行/session 切换时
    // 须先落账再动手),handler 跑完落 terminal。P0-2 的 handler 还没翻成
    // CommandOutcome,status 只按流转给("ok"——failed 分型随 P0-4)。
    const std::string command_id = ledger->BeginCommand(spec_name, spec_name,
                                                        CoarseEffectClass(spec_name));
    const CommandFlow flow = DispatchSessionSlashCommand(table, parsed);
    ledger->EndCommand(command_id, /*ok=*/true, std::string());
    return flow;
}

}  // namespace lubancode::app
