// 会话类 slash 命令:/sessions 列档、/resume 恢复、/export 导出 Markdown。
// 底层读写在 agent/session_store,这里只管选择交互与输出拼装。
//
// 搬家自 main.cpp,行为一字未改;依赖只认 agent/cli/platform。


#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "agent/session_store.hpp"
#include "cli/console_input.hpp"
#include "cli/context_tracker.hpp"
#include "cli/i18n.hpp"
#include "cli/theme.hpp"
#include "cli/worktree.hpp"
#include "platform/paths.hpp"

namespace lubancode::app {

using lubancode::cli::tr;
using lubancode::cli::trf;

// 粗略估算一段历史占用了多少"token"——不真调分词器,按字符数打个折扣
// (中英文混排,经验上大致两个字符算一个 token),仅供 /compact 报告
// "压缩前后省了多少"用,数字前带 ~ 提醒这是估算值,不是真实用量(真实
// 用量要靠 usage.input_tokens,那个得等实际发一次请求才知道)。
std::size_t EstimateHistoryChars(const std::vector<lubancode::api::Message>& history);

std::size_t EstimateTokens(std::size_t chars);


// /context 命令:不带参数打分类占用分析(系统提示/工具定义/对话历史三类
// 字符数估 token + 条形图,拼装规则全在 FormatContextBreakdown,这里只管
// 收集与打印);带参数(256k/512k/1m/裸数字)临时改窗口大小,只本会话
// 生效,不改配置文件。sys_chars/tools_chars/history_chars 由调用方在会话
// 现场收集(裸敲才用得上,带参数分支忽略),缓存命中/窗口/实测占用都从
// context_tracker 拿。
void HandleContextCommand(const std::string& args, lubancode::cli::ContextTracker& context_tracker,
                           std::size_t sys_chars, std::size_t tools_chars, std::size_t history_chars,
                           const lubancode::cli::Theme& theme);


// /compact 命令:把当前历史整段发给模型换一份压缩存档,顶替掉中间那段
// 老对话,只留 archive + 最近一轮完整对话。backend 传裸的、没包
// ModelOverrideBackend 的那份——Compact() 会自己把 compact_model 写进
// request.model,要是走了 ModelOverrideBackend,会被强制换回当前会话
// model,压缩模型这个字段就形同虚设了。
// 压缩成功时返回对应的 compact 事件(archive + kept_from),调用方追加写进
// 存档流水,/resume 才能回放出压缩后的活状态;失败/没得压给 nullopt。
std::optional<lubancode::agent::CompactEvent> HandleCompactCommand(
    const std::string& args, lubancode::agent::AgentLoop& loop, lubancode::api::Backend& raw_backend,
    const std::string& compact_model, const lubancode::cli::Theme& theme, bool spinner_enabled);

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
                    lubancode::cli::WorktreeSession* worktree_session = nullptr);


// /export [路径]:当前会话导出 Markdown,默认写 sessions/<id>.md。
// 有存档文件就从文件读**全量流水**导出(压缩不丢内容,发生点插一行标注);
// 没有存档文件(没落过盘)退回导内存里这份历史。/title 设过的标题当大标题。
void HandleExportCommand(const std::string& args, const lubancode::agent::AgentLoop& loop,
                          const lubancode::agent::SessionStore& store, const std::string& sessions_dir,
                          const lubancode::agent::SessionMeta& session_meta, const std::string& session_title);

}  // namespace lubancode::app
