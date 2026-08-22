// 输入/输出分界线:用户回车提交、模型真要发话之前打一条淡色横线,回合
// 结束的统计行之后再打一条,把一问一答从视觉上框出来。这里只放"这条线
// 该长什么样"的纯逻辑(宽度怎么定、plain 主题下用什么字符),不碰真实
// 控制台——真正探测控制台宽度、决定要不要打、拿什么颜色包这条线,都在
// main.cpp(那边才知道 is_console、theme)。
//
// 回合视觉收束单新添 BuildTurnFooterLine:turn 尾那条带字的分界线
// ("──── Worked for 6m 41s ────")。满宽线只认 turn 边界;文字嵌在横线左
// 侧约两列处,余下横线填满安全宽度;窄于四十列只写文案,不硬塞左右长线。

#pragma once

#include <string>

namespace lubancode::cli {

// console_width 是探测到的控制台实际列数(<= 0 表示探测失败/没探测到,
// 按 max_width 兜底)。实际打印宽度 = min(console_width - 1, max_width)——
// 留一列安全边界,同时不管控制台多宽都不铺满一整行。plain 为真(plain
// 主题,或者干脆不支持 ANSI/不是真控制台)时用半角 "-",否则用整宽字符
// "─"(U+2500)。宽度算出来 <= 0 时返回空串。
// max_width 是宽度上限;探测失败(console_width <= 0)时也拿它兜底。
// 0.21.x:输入框上下横线与输入/输出分界线都要满终端宽,调用点把
// max_width 传成 console_width 自身——min(console_width - 1, console_width)
// 恒等于 console_width - 1,即满宽随终端(留一列安全边界)。默认 80 只是
// 给不关心上限的老调用点兜底。
std::string BuildDividerLine(int console_width, bool plain, int max_width = 80);

// turn 尾分界线(回合视觉收束单):"──── Worked for 6m 41s ────"。
//   - text 嵌在线左:先 2 列横线,接 " text ",再横线填到安全宽
//     (console_width - 1,与 BuildDividerLine 同一把尺);
//   - 窄于 40 列(kTurnFooterMinColumns)只写文案本身,不硬塞长线——
//     窄屏退化,不折行、不压字;
//   - text 已超宽(比整条线还长)也只写文案;
//   - plain 同 BuildDividerLine 用 "-";
//   - 宽度 <= 0 或 text 为空:返回空串(空文案没有线,直接走裸线)。
// 显示宽按 UTF-8 显示列算(文字可含中文/emoji),不切坏多字节。
std::string BuildTurnFooterLine(const std::string& text, int console_width, bool plain);

// 窄屏退化的门槛:比这窄就只写文案(单子:窄于四十列不硬塞左右长线)。
inline constexpr int kTurnFooterMinColumns = 40;

}  // namespace lubancode::cli
