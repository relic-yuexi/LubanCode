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
#include "api/reasoning.hpp"  // ReasoningHistoryMode:current_think_history 的档
#include "app/backend_stack.hpp"
#include "app/interactive_session.hpp"  // InteractiveSessionOptions
#include "app/model_router.hpp"
#include "app/tool_runtime.hpp"
#include "cli/context_tracker.hpp"
#include "cli/spinner_backend.hpp"
#include "runtime/worktree.hpp"
#include "config/config.hpp"
#include "config/project_instructions.hpp"  // ProjectInstructionResolver:AGENTS.md 作用域解析(共用一份)
#include "memory/project_memory.hpp"
#include "package/mounting.hpp"  // PackageSnapshot:会话钉快照(阶段 3/6)
#include "tools/instruction_scope.hpp"  // InstructionScopeState:主 Agent 的已见指纹账
#include "tools/skill_loader.hpp"  // SkillMeta

#include <atomic>
#include <mutex>

namespace lubancode::app {

// 装好的整束件。控制器持引用用,不重复造;可变的会话皮(current_model/
// think/soul/active_provider)也在里头——模型分工的路由器与 detached
// 工厂都要读它们,单一真值只有这一份。
// Package 快照槽:互斥锁护一枚 shared_ptr。读写都短(拷指针还引用计数),
// 几纳秒的锁换三平台通吃——libc++ 对 std::atomic<shared_ptr> 有
// trivially-copyable 硬规(Xcode 26 的 libc++ 拒编),MSVC/GCC 当扩展放行,
// 换档与读档的互斥语义用锁实现一样成立。
class PackageSnapshotSlot {
public:
    PackageSnapshotSlot() = default;
    explicit PackageSnapshotSlot(std::shared_ptr<const lubancode::package::PackageSnapshot> snapshot)
        : snapshot_(std::move(snapshot)) {}
    std::shared_ptr<const lubancode::package::PackageSnapshot> load() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }
    void store(std::shared_ptr<const lubancode::package::PackageSnapshot> snapshot) {
        const std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = std::move(snapshot);
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<const lubancode::package::PackageSnapshot> snapshot_;
};

struct SessionStack {
    // ---- 提示词与记忆材料 ----
    lubancode::config::ConfigResult config_result;  // 会话内配置真值(唯一一份)
    const std::optional<std::string> home_dir;
    const std::optional<std::string> official_skills_dir;
    // Package 会话钉快照(统一封装单阶段 3;阶段 6 升成显式 PackageSnapshot
    // 对象):启动折一份(第 1 折),/package reload 才换新折。声明在 skills
    // 之前——挂载材料先于技能清单装配(两趟:先裸扫 standalone 技能喂包外
    // 短引用账,再带包根合出正式清单)。
    // 快照槽:workflow 并行支线从工作线程经 agent 解析口折材料,换档与
    // 读档不得互踩;快照本身不可变,在跑引用各自钉 shared_ptr 拷贝,照旧
    // 跑完——reload 换档影响的是下一次装配,不是在跑的那批。
    PackageSnapshotSlot package_snapshot;
    std::vector<lubancode::tools::SkillMeta> skills;  // /skills 展示与 agent 工具段
    std::string skills_segment;
    const std::optional<std::string> home_lubancode;
    const std::string prompts_dir;
    std::shared_ptr<lubancode::memory::ProjectMemory> project_memory;
    // ---- AGENTS.md 作用域(作用域单 P0)----
    // resolver 全会话一份,主代理/AgentTool/Workflow 三路共用(§7.6);
    // 主 Agent 的已见指纹账自持一份。声明在 project_instructions 之前:
    // 那截字符串从 resolver 的 baseline 链投影而来(构造序保证)。
    std::shared_ptr<const lubancode::config::ProjectInstructionResolver> instruction_resolver;
    std::shared_ptr<lubancode::tools::InstructionScopeState> instruction_scope_state;
    std::string project_instructions;  // /init、AGENTS.md 改动后可刷新
    // AGENTS.md 逐 source 账(作用域单 P1-2):baseline 链上每份文档的
    // UTF-8 路径,与上面那截拼接串同源同刷;喂给 PromptOptions 的
    // project_instruction_sources,PromptSourceLedger 每份文档记一行。
    std::vector<std::string> project_instruction_sources;
    const std::filesystem::path global_skills_root;
    const std::filesystem::path project_skills_root;

    // ---- 后端栈(骨架拆解批四:五层请求改写后端退役,栈里只剩真实
    // client 的稳定壳与 spinner)----
    RebuildableBackend real_backend;
    std::shared_ptr<std::string> current_model;
    std::shared_ptr<std::string> current_think;
    // 跨轮保留式思考的会话选择(Kimi 保留式思考单 P1):/think history
    // 切换,/resume 恢复,切模型重校验。单一真值与 current_think 同住一排。
    std::shared_ptr<lubancode::api::ReasoningHistoryMode> current_think_history;
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
    // 动态工具 P1:延迟工具模式(proxy 路开没开看 main_proxy/sub_proxy)。
    bool main_proxy = false;
    bool sub_proxy = false;
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
    // 嵌套后台孩子的冻结后端工厂(P0-3"派出时冻结 execution snapshot"):
    // 每只后台任务起跑当口调一次,返回的闭包拷值定格当刻的 model/think/
    // 指令/魂——它造的 client 不跟会话活账走,在跑的任务树中途 /model
    // 换档不受影响。config 一次定格共享(会话期不重读)。
    std::function<lubancode::tools::DetachedAgentBackend()> BuildFrozenBackendSpawner() const;

    // BuildFrozenBackendSpawner 用的配置定格份(会话期不变;与
    // BuildDetachedBackend 的活读各走各的,互不影响)。
    std::shared_ptr<const lubancode::config::Config> frozen_backend_config;

    // 构造 = 原控制器初始化列表的装配(成员声明序即装配序)。
    explicit SessionStack(const InteractiveSessionOptions& options);

    // 现行快照(拷一份 shared_ptr;reload 换档后取到的即新折)。
    std::shared_ptr<const lubancode::package::PackageSnapshot> CurrentPackageSnapshot() const {
        return package_snapshot.load();
    }
};

// 装配本体(组合根):原控制器构造函数的装配段逐字搬来。输出次序与原先
// 一致(模型路由提示 → 图标/横幅 → 目录应用 → stream_usage 提醒 → 陈房
// 清扫 → [mcp] 挂载行 → 延迟索引提示)。
std::unique_ptr<SessionStack> BuildSessionStack(const InteractiveSessionOptions& options);

// Package 会话钉快照的 evolution store 并轨(自进化闭环阶段 4):把
// package-store 里选中版本(active/canary 指针指到的那枚)折成的现成候选
// 递进挂载输入;哈希验完好的才递(store 内文件被手改的拒挂,警告亮出并
// 指路)。交互会话与单发模式同一枚——新会话拿新选中,在跑会话钉着自己的
// 快照照旧跑完。
void AddEvolutionStoreSelections(lubancode::package::PackageMountInput& input);

// 折一份会话装包输入(统一封装单阶段 3/6):四层扫描根 + 包外短引用的
// 兜底账 + evolution store 选中版本 + 启停账(现读
// ~/.lubancode/package-state.json)。信任账由调用方钉好递进来——启动读一次,reload
// 复用同一份(code 门禁会话启动定终身,契约 §7.1)。warnings 非空时收
// "启停账读不动"一类非致命账,调用方决定亮到哪(启动直打,reload 并进
// 回执)。交互会话与单发同一只——两处从前各写一份,现并成一处口径。
lubancode::package::PackageMountInput BuildSessionPackageMountInput(
    const lubancode::config::Config& config, const std::vector<std::string>& package_dirs,
    const lubancode::package::PackageTrustSnapshot& pinned_trust,
    std::vector<std::string>* warnings = nullptr);

}  // namespace lubancode::app
