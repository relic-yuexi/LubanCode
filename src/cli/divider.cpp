#include "cli/divider.hpp"

#include <algorithm>

namespace lubancode::cli {

namespace {
// "─" (U+2500 BOX DRAWINGS LIGHT HORIZONTAL) 的 UTF-8 编码,手写字节串,
// 不依赖任何宽字符转换函数——这条线只由这一个字符重复铺成,不需要走
// line_editor.hpp 那套完整的 UTF-32/东亚宽度机器。
constexpr const char* kBoxDrawingHorizontal = "\xe2\x94\x80";
}  // namespace

std::string BuildDividerLine(int console_width, bool plain, int max_width) {
    const int width = console_width > 0 ? std::min(console_width - 1, max_width) : max_width;
    if (width <= 0) {
        return {};
    }
    const std::string unit = plain ? "-" : kBoxDrawingHorizontal;
    std::string out;
    out.reserve(unit.size() * static_cast<std::size_t>(width));
    for (int i = 0; i < width; ++i) {
        out += unit;
    }
    return out;
}

}  // namespace lubancode::cli
