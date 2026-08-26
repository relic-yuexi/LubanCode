// transcript 控制器实现(合同见 transcript_controller.hpp)。函数体自
// interactive_session 的 PrintViewedTranscript/PrintRecentItems/
// HandleTranscriptUi 原文搬家(改道:成员名归控制器、会话侧依赖走钩子、
// 输出走 TerminalPort),行为一字不差——注释一并随行。

#include "cli/transcript_controller.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "cli/i18n.hpp"
#include "cli/terminal_port.hpp"
#include "cli/turn_renderer.hpp"
#include "platform/console.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "tools/path_utils.hpp"

namespace lubancode::cli {

using lubancode::cli::TermOut;

TranscriptUiController::TranscriptUiController(const Theme& theme) : theme_(theme) {}

void TranscriptUiController::SetHooks(Hooks hooks) { hooks_ = std::move(hooks); }

// 查看帧没有 app 侧擦账(查看态完成退场花屏单,2026-08-17):旧帧擦除
// 只认终端层 console_input 那本 view_body_top——铺帧前现记现擦,不跨
// 调用攒绝对行号。这里(PrintViewedTranscript)只从终端层摆好的光标处
// 起打印。
void TranscriptUiController::PrintViewedTranscript(int viewed_task_id, int tail_rows) {
    std::lock_guard<std::mutex> lock(lubancode::cli::StdoutWriteMutex());
    lubancode::cli::EraseStreamFooterLocked();
    const int width = lubancode::cli::DetectConsoleWidth().value_or(80);

    const auto print_line = [&](const std::string& line) { TermOut() << line << "\n"; };

    if (viewed_task_id == 0) {
        // 回 main:首个可辨标题写明 main(规格"Esc 回 main"五条件),最近
        // 几条摘要重铺,视口/标题/收件目标一同复位。
        print_line(theme_.stats + tr("agent_panel.main_header") + theme_.reset);
        print_line(theme_.stats + tr("agent_panel.back_to_main") + theme_.reset);
        const int width_for_items = width;
        const std::size_t from = items_.size() > 5 ? items_.size() - 5 : 0;
        for (std::size_t i = from; i < items_.size(); ++i) {
            print_line(lubancode::cli::FormatTranscriptItem(
                items_[i], theme_, width_for_items, /*expanded=*/false,
                static_cast<int>(i) == focus_index_));
        }
    } else {
        std::vector<std::string> body;
        if (hooks_.build_task_transcript) {
            body = hooks_.build_task_transcript(viewed_task_id, width);
        }
        if (tail_rows > 0 && static_cast<int>(body.size()) > tail_rows) {
            // 头三行(标题/来源/统计)钉住,其余取最近 tail_rows-3 行:滚屏
            // 不刷屏,正在长的尾巴永远在视口里。
            constexpr int kHeadLines = 3;
            std::vector<std::string> tailed;
            tailed.reserve(static_cast<std::size_t>(tail_rows));
            for (int i = 0; i < kHeadLines && i < static_cast<int>(body.size()); ++i) {
                tailed.push_back(std::move(body[static_cast<std::size_t>(i)]));
            }
            const int keep = tail_rows - kHeadLines;
            for (int i = static_cast<int>(body.size()) - keep; i < static_cast<int>(body.size()); ++i) {
                if (i >= kHeadLines) {
                    tailed.push_back(std::move(body[static_cast<std::size_t>(i)]));
                }
            }
            body = std::move(tailed);
        }
        for (const auto& line : body) {
            print_line(line);
        }
    }
    TermOut().flush();
    lubancode::cli::RedrawStreamFooterLocked();
}

// 聚焦查看返回时的"简化重画":最近几条紧凑摘要(焦点标记照带)。
void TranscriptUiController::PrintRecentItems(std::size_t count) {
    const int width = lubancode::cli::DetectConsoleWidth().value_or(80);
    const std::size_t from = items_.size() > count ? items_.size() - count : 0;
    for (std::size_t i = from; i < items_.size(); ++i) {
        TermOut() << lubancode::cli::FormatTranscriptItem(items_[i], theme_, width, /*expanded=*/false,
                                                          static_cast<int>(i) == focus_index_);
    }
}

bool TranscriptUiController::HandleKey(UiKeyAction action) {
    const int width = lubancode::cli::DetectConsoleWidth().value_or(80);
    const int count = static_cast<int>(items_.size());
    switch (action) {
        case UiKeyAction::ToggleExpand: {
            // 子代理查看态的 Ctrl+O(追加需求"查看态实时思考流"):展开/收起
            // 查看帧里流式的思考/正文尾巴,与 main 流式思考同款折叠规矩——
            // 展开档铺"思考中 · N 字"的正文(约一屏,超了截断收口),紧凑
            // 档只留头行。main 聚焦查看不受影响。
            const int viewed_task = lubancode::cli::CurrentAgentViewedTaskId();
            if (viewed_task != 0) {
                agent_view_expanded_ = !agent_view_expanded_;
                TermOut() << "\n"
                          << theme_.stats
                          << (agent_view_expanded_ ? tr("ui.expanded") : tr("ui.compact")) << theme_.reset << "\n";
                PrintViewedTranscript(viewed_task, /*tail_rows=*/0);
                return true;
            }
            // Ctrl+O:展开/收起最近一条(Claude Code 风格),不再全局全展开。
            // expanded_index 落在最近一条,FormatTranscriptItems 只展开它。
            focus_view_active_ = false;
            if (count == 0) {
                expand_latest_ = false;
                TermOut() << "\n" << theme_.stats << tr("ui.no_items") << theme_.reset << "\n";
                return true;
            }
            expand_latest_ = !expand_latest_;
            TermOut() << "\n" << theme_.stats << (expand_latest_ ? tr("ui.expanded") : tr("ui.compact"))
                      << theme_.reset << "\n";
            TermOut() << lubancode::cli::FormatTranscriptItems(items_, theme_, width, expanded_, focus_index_,
                                                               expand_latest_ ? count - 1 : -1);
            return true;
        }
        case UiKeyAction::FocusOlder:
        case UiKeyAction::FocusNewer: {
            if (count == 0) {
                return false;  // 没条目,键还回去(本来也无事发生)
            }
            if (focus_index_ < 0) {
                focus_index_ = count - 1;  // 起手落在最近一条
            } else if (action == UiKeyAction::FocusOlder) {
                if (focus_index_ > 0) {
                    --focus_index_;  // 到最老一条停住
                }
            } else if (focus_index_ + 1 < count) {
                ++focus_index_;  // 到最新一条停住
            }
            TermOut() << "\n" << theme_.stats << trf("ui.focus", focus_index_ + 1, count) << theme_.reset << "\n";
            TermOut() << lubancode::cli::FormatTranscriptItem(items_[static_cast<std::size_t>(focus_index_)], theme_,
                                                              width, /*expanded=*/false, /*focused=*/true);
            return true;
        }
        case UiKeyAction::FocusView: {
            if (focus_view_active_) {
                // 再按 Ctrl+E:返回。简化重画:横幅 + 最近几条摘要,
                // 聚焦画面留在滚动历史里。
                focus_view_active_ = false;
                TermOut() << "\n" << theme_.stats << tr("ui.back") << theme_.reset << "\n";
                if (hooks_.repaint_banner) {
                    hooks_.repaint_banner();
                }
                PrintRecentItems(5);
                return true;
            }
            if (count == 0) {
                return false;
            }
            const int idx = focus_index_ >= 0 ? focus_index_ : count - 1;
            focus_view_active_ = true;
            TermOut() << "\n" << theme_.banner << trf("ui.focus_view", idx + 1, count) << theme_.reset << "\n";
            // width=0:标题 + 完整参数 + full_output 全文如实铺,不截宽,
            // 超长靠终端自然折行/滚动(不真清屏——conhost 的滚回缓冲跟
            // 屏幕缓冲是同一块,真清会把历史一并抹掉,取舍见报告)。
            TermOut() << lubancode::cli::FormatTranscriptItem(items_[static_cast<std::size_t>(idx)], theme_,
                                                              /*width=*/0, /*expanded=*/true);
            return true;
        }
        case UiKeyAction::Escape: {
            if (focus_view_active_) {
                focus_view_active_ = false;
                TermOut() << "\n" << theme_.stats << tr("ui.back") << theme_.reset << "\n";
                if (hooks_.repaint_banner) {
                    hooks_.repaint_banner();
                }
                PrintRecentItems(5);
                return true;
            }
            // loop 单遗留:空闲态停 loop 键位。聚焦查看态之外,ESC 的老
            // 语义是"清空输入"。这里加一档:composer 空(没在敲字)且有
            // 活 loop(非终态非 Paused 的任务在排)时,ESC 停全部活 loop
            // ——"背景会自己动"的东西得有一枚一键急停(与状态栏恒亮段
            // 配套)。composer 有字(或 stash 有货)照旧还给编辑器,半敲
            // 的话不吞。停 loop 的活是会话侧的账,走钩子。
            if (!lubancode::cli::ComposerStashHasContent() && hooks_.stop_active_loops) {
                const int stopped = hooks_.stop_active_loops();
                if (stopped > 0) {
                    TermOut() << theme_.stats
                              << "已停 " << stopped
                              << " 只 loop 任务(ESC;定义保留,续跑 /loop resume <id>)。" << theme_.reset
                              << "\n";
                    return true;
                }
            }
            return false;  // 没有活 loop:ESC 还给编辑器,老语义不动
        }
        case UiKeyAction::RepaintScreen: {
            // Ctrl+L:终端层已清可视区、作废帧锚点;这里重铺会话画面(session
            // header 一份 + 最近轮次),底栏由终端层随后画回。replace screen
            // ——可视区已清,不往 scrollback 叠第二份 banner。
            // 有 TurnView 存档时优先走同一颗 TerminalTurnRenderer(与实时
            // 画面同源,除 Running 动态外终态文本一致);没有(老轮次/纯
            // slash)退回 transcript 快照。
            if (hooks_.repaint_banner) {
                hooks_.repaint_banner();
            }
            const std::vector<runtime::TurnView>* turn_views =
                hooks_.turn_views ? hooks_.turn_views() : nullptr;
            if (turn_views != nullptr && !turn_views->empty()) {
                const int repaint_width = lubancode::cli::DetectConsoleWidth().value_or(80);
                TurnRenderOptions render_options;
                render_options.width = repaint_width;
                render_options.plain = theme_.reset.empty();
                render_options.expanded = expanded_;
                // 轮界横线(用户输入背景块单):从第二轮起,用户块之前画
                // 一道克制横线把 turn 分开——"上面有没有前一轮"是这里的账
                // (多轮循环),renderer 只照 leading_turn_divider 办事。
                bool first_turn = true;
                for (const runtime::TurnView& turn_view : *turn_views) {
                    render_options.leading_turn_divider = !first_turn;
                    first_turn = false;
                    const std::vector<std::string> lines = lubancode::cli::RenderTurnView(turn_view, theme_,
                                                                                           render_options);
                    for (const std::string& line : lines) {
                        TermOut() << line << "\n";
                    }
                }
            } else {
                PrintRecentItems(count > 0 ? 10 : 0);
            }
            return true;
        }
        case UiKeyAction::PrevUserTurn:
        case UiKeyAction::NextUserTurn: {
            // { / }:在用户提问(轮次)之间走。轮次从活 history 数(非 slash
            // 的用户消息),屏幕上给选中轮的正文摘要,状态行写"第 N/M 轮"。
            const std::vector<api::Message>* history = hooks_.history ? hooks_.history() : nullptr;
            if (history == nullptr) {
                return false;
            }
            std::vector<std::size_t> turn_indexes;
            for (std::size_t i = 0; i < history->size(); ++i) {
                const auto& message = (*history)[i];
                if (message.role != lubancode::api::Role::User || message.content.empty()) {
                    continue;
                }
                const auto* text = std::get_if<lubancode::api::TextBlock>(&message.content.front());
                if (text == nullptr || text->text.empty() || text->text.front() == '/') {
                    continue;
                }
                bool has_tool_result = false;
                for (const auto& block : message.content) {
                    if (std::holds_alternative<lubancode::api::ToolResultBlock>(block)) {
                        has_tool_result = true;
                        break;
                    }
                }
                if (!has_tool_result) {
                    turn_indexes.push_back(i);
                }
            }
            if (turn_indexes.empty()) {
                return false;
            }
            const bool older = action == UiKeyAction::PrevUserTurn;
            if (nav_turn_index_ < 0) {
                nav_turn_index_ = static_cast<int>(turn_indexes.size()) - 1;  // 起手最近一轮
            } else if (older && nav_turn_index_ > 0) {
                --nav_turn_index_;
            } else if (!older && nav_turn_index_ + 1 < static_cast<int>(turn_indexes.size())) {
                ++nav_turn_index_;
            }
            const std::size_t turn = turn_indexes[static_cast<std::size_t>(nav_turn_index_)];
            const auto& message = (*history)[turn];
            const auto* text = std::get_if<lubancode::api::TextBlock>(&message.content.front());
            TermOut() << "\n"
                      << theme_.stats
                      << trf("ui.turn_nav", nav_turn_index_ + 1, turn_indexes.size()) << theme_.reset << "\n";
            if (text != nullptr) {
                const std::string clipped = text->text.substr(0, 400);
                TermOut() << theme_.stats << clipped << (text->text.size() > 400 ? "…" : "")
                          << theme_.reset << "\n";
            }
            return true;
        }
        case UiKeyAction::ToScrollback: {
            // [:完整转录写进终端 scrollback——用终端自带搜索找路。条目按
            // 当前展开档铺,压缩点/截断在 FormatTranscriptItems 里自带标注;
            // 这只是查看,不改活 history(正式存档走 /export)。
            if (count == 0) {
                return false;
            }
            TermOut() << "\n"
                      << theme_.stats << tr("ui.to_scrollback") << theme_.reset << "\n";
            TermOut() << lubancode::cli::FormatTranscriptItems(items_, theme_, width, expanded_);
            TermOut().flush();
            return true;
        }
        case UiKeyAction::ViewInEditor: {
            // v:转录写临时 Markdown,交 $VISUAL/$EDITOR 只读查看。看完回来
            // composer 与光标原样(终端层已收帧重画)。
            const std::vector<api::Message>* history = hooks_.history ? hooks_.history() : nullptr;
            if (history == nullptr) {
                return false;
            }
            std::string markdown;
            for (const auto& message : *history) {
                const char* role_word =
                    message.role == lubancode::api::Role::User ? "## 用户" : "## 助手";
                markdown += role_word;
                markdown += "\n\n";
                for (const auto& block : message.content) {
                    if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block)) {
                        markdown += text->text + "\n\n";
                    } else if (const auto* use = std::get_if<lubancode::api::ToolUseBlock>(&block)) {
                        markdown += "> 工具调用: " + use->name + "\n\n";
                    } else if (const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&block)) {
                        markdown += "> 工具结果: " +
                                    result->content.substr(0, 2000) +
                                    (result->content.size() > 2000 ? "…(截断)" : "") + "\n\n";
                    }
                }
            }
            if (markdown.empty()) {
                return false;
            }
            std::string editor_cmd;
            if (const auto visual = lubancode::platform::GetEnvVar("VISUAL");
                visual.has_value() && !visual->empty()) {
                editor_cmd = *visual;
            } else if (const auto ed = lubancode::platform::GetEnvVar("EDITOR");
                       ed.has_value() && !ed->empty()) {
                editor_cmd = *ed;
            } else {
#ifdef _WIN32
                editor_cmd = "notepad";
#else
                editor_cmd = "vi";
#endif
            }
            std::filesystem::path file;
            try {
                file = std::filesystem::temp_directory_path() /
                       ("lubancode-transcript-" +
                        std::to_string(lubancode::platform::CurrentProcessId()) + ".md");
            } catch (const std::exception&) {
                TermOut() << theme_.error << tr("editor.no_temp") << theme_.reset << "\n";
                return true;
            }
            {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                if (!out) {
                    TermOut() << theme_.error << tr("editor.write_failed") << theme_.reset << "\n";
                    return true;
                }
                out << markdown;
            }
            TermOut() << theme_.stats << trf("ui.view_in_editor", editor_cmd) << theme_.reset << "\n";
            TermOut().flush();
            (void)lubancode::platform::RunInteractiveCommand(editor_cmd + " \"" +
                                                             lubancode::tools::PathToUtf8(file) + "\"");
            return true;
        }
    }
    return false;
}

}  // namespace lubancode::cli
