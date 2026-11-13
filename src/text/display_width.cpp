#include "text/display_width.hpp"

#include "text/grapheme.hpp"

namespace lubancode::text {

std::u32string Utf8ToUtf32(const std::string& text) {
    std::u32string out;
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        char32_t cp = 0;
        std::size_t extra = 0;
        if (c0 < 0x80) {
            cp = c0;
        } else if ((c0 & 0xE0) == 0xC0) {
            cp = c0 & 0x1F;
            extra = 1;
        } else if ((c0 & 0xF0) == 0xE0) {
            cp = c0 & 0x0F;
            extra = 2;
        } else if ((c0 & 0xF8) == 0xF0) {
            cp = c0 & 0x07;
            extra = 3;
        } else {
            ++i;  // 非法首字节,跳过一个
            continue;
        }
        bool ok = true;
        for (std::size_t k = 0; k < extra; ++k) {
            if (i + 1 + k >= n) {
                ok = false;
                break;
            }
            const unsigned char ck = static_cast<unsigned char>(text[i + 1 + k]);
            if ((ck & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (ck & 0x3F);
        }
        if (!ok) {
            ++i;
            continue;
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

std::size_t DisplayWidth(const std::u32string& text) {
    // 按扩展字素簇分段计宽:ZWJ 序列、肤色修饰、组合附标都并进所属簇,
    // 👩‍💻 记 2 列而不是 5 列。这是"编辑/布局/量宽共用一把尺"的入口。
    std::size_t width = 0;
    for (const GraphemeCluster& cluster : SplitGraphemes(text)) {
        width += static_cast<std::size_t>(cluster.width);
    }
    return width;
}

std::size_t DisplayWidthUtf8(const std::string& utf8) { return DisplayWidth(Utf8ToUtf32(utf8)); }

}  // namespace lubancode::text
