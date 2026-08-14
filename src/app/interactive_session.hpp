// 交互会话主循环:整场可变状态、slash 分派、排队与跨会话消息、轮次
// 发送与存档落盘,原先全住在 main.cpp 的 InteractiveLoop 里。实现已整段
// 搬进 interactive_session.cpp(编译边界);把大 lambda 收成方法、局部
// 状态收成成员的收敛在后续 commit 里一类一类做。
//
// 依赖只认 agent/api/cli/config/memory/mcp/lsp/tools/platform 与 app
// 装配层;不反被任何层 include。

#pragma once

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
#include <memory>
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
#include "agent/compact.hpp"
#include "agent/loop.hpp"
#include "agent/peer_session.hpp"
#include "agent/prompts.hpp"
#include "agent/session_store.hpp"
#include "agent/workflow_recorder.hpp"
#include "api/anthropic/client.hpp"
#include "api/backend.hpp"
#include "api/chat/client.hpp"
#include "api/models.hpp"
#include "api/responses/client.hpp"
#include "app/backend_stack.hpp"
#include "app/tool_runtime.hpp"
#include "app/turn_runner.hpp"
#include "app/commands/session_commands.hpp"
#include "app/commands/prompt_commands.hpp"
#include "app/commands/settings_commands.hpp"
#include "app/commands/workspace_commands.hpp"
#include "app/version.hpp"
#include "cli/agent_status.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/diff.hpp"
#include "cli/divider.hpp"
#include "cli/format_utils.hpp"
#include "cli/i18n.hpp"
#include "cli/image_input.hpp"
#include "cli/live_transcript.hpp"
#include "cli/worktree.hpp"
#include "cli/markdown.hpp"
#include "cli/provider_wizard.hpp"
#include "cli/record_command.hpp"
#include "cli/setup_wizard.hpp"
#include "cli/slash_commands.hpp"
#include "cli/spinner.hpp"
#include "cli/terminal_frame.hpp"
#include "cli/theme.hpp"
#include "cli/todo_render.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "config/provider_catalog.hpp"
#include "config/prompt_files.hpp"
#include "config/project_instructions.hpp"
#include "config/skill_store.hpp"
#include "config/update_checker.hpp"
#include "lsp/manager.hpp"
#include "memory/memory_tool.hpp"
#include "memory/project_memory.hpp"
#include "mcp/client.hpp"
#include "mcp/mcp_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/ask_user.hpp"
#include "tools/background_output.hpp"
#include "tools/background_tasks.hpp"
#include "tools/command_safety.hpp"
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

namespace lubancode::app {

// 没带参数时的交互循环:读一行、问一句,exit/quit 或 EOF 退出。
// 空行不退出——只是重新给一次提示符,继续等下一行(老规则"空行退出"跟
// Windows 控制台偶发读空串的老毛病撞在一块,会把读空串误当成用户要退出,
// 改成只认 exit/quit/EOF 才靠谱)。
// AgentLoop 用 std::optional 包着,存一份在循环外面,历史跨轮保留;
// /clear 需要清空历史,而 AgentLoop 的历史是私有成员、没有 Clear()(agent
// 层现有文件不让动),唯一的办法是就地重新构造一份全新的 AgentLoop——
// optional::emplace 走的是构造而不是赋值,AgentLoop 引用成员导致的
// "不可赋值"不影响这条路。
// always_allowed_tools 同样在循环外面建一次,"本会话总是允许"才能真的跨
// 多轮用户输入生效,/clear 不清空它(清没清对话历史跟工具授权是两码事)。
// config_result.config.model 是四级合并出来的初始 model,current_model 是
// 真正"这一刻发请求用哪个 model"的会话级状态,两者可能因为 /model 切换而
// 不一致——ModelOverrideBackend 在真正发请求前把 Request.model 换成
// *current_model,这样 /model 切换才能不碰 agent/loop.hpp/.cpp 就真正生效。
// continue_last:--continue 启动参数,进循环前先自动 /resume 最近一场;
// 一场存档都没有就正常开新会话,不报错。
// law_source:魂法分家(0.16.x)新增,启动时算好的"法从哪儿来"说明
// (CLI 参数/文件/内置),/prompt 裸敲展示用,不参与任何逻辑。
void InteractiveLoop(lubancode::config::ConfigResult config_result, bool auto_confirm,
                      const lubancode::cli::Theme& theme, const std::string& persona, bool spinner_enabled,
                      const lubancode::config::ModelCatalog& model_catalog,
                      const lubancode::config::SettingsLocal& settings_local, bool continue_last = false,
                      const std::string& law_source = "内置默认",
                      const std::string& executable = std::string());

}  // namespace lubancode::app
