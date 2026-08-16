// 向导面板的终端绘制(向导重排单)。接口见 wizard_panel.hpp 文件头。

#include "cli/wizard_panel.hpp"

#include <algorithm>
#include <iostream>
#include <mutex>

#include "cli/line_editor.hpp"
#include "platform/console.hpp"

namespace lubancode::cli {

namespace {

// synchronized output(DEC 2026):整帧画/清期间不让终端刷出半帧。与
// console_input.cpp 那份同一定义,这里文件内自持一份,不跨 TU 暴露。
constexpr const char* kPanelSyncBegin = "\x1b[?2026h";
constexpr const char* kPanelSyncEnd = "\x1b[?2026l";

// 一条横贯终端宽度的分隔线。窄终端跟着窄,不写死六十列。
std::string PanelRule(int width) {
    const int n = width > 4 ? width - 2 : 20;
    std::string rule;
    rule.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        rule += '-';
    }
    return rule;
}

// 标题居左、进度贴右的头部行。宽度不够(窄终端)就折成两行,不许重叠。
std::vector<std::string> PanelHeader(const std::string& title, const std::string& progress, int width) {
    if (title.empty() && progress.empty()) {
        return {};
    }
    if (progress.empty()) {
        return {title};
    }
    if (title.empty()) {
        return {progress};
    }
    const std::size_t title_width = DisplayWidthUtf8(title);
    const std::size_t progress_width = DisplayWidthUtf8(progress);
    if (title_width + progress_width + 4 <= static_cast<std::size_t>(width)) {
        const std::size_t pad = static_cast<std::size_t>(width) - 2 - title_width - progress_width;
        return {title + std::string(pad, ' ') + progress};
    }
    return {title, progress};
}

}  // namespace

bool WizardPanel::Available() {
    return platform::StdinIsInteractive() && platform::ProbeStdoutConsole().is_console &&
           platform::SupportsScreenRepaint();
}

WizardPanel::WizardPanel() {
    active_ = Available();
}

WizardPanel::~WizardPanel() {
    Finish();
}

void WizardPanel::Draw(const WizardFrame& frame, int reserve_rows) {
    if (!active_) {
        return;
    }
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (!info.has_value()) {
        active_ = false;  // 探不到屏面就别硬画,调用方退朴素逐行
        return;
    }
    width_ = info->width > 8 ? info->width : 80;

    // 组帧:上分隔线 / 头部 / 空行 / 正文 / 错误 / [prompt 行] / 空行 /
    // [预留区(选择菜单)] / footer / 下分隔线。文本帧没有预留区。
    std::vector<std::string> lines;
    lines.push_back(PanelRule(width_));
    for (const std::string& header : PanelHeader(frame.title, frame.progress, width_)) {
        lines.push_back(header);
    }
    lines.push_back("");
    for (const std::string& body : frame.body) {
        for (const std::string& wrapped : WrapUtf8ToDisplayWidth(body, width_ > 4 ? width_ - 2 : width_)) {
            lines.push_back(wrapped);
        }
    }
    if (!frame.error.empty()) {
        for (const std::string& wrapped : WrapUtf8ToDisplayWidth("! " + frame.error,
                                                                width_ > 4 ? width_ - 2 : width_)) {
            lines.push_back(wrapped);
        }
    }
    prompt_row_ = -1;
    menu_top_ = -1;
    if (!frame.prompt.empty()) {
        lines.push_back("");
        prompt_row_ = static_cast<int>(lines.size());
        lines.push_back(frame.prompt);
    }
    lines.push_back("");
    if (reserve_rows > 0) {
        menu_top_ = static_cast<int>(lines.size());
        for (int i = 0; i < reserve_rows; ++i) {
            lines.push_back("");
        }
    }
    if (!frame.footer.empty()) {
        lines.push_back(TruncateUtf8ToDisplayWidth(frame.footer, width_ > 2 ? width_ - 2 : width_));
    }
    lines.push_back(PanelRule(width_));

    // 预留整帧(外加两行余量:ReadLine 提交要打一个换行,菜单还带一行
    // 提示)。预留可能把缓冲区顶滚:预留前后各看一眼光标行,差多少就把
    // 面板起点往上挪多少——帧反正要整帧重画,锚点对齐就行。
    const int rows_needed = static_cast<int>(lines.size()) + 2;
    const int cursor_before = info->cursor_y;
    if (!EnsureStreamScreenRowsLocked(rows_needed)) {
        active_ = false;
        return;
    }
    info = platform::GetScreenInfo();
    if (!info.has_value()) {
        active_ = false;
        return;
    }
    if (rows_drawn_ == 0) {
        start_row_ = info->cursor_y;
    } else {
        const int expected_end = start_row_ + rows_drawn_;
        const int actual_end = info->cursor_y;
        if (actual_end < expected_end && cursor_before < expected_end) {
            start_row_ -= (expected_end - actual_end);
            if (start_row_ < 0) {
                start_row_ = 0;
            }
        }
    }

    // 清旧画新:旧区域逐行硬清(连背景属性一起还原),再从同一处画回。
    const int rows_to_draw = static_cast<int>(lines.size());
    const int clear_rows = (std::max)(rows_drawn_, rows_to_draw);
    std::cout << kPanelSyncBegin << "\x1b[?25l";
    for (int r = 0; r < clear_rows; ++r) {
        platform::ClearRowHardFrom(0, start_row_ + r, width_);
    }
    for (int r = 0; r < rows_to_draw; ++r) {
        platform::SetCursorPos(0, start_row_ + r);
        std::cout << lines[static_cast<std::size_t>(r)];
    }
    rows_drawn_ = rows_to_draw;
    std::cout << kPanelSyncEnd;
    std::cout.flush();

    // 光标:文本帧停在 prompt 行末(ReadText 接手编辑);选择帧停在预留区
    // 首行(ReadChoiceMenu 在那里画菜单)。
    std::cout << "\x1b[?25h";
    if (prompt_row_ >= 0) {
        platform::SetCursorPos(static_cast<int>(DisplayWidthUtf8(frame.prompt)), start_row_ + prompt_row_);
    } else if (menu_top_ >= 0) {
        platform::SetCursorPos(0, start_row_ + menu_top_);
    } else {
        platform::SetCursorPos(0, start_row_ + rows_to_draw);
    }
    std::cout.flush();
}

std::optional<std::string> WizardPanel::ReadText(ReadExitReason* reason) {
    // ReadLine 传空 prompt:prompt 文字已由 Draw 印在面板里,不让它再打。
    // 真面板(active_)与非面板(POSIX 无重画支持的交互终端)都走它——
    // 前者从面板 prompt 行编辑,后者在当前光标处起一行。
    const std::optional<std::string> line =
        cli::ReadLine("", Theme{}, /*esc_rejects=*/true, /*composer=*/false, reason);
    // 提交/退场的换行占了 prompt 行的下一行,记进账里,下一帧清场一并擦。
    if (active_ && prompt_row_ >= 0) {
        rows_drawn_ = (std::max)(rows_drawn_, prompt_row_ + 2);
    }
    return line;
}

void WizardPanel::Finish() {
    if (!active_ || rows_drawn_ == 0) {
        active_ = false;
        return;
    }
    std::lock_guard<std::mutex> lock(StdoutWriteMutex());
    const std::optional<platform::ScreenInfo> info = platform::GetScreenInfo();
    if (info.has_value()) {
        std::cout << kPanelSyncBegin;
        for (int r = 0; r < rows_drawn_; ++r) {
            platform::ClearRowHardFrom(0, start_row_ + r, info->width);
        }
        platform::SetCursorPos(0, start_row_);
        std::cout << kPanelSyncEnd;
        std::cout.flush();
    }
    rows_drawn_ = 0;
    prompt_row_ = -1;
    menu_top_ = -1;
    active_ = false;
}

}  // namespace lubancode::cli
