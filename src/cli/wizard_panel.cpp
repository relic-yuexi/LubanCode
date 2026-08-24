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
    prompt_ = frame.prompt;
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
    // 提示)。若预留把缓冲区顶滚,EnsureStreamScreenRowsLocked 会把锚点
    // 随内容一同上移;只平移 viewport 时绝对行号不动。
    const int rows_needed = static_cast<int>(lines.size()) + 2;
    // 后续帧从旧面板锚点探底。光标平时停在输入行或菜单首行，若拿它
    // 当整帧起点，下面预留会算重；更不能把“光标在帧内”误判成滚屏，
    // 否则 start_row_ 会每帧往上漂，旧 footer 便留在原处。
    if (rows_drawn_ > 0) {
        platform::SetCursorPos(0, start_row_);
    }
    if (!EnsureStreamScreenRowsLocked(rows_needed)) {
        active_ = false;
        return;
    }
    info = platform::GetScreenInfo();
    if (!info.has_value()) {
        active_ = false;
        return;
    }
    // EnsureStreamScreenRowsLocked 若真滚了内容，会把刚才那枚锚点按滚动量
    // 上移后再放回光标；若只平移 viewport，绝对行号不动。两种情形都以
    // 此刻光标行为准，旧帧与新帧便落在同一处。
    start_row_ = info->cursor_y;

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
    // 行编辑器每次按键都会清整行再画。须把 prompt 原文交给它；只把光标
    // 留在 prompt 末尾、却传空串，会把“名字: ”擦掉，正文落到第 0 列，
    // 光标仍按静态前缀宽度计算，遂出现字在左、光标在右。
    if (active_ && prompt_row_ >= 0) {
        platform::SetCursorPos(0, start_row_ + prompt_row_);
    }
    const std::optional<std::string> line = cli::ReadLine(
        active_ && prompt_row_ >= 0 ? prompt_ : std::string(), Theme{},
        /*esc_rejects=*/true, /*composer=*/false, reason);
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
    prompt_.clear();
    active_ = false;
}

}  // namespace lubancode::cli
