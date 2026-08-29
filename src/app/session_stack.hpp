// 组合根的会话装配件(会话终章):TerminalSessionController 构造函数里
// 的装配(提示词材料、后端栈、工具全栈、配置接线)拆到组合根——cli_app
// 调 BuildSessionStack 拿装好的件,控制器只收。装配次序与输出(横幅/
// [mcp] 挂载行/目录应用)与原先的构造函数逐字对齐,行为零变。
//
// 寿命规矩随行搬来:worktree_session 先于 ToolRuntime(worktree 工具持它
// 的引用);ToolRuntime 的三表先于控制器里的 AgentLoop;晚绑定槽
//(after_worktree_moved/summarize_artifact)由控制器在装配尾填——工厂
// 起线程前才拷材料的老规矩不变。
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agent/artifact_store.hpp"
#include "app/backend_stack.hpp"
#include "app/interactive_session.hpp"  // InteractiveSessionOptions
#include "app/model_router.hpp"
#include "app/tool_runtime.hpp"
#include "cli/context_tracker.hpp"
#include "cli/spinner_backend.hpp"
#include "cli/worktree.hpp"
#include "config/config.hpp"
#include "memory/project_memory.hpp"
#include "package/mounting.hpp"  // PackageMount:会话钉快照(阶段 3)
#include "tools/skill_loader.hpp"  // SkillMeta

namespace lubancode::app {

// 装好的整束件。控制器持引用用,不重复造;可变的会话皮(current_model/
// think/soul/active_provider)也在里头——模型分工的路由器与 detached
// 工厂都要读它们,单一真值只有这一份。
struct SessionStack {
    // ---- 提示词与记忆材料 ----
    lubancode::config::ConfigResult config_result;  // 会话内配置真值(唯一一份)
    const std::optional<std::string> home_dir;
    const std::optional<std::string> official_skills_dir;
    // Package 会话钉快照(统一封装单阶段 3):启动扫一次,运行中不热生效。
    // 声明在 skills 之前——挂载材料先于技能清单装配(两趟:先裸扫 standalone
    // 技能喂包外短引用账,再带包根合出正式清单)。
    const lubancode::package::PackageMount package_mount;
    std::vector<lubancode::tools::SkillMeta> skills;  // /skills 展示与 agent 工具段
    std::string skills_segment;
    const std::optional<std::string> home_lubancode;
    const std::string prompts_dir;
    std::shared_ptr<lubancode::memory::ProjectMemory> project_memory;
    std::string project_instructions;  // /init、AGENTS.md 改动后可刷新
    const std::filesystem::path global_skills_root;
    const std::filesystem::path project_skills_root;

    // ---- 后端栈(骨架拆解批四:五层请求改写后端退役,栈里只剩真实
    // client 的稳定壳与 spinner)----
    RebuildableBackend real_backend;
    std::shared_ptr<std::string> current_model;
    std::shared_ptr<std::string> current_think;
    std::shared_ptr<std::string> current_model_instructions;
    std::string current_soul_name;
    std::shared_ptr<std::string> current_soul;
    std::string active_provider;
    // 渐进式上下文仓:会话建档那一刻才 Open,没开的仓一切操作安全退化。
    std::shared_ptr<lubancode::agent::ContextArtifactStore> artifact_store;
    lubancode::cli::SpinnerBackend wrapped_backend;
    lubancode::cli::ContextTracker context_tracker;
    // 统一模型路由:compact/记忆抽取/标题这类后台小活按 TaskKind 取路由。
    std::unique_ptr<lubancode::app::ModelRouterService> model_router;

    // ---- 工具全栈 ----
    lubancode::cli::WorktreeSession worktree_session;
    std::optional<lubancode::app::ToolRuntime> tool_runtime;
    bool main_deferral = false;
    bool sub_deferral = false;
    int tool_search_threshold = 0;
    // 后台任务 detached registry 的注册时点快照(不追 /skill 安装)。
    const std::vector<lubancode::tools::SkillMeta> detached_skills;
    const lubancode::config::SearchConfig detached_search;
    // 项目配置若显式钉了 active_provider,后续切换继续写回项目;没钉就记
    // 全局"上次使用"。
    const std::optional<std::string> active_provider_write_path;

    // ---- 晚绑定槽(会话控制器装配尾填)----
    // worktree 工具 enter/exit 的善后(目录同步)。
    std::function<void()> after_worktree_moved;
    // main 侧 context_read 的按需摘要(cheap token,会话尾款接线)。
    std::function<std::string(const lubancode::agent::ArtifactRef&)> summarize_artifact;

    // ---- 窄口(ToolRuntime 在构造体内 emplace,统一走这几个)----
    lubancode::tools::ToolRegistry& registry();
    lubancode::tools::ToolRegistry& sub_registry();
    lubancode::tools::AgentTool* agent_tool();
    const std::shared_ptr<lubancode::tools::TodoListState>& todo_state();
    const std::shared_ptr<std::set<std::string>>& loaded_tools();
    const std::vector<McpServerRuntime>& mcp_servers();
    std::optional<lubancode::lsp::Manager>& lsp_manager();
    const std::vector<PluginMountInfo>& plugin_mounted();
    const std::vector<std::string>& plugin_warnings();
    const std::function<bool(const lubancode::tools::Tool&)>& main_tool_filter();
    const std::function<bool(const lubancode::tools::Tool&)>& sub_tool_filter();

    // 后台子代理的 detached 装配(每个任务各造一份 client 与基础工具表)。
    lubancode::tools::DetachedAgentBackend BuildDetachedBackend() const;
    std::unique_ptr<lubancode::tools::ToolRegistry> BuildDetachedRegistry() const;

    // 构造 = 原控制器初始化列表的装配(成员声明序即装配序)。
    explicit SessionStack(const InteractiveSessionOptions& options);
};

// 装配本体(组合根):原控制器构造函数的装配段逐字搬来。输出次序与原先
// 一致(模型路由提示 → 图标/横幅 → 目录应用 → stream_usage 提醒 → 陈房
// 清扫 → [mcp] 挂载行 → 延迟索引提示)。
std::unique_ptr<SessionStack> BuildSessionStack(const InteractiveSessionOptions& options);

}  // namespace lubancode::app
