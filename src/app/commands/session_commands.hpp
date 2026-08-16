// 会话类 slash 命令:/sessions 列档、/resume 恢复、/export 导出 Markdown。
// 底层读写在 agent/session_store,这里只管选择交互与输出拼装。
//
// 搬家自 main.cpp,行为一字未改;依赖只认 agent/cli/platform。


#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "agent/compact.hpp"
#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "api/backend.hpp"
#include "app/commands/command_flow.hpp"
#include "config/config.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "cli/worktree.hpp"
#include "platform/paths.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

// 粗略估算一段历史占用了多少"token"——统一口径在 agent/context.hpp
// (ASCII 4 字符 1 token,非 ASCII 1.5 token/字);这里只是转发,老调用点
// (/context 分类明细)不必各认一遍。数字带 ~ 提醒是估算值,真实用量以
// provider usage 为准。
std::size_t EstimateHistoryChars(const std::vector<lubancode::api::Message>& history);

std::size_t EstimateHistoryTokens(const std::vector<lubancode::api::Message>& history);


// /context 命令:不带参数打分类占用分析(系统提示/工具定义/对话历史三类
// 统一口径 token 估算 + 条形图,拼装规则全在 FormatContextBreakdown,这里
// 只管收集与打印);带参数(256k/512k/1m/裸数字)临时改窗口大小,只本会话
// 生效,不改配置文件。sys_tokens/tools_tokens/history_tokens 由调用方在会话
// 现场按统一口径(agent/context.hpp)算好(裸敲才用得上,带参数分支忽略),
// 缓存命中/窗口/实测占用都从 context_tracker 拿。cache_epoch 是 loop 的
// 前缀记账序号(agent/prefix.hpp),有实测 usage 时多打一行前缀缓存账。
// main_profile(可空):当前 loop 实际吃到的运行策略,非空时多打一行输出
// 上限与来源(规格根因一:"本轮上限"看得见,unset 也说破)。
void HandleContextCommand(const std::string& args, lubancode::cli::ContextTracker& context_tracker,
                           std::size_t sys_tokens, std::size_t tools_tokens, std::size_t history_tokens,
                           const lubancode::cli::Theme& theme, int cache_epoch = 1,
                           const lubancode::agent::AgentRuntimeProfile* main_profile = nullptr);


// /compact 命令的结果:event 是 compact_v2 压缩事件(archive + kept_from +
// manifest + metrics),成功时调用方追加写进存档流水,/resume 才能回放出
// 压缩后的活状态;失败/没得压给 nullopt。before/after tokens 用统一估算
// 口径;manifest_* 是摘要 manifest 里守住的目标数,供成功提示带一句
// "保留了几条约束/待办"。
struct CompactCommandResult {
    std::optional<lubancode::agent::CompactV2Event> event;
    std::size_t before_tokens = 0;
    std::size_t after_tokens = 0;
    std::size_t manifest_constraints = 0;
    std::size_t manifest_open_items = 0;
};

// /compact 命令:分层压缩(装得下单次摘要;装不下按 episode 分块 map、
// 归并 reduce),顶替掉中间那段老对话,热区按 token 预算保留。backend 传
// 裸的、没包 ModelOverrideBackend 的那份——CompactHierarchical() 会自己把
// compact_model 写进 request.model。args 是 /compact 的重点文本(或
// --dry-run);compact_epoch 进出两头用:进 = 本场已压过几次,出 = 本次
// 压完的序号(写进 v2 事件);options 携带窗口预算与必须守恒的待办。
// 压缩模型窗口装不下(分块也救不了)时明确拒绝、不静默截史;manifest
// 校验不过同样旧 history 不动。
CompactCommandResult HandleCompactCommand(const std::string& args, lubancode::agent::AgentLoop& loop,
                                          lubancode::api::Backend& raw_backend, const std::string& compact_model,
                                          const lubancode::cli::Theme& theme, bool spinner_enabled,
                                          const lubancode::agent::CompactOptions& options, int& compact_epoch);

void PrintSessionsCommand(const std::string& sessions_dir, const std::string& args);


// /resume 裸敲:本目录最近 20 场直接做成方向键菜单。显式编号/id 仍由
// ResumeSession 解析，脚本和熟手用法不变。
std::optional<std::string> PromptResumeTarget(const std::string& sessions_dir,
                                              const lubancode::cli::Theme& theme);


// /resume <编号或id> 和 --continue 共用的执行逻辑。target 是编号(按
// ListSessions 的倒序编号)、会话 id、或空串(--continue:最近一场)。
// 编号和"最近一场"都只在**本目录**(meta.cwd == 当前 cwd)的场子里数;
// 直接给 id 的仍然全局能找(拼路径兜底),跨目录恢复留了这条明路。
// 成功:回放事件 + 成对修补 + ReplaceHistory + 接管文件继续追加,返回 true;
// session_title 同步成存档里最后一条 title 事件(没有就清空)。
// quiet_if_none:--continue 本目录找不到任何存档时不报错、安静开新会话。
// worktree_session(0.27.x,可空):会话档 meta.cwd(含 cwd 事件回放)若指向
// 一间还在的 worktree 房,验明正身后把会话搬回去;房没了回落启动目录并
// 说一声;马甲房(.git 指回主仓那类)拒进并报原因。非空时成功恢复后由
// 调用方做一次目录善后同步(重拼系统提示那条路)。
bool ResumeSession(const std::string& target, const std::string& sessions_dir,
                    lubancode::agent::AgentLoop& loop, lubancode::agent::SessionStore& store,
                    std::size_t& persisted_count, lubancode::agent::SessionMeta& session_meta,
                    std::string& session_title, const std::string& wire_str, const std::string& current_model,
                    const lubancode::cli::Theme& theme, bool quiet_if_none,
                    lubancode::cli::WorktreeSession* worktree_session = nullptr,
                    int* compact_epoch_out = nullptr);


// /export [路径]:当前会话导出 Markdown,默认写 sessions/<id>.md。
// 有存档文件就从文件读**全量流水**导出(压缩不丢内容,发生点插一行标注);
// 没有存档文件(没落过盘)退回导内存里这份历史。/title 设过的标题当大标题。
void HandleExportCommand(const std::string& args, const lubancode::agent::AgentLoop& loop,
                          const lubancode::agent::SessionStore& store, const std::string& sessions_dir,
                          const lubancode::agent::SessionMeta& session_meta, const std::string& session_title);


// ---------------------------------------------------------------------------
// 会话命令的窄状态:handler 只借引用干活,不拥有会话资源。
// ---------------------------------------------------------------------------

// /clear /title /resume 共用的会话侧状态。全部借用:会话
// (InteractiveSession)在命令执行期间保证存活。
struct SessionCommandState {
    std::function<void(bool)> rebuild_loop;  // /clear 传 false = 丢历史重建
    lubancode::agent::AgentLoop& loop;        // /compact /resume /export 用
    lubancode::agent::SessionStore& store;
    std::size_t& persisted_count;             // 落盘基线
    int& compact_epoch;                       // 压缩序号(/resume 接旧账,/compact 进出两头用)
    lubancode::agent::SessionMeta& meta;
    std::string& title;
    bool& title_pending;
    bool& store_broken;
    std::string& start_ts;                   // /clear 翻新会话 id 时间戳
    std::function<void()> on_session_restarted;  // /clear 善后(project memory 源)
    std::function<void(const std::string&)> on_title_changed;  // peer 名册改名,可空
    std::function<void()> sync_worktree_directory;  // resume 搬房后的善后
    // /clear 的子代理清场(0.28.x):停掉全部后台任务、把未送达的介入消息
    // 报给人看——面板规格"清场不能无声遗失"的一条。可空(单测不接)。
    std::function<void()> on_agents_cleanup;
    lubancode::cli::WorktreeSession* worktree_session = nullptr;  // 可空
    const std::string& sessions_dir;
    const std::string& wire_str;
    const std::shared_ptr<std::string>& current_model;
    // hooks 框架:/resume 成功后回调(SessionStart source=resume 的发射口,
    // 会话层接)。可空(单测不接)。
    std::function<void()> on_resumed;
};

// /clear:丢历史重建、存档翻篇、标题翻篇。
CommandFlow HandleClearCommand(SessionCommandState& state, const lubancode::config::Config& config,
                               const lubancode::cli::Theme& theme, bool spinner_enabled);

// /title [标题]:看/设标题;建档前设的先挂起,建档成功由落盘路径补写事件行。
CommandFlow HandleTitleCommand(SessionCommandState& state, const std::string& args,
                               const lubancode::cli::Theme& theme);

// /resume [编号或id]:裸敲弹最近 20 场菜单;成功后接管存档继续追加。
CommandFlow HandleResumeCommand(SessionCommandState& state, const std::string& args,
                                const lubancode::cli::Theme& theme);

}  // namespace lubancode::app
