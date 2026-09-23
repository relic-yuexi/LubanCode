// provider_panel_core.hpp 的 TTY 宿主:真终端下画帧、收方向键。路数与
// context_window_panel 同款——platform 原语(RawInputScope/KeyReader/
// SetCursorPos/ClearRowHardFrom)画帧,单帧事务不闪屏;取消、EOF 与异常
// 均清场恢复输入。临时面板不写任何配置:确认后的预检/补救/热切换归调用
// 方(app 层 settings_commands 的 HandleProviderCommand)。
//
// 归 lubancode_app(要用 console_input 的锁与 EnsureStreamScreenRowsLocked)。

#include "cli/provider_panel_core.hpp"
#include "cli/terminal_port.hpp"  // TermOut/TermErr:散打 std::cout 清零,统一走输出端口

#include <algorithm>
#include <mutex>

#include "cli/console_input.hpp"
#include "cli/panel_chrome.hpp"  // PanelRowTone:行级语义档(排版批 6)
#include "platform/console.hpp"

namespace lubancode::cli {

namespace {

// platform::KeyInput -> cli::KeyEvent 的本地映射(本面板用得上的那几个键;
// 与 context_window_panel 同款规矩,console_input 的 MapKey 是私货)。
std::optional<KeyEvent> MapPanelKey(const platform::KeyInput& key) {
    using PK = platform::KeyInput::Kind;
    switch (key.kind) {
        case PK::Up:
            return KeyEvent::Simple(KeyKind::Up);
        case PK::Down:
            return KeyEvent::Simple(KeyKind::Down);
        case PK::Tab:
            return KeyEvent::Simple(KeyKind::Tab);
        case PK::ShiftTab:
            return KeyEvent::Simple(KeyKind::ShiftTab);
        case PK::Home:
            return KeyEvent::Simple(KeyKind::Home);
        case PK::End:
            return KeyEvent::Simple(KeyKind::End);
        case PK::PageUp:
            return KeyEvent::Simple(KeyKind::PageUp);
        case PK::PageDown:
            return KeyEvent::Simple(KeyKind::PageDown);
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
        case PK::Left:
        case PK::Right:
        case PK::None:
        default:
            return std::nullopt;  // 面板不收字符:这块只挑不筛,敲字当没看见
    }
}

}  // namespace

ProviderPanelResult RunProviderPanel(const ProviderPanelView& view, const Theme& theme) {
    ProviderPanelResult result;
    // 非 TTY(管道/重定向)不读键、不挂起:退给调用方走老路(打印列表
    // 或 switch 短用法)。
    if (!platform::StdinIsInteractive() || !platform::ProbeStdoutConsole().is_console ||
        !platform::SupportsScreenRepaint()) {
        return result;
    }

    std::lock_guard<std::recursive_timed_mutex> console_read_lock(ConsoleReadMutex());
    platform::RawInputScope raw_scope;
    if (!raw_scope.ok()) {
        return result;
    }

    // visible_capacity 没给就落默认档;core 与帧拼装用同一把尺。
    ProviderPanelView base = view;
    if (base.visible_capacity == 0) {
        base.visible_capacity = kProviderPanelDefaultVisibleRows;
    }
    ProviderPanelCore core(base.entries.size(), base.visible_capacity, base.cursor);

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

        // 每帧从 core 状态抄光标/窗口进 view 副本,纯拼装不碰状态。
        ProviderPanelView frame_view = base;
        frame_view.cursor = core.state().cursor;
        frame_view.offset = core.state().offset;
        const ProviderPanelFrame frame = BuildProviderPanelFrame(frame_view, width);

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
                continue;
            }
            // 行级语义档(排版批 6):[0] 标题 frame_title、[1] 当前端行
            // row_muted、末行键提示 footer key_hint(BuildProviderPanelFrame
            // 的行序契约),其余正文行默认前景;plain 全空串零转义。
            PanelRowTone tone = PanelRowTone::Body;
            if (r == 0) {
                tone = PanelRowTone::Title;
            } else if (r == 1) {
                tone = PanelRowTone::Muted;
            } else if (r == rows_to_draw - 1) {
                tone = PanelRowTone::Hint;
            }
            const std::string& color = PanelRowToneColor(theme, tone);
            if (!line.empty() && !color.empty()) {
                TermOut() << color << line << theme.reset;
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
    result.index = core.state().cursor;
    clear();
    return result;
}

}  // namespace lubancode::cli
