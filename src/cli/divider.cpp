#include "cli/divider.hpp"

#include <algorithm>
#include <string>

#include "cli/line_editor.hpp"  // DisplayWidthUtf8

namespace lubancode::cli {

namespace {
// "─" (U+2500 BOX DRAWINGS LIGHT HORIZONTAL) 的 UTF-8 编码,手写字节串,
// 不依赖任何宽字符转换函数——这条线只由这一个字符重复铺成,不需要走
// line_editor.hpp 那套完整的 UTF-32/东亚宽度机器。footer 带字线同用。
constexpr const char* kBoxDrawingHorizontal = "\xe2\x94\x80";

std::string RepeatRule(std::size_t count, bool plain) {
    const std::string unit = plain ? "-" : kBoxDrawingHorizontal;
    std::string out;
    out.reserve(unit.size() * count);
    for (std::size_t i = 0; i < count; ++i) {
        out += unit;
    }
    return out;
}
}  // namespace

std::string BuildDividerLine(int console_width, bool plain, int max_width) {
    const int width = console_width > 0 ? std::min(console_width - 1, max_width) : max_width;
    if (width <= 0) {
        return {};
    }
    return RepeatRule(static_cast<std::size_t>(width), plain);
}

std::string BuildTurnFooterLine(const std::string& text, int console_width, bool plain) {
    if (text.empty()) {
        return {};
    }
    // 探测失败(<= 0)按 80 兜底,与 BuildDividerLine 的老规矩一致。
    const int width = console_width > 0 ? console_width - 1 : 80;
    if (width <= 0) {
        return text;
    }
    const int text_cols = static_cast<int>(DisplayWidthUtf8(text));
    // 窄屏退化:比门槛窄、或文案加两侧余线(2 + 1 + 1 = 至少 4 列装饰)
    // 塞不下整行,只写文案——不硬塞、不折行、不压字。
    const int decorated_cols = text_cols + 4;
    if (width < kTurnFooterMinColumns || decorated_cols >= width) {
        return text;
    }
    // 文字嵌在线左:先 2 列横线,接 " text ",余下填满安全宽。
    const std::size_t tail = static_cast<std::size_t>(width - 2 - 1 - text_cols - 1);
    return RepeatRule(2, plain) + " " + text + " " + RepeatRule(tail, plain);
}

}  // namespace lubancode::cli
