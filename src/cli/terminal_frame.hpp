#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "platform/console.hpp"  // NativeRowCell:原生直写行的单元格(单 2 二轮)
#include "platform/terminal_batch.hpp"

namespace lubancode::cli {

struct WrappedComposerRow {
    std::u32string text;
    std::size_t logical_row = 0;
    std::size_t source_begin = 0;
    std::size_t source_end = 0;
    int display_width = 0;
};

struct WrappedComposerLayout {
    std::vector<WrappedComposerRow> rows;
    std::size_t cursor_row = 0;
    int cursor_col = 0;
};

// 第一物理行容下提示符后的窄区；往后的物理行统一走续行宽度。
WrappedComposerLayout LayoutComposerRows(const std::vector<std::u32string>& logical_lines,
                                         std::size_t cursor_row, std::size_t cursor_col,
                                         int first_width, int continuation_width);

struct InlineFrameRow {
    int x = 0;
    int clear_width = 0;
    bool hard_clear = false;
    std::string text;

    bool operator==(const InlineFrameRow&) const = default;
};

struct InlineFrame {
    std::vector<InlineFrameRow> rows;
    int cursor_x = 0;
    int cursor_row = 0;
};

struct InlineFrameDiffStats {
    std::size_t compared_rows = 0;
    std::size_t changed_rows = 0;
    bool cursor_changed = false;
    bool emitted = false;
};

// 行级双缓冲：没变的行一字不写，变化行整行清后重画；所有命令只进 batch。
InlineFrameDiffStats QueueInlineFrameDiff(platform::TerminalBatch& batch,
                                          const InlineFrame* previous,
                                          const InlineFrame& next, int origin_y);

// ---------------------------------------------------------------------------
// 原生行直写(单 2 二轮·8.2):Windows 真 console 上 footer 行级重画的正路
// ---------------------------------------------------------------------------
// 把一行"UTF-8 正文 + SGR 配色"的 footer 行翻成按格排布的单元格:SGR 译
// 成 16 色属性位(kNativeFg*/kNativeBg*;认 0/1/2/22、30-37/90-97、38/48
// 的 5;N 与 2;R;G;B(近似到最近 16 色)、39/49,其余码忽略),宽字
// (显示宽 2)占双格并打半格旗标,尾部补默认属性空格铺满 cell_count,
// 超宽整字截断(不劈半个宽字)。纯函数,帧测试钉合同——WriteNativeRow
// 只管落盘,"落什么"全在这里看得见、测得着。
std::vector<platform::NativeRowCell> BuildNativeRowCells(std::string_view utf8_text, int cell_count);

// 行级双缓冲的原生直写版:diff 的账与 QueueInlineFrameDiff 同一把(没变
// 的行一字不写),但每一脏行按坐标 WriteNativeRow 直写(字符+属性一次
// 落),**全程不挪光标**——藏光标/CUP 回/显光标那一串从帧序列里清出去
// (8.1 高频轨迹实锤:conhost/WT 的 2026 实现只缓冲文本渲染,批内 CUP 照
// 搬 buffer 光标)。返回 false = 原生路不可用(非真 console/写失败),调
// 用方退 PaintInlineFrameLegacy 老路;painted_rows(可空)回带实际直写的
// 脏行数,帧账审计用。光标末态由调用方一笔 SetCursorPos 权威钉回,本函数
// 绝不碰光标。
bool PaintInlineFrameNativeRows(const InlineFrame* previous, const InlineFrame& next, int origin_y,
                                std::size_t* painted_rows = nullptr);

// 活动行的重画判据(终端思考活动条单·P0 止血):只有秒数、阶段标签或
// 中断态变化,这一行才算变了——逐字扫光撤除后动画不再是变化源,同一秒
// 内的心跳闲拍照此直接收手,帧审计零新增落笔。纯函数,帧测试钉合同。
bool TurnActivityRowChanged(std::string_view old_label, long long old_seconds, bool old_interrupted,
                            std::string_view new_label, long long new_seconds, bool new_interrupted);

// ---------------------------------------------------------------------------
// 帧账的"保锚可见"决策(多智能体真机回归单,纯函数):从 top_row 起
// rows_needed 行要画,可视窗口装不下时怎么腾——
//   pan_rows:窗口底下还有缓冲行(经典 conhost 长缓冲)就平移视口,内容
//             与绝对锚点一个不动;
//   scroll_rows:平移到头(视口贴缓冲区底,WT/ConPTY 常态)剩下的靠"末行
//             写换行"滚内容,锚点要上移对账。
// 执行侧在 console_input.cpp 的 EnsureViewportRowsLocked;决策抽成纯函数
// 是为了单测钉死两套形态的账(沙箱/CI 里没有真长缓冲控制台)。
// ---------------------------------------------------------------------------
struct ViewportRevealPlan {
    int pan_rows = 0;
    int scroll_rows = 0;
};

ViewportRevealPlan ComputeViewportReveal(int buffer_height, int viewport_y, int viewport_height, int top_row,
                                         int rows_needed);

// 忙碌 footer 遇到终端改宽后的旧帧清理计划。Windows Terminal 会把旧行
// reflow，原先的绝对 top_row 随即失效；物理光标却仍跟着输入行走。拿上一
// 帧各逻辑行的显示宽、输入行下标与当前光标行，便能反推出旧框现处，并算
// 出它在新宽度下占了多少物理行。经典控制台若没有 reflow(cursor_y 未动)，
// 仍沿用旧坐标，免得凭空上移。
struct FooterResizeRecoveryPlan {
    int top_row = 0;
    int rows_to_clear = 0;
    bool cursor_reflowed = false;
};

FooterResizeRecoveryPlan ComputeFooterResizeRecovery(
    int previous_top_row, int previous_input_row, int current_cursor_row,
    const std::vector<int>& previous_row_widths, std::size_t input_row_index,
    int input_cursor_column, int current_width);

}  // namespace lubancode::cli
