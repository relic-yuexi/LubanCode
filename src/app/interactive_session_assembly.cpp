// TerminalSessionController 的装配半边(会话终章):构造/析构、工具窄口、
// 回合重建与皮上刷新、分派材料装配、会话命令材料包——函数体原样自
// interactive_session.cpp 搬来(行为一字未改,注释随行);运行半边(主
// 循环/泵仲裁/回合入口)在同一只类的 interactive_session.cpp。
// 骨架拆解反弹·问题 2:本文件原名 interactive_session_wiring.cpp,与组合根
// 目录 src/app/wirings/ 撞车(那儿的文件才是"把零件接起来"的装配根);
// 这里是同一个大类的另一半方法实现,按内容改名为 assembly(装配半边)。
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
#include "agent/resolved_prompt_builder.hpp"
#include "skills/workflow_recorder.hpp"
#include "api/backend.hpp"
#include "api/models.hpp"
#include "app/backend_stack.hpp"
#include "app/commands/command_registry.hpp"  // 命令注册制:47 案分派的注册表与路由
#include "app/session_stack.hpp"  // 组合根装配件(会话终章):控制器只收装好的件
// 子系统接线器(会话终章):goal/loop/plan/peer/录制各一只(状态+装配+
// 泵+恢复),控制器持句柄调;会话级状态(theme/config/标题活值)
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
#include "app/commands/background_commands.hpp"  // BuildBackgroundStatusSegment:底栏后台段折数
#include "app/commands/hook_commands.hpp"
#include "app/commands/peer_commands.hpp"
#include "app/commands/doctor_commands.hpp"
#include "app/commands/workflow_commands.hpp"
#include "workflow/host_executors.hpp"
#include "app/version.hpp"
#include "runtime/command_service.hpp"
#include "runtime/event_sinks.hpp"
#include "runtime/plan_mode.hpp"
#include "runtime/secret_resolver.hpp"  // T2 遥测出口凭证(§15.4 唯一 owner)
#include "runtime/session_runtime.hpp"
#include "runtime/tool_trace_hub.hpp"
#include "workspace/identity.hpp"  // P0-1:终端面 workspace 身份裁决
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
#include "runtime/worktree.hpp"
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
#include "tools/instruction_scope.hpp"  // BuildScopeGateCallback/MarkBaselineSeen(作用域单 P0)
#include "config/settings_local.hpp"
#include "config/skill_store.hpp"
#include "lsp/manager.hpp"
#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"
#include "app/memory_extract.hpp"
#include "app/mention_support.hpp"  // @ 提及支件(会话终章)
#include "app/model_router.hpp"
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

// ---------------------------------------------------------------------------
// P0-4 环境快照的取材件(§9.1):工具集规范摘要与脱敏配置快照。
// ---------------------------------------------------------------------------

// 工具定义集合的规范摘要:工具名 + input_schema 按名排序拼数组,整份
// sha256 + 计数(environment.hpp 的 ToolsetSummary 口径)。同 registry
// 必得同摘要——摘要变即工具面变,重放侧据此判环境是否漂移。
lubancode::trajectory::ToolsetSummary BuildToolsetSummary(
    const std::vector<std::unique_ptr<lubancode::tools::Tool>>& tools) {
    std::vector<const lubancode::tools::Tool*> ordered;
    ordered.reserve(tools.size());
    for (const auto& tool : tools) {
        ordered.push_back(tool.get());
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const lubancode::tools::Tool* a, const lubancode::tools::Tool* b) {
                  return a->name() < b->name();
              });
    nlohmann::json definitions = nlohmann::json::array();
    for (const lubancode::tools::Tool* tool : ordered) {
        definitions.push_back(nlohmann::json{{"name", tool->name()},
                                             {"input_schema", tool->input_schema()}});
    }
    lubancode::trajectory::ToolsetSummary summary;
    summary.toolset_sha256 = lubancode::hooks::Sha256Hex(definitions.dump());
    summary.tool_count = ordered.size();
    return summary;
}

// 脱敏配置快照:只挑非敏感键,密钥/token/base_url 一概不进(§十二:
// 脱敏在落盘前做)。键集刻意收窄——将来要加,先过"这键泄不泄密"一问。
nlohmann::json BuildRedactedConfigSnapshot(const lubancode::config::Config& config) {
    return nlohmann::json{
        {"language", config.language},
        {"features",
         {{"goals", config.features_goals},
          {"loop", config.features_loop}}},
        {"connect_timeout_ms", config.connect_timeout_ms},
        {"request_timeout_secs", config.request_timeout_secs}};
}

// 窄口全指组合根装好的 ToolRuntime(stack_ 里的那份);控制器方法名沿用,
// 方法体原样。
lubancode::tools::ToolRegistry& TerminalSessionController::registry() { return stack_.registry(); }

lubancode::tools::ToolRegistry& TerminalSessionController::sub_registry() { return stack_.sub_registry(); }

lubancode::tools::AgentTool* TerminalSessionController::session_agent_tool() { return stack_.agent_tool(); }

const std::shared_ptr<lubancode::tools::TodoListState>& TerminalSessionController::todo_state() {
    return stack_.todo_state();
}

const std::shared_ptr<std::set<std::string>>& TerminalSessionController::loaded_tools() {
    return stack_.loaded_tools();
}

const std::vector<McpServerRuntime>& TerminalSessionController::mcp_servers() { return stack_.mcp_servers(); }

std::optional<lubancode::lsp::Manager>& TerminalSessionController::lsp_manager() { return stack_.lsp_manager(); }

const std::vector<PluginMountInfo>& TerminalSessionController::plugin_mounted() {
    return stack_.plugin_mounted();
}

const std::vector<std::string>& TerminalSessionController::plugin_warnings() {
    return stack_.plugin_warnings();
}

const std::function<bool(const lubancode::tools::Tool&)>& TerminalSessionController::main_tool_filter() {
    return stack_.main_tool_filter();
}

const std::function<bool(const lubancode::tools::Tool&)>& TerminalSessionController::sub_tool_filter() {
    return stack_.sub_tool_filter();
}

lubancode::workflow::ToolExecutor::Options TerminalSessionController::BuildWorkflowToolOptions() {
    // 封暗道(骨架拆解批一·病二):workflow 工具节点不再直捅
    // tool->execute(),改走 agent::RunOneTool 正门。这里把主回合那套横切
    // 链原样接上:hooks(PreToolUse/PostToolUse)、Plan 闸、逐枚 trace
    //(发号 + 分线 + 副作用闸)。确认门不在这填——BuildWorkflowExecutors
    // 装配时借 build_agent_callbacks 那份补上,与 agent 节点同一道门。
    lubancode::workflow::ToolExecutor::Options options;
    options.registry = &registry();
    options.thread_id = session_runtime_.thread_id();
    if (trace_hub_.has_value()) {
        lubancode::runtime::ToolTraceHub* hub = &*trace_hub_;
        options.execution_id_issuer = [hub] { return hub->NextExecutionId(); };
        options.callbacks.on_tool_trace = [hub](const lubancode::agent::ToolTraceEvent& event) {
            hub->OnTrace(event);
        };
        options.callbacks.on_tool_trace_blocked = [hub](const std::string& execution_id) {
            return hub->IsExecutionBlocked(execution_id);
        };
    }
    lubancode::hooks::HookDispatcher* dispatcher = lubancode::app::HookRuntime();
    if (dispatcher != nullptr && lubancode::runtime::HasToolHooks(dispatcher)) {
        options.callbacks.on_pre_tool_use_hook =
            [dispatcher](const std::string&, const std::string& name, const nlohmann::json& input) {
                return lubancode::runtime::EmitPreToolUse(dispatcher, name, input);
            };
        options.callbacks.on_post_tool_use_hook =
            [dispatcher](const std::string&, const std::string& name, const nlohmann::json& input,
                         const lubancode::tools::Tool::Result& result) {
                return lubancode::runtime::EmitPostToolUse(dispatcher, name, input, result);
            };
    }
    // Plan 闸:workflow 工具节点同过 ModePolicy(单子:不另开旁路)。
    options.callbacks.on_mode_policy = [this](const std::string& tool_name, const nlohmann::json& input) {
        return plan_wiring_.EvaluateGate(tool_name, input);
    };
    // 写前作用域闸(AGENTS.md 作用域单 P0):workflow 的工具节点由宿主会话
    // 驱动,确认账挂主 Agent 那份(已见过的 scope 不重复拦);agent 节点
    // 是独立 Agent,在 AgentExecutor 里自起一份,不走这里。
    options.callbacks.on_scope_gate = lubancode::tools::BuildScopeGateCallback(stack_.instruction_resolver,
                                                                               stack_.instruction_scope_state);
    return options;
}

lubancode::agent::TurnWiring TerminalSessionController::BuildWorkflowAgentCallbacks() {
    // workflow agent 节点的审批口(治"确认门暂不接"欠下的账):回调直通
    // ConfirmToolUse,与主回合同一颗脑袋——EvaluatePermission 先裁定
    //(yolo/auto/预放行不问),真要问时 diff 预览 + 三档菜单 + "总是允许"
    // 落回会话账(always_allowed_tools 按引用进 ConfirmToolUse)都在里头。
    lubancode::agent::TurnWiring wiring;
    wiring.on_tool_confirm = [this](const std::string& tool_use_id, const std::string& name,
                                    const nlohmann::json& input) -> bool {
        // 每次现起一只 ToolDisplay:workflow 的工具不在会话条目账上,
        // OnConfirmRequest 查不到 id 拿 -1,确认块退化成纯打印,不会去动
        // 主回合的条目;diff 预览照画(文件工具),菜单照问。
        lubancode::cli::ToolDisplay display(transcript_ui_.items(), theme,
                                            lubancode::platform::ProbeStdoutConsole().is_console,
                                            todo_state(), /*cancel=*/nullptr, transcript_ui_.expanded_flag());
        const lubancode::runtime::ToolHookDecision pre;  // workflow 路不跑 PreToolUse 钩子,空表态
        return lubancode::app::ConfirmToolUse(tool_use_id, auto_confirm, always_allowed_tools, theme, display,
                                              settings_local.allow_commands, settings_local.deny_commands,
                                              /*hook_dispatcher=*/nullptr, pre,
                                              /*has_permission_hooks=*/false, name, input);
    };
    // 权限下限(阶段 5,R 单遗留——"确认口走 ConfirmToolUse 缺省无下限,
    // 等 resolver 接线时一并喂"):`agent: <name>` 节点的自定义 Agent 定义
    // 比会话档严时,AgentExecutor 用这枚带下限的口——同一颗 ConfirmToolUse,
    // 会话档向下并到下限再裁定,父会话开着 yolo 也不免问。与 agent 工具
    // 路的 Hooks::on_tool_confirm_floored 同一先例(0.26.96)。
    wiring.on_tool_confirm_floored =
        [this](const std::string& tool_use_id, const std::string& name, const nlohmann::json& input,
               lubancode::agent::AgentPermissionMode floor) -> bool {
        lubancode::cli::ToolDisplay display(transcript_ui_.items(), theme,
                                            lubancode::platform::ProbeStdoutConsole().is_console,
                                            todo_state(), /*cancel=*/nullptr, transcript_ui_.expanded_flag());
        const lubancode::runtime::ToolHookDecision pre;
        lubancode::runtime::PermissionMode runtime_floor = lubancode::runtime::PermissionMode::Yolo;
        switch (floor) {
            case lubancode::agent::AgentPermissionMode::Confirm:
                runtime_floor = lubancode::runtime::PermissionMode::Confirm;
                break;
            case lubancode::agent::AgentPermissionMode::Auto:
                runtime_floor = lubancode::runtime::PermissionMode::Auto;
                break;
            case lubancode::agent::AgentPermissionMode::Yolo:
                runtime_floor = lubancode::runtime::PermissionMode::Yolo;
                break;
        }
        return lubancode::app::ConfirmToolUse(tool_use_id, auto_confirm, always_allowed_tools, theme, display,
                                              settings_local.allow_commands, settings_local.deny_commands,
                                              /*hook_dispatcher=*/nullptr, pre,
                                              /*has_permission_hooks=*/false, name, input, {},
                                              runtime_floor);
    };
    return wiring;
}

// 端云协同可观测单 T1/T2:遥测服务装配(构造函数与 EnableTelemetryForSession
// 共用)。telemetry{} 段的 queue/spool/exporter 键折进 options(§24.1;
// canonical 默认在 config::TelemetryConfig),出口 token 走 runtime::
// SecretResolver(§15.4 不造第二份密钥管理;telemetry 在 engine 层不认
// runtime,凭证经 token_source 这道窄缝递进去,值即取即弃不落任何账面)。
std::unique_ptr<lubancode::telemetry::TelemetryService> AssembleTelemetryServiceForSession(
    const lubancode::config::Config& config, bool config_telemetry,
    const std::optional<std::string>& home_lubancode, bool trajectory_enabled,
    lubancode::runtime::TrajectorySessionLedger* ledger, std::string* note) {
    lubancode::telemetry::TelemetryAssemblyInputs telemetry_inputs;
    telemetry_inputs.config_telemetry = config_telemetry;
    telemetry_inputs.config_trajectory = trajectory_enabled;
    if (home_lubancode.has_value()) {
        telemetry_inputs.options.telemetry_root =
            lubancode::tools::Utf8ToPath(*home_lubancode) / "telemetry";
        telemetry_inputs.options.resource.service_version = std::string(lubancode::app::kVersion);
        telemetry_inputs.options.resource.frontend = "terminal";
    }
    telemetry_inputs.options.queue.capacity_items =
        static_cast<std::size_t>(config.telemetry.queue_capacity_items);
    telemetry_inputs.options.queue.capacity_bytes =
        static_cast<std::uint64_t>(config.telemetry.queue_capacity_bytes);
    telemetry_inputs.options.spool_enabled = config.telemetry.spool_enabled;
    telemetry_inputs.options.spool.total_bytes_cap =
        static_cast<std::uint64_t>(config.telemetry.spool_max_bytes);
    telemetry_inputs.options.spool.max_age_ms = config.telemetry.spool_max_age_hours * 3600 * 1000;
    if (auto data_class = lubancode::telemetry::DataClassFromName(config.telemetry.data_class);
        data_class.has_value()) {
        telemetry_inputs.options.data_class = *data_class;
    }
    telemetry_inputs.options.exporter.endpoint = config.telemetry.exporter_endpoint;
    telemetry_inputs.options.exporter.compression = config.telemetry.exporter_compression;
    telemetry_inputs.options.exporter.timeout_ms = config.telemetry.exporter_timeout_ms;
    telemetry_inputs.options.exporter.secret_ref = config.telemetry.exporter_secret_ref;
    if (!config.telemetry.exporter_secret_ref.empty()) {
        // 凭证适配(§15.4):EnvDotEnvSecretResolver 按声明解析(host feature
        // 无插件数据目录,.env 来源没有,只有宿主环境);resolver 不缓存,
        // 每次出口现读——轮换 Key 无需重启。
        auto resolver = std::make_shared<lubancode::runtime::EnvDotEnvSecretResolver>();
        lubancode::runtime::SecretDeclaration declaration;
        declaration.id = "telemetry.exporter";
        declaration.env = config.telemetry.exporter_secret_ref;
        declaration.required = false;  // 缺失时匿名发,由 collector 裁
        telemetry_inputs.options.exporter.token_source =
            [resolver, declaration]() -> std::optional<std::string> {
            auto resolved = resolver->Resolve(declaration);
            if (!resolved.has_value() || !resolved->HasValue()) {
                return std::nullopt;
            }
            return std::string(resolved->View());
        };
    }
    auto service = lubancode::telemetry::TryAssembleTelemetryService(telemetry_inputs, note);
    if (service != nullptr && ledger != nullptr) {
        service->RegisterSession(ledger->workspace_key(), ledger->session_id(), ledger->session_dir());
        ledger->SetTelemetryWake(service.get());
    }
    return service;
}

TerminalSessionController::TerminalSessionController(const InteractiveSessionOptions& options,
                                                     SessionStack& stack)
    : opts_(options),
      stack_(stack),
      config_result(stack_.config_result),
      config(stack_.config_result.config),
      theme(options.theme),
      transcript_ui_(theme),
      agent_panel_presenter_(theme),
      auto_confirm(options.auto_confirm),
      persona(options.persona),
      spinner_enabled(options.spinner_enabled),
      model_catalog(options.model_catalog),
      settings_local(options.settings_local),
      home_dir(stack_.home_dir),
      official_skills_dir(stack_.official_skills_dir),
      skills(stack_.skills),
      skills_segment(stack_.skills_segment),
      home_lubancode(stack_.home_lubancode),
      prompts_dir(stack_.prompts_dir),
      project_memory(stack_.project_memory),
      project_instructions(stack_.project_instructions),
      project_instruction_sources(stack_.project_instruction_sources),
      global_skills_root(stack_.global_skills_root),
      project_skills_root(stack_.project_skills_root),
      real_backend(stack_.real_backend),
      current_model(stack_.current_model),
      current_think(stack_.current_think),
      current_think_history(stack_.current_think_history),
      active_provider(stack_.active_provider),
      model_router(stack_.model_router),
      artifact_store(stack_.artifact_store),
      current_model_instructions(stack_.current_model_instructions),
      current_soul_name(stack_.current_soul_name),
      current_soul(stack_.current_soul),
      wrapped_backend(stack_.wrapped_backend),
      context_tracker(stack_.context_tracker),
      worktree_session(stack_.worktree_session),
      tool_runtime_(stack_.tool_runtime),
      main_deferral(stack_.main_deferral),
      sub_deferral(stack_.sub_deferral),
      main_proxy(stack_.main_proxy),
      sub_proxy(stack_.sub_proxy),
      main_native(stack_.main_native),
      sub_native(stack_.sub_native),
      native_server_tool_search(stack_.native_server_tool_search),
      tool_search_threshold(stack_.tool_search_threshold),
      tool_search_token_floor(stack_.tool_search_token_floor),
      config_file_path(stack_.config_result.config_file_path),
      always_allowed_tools(session_runtime_.always_allowed()),
      // P6:会话真账本体在 SessionRuntime(轨迹账本);wire_str 先落值,
      // runtime 的 Options 要吃它。P0-6:旧 sessions_dir 不再传。
      session_runtime_([&] {
          lubancode::runtime::SessionRuntime::Options runtime_options;
          runtime_options.wire_name = lubancode::config::ProviderWireName(config.wire);
          runtime_options.start_ts = lubancode::tools::NowIdTimestamp();
          // P0-2(Trajectory 升为唯一 Session):feature/env 开关已删,恒开
          // 轨迹账;开张失败在 ctor 后由 trajectory_open_error 报,Run()
          // 入口据此失败启动,不回退旧 SessionStore。
          // P0-1:终端面身份按四级裁决冻结(commondir→marker→config→cwd),
          // 同仓子目录/linked worktree 同一把钥匙;裁决料整份递给 ledger,
          // 账本不再各算各的 key。
          {
              const std::filesystem::path identity_home =
                  home_lubancode.has_value()
                      ? lubancode::tools::Utf8ToPath(*home_lubancode)
                      : std::filesystem::path();
              auto identity = lubancode::workspace::ResolveWorkspaceIdentity(
                  std::filesystem::current_path(), identity_home);
              if (identity.has_value()) {
                  runtime_options.trajectory_workspace_identity = std::move(*identity);
              }
          }
          runtime_options.lubancode_version = std::string(lubancode::app::kVersion);
          // --continue(§10.4):启动路直接开 start_reason=resume 的新场,
          // 不先造空 session;没有可恢复场回落普通开张(同旧路
          // quiet_if_none)。恢复的历史由启动善后段灌进 loop。
          runtime_options.trajectory_resume_at_launch = options.continue_last;
          return runtime_options;
      }()),
      wire_str(lubancode::config::ProviderWireName(config.wire)),
      session_start_ts(session_runtime_.start_ts()),
      // 两层标题的账(骨架拆解反弹·问题 2):绑本类标题活值与轨迹账本,
      // 判定本体在 app/session_title_account。
      titles_(session_title, session_runtime_.trajectory()),
      recordings_root(home_lubancode.has_value() ? lubancode::tools::Utf8ToPath(*home_lubancode) / "recordings"
                                                 : std::filesystem::path()),
      // 非 turn 通知的终端画法(骨架拆解反弹·问题 2):controller 经
      // notice_sink() 的基类口递通知。声明序在杂项段前,列表同序。
      notice_sink_terminal_(theme),
      active_provider_write_path(stack_.active_provider_write_path) {
    // 逐枚追踪单:trace hub 安家(抓 session_runtime_ 的 ids/store 引用;
    // 分线 canonical 工具事件到 session 栅栏/录制投影/UI 投影)。
    trace_hub_.emplace(session_runtime_.ids());
    // 事件流接线(骨架拆解批二,补显示剥离第六步停下的那段):sink 列表
    // 一处配齐——终端账本先挂;SessionRuntime 的每轮 adapter 与 hub 的
    // trace 投影都落到这串。往后加消费方(app-server 直出、脚本桥)只往
    // fanout 里 Add,装配点不再各挑回调。
    session_events_.Add(&terminal_event_ledger_);
    session_runtime_.AttachSink(&session_events_);
    trace_hub_->AttachSink(&session_events_);

    // 端云协同可观测单 T1/T2:features.telemetry 激活(且轨迹真开了)才装
    // 本地遥测服务——TryAssemble 非 Active 一律 nullptr,零目录零线程
    // (§8.5);开了则注册本场 session 并把 committed wake 挂到账本。
    // telemetry{} -> options 的折法在 AssembleTelemetryServiceForSession
    // (EnableTelemetryForSession 复用同一份)。
    if (auto* ledger = session_runtime_.trajectory()) {
        std::string telemetry_note;
        telemetry_service_ = AssembleTelemetryServiceForSession(
            config, config.features_telemetry, home_lubancode,
            session_runtime_.trajectory() != nullptr, ledger, &telemetry_note);
        if (telemetry_service_ == nullptr &&
            lubancode::telemetry::ResolveTelemetryActivation(
                config.features_telemetry, session_runtime_.trajectory() != nullptr)
                    .status ==
                lubancode::telemetry::TelemetryActivationStatus::RequiresTrajectory) {
            // §8.2:telemetry 开了 trajectory 没开——明说,不暗开。
            TermErr() << theme.error << tr("error.prefix")
                      << "遥测需要轨迹账在场,本次未启用: "
                      << telemetry_note << theme.reset << "\n";
        }
    }

    // 工具全栈在组合根已装好(stack_.tool_runtime);逐枚追踪单第四期:
    // hub 已安家,挂 undo_file_edit(条件式撤销:凭
    // hub 的账本翻凭据,走与 write/edit 同一道确认门)。
    tool_runtime_->AttachUndoTool(&*trace_hub_);
    // 可追回 artifact 的两把只读钥匙(第二期):main 与子代理同级都有。
    // main 的 context_read 另接按需摘要:模型显式写 summarize=true 才花
    // cheap token,回执自然追加在尾部。子代理只给原文读取,免并发改路由账。
    registry().Register(std::make_unique<lubancode::tools::ContextSearchTool>(artifact_store));
    registry().Register(std::make_unique<lubancode::tools::ContextReadTool>(
        artifact_store, [this](const lubancode::agent::ArtifactRef& ref) {
            return lubancode::app::SummarizeArtifactOnDemand(MakeTailContext(), ref);
        }));
    sub_registry().Register(std::make_unique<lubancode::tools::ContextSearchTool>(artifact_store));
    sub_registry().Register(std::make_unique<lubancode::tools::ContextReadTool>(artifact_store));
    // 子系统接线器(会话终章):goal/loop/plan/peer/录制各配一只 Host(全
    // 借用 + 晚绑定槽),装配与状态归接线器,控制器持句柄调。会话级状态
    //(theme/config/标题活值)留本类,两边不互相摸。
    {
        GoalSessionWiring::Host goal_host;
        goal_host.theme = &theme;
        goal_host.config = &config;
        goal_host.trace_hub = &*trace_hub_;
        goal_host.model_router = model_router.get();
        goal_host.evaluation_backend = &wrapped_backend;
        goal_host.current_model = current_model;
        goal_host.agent_tool = [this]() { return session_agent_tool(); };
        goal_host.loop_scheduler = [this]() { return loop_wiring_.scheduler(); };
        goal_host.start_turn = [this](const std::string& text, bool* turn_failed) {
            RunSessionTurn(text, TurnSource::User, turn_failed);
        };
        // 渲染事件出口(骨架拆解反弹·问题 3):goal 接线器不再直接画终端,
        // 通知从这递出来——is_error 定色,文案由接线器拼好,渲染逐字节照旧。
        goal_host.notify = [this](bool is_error, const std::string& text) {
            TermOut() << (is_error ? theme.error : theme.stats) << text << theme.reset << "\n";
        };
        goal_wiring_.AttachHost(std::move(goal_host));

        LoopSessionWiring::Host loop_host;
        loop_host.theme = &theme;
        loop_host.interactive = spinner_enabled;
        loop_host.config = &config;
        loop_host.session_runtime = &session_runtime_;
        loop_host.home_lubancode = &home_lubancode;
        loop_host.idle_wakes = &idle_wakes_;
        loop_host.start_turn = [this](const std::string& text, bool* turn_failed) {
            RunSessionTurn(text, TurnSource::User, turn_failed);
        };
        // 渲染事件出口(骨架拆解反弹·问题 3):单拍状态机(runtime 的
        // LoopTickDriver)产 LoopTickNotice,接线器只转发,画法在这。
        loop_host.notify = [this](const lubancode::runtime::loop::LoopTickNotice& notice) {
            TermOut() << (notice.kind == lubancode::runtime::loop::LoopTickNotice::Kind::Error ? theme.error
                                                                                              : theme.stats)
                      << notice.text << theme.reset << "\n";
        };
        loop_wiring_.AttachHost(std::move(loop_host));

        PlanSessionWiring::Host plan_host;
        plan_host.theme = &theme;
        plan_host.session_runtime = &session_runtime_;
        plan_host.prompt_options = &prompt_options;
        plan_host.artifact_store = artifact_store.get();
        plan_host.main_agent = [this]() { return main_agent.has_value() ? &*main_agent : nullptr; };
        plan_host.registry = [this]() { return &registry(); };
        plan_host.agent_tool = [this]() { return session_agent_tool(); };
        plan_host.rebuild_preserving = [this]() { RebuildLoop(/*preserve_history=*/true); };
        plan_host.start_turn = [this](const std::string& text, bool* turn_failed) {
            RunSessionTurn(text, TurnSource::User, turn_failed);
        };
        plan_wiring_.AttachHost(std::move(plan_host));

        PeerSessionWiring::Host peer_host;
        peer_host.theme = &theme;
        peer_host.interactive = spinner_enabled;
        peer_host.home_lubancode = &home_lubancode;
        peer_host.session_title = [this]() { return session_title; };
        peer_host.permission_mode = [] {
            return static_cast<int>(lubancode::cli::CurrentConfirmMode());
        };
        peer_wiring_.AttachHost(std::move(peer_host));

        RecordSessionWiring::Host record_host;
        record_host.recordings_root = &recordings_root;
        record_host.project_skills_root = &project_skills_root;
        record_host.global_skills_root = &global_skills_root;
        record_host.refresh_skills = [this]() { RefreshSkills(); };
        // P0-2 轨迹:flag 开的会话,/record 走选段器(轨迹账本在
        // SessionRuntime;record_session_wiring 只借指针)。
        record_host.trajectory = session_runtime_.trajectory();
        record_wiring_.AttachHost(std::move(record_host));
    }
    // goal/loop 的窄工具(goal 单第 2 期 + loop 单第 4 期的注册欠账):注册进
    // 主表,靠 turn 级动态过滤放行——goal_checkpoint 只在 goal iteration 的
    // turn 里露面,loop_control 只在 scheduled tick 的 turn 里露面,普通轮、
    // 子代理、MCP 一概看不见(单子:动态 tool set 的 scope 语义)。状态在
    // 接线器里先造(注册要用),id 由发 turn 的那两处泵灌。
    goal_wiring_.RegisterTools(registry());
    loop_wiring_.RegisterTools(registry());
    // 后台子代理面板的数据源(缓存 + 修订号,面板每 100ms 拉一次)。列表走
    // 轻量全量(TaskSummaries,不截 8 只);查看态的长正文由视图切换钩子按
    // viewed_task_id 现取,整块换进上方会话视口——导航坞只放导航。
    lubancode::cli::SessionAgentPanelHost().SetProvider(
        [this]() { return agent_panel_presenter_.Entries(session_agent_tool()); });
    lubancode::cli::SetAgentViewSwitchHook(
        [this](int viewed_task_id, int tail_rows) {
            transcript_ui_.PrintViewedTranscript(viewed_task_id, tail_rows);
        });

    // 面板动作接线(x 停止/清除、Ctrl+X Ctrl+K 两段确认停全部):只发信号/
    // 清台账,面板等任务线程报终态的那一拍自己改灯。
    lubancode::cli::AgentPanelActions panel_actions;
    panel_actions.cancel_task = [this](int task_id) {
        return session_agent_tool() != nullptr && session_agent_tool()->CancelTask(task_id);
    };
    panel_actions.clear_task = [this](int task_id) {
        return session_agent_tool() != nullptr && session_agent_tool()->ClearFinishedTask(task_id);
    };
    panel_actions.cancel_all = [this]() {
        return session_agent_tool() != nullptr ? session_agent_tool()->CancelAllTasks() : 0;
    };
    lubancode::cli::SessionAgentPanelHost().SetActions(panel_actions);

    // 刮屏驱动器专用(tests/manual/agent_panel_driver.cpp,不进 ctest):设
    // LUBANCODE_AGENT_PANEL_DEMO=N 时面板显示 N 只假代理,便于真控制台断言
    // 导航坞贴底与残帧计数。正常启动不设这个变量,provider 还是真数据。
    // LUBANCODE_AGENT_PANEL_DEMO_IDLE=K 让前 K 只处于完成态,驱动闲置折叠
    // (完成行最多单列三只,更多折成一行汇总)。
    if (const auto demo = lubancode::platform::GetEnvVar("LUBANCODE_AGENT_PANEL_DEMO");
        demo.has_value() && !demo->empty()) {
        const int demo_count = std::max(1, std::atoi(demo->c_str()));
        int demo_idle = 0;
        if (const auto idle = lubancode::platform::GetEnvVar("LUBANCODE_AGENT_PANEL_DEMO_IDLE");
            idle.has_value() && !idle->empty()) {
            demo_idle = std::min(std::max(0, std::atoi(idle->c_str())), demo_count);
        }
        lubancode::cli::SessionAgentPanelHost().SetProvider([demo_count, demo_idle]() {
            std::vector<lubancode::cli::AgentPanelEntry> fake;
            for (int i = 1; i <= demo_count; ++i) {
                lubancode::cli::AgentPanelEntry entry;
                entry.task_id = i;
                entry.name = "general-purpose #" + std::to_string(i);
                entry.title = "演示任务 " + std::to_string(i);
                entry.running = i > demo_idle;
                entry.state = entry.running ? "运行中(2 次工具调用 · 1.2k tokens · 12s)"
                                            : "完成(2 次工具调用 · 1.2k tokens · 12s)";
                fake.push_back(std::move(entry));
            }
            return fake;
        });
        // 演示钩子同样接管视图切换:假代理不在 AgentTool 台账里,查看态的
        // 头行从假条目表出,正文给一行占位——刮屏驱动器照旧只认屏面。
        lubancode::cli::SetAgentViewSwitchHook([demo_count, demo_idle, this](int viewed_task_id, int tail_rows) {
            (void)tail_rows;  // 演示代理没有实时流,重铺拍与整铺同款
            std::lock_guard<std::mutex> stdout_lock(lubancode::cli::StdoutWriteMutex());
            TermOut() << "\n";
            if (viewed_task_id == 0) {
                TermOut() << theme.stats << tr("agent_panel.main_header") << theme.reset << "\n";
                TermOut() << theme.stats << tr("agent_panel.back_to_main") << theme.reset << "\n";
            } else if (viewed_task_id >= 1 && viewed_task_id <= demo_count) {
                TermOut() << trf("agent_panel.view_header", "general-purpose #" + std::to_string(viewed_task_id),
                                 "演示任务 " + std::to_string(viewed_task_id))
                          << "\n"
                          << "  [" << (viewed_task_id > demo_idle ? "后台" : "完成") << "] 演示条目,无 transcript 台账\n";
            }
            TermOut().flush();
        });
    }

    // 后台子代理结果回流(空闲唤醒):任务在会话空闲时跑完的,不能干等用户
    // 再敲一行才送达。ReadLine 等键的 100ms 面板刷新一拍里问这里,有未投递
    // 的完成结果就让位,主循环顶另起一轮把结果交回主代理。
    // loop 单起改多路:子代理与 loop 的 due 都挂进 IdleWakeCoordinator,
    // 单枚 SetIdleWakeHook 只装"问总口"的一枚总钩,谁也不覆盖谁。
    subagent_wake_token_ = idle_wakes_.AddSource("subagent", [this]() {
        return session_agent_tool() != nullptr && session_agent_tool()->HasUndeliveredCompletions();
    });
    // 后台命令任务的唤醒源(background 管理面单):watcher 把某条任务翻成
    // 终态那一刻(HasUnreportedCompletions 只看不取),空闲 composer 让位,
    // 主循环顶打完成/失败通知行,顺手重折底栏计数——用户不敲一行也当拍
    // 看见,通知不闪一下就没(台账留着,/background 随时回看)。
    background_wake_token_ = idle_wakes_.AddSource("background_tasks", []() {
        return lubancode::tools::BackgroundTaskRegistry::Instance().HasUnreportedCompletions();
    });
    // 标题精修完工的唤醒源(通知时序缺陷单):精修完成后不再借下一次用户
    // 输入收货——Ready 只在"结果备好待收"翻真(运行中不醒),空闲
    // composer 的 100ms 拍一看真就让位,ReadLine 以空串返回,主循环的
    // 收货点当场记 usage、改名、打"标题已设为"(在提示符重画前)。失败
    // 的结局也完工:usage 是真花的,照样叫醒来收账;收完 Ready 翻假,
    // 不空转。
    title_wake_token_ = idle_wakes_.AddSource("session_title", [this]() {
        return HasFinishedTitleRefinement();
    });
    lubancode::cli::SetIdleWakeHook([this]() { return idle_wakes_.AnyReady(); });

    // 底栏状态行的后台任务段数据源(background 管理面单):BuildStatusLine
    // 每次组行现折一次(空闲 100ms 拍与流式 footer 每帧),"后台 N 运行 /
    // M 完成"跟着台账走,无任务空串收起。终端层不认台账,折数归这层。
    lubancode::cli::SetBackgroundStatusProvider([]() {
        return lubancode::app::BuildBackgroundStatusSegment(
            lubancode::tools::BackgroundTaskRegistry::Instance().List());
    });
    lubancode::cli::SetSessionSkillCount(skills.size());

    // 后台代理权限拒绝的当场告知(后台代理权限拒绝无告知单,2026-08-17):
    // 后台任务的 needs_confirm 工具被拒那一刻,AgentTool 已把一行通知推进
    // 台账;空闲 composer 的 100ms 拍在这里取走——导航坞 toast 一枚(几秒
    // 自收)+ transcript 记一条有归属的事件,用户当拍看见,不攒到最终报告。
    // 只落 toast 与台账,不打裸行:不打断 composer,查看态零扰动。
    lubancode::cli::SetBackgroundNoticeHook([this]() {
        if (session_agent_tool() == nullptr) {
            return;
        }
        std::vector<std::string> notices = session_agent_tool()->TakePermissionDenialNotices();
        // 监督器通知(监督器单 P0-2):同一枚空闲拍取走,同一条 toast +
        // transcript 通路——疑似断流/空转提醒/恢复成功按 task+epoch+reason
        // 去重过,不会每拍刷屏(单子 §十)。
        const std::vector<std::string> supervisor_notices = session_agent_tool()->TakeSupervisorNotices();
        for (const std::string& notice : supervisor_notices) {
            if (std::find(notices.begin(), notices.end(), notice) == notices.end()) {
                notices.push_back(notice);
            }
        }
        if (notices.empty()) {
            return;
        }
        for (const std::string& notice : notices) {
            lubancode::cli::ShowPanelToast(notice);
            auto& transcript = transcript_ui_.items();
            transcript.push_back(lubancode::cli::MakeNoticeItem(
                static_cast<int>(transcript.size()) + 1, tr("agent_panel.denial_notice_title"),
                lubancode::cli::TranscriptStatus::Error, {notice}));
        }
    });

    // Ctrl+R 提问历史搜索的数据源(0.30.x 第二批):只读 session 事件账,
    // 打开搜索框时取一次(范围轮换在终端层本地过滤,不反复读盘)。
    // 轮次打断/用户排队的唤醒广播(监督器单 P1-0):ESC 打断或用户往待发
    // 队列排消息那一刻,叫醒睡在 agent_watch 有界等待里的 watcher——单子
    // §9.2"用户输入、父取消、session close 提前唤醒"。只做唤醒(台账推一
    // 代监督可见修订),在监听线程上被调,零重活。
    lubancode::cli::SetTurnInterruptBroadcast([this]() {
        if (session_agent_tool() != nullptr) {
            session_agent_tool()->ledger().NotifyExternalWake();
        }
    });

    lubancode::cli::SetPromptHistoryProvider([this]() { return CollectPromptHistory(); });

    // @ 文件提及菜单的数据源(0.30.x 第三批):按 Git 根(没有就 cwd)扫
    // 一份相对路径清单,根变了才重扫;排除 .git/构建产物与隐藏目录。
    lubancode::cli::SetFileMentionProvider([this]() { return mention_support_.Snapshot(); });

    // -----------------------------------------------------------------------
    // UI-D(0.16.0):Ctrl+O 紧凑/详细 + 焦点导航 + Ctrl+E 聚焦查看。
    // 按键语义翻译在 LineEditorCore(composer 空不空、键是什么),转发管道
    // 在 console_input 的 SetTranscriptUiHandler。导航/查看态/重画的本体在
    // cli::TranscriptUiController(终端接线收尾单自大类搬出),这里只装钩子
    // (查看态视口/横幅重画/ESC 急停/活历史/轮视图存档)并转发按键。只在等
    // 输入时会被调(流式期间监听线程天然吞不进这些键);管道模式走不到逐键
    // 路径,整套无感。
    // -----------------------------------------------------------------------
    {
        lubancode::cli::TranscriptUiController::Hooks transcript_hooks;
        transcript_hooks.build_task_transcript = [this](int task_id, int width) {
            const auto& workflow_detail = lubancode::cli::SessionAgentPanelHost().transcript_provider();
            if (workflow_detail) {
                auto lines = workflow_detail(task_id, width, transcript_ui_.agent_view_expanded());
                if (lines.has_value()) return *lines;
            }
            return agent_panel_presenter_.TaskTranscriptLines(session_agent_tool(), task_id, width,
                                                              transcript_ui_.agent_view_expanded());
        };
        transcript_hooks.repaint_banner = [this]() { PrintBanner(config, theme); };
        // ESC 急停(空闲态、composer 空):活 loop 一键全停,定义保留,
        // 续跑 /loop resume <id>。停了的账落档(FlushLoopEvents)。
        transcript_hooks.stop_active_loops = [this]() -> int { return loop_wiring_.StopAllForEsc(); };
        transcript_hooks.history = [this]() -> const std::vector<lubancode::api::Message>* {
            return main_agent.has_value() ? &main_agent->History() : nullptr;
        };
        transcript_hooks.turn_views = [this]() -> const std::vector<lubancode::runtime::TurnView>* {
            return &turn_views_;
        };
        transcript_ui_.SetHooks(std::move(transcript_hooks));
    }
    lubancode::cli::SetTranscriptUiHandler(
        [this](lubancode::cli::UiKeyAction action) -> bool { return transcript_ui_.HandleKey(action); });

    // 0.19.x 提示词模块化:系统提示按会话实际启用的能力条件拼装——
    // skills 有技能才注、mcp/web/lsp 配了才注、平台段按 wire。法(persona)
    // 非空时 core 模块让位,环境/features 段照拼。
    prompt_options.cwd = CurrentDirUtf8();
    prompt_options.persona = persona;
    prompt_options.skills_segment = skills_segment;
    prompt_options.project_instructions = project_instructions;
    prompt_options.project_instruction_sources = project_instruction_sources;
    prompt_options.mcp = !config.mcp_servers.empty();
    prompt_options.web = config.search.Configured();
    prompt_options.lsp = !config.lsp_servers.empty();
    prompt_options.wire = lubancode::config::ProviderWireName(config.wire);
    prompt_options.prompts_dir = prompts_dir;  // 运行时模块:拼装时现读现拼
    // 统一 Package 封装单阶段 3:包层 Profile 根——主 Agent 点名 canonical
    // Profile("<包id>:<名>")时在包根里解析;裸名照旧走内置/用户/项目层。
    // (阶段 6:折现行快照;镜像成员在此起持有,ctx.package_mount 借它的账。)
    package_snapshot_view_ = stack_.CurrentPackageSnapshot();
    prompt_options.package_profile_roots =
        lubancode::package::MountProfileRoots(package_snapshot_view_->mount());

    // 跨会话传话(0.25.x 同机首版):loop 每次 rebuild(/clear、/model、
    // provider 切换)都会 emplace 重来,安全收件点(SetInbox)得跟着重灌。
    // 这里先挂一个可空的重灌钩子,PeerRuntime 起来之后再填实(见下)。
    RebuildLoop();

    std::set<std::string>& allowed = always_allowed_tools;
    // settings.local.json 的 allow_tools:启动即注入会话"总是允许"集合,这些
    // 工具本会话直接免确认(跟按 a 落进来的是同一个集合)。
    for (const std::string& tool_name : settings_local.allow_tools) {
        allowed.insert(tool_name);
    }

    // 后台子代理的放行账源(修"后台审批不查放行账",2026-08):agent 工具
    // LaunchBackground 派工当口在主线程调它,拷一份定格快照带进任务线程。
    // 账面与主会话 ConfirmToolUse 同一本:上面的 allow_tools 注入 + 会话内
    // 按 a 落的 always_allowed_tools,外加 settings.local 的 allow/deny 命令
    // 前缀。快照为什么定格在派出时刻,见 tools/agent_tool.hpp 里
    // BackgroundPermissionLedger 的注释。闭包按 Hooks 的寿命规矩捕获引用:
    // 控制器死后主循环不在,源不会再被调。
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry().Find("agent"));
        agent_tool != nullptr) {
        const lubancode::config::SettingsLocal& local_permissions = settings_local;
        agent_tool->SetBackgroundPermissionSource([&allowed, &local_permissions]() {
            lubancode::tools::BackgroundPermissionLedger ledger;
            ledger.always_allowed = allowed;
            ledger.allow_commands = local_permissions.allow_commands;
            ledger.deny_commands = local_permissions.deny_commands;
            return ledger;
        });
    }

    // -----------------------------------------------------------------------
    // 会话真账(P0-2 起 trajectory Journal,P0-6 旧存档已删):消息事实由
    // typed 事件即时落账;memory 的会话源接轨迹场 id。
    // -----------------------------------------------------------------------
    if (project_memory != nullptr) {
        project_memory->set_source_session(session_start_ts);
        // 存储 v2 P0-3:轨迹账开着就把 memory 的落账桥挂上——召回快照与
        // 写入因果边进本场 Journal;source session 用轨迹场的 session id
        // (全限定引用的 session 段与 Journal 同源)。flag 关的会话不挂,
        // 一笔不落,行为与从前一致。
        if (auto* memory_ledger = session_runtime_.trajectory()) {
            memory_ledger_bridge_ = std::make_unique<lubancode::app::MemoryLedgerBridge>(*memory_ledger);
            project_memory->set_accounting(memory_ledger_bridge_.get());
            project_memory->set_source_session(memory_ledger->session_id());
        }
    }

    // --continue:等价开场自动 /resume 本目录最近一场;本目录没有存档就
    // 安静开新会话。resume 可能把会话搬回存档里的 worktree 房,搬没搬记
    // 一笔,后面 sync 定义好了再善后。
    bool resume_moved_into_worktree = false;
    // P0-3 轨迹档:--continue 走 §10.4 启动路——账本在 SessionRuntime ctor
    // 里已按 resume-as-new 开张(start_reason=resume 的新 session,source
    // 只读),这里只把折叠投影灌进 loop;旧 SessionStore 的 resume 路不碰。
    if (opts_.continue_last && session_runtime_.trajectory() != nullptr) {
        if (session_runtime_.trajectory()->resumed_at_launch()) {
            const std::vector<lubancode::api::Message> resumed =
                session_runtime_.trajectory()->LaunchResumeHistory();
            // 动态工具 P1:恢复走 RestoreSessionHistory——历史进账的同时从
            // 正式 discovery event 重建 DiscoveryLedger(单子 §9.2);compact
            // 那一路仍走 ReplaceHistory,引用账不丢(§9.3)。
            main_agent->RestoreSessionHistory(resumed);
            TermOut() << theme.banner
                      << trf("cmd.resume.restored", session_runtime_.trajectory()->session_id(),
                             resumed.size())
                      << theme.reset << " (resume-as-new)\n";
            TermOut() << trf("cmd.resume.estimate", lubancode::agent::EstimateHistoryTokens(resumed))
                      << "\n";
            // resume 的历史开新账(SessionStart source=resume),仓按新场开。
            EmitSessionHook(lubancode::hooks::HookEvent::SessionStart,
                            nlohmann::json{{"source", "resume"}}, "resume");
            OpenArtifactStore();
            goal_wiring_.RestoreFromArchive();
            BackfillTitleOnResume();
        }
    }
    // (P0-6:--continue 的旧 SessionStore resume 路已删;账本恒开,
    // resumed_at_launch 为假即安静开新会话,同旧 quiet_if_none 语义。)
    // -----------------------------------------------------------------------
    // 跨会话传话:登记名册、起 pipe/socket 服务与心跳。只在交互会话启用
    // (spinner_enabled = 真控制台;管道/单发没有可回话的人,也不该挂监听)。
    // Start 失败不拦着聊,只打一行提示——这场不在名册上,/peers 看不见
    // 别人,别人也递不进话。
    //
    // 收发规矩全在 agent/peer_session.* 与 agent/peer_mailbox.*:传输线程只
    // 把信放进 PeerMailbox(自带锁),不碰 history、不碰终端;主线程在轮次
    // 边界(loop 的安全收件点)与空闲(Run() 循环顶)取走。held 的信由主
    // 线程弹 [y/N] 确认,用户点头才交给模型;点头与否都不影响传输层已经
    // 回掉的 held。
    // -----------------------------------------------------------------------
    peer_wiring_.Start(registry());
    std::function<std::optional<lubancode::api::Message>()> peer_inbox_poll = peer_wiring_.BuildInboxPoll();
    // 0.28.x:轮次边界收件点改为常驻(不再依赖 peer 是否启动)。收件链两段,
    // 排队消息在前(用户自己的话优先)、peer 来信在后,来源文案分清:
    //   - 子代理目标:当场转投任务 inbox(PumpSteeringToSubagents,与面板
    //     定向介入同一条通道),不混进 main 的注入批次;
    //   - main 目标:一次边界攒下的多条合成一条 user 消息(一块一条
    //     TextBlock,顺序保留),InjectIncomingMessage 会追加在刚入 history 的
    //     tool_result 之后——tool result 在前、介入消息在后、下一 request 最后,
    //     钉死的正是这个次序。危险工具执行中途没有任何调用点,天然插不进话。
    reapply_peer_inbox = [this, peer_inbox_poll]() {
        // 批四·病十二:inbox 是接线,进 AgentWiring(其余接线照原样带上)。
        lubancode::agent::AgentWiring wiring = main_agent->wiring();
        wiring.inbox = [this, peer_inbox_poll]() -> std::optional<lubancode::api::Message> {
            PumpSteeringToSubagents();
            // 问题二(忙碌期排队的 /context 被当普通消息送模型):TakeDeliverable
            // 在队列层对 slash 条目让路——这里取到的只有普通文字,slash 留在
            // 队列,由轮末会话泵(主循环 TakeFirstAutoSendable → ProcessLine)
            // 本地执行,任何模型请求都见不到它。
            const auto queued = SessionSteeringQueue().TakeDeliverable(lubancode::cli::MessageTarget::Main());
            if (!queued.empty()) {
                // 取走即不再属于 footer 的活队列；若入队那一帧恰因终端隐藏/
                // 屏幕查询失败没画出来，后续快照也不会再有机会显示它。先在
                // 终端历史留下用户回显，再把同一批正文注入下一次模型请求。
                lubancode::cli::EchoDeliveredQueuedMessages(queued, theme);
                lubancode::api::Message inject;
                inject.role = lubancode::api::Role::User;
                for (const auto& item : queued) {
                    inject.content.push_back(lubancode::api::TextBlock{
                        "[用户排队消息] 用户在上一只工具执行期间补了话,按排队顺序接上,不另起新任务:\n" +
                        item.text});
                }
                // 送走的即出档(路径二):快照事件行记当前活队列,已注入的
                // 不在里头,resume 不复活已送出的消息。崩在这之后的半轮里,
                // 消息本体也已在 history 落盘路上(PersistNewMessages)。
                // P0-4 排队账(§5.5):注入消息已成形,dequeued 在这落锤
                //(取走即消费,不会再退还)。
                if (session_runtime_.trajectory() != nullptr) {
                    for (const auto& item : queued) {
                        session_runtime_.trajectory()->NoteQueueDequeued(
                            lubancode::cli::QueueItemId(item.id), "tool_boundary_delivery");
                    }
                }
                return inject;
            }
            if (peer_inbox_poll) {
                return peer_inbox_poll();
            }
            return std::nullopt;
        };
        main_agent->SetWiring(std::move(wiring));
    };
    reapply_peer_inbox();
    // P0-4 排队账(§5.5):队列的终态安全变化(enqueue/用户删除)从队列
    // 层广播,这里接进轨迹账本——监听线程落队时 cli 层够不着账本,观察
    // 口是唯一不破坏分层的路。flag 关的会话不挂,零开销。
    if (session_runtime_.trajectory() != nullptr) {
        SessionSteeringQueue().SetChangeObserver(
            [this](lubancode::cli::QueueChangeKind kind, const lubancode::cli::QueuedMessage& item) {
                lubancode::runtime::TrajectorySessionLedger* ledger = session_runtime_.trajectory();
                if (ledger == nullptr) {
                    return;  // /clear 换账后的空窗:没有账可落,如实跳过
                }
                if (kind == lubancode::cli::QueueChangeKind::Enqueued) {
                    ledger->NoteQueueEnqueued(lubancode::cli::QueueItemId(item.id),
                                              item.target.short_label(), "busy_enqueue");
                } else {
                    ledger->NoteQueueCancelled(lubancode::cli::QueueItemId(item.id), "user_removed");
                }
            });
    }
    // loop 已就位,把 worktree 工具 enter/exit 的善后接到这条 sync 上。
    stack_.after_worktree_moved = [this]() { SyncWorktreeDirectory(); };
    // --continue 若把会话搬回了存档里的房,提示词与子代理 cwd 跟着同步。
    if (resume_moved_into_worktree) {
        SyncWorktreeDirectory();
    }
    // Plan 模式单:起手档(--mode/env/settings 在 RunCli 算好)。--continue
    // 恢复出旧档的场合:档里有 mode 行的,旧账真值已灌进 runtime,起手档
    // 不再插手(单子:"/resume 从旧账恢复最后 mode");老档没 mode 行的,
    // 恢复按 Default,起手档照常生效——旧档没有 Plan 账,不存在两本真值。
    if (opts_.start_in_plan && !plan_wiring_.RestoredFromArchive() &&
        session_runtime_.collaboration_mode() != lubancode::runtime::CollaborationMode::Plan) {
        plan_wiring_.SwitchMode(lubancode::runtime::CollaborationMode::Plan, "slash");
    }

    // 状态面板拼装材料(骨架拆解反弹·问题 2):指针字段绑一次,goal/loop
    // 两枚每圈在 Run() 顶刷新;折数在 app/status_panel_assembly。
    status_inputs_.current_model = current_model.get();
    status_inputs_.active_provider = &active_provider;
    status_inputs_.current_think = current_think.get();
    status_inputs_.ptc_resolution = &tool_runtime_->ptc_resolution();
    status_inputs_.context_tracker = &context_tracker;
    status_inputs_.worktree_session = &worktree_session;
    status_inputs_.config_result = &config_result;
    status_inputs_.session_runtime = &session_runtime_;
    status_inputs_.recorder = &record_wiring_.recorder_optional();

    // 命令注册制的分派材料:全部装配完了一次配齐(handler 从这取料)。
    AssembleDispatchContext();
    RefreshWorkflowCompletions();
}

lubancode::app::SessionNoticeSink& TerminalSessionController::notice_sink() {
    // 经基类口递通知:终端画法在 TerminalSessionNoticeSink,往后 app-server
    // 挂第二只 sink 时这里换个返回值就行,调用点一个不动。
    return notice_sink_terminal_;
}

TerminalSessionController::~TerminalSessionController() {
    // 定向介入收场(规格:退出/清场不能无声遗失):停全部、报未送达。任务
    // 线程由 AgentTool 析构统一 join(此刻 tool_runtime_ 还活着,先于成员析构)。
    // 析构走"退场"档:排队账不倒(已落档,resume 接得回),只提示去处。
    CleanupBackgroundAgents(/*dispose_queue=*/false);
    // 跨会话传话收尾:摘掉收件点(别让重建钩子再碰已停的 runtime),写
    // closing、摘名片、停 pipe——此后递来的信连不上,发送方拿 unavailable。
    reapply_peer_inbox = nullptr;
    if (main_agent.has_value()) {
        lubancode::agent::AgentWiring wiring = main_agent->wiring();
        wiring.inbox = nullptr;
        main_agent->SetWiring(std::move(wiring));
    }
    peer_wiring_.Stop();
    // UI 回调清挂(原先的 UiHandlerGuard):回调抓着 this,析构前必须摘掉,
    // 异常退场也走这条。
    lubancode::cli::SetTranscriptUiHandler(nullptr);
    // 面板接线宿主整体收清(provider/actions 双清;终端接线收尾单)。
    lubancode::cli::SessionAgentPanelHost().Reset();
    lubancode::cli::SetAgentViewSwitchHook(nullptr);
    lubancode::cli::SetIdleWakeHook(nullptr);
    lubancode::cli::SetBackgroundNoticeHook(nullptr);
    lubancode::cli::SetBackgroundStatusProvider(nullptr);
    lubancode::cli::SetSessionSkillCount(0);
    lubancode::cli::SetTurnInterruptBroadcast(nullptr);
    lubancode::cli::SetPromptHistoryProvider(nullptr);
    lubancode::cli::SetFileMentionProvider(nullptr);
    lubancode::cli::SetAdditionalSlashCompletionCandidates({});
    // 空闲唤醒源先摘;loop 随后停 timer/join(shutdown 要 join,不能让
    // callback 析构后摸 this)。
    subagent_wake_token_.reset();
    background_wake_token_.reset();
    title_wake_token_.reset();
    loop_wiring_.Shutdown();
}

void TerminalSessionController::RebuildLoop(bool preserve_history) {
    // 每次真正重建会话都重读项目指令。用户手改 AGENTS.md 后敲 /clear，
    // 不必退出进程；provider/技能触发的保历史重建也顺手吃到新内容。
    // P1 起走会话那只 Resolver(全局层/fallback 名单与启动同一口径),
    // 顺手带出逐 source 账;读的是同一只实例,stat 快筛直接吃缓存。
    const lubancode::config::InstructionChain baseline =
        stack_.instruction_resolver->ResolveForPath(std::filesystem::current_path());
    project_instructions = baseline.content;
    project_instruction_sources.clear();
    for (const std::filesystem::path& source : baseline.sources) {
        project_instruction_sources.push_back(lubancode::tools::PathToUtf8(source));
    }
    prompt_options.project_instructions = project_instructions;
    prompt_options.project_instruction_sources = project_instruction_sources;
    // 作用域单 P0:重建时新基线重新预登记(/init 重载、AGENTS.md 手改后
    // /clear 都走这里)。
    lubancode::tools::MarkBaselineSeen(*stack_.instruction_resolver, *stack_.instruction_scope_state,
                                       std::filesystem::current_path(), project_instructions);
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry().Find("agent"));
        agent_tool != nullptr) {
        agent_tool->SetProjectInstructions(project_instructions);
    }
    std::vector<lubancode::api::Message> old_history;
    if (preserve_history && main_agent.has_value()) {
        old_history = main_agent->History();
    }
    // 运行策略走统一 profile(规格根因一):输出上限三级解析(config >
    // provider > 模型目录),unset 交服务端默认,不再有写死的 4096;步数、
    // 上下文、窗口、续跑次数同一份。子代理 tool 也在此处同步拿到派生份
    // (subagent 段的显式覆盖在 BuildSubagentRuntimeProfile 里算),main 与
    // 子代理同级吃同一套有效值。
    // 批四·病十一其三:五层请求改写后端退役后,会话级请求策略(model/
    // effort/模型指令/魂/延迟索引)全在皮上;tool_search 的过滤谓词随重建
    // 重新灌——loaded 集合不清,已挂载的工具跨 /clear 仍然可用。
    const lubancode::agent::AgentRuntimeProfile main_profile =
        lubancode::app::BuildMainRuntimeProfile(config, &model_catalog, *current_model);
    lubancode::agent::AgentProfile main_agent_profile;
    main_agent_profile.provider = active_provider;
    main_agent_profile.request.model = *current_model;
    main_agent_profile.request.reasoning_effort = *current_think;
    main_agent_profile.request.reasoning_history = *current_think_history;
    if (const auto* entry = model_catalog.FindByProviderAndSlug(active_provider, *current_model);
        entry != nullptr) {
        main_agent_profile.request.reasoning = entry->reasoning;
    }
    main_agent_profile.runtime = main_profile;
    // Token 账本单 A1:ResolvedPromptBuilder 底账与 system_prompt 同一次
    // 拼装产出(purpose 缺省即 MainTurn,主会话不必显式复述)。
    {
        lubancode::agent::ResolvedPromptBase base = lubancode::agent::BuildResolvedPromptBase(prompt_options);
        main_agent_profile.system_prompt = base.text;
        main_agent_profile.resolved_prompt_base = std::move(base);
    }
    // P0-4 环境快照(§9.1/§9.2):flag 开的会话在 profile 定型后采一次
    //——system prompt 全文、工具集规范摘要、脱敏配置快照从这取得到
    // 真值;git/cwd/os 由账本现取。落不进账只是缺口(DetermineReplayLevel
    // 如实降档),不拦会话。
    if (session_runtime_.trajectory() != nullptr) {
        lubancode::runtime::TrajectorySessionLedger::EnvironmentFacts environment_facts;
        environment_facts.provider = active_provider;
        environment_facts.wire = session_runtime_.wire_name();
        environment_facts.model = *current_model;
        environment_facts.system_prompt = main_agent_profile.system_prompt;
        environment_facts.toolset = BuildToolsetSummary(registry().All());
        environment_facts.project_instruction_refs = prompt_options.project_instruction_sources;
        environment_facts.config_snapshot_redacted = BuildRedactedConfigSnapshot(config);
        if (!main_agent_profile.request.reasoning_effort.empty()) {
            environment_facts.model_parameters["reasoning_effort"] =
                main_agent_profile.request.reasoning_effort;
        }
        (void)session_runtime_.trajectory()->CaptureEnvironment(environment_facts);
    }
    // 皮上的叠层(从前由传输层的 ModelInstructions/SoulOverlay/
    // DeferredIndex 三只包装后端现拼,现在 Agent 拼请求时就地生效)。
    main_agent_profile.model_instructions = *current_model_instructions;
    main_agent_profile.soul = *current_soul;
    // 动态工具 P3(Claude NativeReference·§7.1):原生路的双字段——皮上
    // native_deferred_tools 让 BuildToolDefinitions 给延迟定义标
    // load_mode=Deferred;request.server_tool_search(请求档案)让 anthropic
    // wire 声明服务端搜索工具。子代理皮从这份拷贝,两字段自然继承(子表
    // 的暴露过滤在 native 档同样放行延迟工具)。
    if (main_native) {
        main_agent_profile.native_deferred_tools = true;
        main_agent_profile.request.server_tool_search = native_server_tool_search;
    }
    // 动态工具 P1:proxy_reference 模式不注逐请求刷新的延迟索引段(单子
    // §8.1——索引按 loaded 集合删行,每删一行 system 就断一次;proxy 路
    // 的 tool_search 自己握 catalog,system 恒定)。legacy 路照旧现查现拼。
    // P3:native 同理不注——发现走 provider 服务端搜索,system 恒定,延迟
    // 定义照发由 provider 排出缓存前缀(§7.1)。
    if (!main_proxy && !main_native) {
        main_agent_profile.deferred_index_provider = [this]() {
            // 发请求前现查现拼:tool_search 命中后的下一份请求,新挂载的工具
            // 自然从索引段里消失;未启用时恒给空串,等于不注。
            return main_deferral
                       ? lubancode::tools::BuildDeferredToolsIndexSegment(registry(), *loaded_tools())
                       : std::string();
        };
    } else if (tool_runtime_.has_value()) {
        // proxy 三件套:解引用器(主侧账)+ 执行资格(只作用于经 tool_invoke
        // 解引用来的调用;直接按名调延迟工具仍被 exposure 过滤拦下)。
        main_agent_profile.tool_ref_resolver = tool_runtime_->main_tool_ref_resolver();
        main_agent_profile.tool_execution_policy = tool_runtime_->main_execution_policy();
        main_agent_profile.tool_execution_denial = tool_runtime_->main_execution_denial();
    }
    // 工具可见性(动态工具 PromptCache 守恒单 P2·§8.2 拆两家):
    //   - 暴露策略(tool_filter = ToolExposurePolicy):goal/loop 窄工具的
    //     定义常驻 tools 数组,只认会话级条件(features 开没开)——旧路按
    //     "这一轮是谁的轮"现放行,每翻一轮 goal/loop,tools hash 就断一次,
    //     普通 turn→goal turn→loop tick 三种回合各是一副工具表,同一类病
    //     (单子 §一/§8.2)。定义不再随轮次进出,hash 恒定;
    //   - 执行策略的生命周期半边(tool_turn_gate = ToolExecutionPolicy):
    //     调用当口现判真实状态(goal iteration 活着?loop tick 在拍上?),
    //     直名调用与经 tool_invoke 解引用的调用同一道闸,拒绝给
    //     turn.tool_not_active 稳定码;
    //   - 给模型看的轮次提示走 RunSessionTurn 的 [turn capabilities] 段
    //     (turn context 尾部追加,不进 system)。
    // 其余工具照旧走 ToolRuntime 的主过滤(延迟挂载原样;memory gate 的
    // 清账见 tool_runtime.cpp——P2 起它也只认注册,不再现查运行档)。
    main_agent_profile.tool_filter = [this](const lubancode::tools::Tool& tool) {
        if (tool.name() == "goal_checkpoint") {
            return goal_wiring_.ToolExposed();
        }
        if (tool.name() == "loop_control") {
            return loop_wiring_.ToolExposed();
        }
        return main_tool_filter()(tool);
    };
    main_agent_profile.tool_filter_denial =
        main_proxy
            ? "延迟工具不能按名直调(发现不等于授权):请先用 tool_search 检索拿到 tool_ref,再以 "
              "tool_invoke({tool_ref, arguments}) 调用。goal/loop 窄工具定义常驻,但只在对应轮次可执行,"
              "普通轮调用会被执行门以 turn.tool_not_active 拒绝。"
            : main_native
                  ? "这只工具当前不可见:goal/loop 窄工具须先开启对应功能(features.goals/features.loop)。"
                    "延迟工具的定义已随 defer_loading 常驻声明,可先经服务端工具搜索发现后直接调用。"
                  : "这只工具当前不可见:延迟工具须先经 tool_search 挂载;goal/loop 窄工具须先开启对应功能"
                    "(features.goals/features.loop)。";
    main_agent_profile.tool_turn_gate = [this](const lubancode::tools::Tool& tool) {
        // 名字先认:子代理表里没有这两枚工具,别的名字一律放行,免得主侧
        // 的轮次状态被无关调用摸到。
        if (tool.name() == "goal_checkpoint") {
            return goal_wiring_.HasActiveIteration();
        }
        if (tool.name() == "loop_control") {
            return loop_wiring_.TickActive();
        }
        return true;
    };
    main_agent_profile.tool_turn_gate_denial =
        std::string(lubancode::agent::kErrTurnToolNotActive) +
        "|goal_checkpoint/loop_control 的定义常驻工具表,但只在对应的 goal 执行轮/loop 定时拍里可执行;"
        "当前轮不是——等相应轮次或换路径,不要重试同一调用。";
    // 病十(批三):四段开关写进皮——与 prompt_options 同源(配置),子代理
    // 派生时同段拷贝,不再有"主代理有、子代理无"的隐性分叉。
    main_agent_profile.prompt_sections.mcp = prompt_options.mcp;
    main_agent_profile.prompt_sections.web = prompt_options.web;
    main_agent_profile.prompt_sections.lsp = prompt_options.lsp;
    main_agent_profile.prompt_sections.wire = prompt_options.wire;
    main_agent.emplace(wrapped_backend, registry(), main_agent_profile);
    if (auto* agent_tool = dynamic_cast<lubancode::tools::AgentTool*>(registry().Find("agent"));
        agent_tool != nullptr) {
        lubancode::agent::AgentProfile subagent_profile = main_agent_profile;
        subagent_profile.runtime = lubancode::app::BuildSubagentRuntimeProfile(main_profile, config);
        // 动态工具 P2:turn 闸不随皮拷给子代理——goal/loop 的生命周期是主
        // 会话的轮次账,子代理表里没有这两枚工具,闸在那边只会白摸主侧
        // 状态;子代理自己的执行资格照旧走 tool_filter(角色 allow/deny)
        // 与 tool_execution_policy(P1 的 sub 侧策略)。
        subagent_profile.tool_turn_gate = nullptr;
        subagent_profile.tool_turn_gate_denial.clear();
        agent_tool->SetAgentProfile(std::move(subagent_profile));
        // 子代理记忆召回(存储 v2 P0-3 §6.2):派工当刻检索一次,整段冻结
        // 下发,子代理不自动扫整库;快照经落账桥进本场 Journal,relations
        // .child_run_id 指向那只孩子。关着(use=false)就不注。
        if (project_memory != nullptr && config.memory.use) {
            agent_tool->SetTurnContextProvider(
                [memory = project_memory](const std::string& task_prompt, const std::string& child_run_id) {
                    return memory->BuildTurnContextForDispatch(task_prompt, std::filesystem::current_path(),
                                                               child_run_id);
                });
        }
    }
    // 可追回 artifact(第二期):重建的 loop 也要接上仓(空仓安全退化)。
    main_agent->context().set_artifact_store(artifact_store.get());
    // mid-turn 上下文安全点(0.27.x):窗口与压力通报随 loop 重建重灌;窗口
    // 的后续变化(/context、/model)由 RunUserTurn 发轮前再同步。
    main_agent->SetContextWindowTokens(context_tracker.window_tokens());
    // 接线(批四·病十二):压力钩进 AgentWiring;inbox 由 peer 钩在底下重灌。
    lubancode::agent::AgentWiring main_wiring;
    main_wiring.on_context_pressure = [this](const lubancode::agent::ContextPressure& pressure) {
        lubancode::app::HandleContextPressure(pressure, MakeCompactInputs());
    };
    main_agent->SetWiring(std::move(main_wiring));
    if (reapply_peer_inbox) {
        reapply_peer_inbox();  // 跨会话收件点:重建的 loop 也要能收信
    }
    if (preserve_history) {
        main_agent->ReplaceHistory(std::move(old_history));
    }
}

void TerminalSessionController::SyncAgentRequestPolicy() {
    // /model、/think、/soul 只改会话状态(current_model/current_think/
    // current_model_instructions/current_soul),loop 不重建——皮上的请求
    // 档案与叠层由这里整份刷新,下一份请求即时生效。reasoning 档位照
    // (provider, model) 从目录现查,与从前 ThinkOverrideBackend 在
    // send_stream 里干的是同一笔账,只是挪进了正门、进了前缀指纹的视野。
    if (!main_agent.has_value()) {
        return;
    }
    lubancode::api::RequestProfile request;
    request.model = *current_model;
    request.reasoning_effort = *current_think;
    request.reasoning_history = *current_think_history;
    if (const auto* entry = model_catalog.FindByProviderAndSlug(active_provider, *current_model);
        entry != nullptr) {
        request.reasoning = entry->reasoning;
    }
    // 动态工具 P3:原生声明随模型能力重算——/model 切到目录未声明
    // deferred_tools 的模型时,defer_loading 与服务端搜索声明一并停发
    //(给不认的模型发是必 400 的空承诺),大声提示;切回有声明的模型即恢复。
    // 延迟定义照常全量发送(不丢定义),tools hash 变动由前缀账如实记
    // epoch 断(§7.3 同款口径)。
    if (main_native) {
        const lubancode::config::DeferredToolsCapability capability = lubancode::config::ClassifyNativeToolSearch(
            model_catalog.FindByProviderAndSlug(active_provider, *current_model) != nullptr
                ? model_catalog.FindByProviderAndSlug(active_provider, *current_model)
                : model_catalog.FindBySlug(*current_model));
        if (capability.declared && capability.tool_reference) {
            request.server_tool_search =
                capability.server_tool_search.empty() ? native_server_tool_search : capability.server_tool_search;
            main_agent->SetNativeDeferredTools(true);
        } else {
            request.server_tool_search.clear();
            main_agent->SetNativeDeferredTools(false);
            TermOut() << theme.error
                      << "[tool_search] 当前模型未声明 deferred_tools 能力,已停发 defer_loading/服务端"
                         "工具搜索声明(延迟工具定义照常全量发送);切回有声明的模型自动恢复。"
                      << theme.reset << "\n";
        }
    }
    main_agent->SetRequestProfile(std::move(request));
    main_agent->SetModelInstructions(*current_model_instructions);
    main_agent->SetSoul(*current_soul);
}

// (P0-6:RestoreThinkHistoryFrom——旧存档 think_history 事件的恢复口——
// 已删;跨轮保留选择的持久账在 trajectory,/think history 每场现设。)

void TerminalSessionController::RefreshSkills() {
    // 包根来自会话钉快照(阶段 3;/package reload 后的 RefreshSkills 折
    // 现行快照):/skill 装了新散装技能照旧重扫;包内清单按现行快照那一
    // 份(reload 之外,包目录变化下回启动才见)。
    skills = lubancode::tools::LoadSkills(
        CurrentDirUtf8(), home_dir, official_skills_dir,
        lubancode::package::MountSkillRoots(stack_.CurrentPackageSnapshot()->mount()));
    lubancode::cli::SetSessionSkillCount(skills.size());
    skills_segment = lubancode::tools::BuildSkillsPromptSegment(skills);
    if (auto* tool = dynamic_cast<lubancode::tools::SkillTool*>(registry().Find("skill")); tool != nullptr) {
        tool->SetSkills(skills);
    }
    if (auto* tool = dynamic_cast<lubancode::tools::SkillTool*>(sub_registry().Find("skill")); tool != nullptr) {
        tool->SetSkills(skills);
    }
    if (auto* tool = dynamic_cast<lubancode::tools::AgentTool*>(registry().Find("agent")); tool != nullptr) {
        tool->SetSkillsSegment(skills_segment);
    }
    prompt_options.skills_segment = skills_segment;
    RefreshWorkflowCompletions();
    RebuildLoop(/*preserve_history=*/true);
}

void TerminalSessionController::RefreshWorkflowCompletions() {
    lubancode::app::WorkflowCommandContext wf_ctx;
    wf_ctx.project_root = std::filesystem::current_path();
    wf_ctx.user_root = home_dir.has_value()
                           ? std::optional<std::filesystem::path>(lubancode::tools::Utf8ToPath(*home_dir))
                           : std::nullopt;
    wf_ctx.packaged_workflows =
        lubancode::package::MountWorkflowSources(stack_.CurrentPackageSnapshot()->mount());
    wf_ctx.theme = &theme;
    for (const auto& skill : skills) wf_ctx.skill_names.push_back(skill.name);
    lubancode::cli::SetAdditionalSlashCompletionCandidates(
        lubancode::app::BuildWorkflowSlashCompletionCandidates(wf_ctx));
}

std::vector<std::string> TerminalSessionController::ReloadPackages() {
    // /package reload 的会话侧(阶段 6):折好才换——重折一份崭新快照,
    // 任何错都兜在 package 层(旧快照一分不动);折成了才原子换档,在跑
    // 引用各自钉着旧 shared_ptr 照旧跑完。code 组件(Plugin/MCP)不重挂:
    // 挂载事务只在会话启动跑,reload 不热插不热卸(回执里如实说)。
    const std::shared_ptr<const lubancode::package::PackageSnapshot> current =
        stack_.CurrentPackageSnapshot();
    std::vector<std::string> warnings;
    lubancode::package::PackageMountInput input = BuildSessionPackageMountInput(
        config, opts_.package_dirs, current ? current->pinned_trust
                                            : lubancode::package::PackageTrustSnapshot{},
        &warnings);
    const lubancode::package::PackageReloadReport report =
        lubancode::package::ReloadPackageSnapshot(current, std::move(input));
    if (!report.ok) {
        warnings.push_back(report.error);
        return warnings;  // 旧快照未动,下游一个不刷
    }
    // 换档三步:原子槽换新折 → 镜像换新折(ctx 借用的账)→ ctx 重指。
    // 命令都在主线程跑,这三步之间没有并发窗口。
    stack_.package_snapshot.store(report.snapshot);
    package_snapshot_view_ = report.snapshot;
    dispatch_ctx_.package_mount = &package_snapshot_view_->mount();
    // 下游刷新:技能清单(含包根,重灌 skill 工具与 agent 工具段)、
    // Profile 根(主 Agent 与 agent 工具两处)、workflow 补全。RefreshSkills
    // 内部自带 RebuildLoop,系统提示段下一轮即新。
    RefreshSkills();
    prompt_options.package_profile_roots =
        lubancode::package::MountProfileRoots(package_snapshot_view_->mount());
    if (lubancode::tools::AgentTool* tool = session_agent_tool(); tool != nullptr) {
        tool->SetPackageProfileRoots(prompt_options.package_profile_roots);
    }
    RefreshWorkflowCompletions();
    warnings.reserve(warnings.size() + report.lines.size());
    for (const std::string& line : report.lines) {
        warnings.push_back(line);
    }
    return warnings;
}

void TerminalSessionController::RefreshProjectInstructions() {
    RebuildLoop(/*preserve_history=*/true);
}

// 分派材料的装配:全借用/回调,handler 不拥有会话资源。回调一律窄口
// (控制器方法一跳),域文件不反向 include 会话层。
void TerminalSessionController::AssembleDispatchContext() {
    lubancode::app::SlashDispatchContext& ctx = dispatch_ctx_;
    ctx.opts = &opts_;
    ctx.config_result = &config_result;
    ctx.config = &config;
    ctx.theme = &theme;
    ctx.model_catalog = &model_catalog;
    ctx.settings_local = &settings_local;
    ctx.spinner_enabled = spinner_enabled;
    ctx.wire_str = &wire_str;
    ctx.active_provider = &active_provider;
    ctx.active_provider_write_path = &active_provider_write_path;
    ctx.config_file_path = &config_file_path;
    ctx.home_dir = &home_dir;
    ctx.home_lubancode = &home_lubancode;
    // 阶段 6:挂载账借镜像持有的现行快照(reload 换档时镜像先换、这里后
    // 指);供应商口拷 shared_ptr 出去,workflow 跑一趟钉一份。
    ctx.package_mount = &package_snapshot_view_->mount();
    ctx.package_snapshot_provider = [this]() { return stack_.CurrentPackageSnapshot(); };
    ctx.prompts_dir = &prompts_dir;
    ctx.persona = &persona;
    ctx.global_skills_root = &global_skills_root;
    ctx.project_skills_root = &project_skills_root;
    ctx.recordings_root = &recordings_root;
    ctx.skills = &skills;
    ctx.real_backend = &real_backend;
    ctx.current_model = current_model;
    ctx.current_think = current_think;
    ctx.current_think_history = current_think_history;
    ctx.current_model_instructions = current_model_instructions;
    ctx.current_soul = current_soul;
    ctx.current_soul_name = &current_soul_name;
    ctx.context_tracker = &context_tracker;
    ctx.model_router = model_router.get();
    ctx.artifact_store = artifact_store;
    ctx.registry = &registry();
    ctx.sub_registry = &sub_registry();
    ctx.agent_tool = session_agent_tool();
    ctx.todo_state = &todo_state();
    ctx.loaded_tools = &loaded_tools();
    ctx.mcp_servers = &mcp_servers();
    ctx.lsp_manager = &lsp_manager();
    ctx.plugin_mounted = &plugin_mounted();
    ctx.plugin_warnings = &plugin_warnings();
    ctx.main_tool_filter = &main_tool_filter();
    ctx.main_deferral = main_deferral;
    ctx.main_proxy_reference = main_proxy;
    ctx.main_native_reference = main_native;
    ctx.tool_search_threshold = tool_search_threshold;
    ctx.tool_search_token_floor = tool_search_token_floor;
    ctx.tool_runtime = tool_runtime_.has_value() ? &*tool_runtime_ : nullptr;
    ctx.worktree_session = &worktree_session;
    ctx.main_agent = main_agent.has_value() ? &*main_agent : nullptr;
    ctx.session_runtime = &session_runtime_;
    ctx.trace_hub = trace_hub_.has_value() ? &*trace_hub_ : nullptr;
    // P0-2 轨迹:命令生命周期记账的账本(flag 开才有)。
    ctx.trajectory = session_runtime_.trajectory();
    // T1 遥测:/telemetry 与 /doctor telemetry 的本地状态面(可空 = 未开)。
    ctx.telemetry_service = telemetry_service_.get();
    // T2:/telemetry enable session 的执行体(当前进程内装,§24.2)。
    ctx.enable_telemetry_session = [this]() { return EnableTelemetryForSession(); };
    ctx.session_events = &session_events_;
    ctx.session_title = &session_title;
    ctx.last_compact_line = &last_compact_line;
    ctx.prompt_options = &prompt_options;
    ctx.project_memory = project_memory.get();
    ctx.peer_wiring = &peer_wiring_;
    ctx.record_wiring = &record_wiring_;
    ctx.rebuild_loop = [this](bool preserve_history) { RebuildLoop(preserve_history); };
    ctx.sync_request_policy = [this]() { SyncAgentRequestPolicy(); };
    ctx.refresh_skills = [this]() { RefreshSkills(); };
    ctx.reload_packages = [this]() { return ReloadPackages(); };
    ctx.refresh_workflow_completions = [this]() { RefreshWorkflowCompletions(); };
    ctx.refresh_project_instructions = [this]() { RefreshProjectInstructions(); };
    // AGENTS.md 作用域单 P1-1:/instructions 与 /doctor instructions 用会话
    // 那只 Resolver(与写前闸、基线预登记同一份账)。
    ctx.instruction_resolver = stack_.instruction_resolver.get();
    ctx.sync_worktree_directory = [this]() { SyncWorktreeDirectory(); };
    ctx.ensure_memory_tool = [this]() { EnsureMemoryTool(); };
    ctx.ensure_goal_coordinator = [this]() { goal_wiring_.Ensure(config); };
    ctx.ensure_loop_scheduler = [this]() { loop_wiring_.Ensure(); };
    ctx.make_goal_wiring = [this]() {
        return goal_wiring_.MakeCommandWiring(session_agent_tool(), loop_wiring_.scheduler());
    };
    ctx.make_loop_wiring = [this]() { return loop_wiring_.MakeCommandWiring(); };
    ctx.make_compact_inputs = [this]() { return MakeCompactInputs(); };
    ctx.make_session_command_state = [this]() { return MakeSessionCommandState(); };
    ctx.handle_plan_command = [this](const std::string& args) { return plan_wiring_.HandleCommand(args); };
    ctx.switch_collaboration_mode = [this](lubancode::runtime::CollaborationMode mode, const std::string& reason) {
        plan_wiring_.SwitchMode(mode, reason);
    };
    ctx.reset_plan_review = [this]() { plan_wiring_.DiscardReview(); };
    ctx.build_workflow_tool_options = [this]() { return BuildWorkflowToolOptions(); };
    ctx.build_workflow_agent_callbacks = [this]() { return BuildWorkflowAgentCallbacks(); };
}

SessionCommandState TerminalSessionController::MakeSessionCommandState() {
    SessionCommandState state{
        [this](bool preserve_history) { RebuildLoop(preserve_history); },
        *main_agent,
        session_compact_epoch,
        session_title,
        session_start_ts,
        [this]() {
            // /clear:旧上下文就此终局——SessionEnd(reason=clear) 先发,新的
            // 空会话用 SessionStart(source=clear) 开账。仓也关掉:工具们持
            // 同一只仓,scope 只跟当前会话,旧场子的 artifact 查不到。
            artifact_store->Close();
            EmitSessionHook(lubancode::hooks::HookEvent::SessionEnd, nlohmann::json{{"reason", "clear"}}, "clear");
            EmitSessionHook(lubancode::hooks::HookEvent::SessionStart, nlohmann::json{{"source", "clear"}},
                            "clear");
            if (project_memory != nullptr) {
                // P0-3:换账后 source session 跟轨迹场走(clear 后是新场 id)。
                project_memory->set_source_session(session_runtime_.trajectory() != nullptr
                                                       ? session_runtime_.trajectory()->session_id()
                                                       : session_start_ts);
            }
            // 两层标题(实测问题 7):翻场翻代,上一场在飞的精炼落地即弃;
            // 新场子的下一问重走本地起名 + 精炼(判定本体在 titles_)。
            titles_.ResetForNewSession();
        },
        [this](const std::string& title) {
            peer_wiring_.SetName(title);
            // 人工 /title 抢先(实测问题 7):翻标题代数,迟到的自动精炼结果
            // 丢弃;在飞的请求顺手取消,省几个 token。
            titles_.BumpGeneration();
            titles_.refiner().RequestCancel();
        },
        [this]() { SyncWorktreeDirectory(); },
        [this]() { CleanupBackgroundAgents(/*dispose_queue=*/true); },
        &worktree_session,
        wire_str,
        current_model,
        // /resume 成功:恢复的历史开新账(SessionStart source=resume)。
        [this]() {
            EmitSessionHook(lubancode::hooks::HookEvent::SessionStart, nlohmann::json{{"source", "resume"}},
                            "resume");
            OpenArtifactStore();
            // 持久目标单:goal 事件账随档恢复(默认 paused-on-resume)。
            goal_wiring_.RestoreFromArchive();
            // 两层标题(实测问题 7):换场善后——翻代、取消在飞的精炼。
            BackfillTitleOnResume();
        }};
    return state;
}

lubancode::agent::CompactOptions TerminalSessionController::BuildCompactOptions() {
    lubancode::agent::CompactOptions options;
    // 窗口预算认压缩路由自己的声明:高级段 model_roles 声明了 context_
    // window 就用它;没有再查模型目录条目;目录里也查不到(自定义模型、
    // 中转起名)就留空——Compact() 不做窗口拦截,但输出会明说"窗口未知,
    // 未校验",不假装核过。cheap 与 normal 窗口不同,分块按 cheap 的窗口
    // 算(规格"预算来源")。
    const auto compact_route = model_router->RouteInfo(lubancode::agent::TaskKind::Compact);
    if (compact_route.context_window.has_value()) {
        options.budget.window_tokens = compact_route.context_window;
    } else if (const auto* entry = model_catalog.FindBySlug(compact_route.model); entry != nullptr) {
        options.budget.window_tokens = entry->context_window_tokens;
    }
    // 预算总账(第四期):协议余量按统一公式算(协议头 + 估算误差边),
    // /context 展示的与 compact 拦截用的是同一本账,不再各写各的魔数。
    {
        lubancode::agent::ContextBudgetInputs budget_inputs;
        budget_inputs.window_tokens = options.budget.window_tokens;
        budget_inputs.requested_output_reserve_tokens = options.budget.output_reserve_tokens;
        budget_inputs.compact_prompt_overhead_tokens = 512;  // 压缩指令的公开估算档
        const auto plan = lubancode::agent::BuildContextBudgetPlan(budget_inputs);
        if (plan.compact_call_input_budget.has_value()) {
            options.budget.protocol_headroom_tokens =
                plan.protocol_headroom + plan.tokenizer_error_margin + plan.compact_prompt_overhead;
        }
    }
    // 活动待办守恒:pending/in_progress 条目原文逐条钉进 manifest 校验,
    // 摘要漏一项就拒收,旧历史不动。
    if (const auto& state = todo_state(); state != nullptr) {
        for (const auto& item : state->items) {
            if (item.status != lubancode::tools::TodoStatus::Completed && !item.content.empty()) {
                options.required_open_items.push_back(item.content);
            }
        }
    }
    // compact_partition_count(Compact 四分区单·阶段 1):compact 触发后切
    // 几份。配置层已在合并时校验 2..8;这里只管带下来。
    options.partition_count = static_cast<std::size_t>(config.compact_partition_count);
    return options;
}

// ---------------------------------------------------------------------------
// /telemetry enable session(端云协同可观测单 T2,§24.2):当前进程内开遥测。
// 只影响本场会话,不写配置文件;下场会话回到 features.telemetry 真值。
// 走 §8.2 同一道激活合同:trajectory 没开就明拒,不暗开。
// ---------------------------------------------------------------------------
std::vector<std::string> TerminalSessionController::EnableTelemetryForSession() {
    std::vector<std::string> lines;
    if (telemetry_service_ != nullptr) {
        lines.push_back("遥测已在本场会话开着,无需再开。");
        return lines;
    }
    auto* ledger = session_runtime_.trajectory();
    if (ledger == nullptr || session_runtime_.trajectory() == nullptr) {
        lines.push_back("开不了:本场会话的轨迹账未装配,遥测没有事实源。");
        return lines;
    }
    const lubancode::telemetry::TelemetryActivation activation =
        lubancode::telemetry::ResolveTelemetryActivation(true, true);
    if (!activation.enabled()) {
        lines.push_back("开不了: " + activation.reason_code + "(环境变量把遥测关死了,配置改不动它)");
        return lines;
    }
    std::string note;
    telemetry_service_ = AssembleTelemetryServiceForSession(
        config, /*config_telemetry=*/true, home_lubancode,
        session_runtime_.trajectory() != nullptr, ledger, &note);
    if (telemetry_service_ == nullptr) {
        lines.push_back("开不出: " + note);
        return lines;
    }
    const auto status = telemetry_service_->Status();
    lines.push_back("遥测已在本场会话开启(只本场;配置文件未动)。");
    lines.push_back("投影根目录: " +
                    lubancode::tools::PathToUtf8(telemetry_service_->options().telemetry_root));
    if (status.exporter.configured) {
        lines.push_back("出口: " + status.exporter.endpoint_display +
                        (status.exporter.gate_reason.empty()
                             ? std::string()
                             : (" [待办: " + status.exporter.gate_reason + "]")));
    } else {
        lines.push_back("出口: 未配置 endpoint,本地投影+spool,不出网。");
    }
    return lines;
}

}  // namespace lubancode::app
