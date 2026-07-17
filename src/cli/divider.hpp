// 输入/输出分界线:用户回车提交、模型真要发话之前打一条淡色横线,回合
// 结束的统计行之后再打一条,把一问一答从视觉上框出来。这里只放"这条线
// 该长什么样"的纯逻辑(宽度怎么定、plain 主题下用什么字符),不碰真实
// 控制台——真正探测控制台宽度、决定要不要打、拿什么颜色包这条线,都在
// main.cpp(那边才知道 is_console、theme)。

#pragma once

#include <string>

namespace lubancode::cli {

// console_width 是探测到的控制台实际列数(<= 0 表示探测失败/没探测到,
// 按 80 兜底)。实际打印宽度 = min(console_width - 1, 80)——留一列安全
// 边界,同时不管控制台多宽都不铺满一整行。plain 为真(plain 主题,或者
// 干脆不支持 ANSI/不是真控制台)时用半角 "-",否则用整宽字符 "─"
// (U+2500)。宽度算出来 <= 0 时返回空串。
std::string BuildDividerLine(int console_width, bool plain);

}  // namespace lubancode::cli
