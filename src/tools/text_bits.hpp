// engine 侧的纯文本小函数(显示系统剥离单第八步:清编译边界)。
//
// CountUtf8Codepoints 与 FormatTokenCount 原住在 cli(transcript.cpp /
// format_utils.cpp),tools/agent_tool.cpp 的活度账与最终报告要吃。那两份
// cli 文件的依赖链(context_tracker/theme/terminal_caps)是终端层的,engine
// 引不动——这里放 engine 自己的一份实现,算法与 cli 侧逐句相同,漂移由
// tests/unit/runtime/test_turn_item.cpp 的"与 cli 同款"用例两边同钉(见 CountUtf8Codepoints
// 的对账断言)。
//
// 零依赖:只认标准库。新代码在 engine 层要数字 k 化/数码点,认这里,不引 cli。

#pragma once

#include <cstdint>
#include <string>

namespace lubancode::tools {

// 数一段 UTF-8 文本有几个码点(活度账"多少字"用)。逐句同
// cli::CountUtf8Codepoints:连续首字节计数,不校验序列合法性(坏序列按
// 字节数吞,与 cli 侧行为一致)。
inline int CountUtf8Codepoints(const std::string& text) {
    int count = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[pos]);
        if ((byte & 0xF8) == 0xF0) {
            pos += 4;
        } else if ((byte & 0xF0) == 0xE0) {
            pos += 3;
        } else if ((byte & 0xE0) == 0xC0) {
            pos += 2;
        } else {
            pos += 1;
        }
        ++count;
    }
    return count;
}

// token 数字 k 化(最终报告/台账行用)。逐句同 cli::FormatTokenCount:
//   n < 10000 原样;>= 10000 一位小数 k(尾随 .0 省略);>= 1000000 两位
// 小数 M(尾随 0 逐位省略)。四舍五入;负数原样,不猜。
inline std::string FormatTokenCount(std::int64_t n) {
    if (n < 10000) {
        return std::to_string(n);
    }
    if (n < 1000000) {
        const std::int64_t tenths = std::llround(static_cast<double>(n) / 100.0);
        std::string out = std::to_string(tenths / 10);
        if (tenths % 10 != 0) {
            out += "." + std::to_string(tenths % 10);
        }
        return out + "k";
    }
    const std::int64_t hundredths = std::llround(static_cast<double>(n) / 10000.0);
    std::string out = std::to_string(hundredths / 100);
    const std::int64_t frac = hundredths % 100;
    if (frac != 0) {
        out += ".";
        out += static_cast<char>('0' + frac / 10);
        if (frac % 10 != 0) {
            out += static_cast<char>('0' + frac % 10);
        }
    }
    return out + "M";
}

}  // namespace lubancode::tools
