// TerminalSessionController(原 InteractiveSession,显示系统剥离单第六步
// 更名):交互会话主循环的终端控制器。原先 main.cpp 的 InteractiveLoop
// 里那一把局部变量与大 lambda,全收成这里的成员与方法;头文件
// (interactive_session.hpp)只露 InteractiveSessionOptions 与
// RunInteractiveSession()。
//
// 寿命规矩写在成员声明旁:拥有者先声明(后析构),借用者后声明(先析
// 构)——AgentLoop 持 backend/registry 引用,必须先死;registry 背后的
// ToolRuntime 后死;peer 工具持 PeerRuntime 引用,PeerRuntime 又要活得比
// loop 久不了(见各成员注释)。回调注册(UI 面板、收件点)与 peer 起停
// 由构造函数开门、析构函数关门,异常退场也走同一条路。
//
// 依赖只认 agent/api/cli/config/memory/mcp/lsp/tools/platform 与 app
// 装配层;不反被任何层 include。

#include "app/interactive_session.hpp"
#include "app/interactive_session_controller.hpp"  // 控制器类声明(私头,会话终章)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent/artifact_store.hpp"
#include "agent/compact.hpp"
#include "agent/context_budget.hpp"
#include "agent/loop.hpp"
#include "peers/peer_session.hpp"
#include "agent/prompts.hpp"
#include "sessions/session_store.hpp"
#include "skills/workflow_recorder.hpp"
#include "api/backend.hpp"
#include "api/models.hpp"
#include "app/backend_stack.hpp"
#include "app/commands/command_registry.hpp"  // 命令注册制:47 案分派的注册表与路由
#include "app/session_stack.hpp"  // 组合根装配件(会话终章):控制器只收装好的件
// 子系统接线器(会话终章):goal/loop/plan/peer/录制各一只(状态+装配+
// 泵+存档恢复),控制器持句柄调;会话级状态(theme/config/session_store)
// 留控制器。
#include "app/wirings/goal_session_wiring.hpp"
#include "app/wirings/loop_session_wiring.hpp"
#include "app/wirings/peer_session_wiring.hpp"
#include "app/wirings/plan_session_wiring.hpp"
#include "app/wirings/record_session_wiring.hpp"
#include "app/runtime_profile.hpp"
#include "app/tool_runtime.hpp"
#include "app/agent_panel_presenter.hpp"
#include "app/commands/memory_commands.hpp"
#include "app/commands/model_commands.hpp"
#include "app/commands/trace_commands.hpp"
#include "app/hook_runtime.hpp"
#include "app/turn_runner.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/workspace_commands.hpp"
#include "app/commands/hook_commands.hpp"
#include "app/commands/peer_commands.hpp"
#include "app/commands/doctor_commands.hpp"
#include "app/commands/workflow_commands.hpp"
#include "workflow/host_executors.hpp"
#include "app/version.hpp"
#include "runtime/command_service.hpp"
#include "runtime/event_sinks.hpp"
#include "runtime/plan_mode.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/tool_trace_hub.hpp"
// 持久目标单:goal 状态机(coordinator)、GoalContext 注入、终端排版。
#include "app/commands/goal_commands.hpp"
#include "runtime/goal_context.hpp"
#include "runtime/goal_compact.hpp"
#include "runtime/goal_coordinator.hpp"
#include "runtime/goal_evaluator.hpp"
#include "runtime/goal_evidence.hpp"
#include "tools/goal_checkpoint_tool.hpp"
#include "tools/loop_control_tool.hpp"
// loop 单:会话定时循环(scheduler、空闲唤醒多路、终端排版)。
#include "app/commands/loop_commands.hpp"
#include "runtime/idle_wake.hpp"
#include "runtime/loop_scheduler.hpp"
#include "runtime/loop_types.hpp"
#include "runtime/session_work_scheduler.hpp"
#include "hooks/hash.hpp"  // Sha256Hex:PlanDocument 内容锚
#include "cli/agent_panel_host.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/diff.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/line_editor.hpp"  // DisplayWidthUtf8:查看帧折行记账
#include "cli/i18n.hpp"
#include "cli/live_transcript.hpp"
#include "cli/worktree.hpp"
#include "cli/markdown.hpp"
#include "cli/keymap.hpp"
#include "cli/mention_menu.hpp"
#include "cli/record_command.hpp"
#include "cli/slash_commands.hpp"
#include "cli/terminal_port.hpp"
#include "cli/spinner.hpp"
#include "cli/spinner_backend.hpp"
#include "cli/terminal_frame.hpp"
#include "cli/theme.hpp"
#include "cli/turn_renderer.hpp"
#include "cli/todo_render.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
#include "cli/transcript_controller.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "config/prompt_files.hpp"
#include "config/project_instructions.hpp"
#include "config/skill_store.hpp"
#include "lsp/manager.hpp"
#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"
#include "app/memory_extract.hpp"
#include "app/mention_support.hpp"  // @ 提及支件(会话终章)
#include "app/model_router.hpp"
#include "app/session_title.hpp"
#include "mcp/client.hpp"
#include "mcp/mcp_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/background_output.hpp"
#include "tools/background_tasks.hpp"
#include "tools/command_safety.hpp"
#include "tools/context_tools.hpp"
#include "tools/edit_file.hpp"
#include "tools/hooks.hpp"
#include "tools/lua_tool.hpp"
#include "tools/path_utils.hpp"
#include "tools/plugin_loader.hpp"
#include "tools/lsp_tool.hpp"
#include "tools/list_sessions_tool.hpp"
#include "tools/read_file.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "tools/search.hpp"
#include "tools/send_session_message_tool.hpp"
#include "tools/skill_loader.hpp"
#include "tools/skill_tool.hpp"
#include "tools/todo_tool.hpp"
#include "tools/tool_search.hpp"
#include "tools/web_fetch.hpp"
#include "tools/web_search.hpp"
#include "tools/write_file.hpp"
#include "platform/console.hpp"
#include "platform/terminal_batch.hpp"
#include "platform/paths.hpp"
#include "platform/clipboard.hpp"

namespace lubancode::app {

using lubancode::app::kVersion;
using lubancode::platform::CurrentDirUtf8;
using lubancode::cli::tr;
using lubancode::cli::trf;
// 终端接线收尾单:本文件(会话控制器)的 stdout/stderr 写全走输出端口
// (TermOut/TermErr),散打清零——改道与锁规矩见 cli/terminal_port.hpp。
using lubancode::cli::TermOut;
using lubancode::cli::TermErr;
// 会话层排队消息账本(0.28.x):流式监听线程落队、会话泵投递,共用这一只。
using lubancode::cli::SessionSteeringQueue;

namespace {

}  // namespace














// 建档(渐进式上下文仓第二期起,第一轮用户输入**之前**就要建):仓要拿
// session id 开张,超长结果在第一轮请求里就得能落盘,不能等回合收尾。首条
// 文本做 slug;建档失败置 session_store_broken 照旧拦落盘,会话本身照跑。
// 建档成功顺手开仓(开不成只告警:超长结果退回内存全文,不产生假引用)。
bool TerminalSessionController::EnsureSessionBegun(const std::string& first_text) {
    // P6:建档本体在 SessionRuntime(错误不再自己打印,由这边按结果印)。
    const auto result =
        session_runtime_.EnsureBegun(first_text, *current_model, CurrentDirUtf8());
    if (result == lubancode::runtime::SessionBeginResult::Failed) {
        TermOut() << theme.error << trf("session.create_failed", sessions_dir) << theme.reset << "\n";
        return false;
    }
    if (result != lubancode::runtime::SessionBeginResult::Begun) {
        return session_store.active();  // Active/Disabled:照旧语义
    }
    // hooks 上下文补真 session id 与转录路径(建档这一刻才齐)。
    if (lubancode::app::HookRuntime() != nullptr) {
        lubancode::hooks::HookContext hook_context = lubancode::app::HookRuntime()->context();
        hook_context.session_id = session_store.session_id();
        hook_context.transcript_path = session_store.file_path();
        lubancode::app::UpdateHookRuntimeContext(hook_context);
    }
    OpenArtifactStore();
    return true;
}

// 开仓:<sessions_dir>/<session-id>/context(与 <session-id>.jsonl 并排,
// /sessions 只扫 *.jsonl,互不干扰)。开不成只告警——仓是加层,不是依赖。
void TerminalSessionController::OpenArtifactStore() {
    if (sessions_dir.empty() || !session_store.active()) {
        return;
    }
    const std::string root = sessions_dir + "/" + session_store.session_id() + "/context";
    if (!artifact_store->Open(root, session_store.session_id())) {
        TermOut() << theme.stats << trf("artifact.store_open_failed", root) << theme.reset << "\n";
    }
}

// 把 history 里 persisted_count 之后的消息逐条追加落盘(append+flush,
// 崩溃安全)。history 被 ReplaceHistory 换短(/compact)的场合由调用处
// 先把 persisted_count 收到新长度,这里只管"只增不减"的常态。
void TerminalSessionController::PersistNewMessages() {
    // P6:增量落盘本体在 SessionRuntime(只增不减、兜底建档同旧路)。
    // store 没开张时的兜底建档也在它那头(首条用户文本抽出来做 slug);
    // 这边只在"Begun 且还没 active"的窗口补一句给用户的话与 hooks 上下文。
    const auto result = session_runtime_.PersistNew(main_agent->History(), *current_model, CurrentDirUtf8());
    if (result == lubancode::runtime::SessionPersistResult::BrokenNow) {
        TermOut() << theme.error << tr("session.append_failed") << theme.reset << "\n";
        return;
    }
    if (result == lubancode::runtime::SessionPersistResult::Nothing && !session_store.active() &&
        !sessions_dir.empty() && !session_store_broken && !main_agent->History().empty()) {
        // 落盘账没动而 store 仍没开张:按旧兜底路走一遍建档(给 hooks 与
        // 仓一齐的机会)。PersistNew 里 EnsureBegun 只填账不碰 hooks,这里
        // 补上与 EnsureSessionBegun 相同的那段。
        std::string first_text;
        for (const auto& message : main_agent->History()) {
            if (message.role != lubancode::api::Role::User) {
                continue;
            }
            for (const auto& block : message.content) {
                if (const auto* tb = std::get_if<lubancode::api::TextBlock>(&block)) {
                    first_text = tb->text;
                    break;
                }
                if (const auto* image = std::get_if<lubancode::api::ImageBlock>(&block)) {
                    first_text = image->filename;
                    break;
                }
            }
            break;
        }
        if (!first_text.empty()) {
            EnsureSessionBegun(first_text);
        }
    }
}



// 子代理目标的排队消息转投任务 inbox(与面板定向介入同一条通道:
// AgentTool::SendTaskMessage——那只子代理自己的 AgentLoop 会在"当前工具
// 收尾、下一次请求未发"的边界收信)。终态明确拒收:标 TargetGone 留在
// 队列原位(屏上带"[目标已结束]"标记),等用户取回改目标或删掉,不改投
// main。只在主线程调(会话泵/轮次边界)。
void TerminalSessionController::PumpSteeringToSubagents() {
    bool queue_changed = false;
    for (const auto& item : SessionSteeringQueue().Snapshot()) {
        if (item.target.kind != lubancode::cli::MessageTarget::Kind::Subagent ||
            item.state != lubancode::cli::QueueItemState::Queued || item.edit_open) {
            continue;
        }
        lubancode::tools::AgentTool* agent_tool = session_agent_tool();
        if (agent_tool == nullptr) {
            SessionSteeringQueue().MarkFailed(item.id, "当前会话没有可用的子代理通道");
            queue_changed = true;
            continue;
        }
        const lubancode::tools::TaskMessageStatus status =
            agent_tool->SendTaskMessage(item.target.task_id, item.text);
        if (status == lubancode::tools::TaskMessageStatus::Queued) {
            // 已进任务 inbox,活队列退场(那边轮次边界自会送达)。
            SessionSteeringQueue().Remove(item.id);
            queue_changed = true;
        } else {
            // 终态(Finished)或任务号认不出(NotFound):一律按"目标已结束"
            // 标注,原文留队,绝不改投 main(规格"队列按目标分账")。
            SessionSteeringQueue().MarkTargetGone(item.id, "目标子代理已结束");
            queue_changed = true;
        }
    }
    if (queue_changed) {
        PersistSteeringQueue();  // 转投/标注也是排队账一变(路径二,快照对齐)
    }
}

// 排队账 -> 存档快照事件行(路径二)。落不了档(没建档/写坏)只安静退:
// 存档从来是加层,坏不到会话本体。TargetGone/Failed 的条目也一并进快照——
// 它们是"等用户处置"的活账,resume 后还该看得见。
void TerminalSessionController::PersistSteeringQueue() {
    if (sessions_dir.empty() || session_store_broken) {
        return;
    }
    if (!session_store.active()) {
        // 一条消息没发过就排了队、又直接 /exit:档还没建。拿队头那条当
        // 首句建档(slug 用得上),排队账才有处落——不然这类场子的队列
        // 依然落空。建不成档安静退,老规矩。
        std::string first_text;
        for (const auto& item : SessionSteeringQueue().Snapshot()) {
            if (!item.text.empty()) {
                first_text = item.text;
                break;
            }
        }
        if (first_text.empty() || !EnsureSessionBegun(first_text)) {
            return;
        }
    }
    const auto snapshot = SessionSteeringQueue().Snapshot();
    std::vector<lubancode::sessions::ArchivedQueueItem> items;
    items.reserve(snapshot.size());
    for (const auto& item : snapshot) {
        lubancode::sessions::ArchivedQueueItem archived;
        archived.id = item.id;
        archived.subagent = !item.target.is_main();
        archived.task_id = item.target.task_id;
        archived.text = item.text;
        archived.attempts = item.delivery_attempts;
        items.push_back(std::move(archived));
    }
    (void)session_store.AppendQueueEvent(items);  // 失败不告警:下一趟账变了再追
}

// 存档快照 -> 会话层队列(resume 路)。RestoreFromArchive 只在队列还空着时
// 收(本场自己还没排队),运行中的账不给旧档盖。
void TerminalSessionController::RestoreSteeringQueueFrom(
    const std::vector<lubancode::sessions::ArchivedQueueItem>& items) {
    if (items.empty()) {
        return;
    }
    std::vector<lubancode::cli::QueuedMessage> restored;
    restored.reserve(items.size());
    for (const auto& archived : items) {
        lubancode::cli::QueuedMessage item;
        item.id = archived.id;
        item.target = archived.subagent ? lubancode::cli::MessageTarget::Agent(archived.task_id)
                                        : lubancode::cli::MessageTarget::Main();
        item.text = archived.text;
        item.state = lubancode::cli::QueueItemState::Queued;
        item.delivery_attempts = archived.attempts;
        restored.push_back(std::move(item));
    }
    SessionSteeringQueue().RestoreFromArchive(std::move(restored));
}

// Ctrl+R 提问历史搜索的数据源:只读 session 事件账。ListSessions 按新→旧
// 给场次,这里倒序遍历(整体旧→新,BuildHistorySearchIndex 认这个序);
// 每场内部 ExtractPromptHistory 本就是旧→新。当前会话若还没建档(首条
// 消息未落地),活 history 里的用户提问也并进来——同一只读规则。
lubancode::cli::PromptHistoryDataset TerminalSessionController::CollectPromptHistory() {
    lubancode::cli::PromptHistoryDataset data;
    data.current_session_id = session_store.session_id();
    data.current_project_key = lubancode::sessions::NormalizePathForCompare(CurrentDirUtf8());
    if (!sessions_dir.empty()) {
        const std::vector<lubancode::sessions::SessionListEntry> listed =
            lubancode::sessions::ListSessions(sessions_dir, /*limit=*/150);
        for (auto it = listed.rbegin(); it != listed.rend(); ++it) {
            const auto bytes = lubancode::sessions::ReadSessionFileBytes(it->file_path);
            if (!bytes.has_value()) {
                continue;  // 读不动这场就跳过,不废整份
            }
            const std::string project_key = lubancode::sessions::NormalizePathForCompare(it->cwd);
            for (auto& record : lubancode::sessions::ExtractPromptHistory(*bytes)) {
                lubancode::cli::PromptHistoryEntry entry;
                entry.text = std::move(record.text);
                entry.ts = std::move(record.ts);
                entry.session_id = it->id;
                entry.title = it->title;
                entry.project_key = project_key;
                data.entries.push_back(std::move(entry));
            }
        }
    }
    // 活 history 兜底:建档前(或建不了档)的本场提问。
    const std::string current_id =
        data.current_session_id.empty() ? std::string("current") : data.current_session_id;
    for (const auto& message : main_agent->History()) {
        if (message.role != lubancode::api::Role::User || message.content.empty()) {
            continue;
        }
        const auto* text = std::get_if<lubancode::api::TextBlock>(&message.content.front());
        if (text == nullptr || text->text.empty() || text->text.front() == '/') {
            continue;  // 与 ExtractPromptHistory 同一只读规则
        }
        bool has_tool_result = false;
        for (const auto& block : message.content) {
            if (std::holds_alternative<lubancode::api::ToolResultBlock>(block)) {
                has_tool_result = true;
                break;
            }
        }
        if (has_tool_result) {
            continue;
        }
        lubancode::cli::PromptHistoryEntry entry;
        entry.text = text->text;
        entry.session_id = current_id;
        entry.project_key = data.current_project_key;
        entry.ts = session_start_ts;
        data.entries.push_back(std::move(entry));
    }
    return data;
}



// 终端标题模板:项目短名 · 分支 · 状态词。纯拼串,可单测可不测(肉眼可核)。
std::string TerminalSessionController::BuildTerminalTitleText(const std::string& state_word) const {
    std::string project;
    if (const auto root = lubancode::cli::FindRepositoryRoot(std::filesystem::current_path())) {
        project = lubancode::tools::PathToUtf8(root->filename());
    }
    if (project.empty()) {
        project = "lubancode";
    }
    const std::string branch = lubancode::cli::CurrentGitBranch(std::filesystem::current_path());
    std::string out = "lubancode · " + project;
    if (!branch.empty()) {
        out += " · " + branch;
    }
    out += " · " + state_word;
    return out;
}

void TerminalSessionController::EnsureMemoryTool() {
    // capability gate:全局未授权时 memory_save 不注册(双保险之一,另一道
    // 在 MemorySaveTool::execute 的运行时判定)。
    if (project_memory != nullptr && project_memory->generate_enabled() &&
        registry().Find("memory_save") == nullptr) {
        registry().Register(std::make_unique<lubancode::memory::MemorySaveTool>(project_memory));
    }
}

void TerminalSessionController::SyncWorktreeDirectory() {
    // 切 worktree 收面板:查看态目标跟着旧房的任务走,别把消息投去旧目标。
    lubancode::cli::ResetAgentPanelSession();
    // @ 提及索引跟着根走:根变了重扫(下一拍 Snapshot 自办)。
    mention_support_.Invalidate();
    prompt_options.cwd = CurrentDirUtf8();
    if (project_memory != nullptr) {
        if (const auto updated = project_memory->SetWorkingDirectory(std::filesystem::current_path());
            !updated.has_value()) {
            TermOut() << trf("cmd.memory.switch_failed", updated.error()) << "\n";
        }
    }
    project_instructions = lubancode::config::LoadProjectInstructions(std::filesystem::current_path()).content;
    prompt_options.project_instructions = project_instructions;
    main_agent->SetSystemPrompt(lubancode::agent::AssembleSystemPrompt(prompt_options));
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry().Find("agent"));
        agent_tool != nullptr) {
        agent_tool->SetWorkingDirectory(prompt_options.cwd);
        agent_tool->SetProjectInstructions(project_instructions);
    }
    // 会话档跟 cwd 走(0.27.x):目录动了就追加一条 cwd 事件,
    // /resume 靠它把会话送回原房。
    if (session_store.active()) {
        session_store.AppendCwdEvent(prompt_options.cwd);
    }
    RefreshWorkflowCompletions();
}

// 处理"确定不是空行、不是裸词 exit/quit"的一行输入,不管这行是刚
// ReadLine() 读到的、还是从 pending_queue 里取出来的自动发送的——两条
// 路径共用这一份 slash 分支 + 自动 compact 检查 + RunTurn 调用,行为
// 完全一致(spec 要求"队列里是 slash 命令也认")。返回 false 表示这一行
// 触发了 /exit,外层循环该退出了。
CommandFlow TerminalSessionController::ProcessLine(const std::string& content, bool* autosend_failed) {
    const lubancode::cli::ParsedSlashCommand parsed = lubancode::cli::ParseSlashCommand(content);
    // 会话级兜底(宽窄转换异常单):slash 命令、普通回合、回合收尾的起名/
    // 记忆抽取,任何 std::exception 都不再穿透顶层把整场掀了——错误上屏、
    // history 里已有的落盘、循环继续。这不是"每层包一遍":回合执行的最内
    // 环已有 RunTurn 那道收口,这里是会话边界唯一的一道;再往外只剩启动
    // 期(cli_app 顶层 catch)才许退进程。
    try {
        if (parsed.command != lubancode::cli::SlashCommand::NotSlash &&
            parsed.command != lubancode::cli::SlashCommand::Image) {
            return DispatchSlashCommand(parsed);
        }
        // 普通正文(含 peer 来信组包后的文字):自动压缩检查 + 发一轮。
        RunSessionTurn(content, TurnSource::User, autosend_failed);
        return CommandFlow::Continue;
    } catch (const std::exception& e) {
        if (autosend_failed != nullptr) {
            *autosend_failed = true;  // 回合异常收场:排队消息按失败退还(路径一的兜底判定)
        }
        {
            std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
            TermErr() << "\n"
                      << theme.error << tr("error.prefix") << trf("error.unexpected", e.what()) << theme.reset
                      << "\n";
            TermErr().flush();
        }
        try {
            PersistNewMessages();  // 已入 history 的部分照常落盘,/resume 接得回来
        } catch (...) {
            // 落盘自己都失败了:报不出更多信息,会话仍续命
        }
        return CommandFlow::Continue;
    }
}

// slash 分派(命令注册制,会话终章):47 案 switch 换成命令注册表——各案
// handler 归各域文件(commands/*.cpp 的 HandleSlashXxx),注册表与查表路由
// 在 commands/command_registry;这里只把会话材料装进 SlashDispatchContext
// (构造尾一次配齐)递过去。
CommandFlow TerminalSessionController::DispatchSlashCommand(const lubancode::cli::ParsedSlashCommand& parsed) {
    return lubancode::app::DispatchSessionSlashCommand(dispatch_ctx_, parsed);
}


// 主循环泵(公平仲裁,goal 分流合流):goal ready continuation 与 due
// loop tick 不共用 trigger(evaluator 判终点 vs 时钟到点),共用这只泵
//(单飞:一场 session 同时只跑一枚主 turn)。先各问一句,凑候选表,
// PumpNextWork 按优先级 + 公平账(goal 连跑三轮让一枚 due loop tick)定
// 谁走;user queue / pending interaction 已在泵前面的主循环各分支消费过,
// 这里只收自动工作两类。每圈只消费一拍,回主循环顶重新检查高优先级来源
//(不一次把八枚 due 全倒进队列,用户按 stop 时还来得及)。
bool TerminalSessionController::PumpScheduledWork() {
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }();
    std::vector<lubancode::runtime::SessionWork> candidates;
    // 有 ready continuation:候选里放一枚占位,真取件(TakeReadyIteration
    // 落 started 事件)等选中后再做——没选中就不动 goal 的账。
    if (goal_wiring_.coordinator() != nullptr && goal_wiring_.work_source().ProbeWork().has_value()) {
        lubancode::runtime::SessionWork work;
        work.kind = lubancode::runtime::WorkKind::GoalContinuation;
        work.id = "goal-continuation";
        candidates.push_back(work);
    }
    if (loop_wiring_.SweepAndCheckDue(now_ms)) {
        lubancode::runtime::SessionWork work;
        work.kind = lubancode::runtime::WorkKind::LoopTick;
        work.id = "loop-tick";
        candidates.push_back(work);
    }
    const auto picked = lubancode::runtime::PumpNextWork(candidates, goal_wiring_.fairness());
    if (!picked.has_value()) {
        lubancode::app::FlushLoopEvents(loop_wiring_.MakeCommandWiring());
        return false;
    }
    if (picked->kind == lubancode::runtime::WorkKind::GoalContinuation) {
        goal_wiring_.PumpContinuation(now_ms);
        return true;
    }
    if (!loop_wiring_.HasActiveTasks()) {
        return false;
    }
    return loop_wiring_.PumpDueTick(now_ms);
}






// 双胞胎合一(会话终章):RunUserTurn/RunPeerTurn 收成这一只带来源参数的
// 回合入口。User 走全套(配置门/建档/窗口同步/自动压缩/提及账/标题铃/
// trace 与 usage 记账/计划与记忆收尾);Incoming(peer 来信、后台子代理
// 回流)走精简路:peer 名册亮忙,silent 档可静默(查看态回流不上屏),
// 不挂录制、不追 usage、不做提及与收尾抽取。两路差异逐一保真:
//   - User 拦空配置(欢迎页逻辑),Incoming 不拦(能起 peer 必已配好);
//   - User 的 turn_id 复 trace 那枚(两本账对得上),Incoming 由适配器现发;
//   - 排队账快照(PersistSteeringQueue)两路都收(轮内可能进队/送走过)。
void TerminalSessionController::RunSessionTurn(const std::string& content, TurnSource source,
                                               bool* autosend_failed, bool silent,
                                               memory::QueryOrigin origin) {
    const bool is_user_turn = source == TurnSource::User;
    if (!is_user_turn) {
        peer_wiring_.SetStatus("busy");
    }
    if (is_user_turn) {
        // 欢迎页允许空配置进主界面;slash 命令在 ProcessLine 上一层已先分流。
        // 普通正文到这里才拦,免得拿空 base_url 真发请求、落下一场假会话。
        if (!lubancode::config::RequireConfigured(config_result).has_value()) {
            TermOut() << theme.error << tr("setup.turn.blocked") << theme.reset << "\n";
            if (autosend_failed != nullptr) {
                *autosend_failed = true;
            }
            return;
        }
        // 建档提前到发轮之前(第二期):仓要拿 session id 开张,第一轮请求里
        // 的超长结果才有地方落盘。失败不拦会话,只是没有 artifact 可追。
        EnsureSessionBegun(content);
        // 窗口同步(0.27.x):/context、/model 改的是 tracker 的窗口,loop 的
        // mid-turn 评估用同一份,发轮前对齐一次。
        main_agent->SetContextWindowTokens(context_tracker.window_tokens());
        // 自动压缩:发真正的用户输入前,占用超过阈值(80%)就先压一压。失败只
        // 警告不拦——字符数硬安全网(TrimHistory)还在,不会真的爆掉;工具循环
        // 中途的溢出由 loop 的压力通报(HandleContextPressure)另走 mid-turn 路。
        if (context_tracker.ShouldAutoCompact()) {
            lubancode::app::TryRunCompact(/*midturn=*/false, MakeCompactInputs());
        }
    }
    // 人在聚焦查看画面里直接敲了正文发送:视为离开聚焦态(新一轮输出
    // 马上往下铺,聚焦画面已经不是"当前画面"了),下次 Ctrl+E 是重新
    // 聚焦,不是"返回"。
    transcript_ui_.ExitFocusView();
    std::string turn_suffix;
    if (is_user_turn) {
        // @ 提及校验(0.30.x 第三批):目标消失/越出项目根,明报错拦下这轮;
        // 活着的提及附账进 turn context(不进永久 history)。
        const auto [mention_error, mention_ledger] = mention_support_.BuildLedger(content);
        if (!mention_error.empty()) {
            TermOut() << theme.error << mention_error << theme.reset << "\n";
            if (autosend_failed != nullptr) {
                *autosend_failed = true;  // 这轮没发出去:自动发送的消息按"没送达"回队
            }
            return;
        }
        turn_suffix = mention_ledger;
    }
    turn_suffix += project_memory != nullptr
                       ? project_memory->BuildTurnContext(content, std::filesystem::current_path(), origin)
                       : std::string();
    // 运行中子代理名册(规格第二节):每条外层用户消息/外来消息到来时给
    // main 一份动态重算的名册——task id + 真 title + 类型 + 待送数,不塞
    // prompt 与日志。走请求级 turn_context:不永久复制进 history,任务状态
    // 变了下轮重算,compact 后照常从台账重注入。主模型认得 task id,才知道
    // agent_message 该投给谁。
    if (session_agent_tool() != nullptr) {
        turn_suffix += session_agent_tool()->RunningTasksRoster();
    }
    // PTC 指南(PTC 单):当前已挂载 stub 的签名索引,随轮次请求视图走
    //(不进稳定的 system——前缀缓存守恒)。tool_search 中途挂载新工具,
    // 下一轮这里自动带上新签名。
    if (tool_runtime_->ptc_tool() != nullptr) {
        turn_suffix += tool_runtime_->ptc_tool()->GuideSegment();
    }
    main_agent->SetTurnContext(std::move(turn_suffix));
    std::size_t history_before = 0;
    std::string trace_turn_id;
    lubancode::runtime::TurnUsageStats turn_usage;
    const auto turn_started = std::chrono::steady_clock::now();
    if (is_user_turn) {
        // 查看帧的 app 侧擦账已拆(见 PrintViewedTranscript 注释):新回合铺
        // 正文不再需要在这里复位什么行账,终端层那本 view_body_top 按读取段
        // 自生灭。终端标题(0.30.x 第四批):跑着/等输入两态,项目·分支跟
        // 着;拿不到焦点状态,不做"未聚焦才通知"的假判断,只在长轮收口时
        // 叫一声铃。
        history_before = main_agent->History().size();
        if (spinner_enabled) {
            lubancode::cli::SetTerminalTitle(BuildTerminalTitleText(tr("notify.state_busy")));
        }
        // usage 出账(模型分工第一期):整轮逐步 usage 带出来记进分角色台账
        // (普通 turn = normal 档);compact/抽取的后台采样在各自路径另记,
        // 不混进这里。
        trace_turn_id = session_runtime_.ids().NextTurnId();
        turn_views_.emplace_back();
    }
    // 批二:这轮的事件适配器(sink 已在 SessionRuntime 上配好;User 的
    // turn_id 复 trace 那枚,两本账对得上;Incoming 由适配器现发——这轮
    // 没有 trace 口径的现成号)。
    lubancode::runtime::TurnEventAdapter turn_events = session_runtime_.MakeTurnAdapter();
    // 批三:RunTurn 二十四参收成一只 TurnContext。
    lubancode::app::TurnContext turn;
    turn.loop = &*main_agent;
    turn.user_input = content;
    turn.auto_confirm = auto_confirm;
    turn.always_allowed_tools = &always_allowed_tools;
    turn.theme = theme;
    turn.context_tracker = &context_tracker;
    turn.registry = &registry();
    turn.hook_dispatcher = lubancode::app::HookRuntime();
    turn.is_console = spinner_enabled;
    turn.transcript = &transcript_ui_.items();
    turn.todo_state = todo_state();
    turn.transcript_expanded = transcript_ui_.expanded_flag();
    turn.allow_commands = settings_local.allow_commands;
    turn.deny_commands = settings_local.deny_commands;
    turn.completion_agent = session_agent_tool();
    turn.recorder = is_user_turn ? record_wiring_.recorder() : nullptr;
    turn.silent = silent;
    turn.turn_events = &turn_events;
    // 模型输出图片的落盘口(ccmoon 巡检单 P0):会话开了档才有目录;
    // 没开(还没建档)就不挂,引擎遇图片明败,不吞图。
    if (sessions_dir.empty() == false && session_store.active()) {
        turn.model_images_dir = sessions_dir + "/" + session_store.session_id() + "/images";
    }
    // 输入图片前置拦截(MiniCPM5 真机巡检单 P2):目录与当前模型身份递给
    // RunTurn,纯文本模型的图片附件发送前就拦住。
    turn.model_catalog = &model_catalog;
    turn.model_id = *current_model;
    turn.active_provider = active_provider;
    if (is_user_turn) {
        turn.usage_out = &turn_usage;
        turn.trace_hub = &*trace_hub_;
        turn.thread_id_for_trace = session_runtime_.thread_id();
        turn.turn_id_for_trace = trace_turn_id;
        turn.turn_view_out = &turn_views_.back();
        turn.mode_gate = [this](const std::string& tool_name, const nlohmann::json& input) {
            return plan_wiring_.EvaluateGate(tool_name, input);
        };
        turn.approval_observer = [this](bool asked, bool allowed) {
            loop_wiring_.NotePermissionWait(asked, allowed);
        };
    }
    const lubancode::app::RunTurnResult turn_result = RunTurn(std::move(turn));
    if (is_user_turn) {
        // 轮次视图存档封顶(最近 N 轮,重铺够用;不无界攒)。
        if (turn_views_.size() > kMaxArchivedTurnViews) {
            turn_views_.erase(turn_views_.begin());
        }
        if (autosend_failed != nullptr) {
            *autosend_failed = turn_result.status != 0;  // 取走即消费单:失败信号原样递给会话泵
        }
        for (const auto& step : turn_usage.steps) {
            api::Usage step_usage;
            step_usage.input_tokens = step.input_tokens;
            step_usage.output_tokens = step.output_tokens;
            step_usage.cache_read_tokens = step.cache_read_tokens;
            step_usage.cache_creation_tokens = step.cache_creation_tokens;
            step_usage.output_reasoning_tokens = step.reasoning_tokens;
            model_router->ledger().Record(lubancode::agent::ModelRole::Normal,
                                          step.model.empty() ? *current_model : step.model, step_usage,
                                          /*duration_ms=*/0, step.reported);
        }
        if (spinner_enabled) {
            lubancode::cli::SetTerminalTitle(BuildTerminalTitleText(tr("notify.state_idle")));
            const auto elapsed = std::chrono::steady_clock::now() - turn_started;
            if (elapsed > std::chrono::seconds(30)) {
                lubancode::cli::NotifyUserAttention();  // 长轮跑完叫一声,每轮至多一次
            }
        }
    }
    // 每轮结束(成功/出错/ESC 打断都算)把新增消息逐条追加落盘。
    PersistNewMessages();
    if (is_user_turn) {
        // Plan 模式(只读研究硬闸单):turn 正常收口后扫本轮 assistant 正文,
        // <proposed_plan> 完整则记 PlanDocument 并弹审阅框(单子:不在解析到
        // </proposed_plan> 的同一次 Provider response 内直接执行——工具表、
        // 提示词、mode event 与 UI 都在半新半旧状态时不动手)。
        if (turn_result.status == 0 && !turn_result.cancelled) {
            plan_wiring_.CollectProposal(history_before, trace_turn_id);
        }
        // 会话起名(cheap 角色):建档后第一轮回合收尾、还没有标题时起一枚,
        // 成功落 title 事件;失败安静降级(/sessions 继续用首句摘要,不拦人)。
        lubancode::app::MaybeGenerateSessionTitle(MakeTailContext(), lubancode::agent::TaskKind::SessionTitle);
        // 回合收尾总结与候选抽取(learn off 时是空操作)。
        lubancode::app::ExtractTurnMemory(MakeTailContext(), content, history_before);
    }
    // 排队账快照落档(路径二):轮内可能进过队/边界注入送走过,趁收尾把
    // 最新一份快照追进存档,/exit 或崩掉后 resume 接得回来。
    PersistSteeringQueue();
    if (!is_user_turn) {
        peer_wiring_.SetStatus("idle");
    }
}
// ---------------------------------------------------------------------------
// 上下文压缩的会话现场路(0.27.x 分层压缩第一期)
// ---------------------------------------------------------------------------


void TerminalSessionController::EmitSessionHook(lubancode::hooks::HookEvent event, nlohmann::json fields,
                                         const std::string& match_value) {
    lubancode::hooks::HookDispatcher* dispatcher = lubancode::app::HookRuntime();
    if (dispatcher == nullptr || dispatcher->Empty() || !dispatcher->HasHandlersFor(event)) {
        return;
    }
    // SessionStart 的两个来源(startup 已在 cli_app 发过、这里管 resume/clear/
    // compact)都要把转录路径与 session id 对齐——存档文件名就是会话 id
    // (MakeSessionId 落盘时定的),resume 后这份才是真的。
    if (event == lubancode::hooks::HookEvent::SessionStart) {
        lubancode::hooks::HookContext ctx = dispatcher->context();
        if (session_store.active()) {
            ctx.transcript_path = session_store.file_path();
            // 会话存档名就是会话 id(MakeSessionId:时间戳 + 首句 slug,slug
            // 原样保留多字节字符)——GBK 机器上 .string() 遇 emoji 就是
            // 1113 异常,一律走 u8 通道。
            ctx.session_id = lubancode::tools::PathToUtf8(
                lubancode::tools::Utf8ToPath(session_store.file_path()).stem());
        }
        lubancode::app::UpdateHookRuntimeContext(ctx);
    }
    lubancode::hooks::HookPayload payload;
    payload.event = event;
    payload.fields = std::move(fields);
    payload.match_value = match_value;
    dispatcher->Emit(event, payload);
}

// /clear 与退出共用的清场:停全部子代理、收面板。排队消息分两档
// (取走即消费单):
//   - dispose_queue=true(/clear):用户明说清场——全数倒掉,落空快照,
//     醒目告知(条数 + 首条预览),"明确丢弃、不静默消失"。
//   - dispose_queue=false(退出/析构):队列**不倒**。账在 Run() 退场前已
//     落档(resume 接得回,验收"排队→/exit→resume 队列还在"),这里只
//     提示一句"已存档几条";倒掉反而把刚落的档废了。
void TerminalSessionController::CleanupBackgroundAgents(bool dispose_queue) {
    // 面板收场:查看态/收件目标整份收干净,不给已收场的任务留悬空目标。
    lubancode::cli::ResetAgentPanelSession();
    if (dispose_queue) {
        // 会话层排队消息:先倒账、再把"空快照"追进存档(resume 不复活已
        // 倒掉的账),醒目告知(路径三:条数 + 首条预览,淡字换醒目色)。
        const auto discarded = SessionSteeringQueue().TakeAllForDisposal();
        if (!discarded.empty()) {
            PersistSteeringQueue();
            for (const std::string& row : lubancode::cli::BuildQueueDisposalRows(discarded)) {
                TermOut() << theme.error << row << theme.reset << "\n";
            }
        }
    } else if (!SessionSteeringQueue().empty()) {
        // 退场:排队账已在 Run() 落档,这里只说一句去处,别让人以为丢了。
        const auto snapshot = SessionSteeringQueue().Snapshot();
        for (const std::string& row : lubancode::cli::BuildQueueArchiveRows(snapshot)) {
            TermOut() << theme.stats << row << theme.reset << "\n";
        }
    }
    if (session_agent_tool() == nullptr) {
        return;
    }
    session_agent_tool()->CancelAllTasks();
    for (const auto& line : session_agent_tool()->TakeUndeliveredInboxReport()) {
        TermOut() << theme.stats << line << theme.reset << "\n";
    }
}







void TerminalSessionController::Run() {
    while (true) {
        // status panel 每圈都重取 cwd 与 Git 分支。/worktree、run_command
        // 切目录/分支，或队列紧接着发下一条时，都不会挂着上一帧的旧值。
        lubancode::cli::StatusPanelData status_data;
        status_data.model = *current_model;
        status_data.cwd = CurrentDirUtf8();
        status_data.git_branch = lubancode::cli::CurrentGitBranch(std::filesystem::current_path());
        status_data.worktree = worktree_session.active_name();
        status_data.provider = lubancode::config::EnvironmentOverridesActiveProvider(
                                   config_result.config, config_result.sources,
                                   config_result.config.active_provider)
                                   ? "env override / unbound"
                                   : active_provider;
        status_data.effort = *current_think;
        status_data.context_percent = context_tracker.UsagePercent();
        status_data.used_tokens = static_cast<long long>(context_tracker.current_tokens());
        status_data.window_tokens = static_cast<long long>(context_tracker.window_tokens());
        // 缓存注记(缓存诊断单):与回合内局部更新同一只 helper、同一只
        // tracker,空闲重建的第一帧不会先新后旧。
        status_data.cache_note =
            lubancode::cli::BuildCacheNote(context_tracker, !context_tracker.usage_stale());
        // 旧值标记同样出自 tracker:回合内 on_usage 局部发布的快照与这里整份
        // 重建读同一只 ContextTracker,数字与 ~ 标记完全一致,收口后的第一只
        // composer 不会先新后旧。
        status_data.context_stale = context_tracker.usage_stale();
        // REC 标记:录制中恒挂状态行第一段(见 StatusPanelData::rec)。
        status_data.rec = lubancode::cli::RecorderStatusMarker(record_wiring_.recorder_optional());
        // 工具调用后端档(PTC 单):json 默认时留空(状态行零变化),
        // programmatic/auto 时恒亮一段,回落原因写全(规格 UI 节)。
        if (config.tool_calling != lubancode::config::ToolCallingMode::Json) {
            status_data.tools = tool_runtime_->ptc_resolution();
        }
        // Plan 模式标记(只读研究硬闸单):与 confirm/auto/yolo 并列(规格
        // "plan · confirm"),不重置确认档。
        if (session_runtime_.collaboration_mode() == lubancode::runtime::CollaborationMode::Plan) {
            status_data.plan_mode = tr("plan.mode_label");
        }
        // goal/loop 会话状态段(goal 单合流):有常驻自动工作在跑才挂。
        status_data.goal_loop =
            lubancode::app::BuildGoalLoopStatusSegment(goal_wiring_.coordinator(), loop_wiring_.scheduler());
        lubancode::cli::SetStatusLineData(status_data, config.status_panel.items, config.status_panel.separator);

        // 后台命令完成通知:每圈开头取一次"新进入终态"的任务,有就打一行淡色
        // 通知给用户。不插进对话流(不发给模型、不消耗 token)——只让人看见
        // "后台那条命令跑完了";模型要是需要细节,自己调 background_output 工具查。
        // 跟 pending_queue 那条路分开:排队消息是用户自己键入的正文,要发给模型;
        // 后台通知是系统侧的状态播报,只给人看。
        if (const auto finished = lubancode::tools::BackgroundTaskRegistry::Instance().DrainCompleted();
            !finished.empty()) {
            std::lock_guard<std::mutex> stdout_lock(lubancode::cli::StdoutWriteMutex());
            for (const auto& t : finished) {
                const char* label = "已结束";
                switch (t.status) {
                    case lubancode::tools::BackgroundTaskStatus::Completed: label = "完成(退出码 0)"; break;
                    case lubancode::tools::BackgroundTaskStatus::Failed: label = "失败"; break;
                    case lubancode::tools::BackgroundTaskStatus::Stopped: label = "已停止"; break;
                    case lubancode::tools::BackgroundTaskStatus::StopFailed: label = "停止失败"; break;
                    default: break;
                }
                TermOut() << theme.stats << "[后台任务 #" << t.task_id << " " << label << "]";
                if (t.status != lubancode::tools::BackgroundTaskStatus::Completed) {
                    TermOut() << " (exit "
                              << (t.exit.exit_code.has_value() ? std::to_string(*t.exit.exit_code) : "unknown")
                              << ")";
                }
                TermOut() << " " << t.command << theme.reset << "\n";
            }
        }

        // 0.28.x 会话泵:把流式期间排下的消息送上路。子代理目标先转投任务
        // inbox(SendTaskMessage 那套,共用面板定向介入的通道);main 目标取
        // 队头自动发送——本轮没再调工具自然收尾的场合,队列紧接着成为下一
        // 次请求的用户消息,不等用户再敲一下(规格)。用户自己的排队消息
        // 优先于 peer 来信与子代理完成回流,所以泵挂在它们前头。
        //
        // 取走即消费单(路径一):拿去自动发送的那条,若这一轮以请求失败
        // 收场,原样还回队首并带"已试过一次"的账——同一条最多自动重试
        // 一次,再失败留队列等用户手动(Shift+← 取回改写再排、或删掉),
        // 错误文案旁明写一句"没送达,已回队",不再无声吞掉。
        PumpSteeringToSubagents();
        if (auto head = SessionSteeringQueue().TakeFirstAutoSendable(lubancode::cli::MessageTarget::Main())) {
            TermOut() << theme.prompt << "> " << theme.reset << head->text << "\n";
            peer_wiring_.SetStatus("busy");
            bool autosend_failed = false;
            const CommandFlow flow = ProcessLine(head->text, &autosend_failed);
            peer_wiring_.SetStatus("idle");
            if (autosend_failed) {
                // 失败退还:回队首(带 attempts+1),文案旁明说。还会再自动
                // 试一次(attempts < 2);到顶的那次退还后队列里留着,泵的
                // 防死循环闸跳过它,等用户处置。
                std::string preview = head->text;  // 先留底,ReturnToFront 会 move 走
                SessionSteeringQueue().ReturnToFront(std::move(*head));
                preview.erase(0, preview.find_first_not_of("\r\n\t "));
                const std::size_t preview_cut = preview.find('\n');
                if (preview_cut != std::string::npos) {
                    preview.resize(preview_cut);
                }
                TermOut() << theme.error << trf("queue.autosend_returned", preview) << theme.reset << "\n";
            }
            PersistSteeringQueue();
            if (flow == CommandFlow::Exit) {
                break;
            }
            continue;
        }
        // main 目标都送空了:"打断并立即送"的状态旗收掉(队列区标题复位;
        // TargetGone/失败条目留在原位等用户处置,不算"没送完")。
        if (!SessionSteeringQueue().HasDeliverable(lubancode::cli::MessageTarget::Main())) {
            SessionSteeringQueue().ClearImmediateDelivery();
        }

        // loop 单:定时循环的泵。优先级在用户排队消息之后、peer 来信与
        // 子代理回流之前——用户输入最高,审批/peer/子代理完成次之,loop
        // 最后;每圈只消费一拍,回主循环顶重新检查高优先级来源(不一次把
        // 八枚 due 全倒进队列,用户按 stop 时还来得及)。泵只在会话空闲
        // (当前没有 loop 拍在跑)时取件;single-flight 由 scheduler 保证。
        if (loop_wiring_.scheduler() != nullptr && !loop_wiring_.TickActive() &&
            loop_wiring_.HasActiveTasks()) {
            if (PumpScheduledWork()) {
                continue;
            }
        }

        // 跨会话来信:空闲当口(不在 Run 里)收进来的信,经确认后直接
        // 另起一轮外来消息,不等用户再敲一行。用户自己的排队消息优先。
        peer_wiring_.CollectHeldMessages();
        const auto incoming_peer = peer_wiring_.TakeReadyMessage();
        if (incoming_peer.has_value() &&
            !SessionSteeringQueue().HasDeliverable(lubancode::cli::MessageTarget::Main())) {
            const lubancode::peers::PeerEnvelope envelope = std::move(*incoming_peer);
            TermOut() << theme.stats
                      << trf("cmd.peers.incoming_notice", envelope.sender_name, envelope.sender_id) << theme.reset
                      << "\n";
            RunSessionTurn(lubancode::app::FormatPeerText(envelope), TurnSource::Incoming);
            continue;
        }

        // 后台子代理结果回流:任务在会话空闲时跑完的,结果不能干等用户再敲
        // 一行才送达——面板只画"完成",真正让主循环动起来的是这里。检测到
        // 未投递的完成结果就另起一轮(同外来消息那条路,不落 slash),RunTurn
        // 开头会把 DrainCompletionNotices 拿到的结果原文附带进消息。用户自己
        // 排队的消息优先:队列非空时先让队头那条走,它起 RunTurn 一样能把
        // 结果捎上。
        // 完成通知(规格"现场二"):短进度行 + 归 main 的 transcript 事件,
        // 有且只有一条——旧底栏已在 wake 路正式退场(RetireIdleChrome),通知
        // 不再夹在两副 chrome 中间当一行来路不明的永久字;Ctrl+O/Ctrl+E 重铺
        // transcript 时它跟着回来。
        // 查看态(回流单规格第一节):用户正看某只子代理时,回流照常发生但
        // 一切终端影响收进后台——通知不打裸 cout(事件照进 main 台账),主轮
        // 走静默档(输出进台账、usage 照记、查看帧零扰动),收口后坞里那行
        // 退场,导航坞提示行给一枚短 toast。回 main 时新内容都在。
        if (session_agent_tool() != nullptr &&
            !SessionSteeringQueue().HasDeliverable(lubancode::cli::MessageTarget::Main()) &&
            session_agent_tool()->HasUndeliveredCompletions()) {
            const bool viewing = lubancode::cli::CurrentAgentViewedTaskId() != 0;
            std::string reflow_ids;
            if (viewing) {
                for (const int id : session_agent_tool()->UndeliveredCompletionTaskIds()) {
                    if (!reflow_ids.empty()) {
                        reflow_ids += " ";
                    }
                    reflow_ids += "#" + std::to_string(id);
                }
            }
            const std::vector<std::string> notices = session_agent_tool()->CompletionNoticeLines();
            // goal 合流:子代理完成喂 goal 的证据/usage 账(没有活跃 goal
            // 零影响);在 RunPeerTurn 之前记,证据落在"消化回流"那轮的
            // 采证之前,checkpoint 引用得着。
            goal_wiring_.NoteSubagentCompletion();
            {
                auto& transcript = transcript_ui_.items();
                lubancode::cli::TranscriptItem item = lubancode::cli::MakeNoticeItem(
                    static_cast<int>(transcript.size()) + 1, tr("agent_panel.completion_notice"),
                    lubancode::cli::TranscriptStatus::Ok, notices);
                if (!viewing) {
                    std::lock_guard<std::mutex> stdout_lock(lubancode::cli::StdoutWriteMutex());
                    TermOut() << theme.tool_line << item.title << theme.reset << "\n";
                    for (const auto& note : notices) {
                        TermOut() << theme.stats << "  ⎿ " << note << theme.reset << "\n";
                    }
                    TermOut().flush();
                }
                transcript.push_back(std::move(item));
            }
            RunSessionTurn("后台子代理有新结果送达(资料附在本条消息里)。请阅读后继续推进手头任务;"
                           "若结论已够用,向用户简要汇报要点,不要重新摸排。",
                           TurnSource::Incoming, /*autosend_failed=*/nullptr,
                           /*silent=*/viewing, memory::QueryOrigin::BackgroundCompletion);
            if (viewing) {
                // 坞行退场由 DrainCompletionNotices 的 TouchTasks + 下一帧带出;
                // toast 替那行留一句人话,几秒自收,不抢屏。
                lubancode::cli::ShowPanelToast(trf("agent_panel.reflow_toast", reflow_ids));
            }
            continue;
        }

        std::string content;
        std::optional<int> composer_target;  // 这条话若出自查看态 composer,收件人是那只子代理
        // UI-A:主提示符是唯一开 composer 的读取点——Alt/Shift+Enter 插
        // 换行、Enter 全发、全空白不发送。别的 ReadLine 调用点(确认提示、
        // /model 编号选择、向导)保持单行语义。查看态里提交的话,收件
        // 目标由面板控制器记着(输入框上横线右端的短标题就是它)。
        const std::optional<std::string> line =
            lubancode::cli::ReadLine(theme.prompt + "> " + theme.reset, theme,
                                      /*esc_rejects=*/false, /*composer=*/true);
        if (!line.has_value()) {
            if (lubancode::cli::ComposerStashHasContent()) {
                TermOut() << theme.stats << tr("stash.still_there") << theme.reset << "\n";
            }
            PersistSteeringQueue();  // EOF 退场同路(路径二,"先留后清")
            break;  // EOF:Ctrl+Z 或管道读尽
        }
        if (line->empty()) {
            continue;  // 空行不退出,重新给提示符
        }
        content = *line;
        composer_target = lubancode::cli::CurrentComposerAgentTarget();

        if (content == "exit" || content == "quit") {
            if (lubancode::cli::ComposerStashHasContent()) {
                TermOut() << theme.stats << tr("stash.still_there") << theme.reset << "\n";
            }
            PersistSteeringQueue();  // 裸退场同样先留账(路径二,"先留后清")
            break;
        }
        // 定向介入(规格第七节):查看态 composer 提交的话直接进那只子代理
        // 自己的 inbox,在"当前工具收尾、下一次请求未发"的边界注入它的
        // history——不经 main,不串台。slash 命令仍走会话主路(/exit 这类
        // 会话级动作不该被子代理视角扣下)。终态明确拒收,不改投 main。
        if (composer_target.has_value() && !content.empty() && content.front() != '/' &&
            session_agent_tool() != nullptr) {
            const lubancode::tools::TaskMessageStatus status =
                session_agent_tool()->SendTaskMessage(*composer_target, content);
            TermOut() << theme.stats
                      << (status == lubancode::tools::TaskMessageStatus::Queued
                              ? trf("agent_panel.target_queued", *composer_target)
                              : trf("agent_panel.target_rejected", *composer_target))
                      << theme.reset << "\n";
            continue;
        }
        peer_wiring_.SetStatus("busy");  // 名册上亮"忙",对端知道别指望立刻回话
        const CommandFlow flow = ProcessLine(content);
        peer_wiring_.SetStatus("idle");
        if (flow == CommandFlow::Exit) {
            // 退出前把排队账最后一眼落档(路径二):/exit 这轮里可能还排着
            // 没送走的话,resume 要接得回来。CleanupBackgroundAgents 里那趟
            // 落的是清账后的空快照,先后次序就是"先留后清"。
            PersistSteeringQueue();
            break;
        }
    }
}

int RunInteractiveSession(const InteractiveSessionOptions& options) {
    // 组合根外迁(会话终章):装配由组合根(cli_app)调 BuildSessionStack
    // 完成并经 options.stack 递进来;没递(单测/旧调用点)就在这现装一份,
    // 两条路共用同一只装配,不走第二套逻辑。
    InteractiveSessionOptions session_options = options;
    std::unique_ptr<SessionStack> owned_stack;
    if (session_options.stack == nullptr) {
        owned_stack = BuildSessionStack(session_options);
        session_options.stack = owned_stack.get();
    }
    TerminalSessionController session(session_options, *session_options.stack);
    session.Run();
    return 0;
}

}  // namespace lubancode::app
