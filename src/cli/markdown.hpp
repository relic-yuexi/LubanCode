// 终端 markdown 渲染(0.18.x):模型正文回合收束后的"重画版"长什么样,
// 全在这两个纯函数里。流式期间正文照旧逐字原样打；main.cpp 在段落边界
// 和回合收束时，把收齐的小段先过 DetectMarkdownStructure，命中才按
// RenderMarkdown 的结果重画——擦与画的 Win32 锚点记账在 main.cpp
// (StreamBodyTracker),这个文件不碰 IO,单测主战场。
//
// 解析器手写,只认一个刻意收窄的子集:# 标题、-/*/1. 列表、``` 代码围栏、
// | 表格 |、> 引用、**粗体**、*斜体*、`行内码`。认不出的行原样过——
// 宁可漏渲染,不可错渲染。plain 主题(theme.reset 为空)下不夹任何 ANSI,
// 只做结构变换(去 #、换 •、表格排版);不过运行时 plain 主题/管道模式
// 压根走不进渲染,这条纯粹是让纯函数自身行为完备、方便单测。

#pragma once

#include <string>
#include <vector>

#include "cli/theme.hpp"

namespace lubancode::cli {

// 正文里有没有 markdown 结构:#/##/### 标题行、-/*/数字. 列表行、``` 围栏、
// 连续两行 |表格|、> 引用行、成对 **粗体**、成对 `行内码`,任一命中即真。
// 判定按行走,故意保守——普通中文散文一条都不该撞上。
bool DetectMarkdownStructure(const std::string& text);

// 把 markdown 正文渲染成一行行可直接打印的字符串(元素不带 \n)。每行
// 显示宽度绝不超过 width-1(锚点铁律:物理折行会毁掉原地重画的行数记账),
// 超宽按显示宽度(CJK=2)截断加 …。width <= 0 表示不限宽。
//   # 标题     -> bold + 主题色,一级加下划线,前后各留一空行
//   - 列表     -> • 圆点,两格缩进,嵌套按层加缩进;1. 数字列表保留数字
//   **粗体**   -> ANSI bold;*斜体* -> ANSI 3;`行内码` -> 反色
//   ``` 代码块 -> 每行两格缩进 + 淡色 │ 前缀,语言标记行是淡色标签,
//                 块内一切 markdown 语法不解析
//   | 表格 |   -> 按各列最大显示宽度对齐,│ ─ 边线,表头 bold,总宽超限
//                 时压缩最宽列、格内截断加 …
//   > 引用     -> 淡色 │ 前缀
std::vector<std::string> RenderMarkdown(const std::string& text, const Theme& theme, int width);

}  // namespace lubancode::cli
