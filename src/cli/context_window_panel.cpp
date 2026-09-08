// context_window_panel.hpp 的 TTY 宿主:真终端下画帧、收方向键。路数与
// provider_switch/session_picker_panel 同款——platform 原语(RawInputScope/
// KeyReader/SetCursorPos/ClearRowHardFrom)画帧,单帧事务不闪屏;取消、
// EOF 与异常均清场恢复输入。临时面板不写永久 transcript、不碰任何配置:
// 确认后的应用归调用方(app 层 session_commands)。
//
// 归 lubancode_app(要用 console_input 的锁与 EnsureStreamScreenRowsLocked)。

#include "cli/context_window_panel.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include <algorithm>
#include <mutex>

#include "cli/console_input.hpp"
#include "platform/console.hpp"

namespace lubancode::cli {

namespace {

// platform::KeyInput -> cli::KeyEvent 的本地映射(本面板用得上的那几个键;
// provider_switch/session_picker 同款规矩,console_input 的 MapKey 是私货)。
std::optional<KeyEvent> MapPanelKey(const platform::KeyInput& key) {
    using PK = platform::KeyInput::Kind;
    switch (key.kind) {
        case PK::Left:
            return KeyEvent::Simple(KeyKind::Left);
        case PK::Right:
            return KeyEvent::Simple(KeyKind::Right);
        case PK::Up:
            return KeyEvent::Simple(KeyKind::Up);
        case PK::Down:
            return KeyEvent::Simple(KeyKind::Down);
        case PK::Tab:
            return KeyEvent::Simple(KeyKind::Tab);
        case PK::ShiftTab:
            return KeyEvent::Simple(KeyKind::ShiftTab);
        case PK::Enter:
            return KeyEvent::Simple(KeyKind::Enter);
        case PK::NewLine:
            return KeyEvent::Simple(KeyKind::NewLine);
        case PK::Esc:
            return KeyEvent::Simple(KeyKind::Esc);
        case PK::CtrlC:
            return KeyEvent::Simple(KeyKind::CtrlC);
        case PK::CtrlD:
            return KeyEvent::Simple(KeyKind::CtrlD);
        case PK::Char:
        case PK::Paste:
        case PK::Backspace:
        case PK::Home:
        case PK::End:
        case PK::PageUp:
        case PK::PageDown:
        case PK::None:
        default:
            return std::nullopt;  // 面板不收字符:敲字当没看见,不进筛选词
    }
}

}  // namespace

ContextWindowPanelResult RunContextWindowPanel(const ContextWindowPanelView& view, const Theme& theme) {
    ContextWindowPanelResult result;
    // 非 TTY(管道/重定向)不读键、不挂起(§三):退给调用方打短说明。
    if (!platform::StdinIsInteractive() || !platform::ProbeStdoutConsole().is_console ||
        !platform::SupportsScreenRepaint()) {
        return result;
    }

    std::lock_guard<std::recursive_timed_mutex> console_read_lock(ConsoleReadMutex());
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        return result;
    }

    ContextWindowPanelCore core(view.window.values.size(), view.effort.options.size(),
                                view.window_index, view.effort_index);

    int start_row = 0;
    int rows_drawn = 0;
    int width = 80;
    auto draw = [&]() {
        std::lock_guard<std::mutex> stdout_lock(StdoutWriteMutex());
        const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
        if (!info.has_value()) {
            return false;
        }
        width = info->width > 8 ? info->width : 80;

        // 每帧从 core 状态抄索引/焦点进 view 副本,纯拼装不碰状态。
        ContextWindowPanelView frame_view = view;
        frame_view.window_index = core.state().window_index;
        frame_view.effort_index = core.state().effort_index;
        frame_view.focus = core.state().focus;
        const ContextWindowPanelFrame frame = BuildContextWindowPanelFrame(frame_view, width);

        const int rows_needed = static_cast<int>(frame.lines.size()) + 2;
        // 首帧从当前光标起画;重画先回旧帧顶再核空间(provider_switch 同款:
        // 从上一帧末尾探底会在旧帧下面另起一块)。
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
        start_row = after->cursor_y;  // 贴底滚动后光标即新帧顶
        const int rows_to_draw = static_cast<int>(frame.lines.size());
        const int clear_rows = (std::max)(rows_drawn, rows_to_draw);
        TermOut() << "\x1b[?2026h\x1b[?25l";  // 单帧事务 + 藏光标
        for (int r = 0; r < clear_rows; ++r) {
            platform::ClearRowHardFrom(0, start_row + r, width);
        }
        for (int r = 0; r < rows_to_draw; ++r) {
            platform::SetCursorPos(0, start_row + r);
            const std::string& line = frame.lines[static_cast<std::size_t>(r)];
            if (line.rfind("> ", 0) == 0 && !theme.confirm.empty()) {
                TermOut() << theme.confirm << line << theme.reset;  // 焦点行上色
            } else {
                TermOut() << line;
            }
        }
        rows_drawn = rows_to_draw;
        TermOut() << "\x1b[?2026l\x1b[?25h";
        platform::SetCursorPos(0, start_row + rows_drawn);
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
    result.opened = true;  // 面板真起来了;之后的任何收场都算"开过"

    platform::KeyReader key_reader;
    while (!core.state().submitted && !core.state().cancelled) {
        const std::optional<platform::KeyInput> raw_key = key_reader.ReadOne();
        if (!raw_key.has_value()) {
            clear();
            return result;  // EOF:取消(opened=true, saved=false),不碰配置
        }
        const std::optional<KeyEvent> mapped = MapPanelKey(*raw_key);
        if (!mapped.has_value()) {
            continue;
        }
        core.HandleKey(*mapped);
        if (!core.state().submitted && !core.state().cancelled) {
            if (!draw()) {
                clear();
                return result;
            }
        }
    }

    result.saved = core.state().submitted;
    result.window_index = core.state().window_index;
    result.effort_index = core.state().effort_index;
    clear();
    return result;
}

}  // namespace lubancode::cli
