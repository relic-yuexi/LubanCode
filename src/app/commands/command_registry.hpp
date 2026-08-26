// 命令分派注册制(会话终章):47 案 slash 分派 switch 换成注册表——
// 名字 → handler → 权限/补全元数据。各案 handler 归各域文件
// (commands/ 下按域接活,签名统一为 HandleSlashXxx);控制器只留路由
// 与门(feature 开关、pipe 拒绝的语义在各域 handler,装配材料经
// SlashDispatchContext 一次配齐)。
//
// 分层规矩:
//   - SlashCommandSpec 是注册行:command 是 cli::SlashCommand 枚举(与
//     cli::ParseSlashCommand 同源),name 是对账名(与 cli::AllSlashCommands
//     的命令名一一对应),handler 是域文件的入口;
//   - needs_console/needs_idle 是权限/补全元数据,供分组展示与后续门用;
//     现状拒绝语义仍在域 handler(如 loop 的非交互明拒、peer 组在管道下
//     由 handler 明说没起服务),不在表上另发明新门;
//   - SlashDispatchContext 全是借用(指针/引用/回调),会话控制器构造时
//     一次配齐,handler 不拥有会话资源。
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "agent/agent.hpp"
#include "agent/artifact_store.hpp"
#include "agent/prompt_assembler.hpp"
#include "app/backend_stack.hpp"
#include "app/commands/command_flow.hpp"
#include "app/commands/goal_commands.hpp"    // GoalWiring
#include "app/commands/loop_commands.hpp"    // LoopWiring
#include "app/commands/session_commands.hpp"  // SessionCommandState/CompactSessionInputs
#include "app/hook_runtime.hpp"
#include "app/interactive_session.hpp"  // InteractiveSessionOptions(/prompt 的 law_source)
#include "app/model_router.hpp"
#include "app/tool_runtime.hpp"  // McpServerRuntime/PluginMountInfo
#include "cli/context_tracker.hpp"
#include "cli/slash_commands.hpp"
#include "cli/theme.hpp"
#include "cli/worktree.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "lsp/manager.hpp"
#include "memory/project_memory.hpp"  // ProjectMemory(/memory 的会话件)
#include "peers/peer_session.hpp"
#include "runtime/event_sinks.hpp"
#include "runtime/session_runtime.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "sessions/session_store.hpp"
#include "workflow/host_executors.hpp"

namespace lubancode::app {

class SlashDispatchContext;

// 一案一行。handler 为空 = 死案(Image 进不来分派、NotSlash 在上一层已
// 分流),表上留名只为 47 案对账齐整。
using SlashHandler = CommandFlow (*)(SlashDispatchContext&, const lubancode::cli::ParsedSlashCommand&);

struct SlashCommandSpec {
    lubancode::cli::SlashCommand command;
    const char* name;       // 对账名(与 cli::AllSlashCommands 同名)
    SlashHandler handler;   // 域文件入口;nullptr = 死案
    bool needs_console;     // 权限元数据:真控制台才有意义(peer 组等)
    bool needs_idle;        // 补全元数据:只在空闲 composer 生效(/plan)
};

// 注册表本体(命令注册制:案子按 switch 旧序登册,枚举可对)。
const std::vector<SlashCommandSpec>& SlashCommandTable();

// 会话控制器的路由入口:按枚举查表调 handler;查无(不可达)按 Continue
// 兜底,与旧 switch 的完备性兜底同语义。
CommandFlow DispatchSessionSlashCommand(SlashDispatchContext& ctx,
                                        const lubancode::cli::ParsedSlashCommand& parsed);

// 会话控制器递给各域 handler 的整束材料。全借用:会话(构造它的
// TerminalSessionController)与组合根(装好的栈)在命令执行期间保证存活。
// 字段按用途分组,域 handler 只取自己那几样,不摸控制器本体。
struct SlashDispatchContext {
    // ---- 会话配置与标量 ----
    const InteractiveSessionOptions* opts = nullptr;  // law_source(/prompt 裸敲)
    lubancode::config::ConfigResult* config_result = nullptr;
    lubancode::config::Config* config = nullptr;
    const lubancode::cli::Theme* theme = nullptr;
    const lubancode::config::ModelCatalog* model_catalog = nullptr;
    const lubancode::config::SettingsLocal* settings_local = nullptr;
    bool spinner_enabled = false;
    std::string* wire_str = nullptr;
    std::string* active_provider = nullptr;
    const std::optional<std::string>* active_provider_write_path = nullptr;
    std::optional<std::string>* config_file_path = nullptr;
    const std::optional<std::string>* home_dir = nullptr;        // /skills 的扫描位
    const std::optional<std::string>* home_lubancode = nullptr;  // /keymap /workflow
    const std::string* prompts_dir = nullptr;
    std::string* persona = nullptr;
    const std::filesystem::path* global_skills_root = nullptr;
    const std::filesystem::path* project_skills_root = nullptr;
    const std::filesystem::path* recordings_root = nullptr;
    std::vector<lubancode::tools::SkillMeta>* skills = nullptr;

    // ---- 后端栈 ----
    RebuildableBackend* real_backend = nullptr;
    std::shared_ptr<std::string> current_model;
    std::shared_ptr<std::string> current_think;
    std::shared_ptr<std::string> current_model_instructions;
    std::shared_ptr<std::string> current_soul;
    std::string* current_soul_name = nullptr;
    lubancode::cli::ContextTracker* context_tracker = nullptr;
    lubancode::app::ModelRouterService* model_router = nullptr;
    std::shared_ptr<lubancode::agent::ContextArtifactStore> artifact_store;

    // ---- 工具全栈 ----
    lubancode::tools::ToolRegistry* registry = nullptr;
    lubancode::tools::ToolRegistry* sub_registry = nullptr;
    lubancode::tools::AgentTool* agent_tool = nullptr;  // 会话级 agent 工具(可空)
    const std::shared_ptr<lubancode::tools::TodoListState>* todo_state = nullptr;
    const std::shared_ptr<std::set<std::string>>* loaded_tools = nullptr;
    const std::vector<McpServerRuntime>* mcp_servers = nullptr;
    std::optional<lubancode::lsp::Manager>* lsp_manager = nullptr;
    const std::vector<PluginMountInfo>* plugin_mounted = nullptr;
    const std::vector<std::string>* plugin_warnings = nullptr;
    const std::function<bool(const lubancode::tools::Tool&)>* main_tool_filter = nullptr;
    const std::function<bool(const lubancode::tools::Tool&)>* sub_tool_filter = nullptr;
    bool main_deferral = false;
    int tool_search_threshold = 0;
    lubancode::app::ToolRuntime* tool_runtime = nullptr;  // process_manifests/explore_registry
    lubancode::cli::WorktreeSession* worktree_session = nullptr;

    // ---- 会话运行时 ----
    lubancode::agent::Agent* main_agent = nullptr;
    lubancode::runtime::SessionRuntime* session_runtime = nullptr;  // 模式档/thread id
    lubancode::runtime::ToolTraceHub* trace_hub = nullptr;          // 可空
    lubancode::runtime::FanoutEventSink* session_events = nullptr;
    lubancode::sessions::SessionStore* session_store = nullptr;
    const std::string* sessions_dir = nullptr;
    lubancode::sessions::SessionMeta* session_meta = nullptr;  // /export 用
    std::string* session_title = nullptr;
    std::string* last_compact_line = nullptr;  // /context 的最近一次 compact 台账
    lubancode::agent::PromptOptions* prompt_options = nullptr;
    lubancode::memory::ProjectMemory* project_memory = nullptr;  // /memory(可空)

    // ---- 子系统接线器(peer/录制;会话终章外迁后的窄口) ----
    class PeerSessionWiring* peer_wiring = nullptr;    // /peers /send /peerperm
    class RecordSessionWiring* record_wiring = nullptr;  // /record

    // ---- 会话回调(控制器递进来的活口) ----
    std::function<void(bool)> rebuild_loop;               // /provider 切换后的重建
    std::function<void()> sync_request_policy;            // /think /soul 的皮上刷新
    std::function<void()> refresh_skills;                 // /skill /record install
    std::function<void()> refresh_project_instructions;   // /init
    std::function<void()> sync_worktree_directory;        // /worktree 搬房善后
    std::function<void()> ensure_memory_tool;             // /memory on/learn 后补注册
    std::function<void()> ensure_goal_coordinator;
    std::function<void()> ensure_loop_scheduler;
    std::function<GoalWiring()> make_goal_wiring;
    std::function<LoopWiring()> make_loop_wiring;
    std::function<CompactSessionInputs()> make_compact_inputs;
    std::function<SessionCommandState()> make_session_command_state;
    std::function<CommandFlow(const std::string&)> handle_plan_command;
    std::function<void(lubancode::runtime::CollaborationMode, const std::string&)> switch_collaboration_mode;
    std::function<void()> reset_plan_review;  // /clear /plan off 的悬稿翻篇
    std::function<lubancode::workflow::ToolExecutor::Options()> build_workflow_tool_options;
};

}  // namespace lubancode::app
