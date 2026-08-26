// 终端接线收尾单:/memory 命令 presenter。原先整只 HandleMemoryCommand
// (387 行单函数)住在 interactive_session 大类里,按病灶二拆出:命令
// 产出数据行(project_memory 的调用与结果),presenter 负责怎么画(输出
// 全走 TerminalPort);大类只留分派(一行调用)与会话状态。
//
// ensure_tool:会话侧 EnsureMemoryTool 的接线——on/learn 档位打开后要把
// memory_save 工具补注册进 registry,这事归会话(工具栈在它那),命令层
// 只回调报一声。

#pragma once

#include "app/commands/command_flow.hpp"  // CommandFlow(分派注册制)
#include "cli/slash_commands.hpp"          // ParsedSlashCommand(分派注册制)

#include <expected>
#include <functional>
#include <string>

#include "agent/agent.hpp"        // Agent(批四自立门户)
#include "agent/artifact_store.hpp"  // ArtifactRef/ContextArtifactStore(按需摘要)
#include "agent/model_router.hpp"  // TaskKind/ModelRole

namespace lubancode::memory {
class ProjectMemory;
}
namespace lubancode::sessions {
class SessionStore;
}
namespace lubancode::cli {
struct Theme;
}

namespace lubancode::app {

class ModelRouterService;

struct MemoryCommandContext {
    lubancode::memory::ProjectMemory* project_memory = nullptr;  // 空 = 未装配(unavailable)
    const lubancode::cli::Theme* theme = nullptr;
    // on/learn 成功后回调(补注册 memory_save;可空 = 单测/无 registry 场景)。
    std::function<void()> ensure_tool;
};

// /memory 的全部动作:status/on/off/use/learn/review/accept/reject/edit/
// why/list/remember/forget/rebuild/stale/verify/refresh/show/open/migrate。
// 用法不对打用法;project_memory 未装配打 unavailable。
void HandleMemoryCommand(const MemoryCommandContext& ctx, const std::string& raw_args);

// ---- 会话尾款的 memory 接线(终端接线收尾单自大类搬出) --------------------
//
// 回合收尾的记忆抽取、artifact 按需摘要、cheap 起名——三样都是"回合外面
// 的后台小活",原先住在大类里,搬到这里;材料经 SessionTailContext 递入。
struct SessionTailContext {
    lubancode::memory::ProjectMemory* project_memory = nullptr;
    lubancode::agent::Agent* agent = nullptr;          // 活 loop(history 与路由)
    lubancode::app::ModelRouterService* model_router = nullptr;
    const std::string* prompts_dir = nullptr;          // 抽取系统提示的运行时模块
    lubancode::agent::ContextArtifactStore* artifact_store = nullptr;  // 可空
    lubancode::sessions::SessionStore* session_store = nullptr;
    const lubancode::cli::Theme* theme = nullptr;
    // 会话标题账(runtime 那份的引用,MaybeGenerateSessionTitle 读写)。
    std::string* session_title = nullptr;
    const bool* session_title_pending = nullptr;
    bool* session_title_auto_attempted = nullptr;
};

// 回合收尾抽取:只看本轮增量,借 cheap 路由产严格 JSON;候选进待审区
// (auto 档且证据齐的直写),检索扩展词留给下一轮召回。失败降级一行字,
// 不影响主会话,也不重试。
void ExtractTurnMemory(const SessionTailContext& ctx, const std::string& user_text, std::size_t history_before);

// context_read(summarize=true) 的按需摘要:独占 cheap backend 读 artifact
// 真本,返回给工具回执追加在历史尾部。
std::expected<std::string, std::string> SummarizeArtifactOnDemand(const SessionTailContext& ctx,
                                                                  const lubancode::agent::ArtifactRef& ref);

// 会话起名(cheap 角色):新会话首轮收尾或 resume 进来一场没标题的旧档时,
// 拿开头几条消息起一枚短标题,成功落 title 事件;失败安静降级。一场只试
// 一次,不追着重试。
void MaybeGenerateSessionTitle(const SessionTailContext& ctx, lubancode::agent::TaskKind kind);

// 命令分派注册制(会话终章):/memory 的分派位(case 体原样搬自大 switch)。
struct SlashDispatchContext;
CommandFlow HandleSlashMemory(SlashDispatchContext& ctx, const lubancode::cli::ParsedSlashCommand& parsed);

}  // namespace lubancode::app
