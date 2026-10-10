// 中立文本列宽：共用 Unicode 字素表，不依赖终端或编辑器状态。
#pragma once

#include <cstddef>
#include <string>

namespace lubancode::text {

// 保留原编辑器的宽松解码：坏首字节/续字节或截断序列跳一字节继续。
// 不是外部输入的严格 UTF-8 校验器；需要校验时用 platform::IsValidUtf8。
std::u32string Utf8ToUtf32(const std::string& text);

// 按完整字素簇计列宽，ZWJ、附标、旗帜等与编辑器/标题校验同一口径。
std::size_t DisplayWidth(const std::u32string& text);
std::size_t DisplayWidthUtf8(const std::string& utf8);

}  // namespace lubancode::text
