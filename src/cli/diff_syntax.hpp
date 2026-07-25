#pragma once

#include <string>
#include <string_view>

#include "cli/theme.hpp"

namespace lubancode::cli {

// diff 只需一层轻词法着色,不做 AST。按文件名选词法规则;Unknown 原样
// 返回,绝不拿猜错的语言给普通文本乱上色。
enum class DiffSyntaxLanguage { Unknown, Cpp, Python, JavaScript, Json, Shell, Rust, Go };

DiffSyntaxLanguage DetectDiffSyntaxLanguage(std::string_view file_path);

// 给一行代码添 ANSI 前景色。返回值不带行末 reset;调用方在最外层收口。
// 这里发出的 SGR 都不改背景色,可安全叠在 diff_add_bg/diff_del_bg 上。
std::string HighlightDiffCodeLine(std::string_view code, DiffSyntaxLanguage language,
                                  const Theme& theme);

}  // namespace lubancode::cli
