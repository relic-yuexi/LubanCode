// 会话类 slash 命令:/sessions 列档、/resume 恢复、/export 导出 Markdown。
// 底层读写在 agent/session_store,这里只管选择交互与输出拼装。
//
// 搬家自 main.cpp,行为一字未改;依赖只认 agent/cli/platform。

#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/spinner.hpp"
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
std::size_t EstimateHistoryChars(const std::vector<lubancode::api::Message>& history) {
    std::size_t total = 0;
    for (const auto& message : history) {
        for (const auto& block : message.content) {
            std::visit(
                [&total](const auto& b) {
                    using T = std::decay_t<decltype(b)>;
                    if constexpr (std::is_same_v<T, lubancode::api::TextBlock>) {
                        total += b.text.size();
                    } else if constexpr (std::is_same_v<T, lubancode::api::ImageBlock>) {
                        total += b.media_type.size() + b.data.size() + b.filename.size();
                    } else if constexpr (std::is_same_v<T, lubancode::api::ToolUseBlock>) {
                        total += b.name.size() + b.input.dump().size();
                    } else if constexpr (std::is_same_v<T, lubancode::api::ToolResultBlock>) {
                        total += b.content.size();
                    }
                },
                block);
        }
    }
    return total;
}

std::size_t EstimateTokens(std::size_t chars) { return (chars + 1) / 2; }

// /context 命令:不带参数打分类占用分析(系统提示/工具定义/对话历史三类
// 字符数估 token + 条形图,拼装规则全在 FormatContextBreakdown,这里只管
// 收集与打印);带参数(256k/512k/1m/裸数字)临时改窗口大小,只本会话
// 生效,不改配置文件。sys_chars/tools_chars/history_chars 由调用方在会话
// 现场收集(裸敲才用得上,带参数分支忽略),缓存命中/窗口/实测占用都从
// context_tracker 拿。
void HandleContextCommand(const std::string& args, lubancode::cli::ContextTracker& context_tracker,
                           std::size_t sys_chars, std::size_t tools_chars, std::size_t history_chars,
                           const lubancode::cli::Theme& theme) {
    if (args.empty()) {
        const auto lines = lubancode::cli::FormatContextBreakdown(
            sys_chars, tools_chars, history_chars, context_tracker.last_cache_read_tokens(),
            context_tracker.window_tokens(), context_tracker.current_tokens(), theme);
        for (const auto& line : lines) {
            std::cout << line << "\n";
        }
        // 口径说明:状态栏与这里读的是同一只 tracker,都是"最近一次主请求
        // 的占用",不是会话累计花销,也不含独立子代理的 token。最近一次请求
        // 没带回 usage 时再补一行旧值提醒(状态栏同款 ~ 前缀的完整说法)。
        std::cout << tr("cmd.context.note.semantics") << "\n";
        if (context_tracker.usage_stale()) {
            std::cout << tr("cmd.context.note.stale") << "\n";
        }
        if (context_tracker.ShouldAutoCompact()) {
            std::cout << tr("cmd.context.compact_hint") << "\n";
        }
        return;
    }
    const auto parsed = lubancode::config::ParseContextWindowTokens(args);
    if (!parsed.has_value()) {
        std::cout << parsed.error() << "\n";
        return;
    }
    context_tracker.set_window_tokens(*parsed);
    std::cout << trf("cmd.context.window_changed", *parsed) << "\n";
}

// /compact 命令:把当前历史整段发给模型换一份压缩存档,顶替掉中间那段
// 老对话,只留 archive + 最近一轮完整对话。backend 传裸的、没包
// ModelOverrideBackend 的那份——Compact() 会自己把 compact_model 写进
// request.model,要是走了 ModelOverrideBackend,会被强制换回当前会话
// model,压缩模型这个字段就形同虚设了。
// 压缩成功时返回对应的 compact 事件(archive + kept_from),调用方追加写进
// 存档流水,/resume 才能回放出压缩后的活状态;失败/没得压给 nullopt。
std::optional<lubancode::agent::CompactEvent> HandleCompactCommand(
    const std::string& args, lubancode::agent::AgentLoop& loop, lubancode::api::Backend& raw_backend,
    const std::string& compact_model, const lubancode::cli::Theme& theme, bool spinner_enabled) {
    const std::vector<lubancode::api::Message>& history = loop.History();
    if (history.empty()) {
        std::cout << tr("cmd.compact.empty") << "\n";
        return std::nullopt;
    }
    const std::size_t before_tokens = EstimateTokens(EstimateHistoryChars(history));
    const std::size_t old_size = history.size();

    lubancode::cli::Spinner spinner(theme, spinner_enabled);
    const auto result = lubancode::agent::Compact(raw_backend, compact_model, history, args);
    spinner.Stop();

    if (!result.has_value()) {
        std::cout << theme.error << trf("cmd.compact.failed", result.error().message) << theme.reset << "\n";
        return std::nullopt;
    }

    const auto new_history = lubancode::agent::BuildCompactedHistory(history, *result);
    const auto event = lubancode::agent::MakeCompactEvent(old_size, new_history);
    loop.ReplaceHistory(new_history);
    const std::size_t after_tokens = EstimateTokens(EstimateHistoryChars(new_history));
    std::cout << trf("cmd.compact.result", before_tokens, after_tokens) << "\n";
    return event;
}

void PrintSessionsCommand(const std::string& sessions_dir, const std::string& args) {
    if (sessions_dir.empty()) {
        std::cout << tr("session.no_home") << "\n";
        return;
    }
    const bool all = args == "all";
    if (!args.empty() && !all) {
        std::cout << tr("cmd.sessions.usage") << "\n";
        return;
    }
    const auto entries =
        lubancode::agent::ListSessions(sessions_dir, 20, all ? std::string() : CurrentDirUtf8());
    if (entries.empty()) {
        if (all) {
            std::cout << trf("cmd.sessions.none_all", sessions_dir) << "\n";
        } else {
            std::cout << tr("cmd.sessions.none_here") << "\n";
        }
        return;
    }
    std::cout << trf("cmd.sessions.header", entries.size(),
                      all ? tr("cmd.sessions.scope_all") : tr("cmd.sessions.scope_here"))
              << "\n";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        // 标题优先,没设过标题回退首句摘要。
        const std::string& label = !entry.title.empty() ? entry.title : entry.first_user_text;
        std::cout << "  " << (i + 1) << ") " << entry.id << "\n"
                   << trf("cmd.sessions.entry",
                           entry.started_at.empty() ? tr("cmd.sessions.unknown_time") : entry.started_at,
                           entry.message_count,
                           label.empty() ? tr("cmd.sessions.no_text")
                                          : lubancode::agent::TruncateUtf8Chars(label, 40))
                   << "\n";
        if (all) {
            std::cout << trf("cmd.sessions.dir_line",
                              entry.cwd.empty() ? tr("cmd.sessions.dir_unknown")
                                                 : lubancode::agent::AbbreviateUtf8Middle(entry.cwd, 48))
                       << "\n";
        }
    }
}

// /resume 裸敲:本目录最近 20 场直接做成方向键菜单。显式编号/id 仍由
// ResumeSession 解析，脚本和熟手用法不变。
std::optional<std::string> PromptResumeTarget(const std::string& sessions_dir,
                                              const lubancode::cli::Theme& theme) {
    if (sessions_dir.empty()) {
        std::cout << tr("session.no_home") << "\n";
        return std::nullopt;
    }
    const auto entries = lubancode::agent::ListSessions(sessions_dir, 20, CurrentDirUtf8());
    if (entries.empty()) {
        std::cout << tr("cmd.resume.none") << "\n";
        return std::nullopt;
    }
    if (!lubancode::platform::StdinIsInteractive() || !lubancode::platform::ProbeStdoutConsole().is_console) {
        std::cout << tr("cmd.resume.usage") << "\n";
        return std::nullopt;
    }

    std::cout << "\n" << theme.banner << tr("cmd.resume.menu_title") << theme.reset << "\n";
    std::vector<lubancode::cli::ChoiceMenuItem> items;
    items.reserve(entries.size());
    for (const auto& entry : entries) {
        const std::string& raw_label = !entry.title.empty() ? entry.title : entry.first_user_text;
        const std::string label = raw_label.empty() ? tr("cmd.sessions.no_text")
                                                    : lubancode::agent::TruncateUtf8Chars(raw_label, 56);
        items.push_back({label,
                         trf("cmd.resume.menu_description",
                             entry.started_at.empty() ? tr("cmd.sessions.unknown_time") : entry.started_at,
                             entry.message_count, entry.id)});
    }
    lubancode::cli::ChoiceMenuOptions options;
    options.hint = tr("cmd.resume.menu_hint");
    options.invalid_hint = tr("cmd.resume.menu_hint");
    options.editable_hint = tr("cmd.resume.menu_hint");
    const auto selected = lubancode::cli::ReadChoiceMenu(items, options, theme);
    if (!selected.has_value() || selected->selected_indices.empty()) {
        std::cout << theme.stats << tr("cmd.resume.cancelled") << theme.reset << "\n";
        return std::nullopt;
    }
    return entries[selected->selected_indices.front()].id;
}

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
                    lubancode::cli::WorktreeSession* worktree_session = nullptr) {
    if (sessions_dir.empty()) {
        std::cout << tr("session.no_home") << "\n";
        return false;
    }
    const auto entries = lubancode::agent::ListSessions(sessions_dir, 20, CurrentDirUtf8());

    std::string id;
    std::string file_path;
    bool all_digits = !target.empty();
    for (const char c : target) {
        if (c < '0' || c > '9') {
            all_digits = false;
            break;
        }
    }
    if (target.empty()) {
        // --continue:本目录最近一场;一场都没有就按 quiet_if_none 处理。
        if (entries.empty()) {
            if (!quiet_if_none) {
                std::cout << tr("cmd.resume.none") << "\n";
            }
            return false;
        }
        id = entries.front().id;
        file_path = entries.front().file_path;
    } else if (all_digits) {
        std::size_t n = 0;
        try {
            n = static_cast<std::size_t>(std::stoul(target));
        } catch (...) {
            n = 0;
        }
        if (n < 1 || n > entries.size()) {
            std::cout << trf("cmd.resume.out_of_range", target, entries.size()) << "\n";
            return false;
        }
        id = entries[n - 1].id;
        file_path = entries[n - 1].file_path;
    } else {
        // 按 id 找:先在列表里对,不在(比 20 场更老)就直接拼路径试。
        for (const auto& entry : entries) {
            if (entry.id == target) {
                id = entry.id;
                file_path = entry.file_path;
                break;
            }
        }
        if (id.empty()) {
            id = target;
            file_path = sessions_dir + "/" + target + ".jsonl";
        }
    }

    const auto content = lubancode::agent::ReadSessionFileBytes(file_path);
    if (!content.has_value()) {
        std::cout << trf("cmd.resume.read_failed", file_path) << "\n";
        return false;
    }
    auto session = lubancode::agent::ParseSessionFile(*content);
    if (!session.has_value()) {
        std::cout << trf("cmd.resume.bad_meta", file_path) << "\n";
        return false;
    }

    loop.ReplaceHistory(session->messages);
    persisted_count = session->messages.size();
    if (!store.ResumeAt(file_path, id)) {
        std::cout << theme.error << trf("cmd.resume.takeover_failed", file_path) << theme.reset << "\n";
    }
    session_meta = session->meta;
    session_title = session->title;

    const std::string restored_history = lubancode::cli::FormatRestoredHistory(
        session->all_messages, theme, lubancode::cli::DetectConsoleWidth().value_or(80),
        session->compact_positions);
    if (!restored_history.empty()) {
        std::cout << "\n" << theme.banner << trf("cmd.resume.history.header", id) << theme.reset << "\n\n"
                  << restored_history << theme.stats << tr("cmd.resume.history.end") << theme.reset << "\n\n";
    }

    if (session->compact_count > 0) {
        // 经过压缩的场子:恢复的是回放出来的有效态,不是全量流水。
        std::cout << trf("cmd.resume.restored_compact", id, session->messages.size(),
                          session->all_messages.size(), session->compact_count);
    } else {
        std::cout << trf("cmd.resume.restored", id, session->messages.size());
    }
    if (session->repaired > 0) {
        std::cout << trf("cmd.resume.repaired", session->repaired);
    }
    if (session->skipped_lines > 0) {
        std::cout << trf("cmd.resume.skipped", session->skipped_lines);
    }
    std::cout << "。\n";
    // context 记账:真实 usage 得等恢复后第一次请求才校准,这里先按字符
    // 粗估打一行,心里有数。
    std::cout << trf("cmd.resume.estimate", EstimateTokens(EstimateHistoryChars(session->messages))) << "\n";
    if (!session->meta.model.empty() && session->meta.model != current_model) {
        std::cout << theme.stats << trf("cmd.resume.model_mismatch", session->meta.model, current_model)
                  << theme.reset << "\n";
    }
    if (!session->meta.wire.empty() && session->meta.wire != wire_str) {
        std::cout << theme.stats << trf("cmd.resume.wire_mismatch", session->meta.wire, wire_str) << theme.reset
                  << "\n";
    }

    // 会话跟 cwd 走(0.27.x):存档记录的 cwd 是一间房就验明正身搬回去。
    if (worktree_session != nullptr) {
        const std::string saved = session->meta.cwd;
        const std::string now = CurrentDirUtf8();
        if (!saved.empty() &&
            lubancode::agent::NormalizePathForCompare(saved) != lubancode::agent::NormalizePathForCompare(now)) {
            const std::filesystem::path saved_path(
                std::u8string(reinterpret_cast<const char8_t*>(saved.data()), saved.size()));
            std::error_code path_ec;
            if (!std::filesystem::exists(saved_path, path_ec)) {
                std::cout << theme.stats << trf("cmd.resume.worktree_gone", saved) << theme.reset << "\n";
            } else {
                const auto entered = worktree_session->EnterByPath(saved_path);
                if (entered.code == lubancode::cli::WorktreeResultCode::VerificationFailed) {
                    std::cout << theme.error << trf("cmd.resume.worktree_refused", saved, entered.detail)
                              << theme.reset << "\n";
                } else if (entered.code == lubancode::cli::WorktreeResultCode::Created) {
                    std::cout << theme.stats << trf("cmd.resume.worktree_back", saved) << theme.reset << "\n";
                }
            }
        }
    }
    return true;
}

// /export [路径]:当前会话导出 Markdown,默认写 sessions/<id>.md。
// 有存档文件就从文件读**全量流水**导出(压缩不丢内容,发生点插一行标注);
// 没有存档文件(没落过盘)退回导内存里这份历史。/title 设过的标题当大标题。
void HandleExportCommand(const std::string& args, const lubancode::agent::AgentLoop& loop,
                          const lubancode::agent::SessionStore& store, const std::string& sessions_dir,
                          const lubancode::agent::SessionMeta& session_meta, const std::string& session_title) {
    const auto& history = loop.History();
    if (history.empty()) {
        std::cout << tr("cmd.export.empty") << "\n";
        return;
    }
    const std::string id =
        !store.session_id().empty() ? store.session_id() : lubancode::agent::NowIdTimestamp() + "-export";
    std::string out_path = args;
    if (out_path.empty()) {
        if (sessions_dir.empty()) {
            std::cout << tr("cmd.export.need_path") << "\n";
            return;
        }
        out_path = sessions_dir + "/" + id + ".md";
    }

    // 全量优先:存档文件在,就按文件里的流水导(含压缩标注);读不动再退
    // 回内存这份(此时没有压缩位置可标)。
    std::string markdown;
    bool exported_from_file = false;
    if (!store.file_path().empty()) {
        if (const auto content = lubancode::agent::ReadSessionFileBytes(store.file_path());
            content.has_value()) {
            if (const auto session = lubancode::agent::ParseSessionFile(*content); session.has_value()) {
                const std::string& title = !session->title.empty() ? session->title : session_title;
                markdown = lubancode::agent::ExportSessionMarkdown(session->meta, session->all_messages, id,
                                                                     /*max_result_lines=*/30, title,
                                                                     session->compact_positions);
                exported_from_file = true;
            }
        }
    }
    if (!exported_from_file) {
        markdown = lubancode::agent::ExportSessionMarkdown(session_meta, history, id,
                                                             /*max_result_lines=*/30, session_title);
    }

    const std::filesystem::path path(
        std::u8string(reinterpret_cast<const char8_t*>(out_path.data()), out_path.size()));
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        std::cout << trf("cmd.export.write_failed", out_path) << "\n";
        return;
    }
    file << markdown;
    file.close();
    std::cout << trf("cmd.export.done", out_path) << "\n";
}

}  // namespace lubancode::app
