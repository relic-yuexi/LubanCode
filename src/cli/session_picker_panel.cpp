// session_picker_panel.hpp 的实现:TTY 帧循环。
//
// 数据流:接线层把当前 scope/sort 下的一整页喂进来;面板里改 Filter/
// Sort 只改本地形状,退出时把形状带回去——接线层看见形状变了,重查
// catalog(指纹缓存兜着,没动的场不重读)再进面板,选中项按 id 留住。
// 搜索词在面板内本地筛(单子口径:键盘搜索不重读盘)。

#include "cli/session_picker_panel.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include <iostream>
#include <mutex>

#include "cli/console_input.hpp"
#include "cli/i18n.hpp"
#include "cli/line_editor.hpp"
#include "platform/console.hpp"

#include <utility>

namespace lubancode::cli {

namespace {

// platform::KeyInput -> cli::KeyEvent 的本地映射(本面板用得上的键)。
// console_input.cpp 的 MapKey 是私货,这里照 provider_switch 的规矩自带
// 一份小的。
std::optional<KeyEvent> MapPickerKey(const platform::KeyInput& key) {
    using PK = platform::KeyInput::Kind;
    switch (key.kind) {
        case PK::Char: {
            KeyEvent event = KeyEvent::Char(key.ch);
            event.ctrl = key.ctrl;
            event.alt = key.alt;
            return event;
        }
        case PK::Paste:
            return KeyEvent::Paste(key.text);
        case PK::Backspace:
            return KeyEvent::Simple(KeyKind::Backspace);
        case PK::Left:
            return KeyEvent::Simple(KeyKind::Left);
        case PK::Right:
            return KeyEvent::Simple(KeyKind::Right);
        case PK::Home:
            return KeyEvent::Simple(KeyKind::Home);
        case PK::End:
            return KeyEvent::Simple(KeyKind::End);
        case PK::Up:
            return KeyEvent::Simple(KeyKind::Up);
        case PK::Down:
            return KeyEvent::Simple(KeyKind::Down);
        case PK::PageUp:
            return KeyEvent::Simple(KeyKind::PageUp);
        case PK::PageDown:
            return KeyEvent::Simple(KeyKind::PageDown);
        case PK::Tab:
            return KeyEvent::Simple(KeyKind::Tab);
        case PK::ShiftTab:
            return KeyEvent::Simple(KeyKind::ShiftTab);
        case PK::Enter:
            return KeyEvent::Simple(KeyKind::Enter);
        case PK::NewLine:
            return KeyEvent::Simple(KeyKind::NewLine);
        case PK::CtrlO:
            return KeyEvent::Simple(KeyKind::CtrlO);
        case PK::CtrlE:
            return KeyEvent::Simple(KeyKind::CtrlE);
        case PK::CtrlT:
            return KeyEvent::Simple(KeyKind::CtrlT);
        case PK::Esc:
            return KeyEvent::Simple(KeyKind::Esc);
        case PK::CtrlC:
            return KeyEvent::Simple(KeyKind::CtrlC);
        case PK::CtrlD:
            return KeyEvent::Simple(KeyKind::CtrlD);
        case PK::None:
        default:
            return std::nullopt;
    }
}

}  // namespace

SessionPickerPanelResult RunSessionPickerPanel(const SessionPickerFeed& feed, const Theme& theme,
                                               SessionPickerScope initial_scope,
                                               SessionPickerSort initial_sort, const std::string& prefer_id,
                                               std::size_t visible_capacity,
                                               const SessionTranscriptProvider& transcript_provider) {
    SessionPickerPanelResult result;
    result.scope = initial_scope;
    result.sort = initial_sort;
    if (!platform::StdinIsInteractive() || !platform::ProbeStdoutConsole().is_console ||
        !platform::SupportsScreenRepaint()) {
        return result;  // 非 TTY/管道:不开面板,调用方打短用法
    }

    std::lock_guard<std::recursive_timed_mutex> console_read_lock(ConsoleReadMutex());
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        return result;
    }

    SessionPickerCore core(visible_capacity);
    core.state().scope = initial_scope;
    core.state().sort = initial_sort;
    core.SetEntries(feed.entries, prefer_id);

    int start_row = 0;
    int rows_drawn = 0;
    int width = 80;
    const auto selected_id_now = [&]() -> std::string {
        const SessionPickerEntry* entry = core.SelectedEntry();
        return entry == nullptr ? std::string() : entry->id;
    };

    // 转录浮层的账(P3 第二棒:游标分页):开着的浮层显示哪场的转录 +
    // 滚动账(已取回的行 + 视口位 + 两个方向"还有货")。选中 id 变了
    // (或头一回开浮层)才回调 provider 取首页;之后滚动触边且"还有货"
    // 时按游标补页——不是每键读盘。
    std::string transcript_for_id;
    SessionTranscriptScroller transcript_scroll;
    // 视口行数跟屏走(resize 安全;draw 里每帧重探)。
    std::size_t transcript_view_rows = 6;
    // 一页取多少行:视口两屏(翻页键一跳就是一页,余量在屏里)。
    const auto transcript_page_size = [&]() -> std::size_t {
        return transcript_view_rows * 2;
    };
    const auto refresh_transcript = [&](const std::string& id) {
        transcript_for_id = id;
        transcript_scroll.Reset();
        if (transcript_provider == nullptr || id.empty()) {
            return;
        }
        SessionTranscriptPageQuery query;
        query.session_id = id;
        query.max_lines = transcript_page_size();
        transcript_scroll.LoadInitial(transcript_provider(query));
    };
    // 触边补页:游标取一页喂滚动账(空页自会封口);取不到 provider
    //(空回调)时滚动账的"还有货"本就不会置位,这里自然不进来。
    const auto fetch_older_page = [&](const std::string& id) {
        if (transcript_provider == nullptr) {
            return;
        }
        SessionTranscriptPageQuery query;
        query.session_id = id;
        query.before_seq = transcript_scroll.OlderCursor();
        query.max_lines = transcript_page_size();
        transcript_scroll.AppendOlder(transcript_provider(query));
    };
    const auto fetch_newer_page = [&](const std::string& id) {
        if (transcript_provider == nullptr) {
            return;
        }
        SessionTranscriptPageQuery query;
        query.session_id = id;
        query.after_seq = transcript_scroll.NewerCursor();
        query.max_lines = transcript_page_size();
        transcript_scroll.AppendNewer(transcript_provider(query));
    };

    auto draw = [&]() {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return false;
        }
        width = info->width > 8 ? info->width : 80;
        // 浮层开着画转录帧;关着画台账帧。转录按需取(选中 id 变了才读)。
        const SessionPickerFrame frame = [&]() -> SessionPickerFrame {
            if (!core.state().transcript_open) {
                return BuildSessionPickerFrame(core, width, feed.diagnostic);
            }
            const std::string id = selected_id_now();
            const int viewport_height =
                info->viewport_height > 0 ? info->viewport_height : info->height;
            // 视口行数跟屏走:标题 + 空行×2 + 底栏占 4 行,再留 2 行余量
            //(首帧不蹭滚动),单帧正好嵌进窗口。
            transcript_view_rows =
                viewport_height > 8 ? static_cast<std::size_t>(viewport_height - 6) : 6;
            transcript_scroll.SetViewportRows(transcript_view_rows);
            if (id != transcript_for_id) {
                refresh_transcript(id);
            }
            const std::string title =
                id.empty() ? std::string(tr("picker.transcript.title"))
                           : trf("picker.transcript.title", id);
            return BuildSessionTranscriptFrame(title, transcript_scroll.VisibleLines(), width,
                                               transcript_scroll.hint());
        }();

        const int rows_needed = static_cast<int>(frame.lines.size()) + 2;
        // 首帧从当前光标起画。后续重画须先回旧帧顶再核空间;旧代码拿
        // “上一帧末尾”的光标作起点,每按一键便在旧帧下方再预留整屏,
        // 终端只得滚动,旧标题遂一副副沉进回滚区。
        if (rows_drawn > 0) {
            platform::SetCursorPos(0, start_row);
        }
        if (!EnsureStreamScreenRowsLocked(rows_needed)) {
            return false;
        }
        const std::optional<platform::ScreenInfo> after = platform::GetScreenInfo();
        if (!after.has_value()) {
            return false;
        }
        // Ensure... 若因贴底而滚了内容,会把光标拨回滚后的真实位置;
        // 这就是新帧顶。没滚时仍是旧 start_row,自然原地覆盖。
        start_row = after->cursor_y;
        const int rows_to_draw = static_cast<int>(frame.lines.size());
        const int clear_rows = (std::max)(rows_drawn, rows_to_draw);
        TermOut() << "\x1b[?2026h\x1b[?25l";  // 单帧事务 + 藏光标,不闪屏
        for (int r = 0; r < clear_rows; ++r) {
            platform::ClearRowHardFrom(0, start_row + r, width);  // 整行清,不残尾巴
        }
        for (int r = 0; r < rows_to_draw; ++r) {
            platform::SetCursorPos(0, start_row + r);
            const std::string& line = frame.lines[static_cast<std::size_t>(r)];
            const std::size_t match = frame.row_match_index[static_cast<std::size_t>(r)];
            if (match != SessionPickerFrame::kNoMatch && match == core.selected()) {
                TermOut() << theme.confirm << TruncateUtf8ToDisplayWidth(line, width - 1) << theme.reset;
            } else if (match == SessionPickerFrame::kNoMatch) {
                // 标题/搜索/筛选行与底栏:淡色;列表普通行原色。
                TermOut() << theme.stats << TruncateUtf8ToDisplayWidth(line, width - 1) << theme.reset;
            } else {
                TermOut() << TruncateUtf8ToDisplayWidth(line, width - 1);
            }
        }
        rows_drawn = rows_to_draw;
        TermOut() << "\x1b[?2026l";
        platform::SetCursorPos(0, start_row + rows_to_draw);
        TermOut() << "\x1b[?25h";
        TermOut().flush();
        return true;
    };

    auto clear = [&] {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (info.has_value()) {
            for (int r = 0; r < rows_drawn; ++r) {
                platform::ClearRowHardFrom(0, start_row + r, info->width);
            }
            platform::SetCursorPos(0, start_row);
        }
        rows_drawn = 0;
    };

    if (!draw()) {
        return result;
    }

    platform::KeyReader key_reader;
    while (!core.state().submitted && !core.state().cancelled) {
        const std::optional<platform::KeyInput> raw_key = key_reader.ReadOne();
        if (!raw_key.has_value()) {
            clear();
            return result;  // EOF:取消,不动盘
        }
        const std::optional<KeyEvent> mapped = MapPickerKey(*raw_key);
        if (!mapped.has_value()) {
            continue;
        }
        // 转录浮层开着:滚动键(↑↓/翻页/首尾)先喂滚动账,触边且"还有
        // 货"就按游标补一页;其余键放行给 core(Enter/Esc/Ctrl+T/Ctrl+E
        // 那套收浮层/提交的规矩在 core 里,一字不动)。
        if (core.state().transcript_open &&
            transcript_scroll.HandleKey(mapped->kind)) {
            const std::string id = selected_id_now();
            if (transcript_scroll.NeedsOlderPage()) {
                fetch_older_page(id);
            }
            if (transcript_scroll.NeedsNewerPage()) {
                fetch_newer_page(id);
            }
            if (!core.state().submitted && !core.state().cancelled) {
                if (!draw()) {
                    clear();
                    return result;
                }
            }
            continue;
        }
        const std::string keep_id = selected_id_now();
        const std::string search_before = core.state().search;
        const auto scope_before = core.state().scope;
        const auto sort_before = core.state().sort;
        core.HandleKey(*mapped);
        // 搜索词变了:本地重筛(不重读盘),按 id 留住选中。
        if (core.state().search != search_before) {
            core.SetEntries(feed.entries, keep_id);
        }
        // 筛选/排序变了:面板收摊,形状带回给接线层重喂数据(数据在
        // catalog 手里,面板不知道源在哪)。
        if (core.state().scope != scope_before || core.state().sort != sort_before) {
            result.scope = core.state().scope;
            result.sort = core.state().sort;
            result.selected_id = keep_id;
            clear();
            return result;
        }
        if (!core.state().submitted && !core.state().cancelled) {
            if (!draw()) {
                clear();
                return result;
            }
        }
    }

    result.scope = core.state().scope;
    result.sort = core.state().sort;
    result.selected_id = selected_id_now();
    if (core.state().cancelled) {
        clear();
        return result;
    }
    const SessionPickerEntry* entry = core.SelectedEntry();
    if (entry == nullptr) {
        clear();
        return result;
    }
    const std::string id = entry->id;
    clear();
    result.picked_id = id;
    return result;
}

}  // namespace lubancode::cli
