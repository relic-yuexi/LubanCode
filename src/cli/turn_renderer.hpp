// TerminalTurnRenderer(终端回合视觉收束单):TurnView -> 终端行组。
//
// 落地次序第 4 步的纯函数核:按 TurnView 画一整轮(user 条目、model step
// 分组、工具批次、正文、turn footer),收回散落的裸打印——这里只产文本
// 行,不碰 IO(真终端的锚点/擦除在 TranscriptPainter,那个管"画在哪",
// 这个管"画成什么字")。
//
// 排法照单子第二节"一轮的排法":
//   > 用户问题正文
//   (空行)
//   • 思考 2.3s
//   • read_file(path)
//     └ 读取 31 行
//   (model step 换拍时:一行轻间隔,不画满宽横线——满宽线只认 turn)
//   助手最后答复……
//   ──── Worked for 12.8s ────
//
// 黄金画面:tests/unit/runtime/test_turn_view.cpp 的快照幕直接钉这里产出的行组
// (80/120/160 列纯文本),Resize/Ctrl+L/resume 重放都从同一颗 renderer 出,
// 除 Running 动态外终态文本一致(单子验收最后一条)。

#pragma once

#include <string>
#include <vector>

#include "cli/theme.hpp"
#include "runtime/turn_view.hpp"

namespace lubancode::cli {

// 一轮的渲染选项。
struct TurnRenderOptions {
    int width = 80;             // 终端列宽;<= 0 按 80 兜底
    bool plain = false;         // plain 主题(裸文本,无 ANSI)
    bool expanded = false;      // 详细态:条目展开(完整参数/输出)
    bool include_user = true;   // 画不画 user 条目(resume 重放可能自带)
    bool include_footer = true; // 画不画 turn footer(紧凑态实时画面不画,
                                // footer 由 RunTurn 收口时单独落)
    // 用户条目之前画不画一道横线(turn 分隔,克制样式:stats 淡色满宽线,
    // 与 BuildDividerLine 同款)。多轮重放(Ctrl+L)时由调用方从第二轮起
    // 置真——"上面有没有前一轮"这件事只有 caller 知道,renderer 的局部
    // 状态带不过轮;首轮(会话开头)不画,实时画面那道由 RunTurn 的
    // PrintDivider 自己落,不双打。
    bool leading_turn_divider = false;
    // 画不画 Text 条目(助手正文)。实时流里正文由 markdown 正文流当场铺,
    // 默认不画免得双打;但"整屏重建"(resize 改宽/Ctrl+L)会把屏上正文连
    // 根擦掉——那种场合没有第二条路把正文带回屏上,必须由 renderer 重铺
    // (改宽瞬间正文凭空消失单:Resize 后只见 user 块与 Worked 线,答案
    // 整段不见了)。默认 false,老调用点一字不变;重建路的 caller 置真。
    bool include_text = false;
};

// TurnView -> 行组(每行已含 ANSI 或纯文本,不含行尾换行)。
//   - 状态灯与摘要文案复用 TranscriptItem 的现有投影:这里把 TurnItemView
//     折成 TranscriptItem 再走 FormatTranscriptItem,不抄第二遍措辞;
//   - model step 换拍(第二拍起)在两组条目之间垫一空行(轻间隔);
//   - 满宽分界线只认 turn:开头一条(可选)、结尾 Worked footer 一条;
//   - footer 词干按 view.status 挑:cancelled/interrupted -> Stopped,
//     failed -> Failed,其余 Worked;墙钟用 metrics.wall_duration_ms。
std::vector<std::string> RenderTurnView(const lubancode::runtime::TurnView& view, const Theme& theme,
                                        const TurnRenderOptions& options);

// 单枚条目的投影(TurnItemView -> TranscriptItem 的字段折算;渲染走
// FormatTranscriptItem,文案一处定)。公开给快照重放(Ctrl+L)逐条铺。
// TranscriptItem 的完整定义在 cli/transcript.hpp;这里前置声明,实现里
// include(transcript 不认 runtime,方向不倒)。
//
// 整组布局的职责归属(主/Subagent 面板同构渲染单 P1 清点):FormatTranscriptItems
// 管"一组条目之内"——紧凑档 SubTool 过滤、条目间 GapBetween 间距、单条展开;
// RenderTurnView 管"一轮的骨架"——user 背景块、step 轻间隔、turn footer、
// 轮界横线。两者各认一半、互不越界:条目间距不在这再排一遍,轮骨架不进
// 条目组。会话块级(Main/Subagent 查看页共用)的入口是 RenderSessionBlocks
// (cli/transcript.hpp),它把 Items 块折回 FormatTranscriptItems——三处
// (live 快照重打、Ctrl+L 轮重放、查看页)同一张间距表、同一颗折叠开关。
struct TranscriptItem;
lubancode::cli::TranscriptItem ProjectTurnItem(const lubancode::runtime::TurnItemView& item);

}  // namespace lubancode::cli
