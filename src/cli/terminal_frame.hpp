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
// 的 5;N 与 2;R;G;B(近似到最近 16 色)、39/49,其余码忽略),宽字/宽
// 簇占双格并打半格旗标,尾部补默认属性空格铺满 cell_count,超宽整簇截断
// (不劈半个宽字)。纯函数,帧测试钉合同——WriteNativeRow 只管落盘,
// "落什么"全在这里看得见、测得着。
//
// Unicode emoji 治理单 P1(§9.2)的两条硬合同:
//   1. 编码单元映射:conhost 的 CHAR_INFO 一格只装得下一个 UTF-16 码元,
//      非 BMP 码点(含量宽为一列的孤立区域指示符、乐谱符号这类)必须
//      leading/trailing 双格打代理对——单格非 BMP 会让 WriteNativeRow 只
//      写高代理,产出非法 UTF-16。这是编码表示的硬约束,不是量宽策略。
//   2. 多码点字素簇(ZWJ 序列/肤色/组合附标/keycap)装不进单格单码元的
//      cell 模型:默认按"簇首码点占格"降级(宽账仍按整簇),utf16_lossy
//      出参(可空)置 true 告发。PaintInlineFrameNativeRows 见 lossy 把该
//      行改走字节回退路——那边 UTF-8 原文全保真,支持合成渲染的终端自己
//      画,不静默丢附标(单子 §9.2 第二条)。
//
// 逻辑列宽 vs 物理格数(conhost 原生几何分叉单):cell_count 是**逻辑列预
// 算**(VT 口径的列,与布局/折行/光标同一把尺)。产出格数可以超出预算——
// 每个"量宽一列的非 BMP"在 cell 模型恒占两格(编码硬约束),超出的格数
// 恰等于这类字符的个数,NativeColumnForLogical 能把它折算回光标列;整簇
// 截断只按逻辑账判(不为编码开销劈掉行尾),尾部空格至少铺满 cell_count。
std::vector<platform::NativeRowCell> BuildNativeRowCells(std::string_view utf8_text, int cell_count,
                                                         bool* utf16_lossy = nullptr);

// 这行 UTF-8 文本在原生 cell 模型里装不下、须走字节回退路吗?多码点簇/
// 孤立零宽/坏代理为真;纯单码点(含窄非 BMP——它有代理对表示,几何账另
// 算)为假。与 BuildNativeRowCells 的 utf16_lossy 同一把尺,纯函数。
bool NativeRowNeedsByteFallback(std::string_view utf8_text);

// 逻辑列 -> 原生路物理格列:同一段文字,原生路比 VT 路宽出的格数全来自
// "量宽一列的非 BMP"(各多占一格)。光标要钉在与 VT 路相同的文字位置,
// 列号就得带上这段差。logical_column 超出文本逻辑宽时按文本物理末尾返回
// (防御);CSI 配色段不占列。两路一致性的尺:无窄非 BMP 的文本恒等值。
int NativeColumnForLogical(std::string_view utf8_text, int logical_column);

// 一帧在原生直写路上的分路账(纯函数,计数型断言的册):脏行里几行可原
// 生直写、几行含 cell 模型装不下的簇须走字节回退。emoji 常驻 composer 而
// 该行不脏时,byte_fallback_rows 为 0——退化与否按脏行内容判,不按帧里
// 有没有 emoji 判,这是"不整帧全量"的判定核心。
struct InlineFrameNativePlan {
    std::size_t compared_rows = 0;
    std::size_t changed_rows = 0;
    std::size_t native_rows = 0;          // 脏行中可原生直写的行数
    std::size_t byte_fallback_rows = 0;   // 脏行中须走字节回退的行数
};
InlineFrameNativePlan PlanInlineFrameNativePaint(const InlineFrame* previous, const InlineFrame& next);

// 行级双缓冲的原生直写版:diff 的账与 QueueInlineFrameDiff 同一把(没变
// 的行一字不写),脏行按坐标 WriteNativeRow 直写(字符+属性一次落)。
// **帧级判定先于落笔**:哪行直写、哪行字节回退,先算清再动笔——直写行
// 全程不挪光标(8.1 高频轨迹实锤:conhost/WT 的 2026 实现只缓冲文本渲染,
// 批内 CUP 照搬 buffer 光标);字节回退行按 legacy 语义清行+落字(光标会
// 被挪到该行末,这是 cell 模型装不下整簇时的有界例外),帧末仍由调用方
// 一笔 SetCursorPos 权威钉回,本函数不补第二笔。非真 console(GetScreenInfo
// 探不到)或有直写失败:一字节不写、返回 false,调用方退 PaintInlineFrame
// Legacy 老路——字节不落两遍。painted_rows(可空)回带实际落笔的脏行数
// (直写 + 字节回退,帧账审计用);byte_fallback_rows(可空)是其中字节
// 路的行数。
bool PaintInlineFrameNativeRows(const InlineFrame* previous, const InlineFrame& next, int origin_y,
                                std::size_t* painted_rows = nullptr,
                                std::size_t* byte_fallback_rows = nullptr);

// ---------------------------------------------------------------------------
// 思考活动条扫光(思考活动条扫光复活单):一轮砍掉的逐字高亮在原生直写
// 地基上复活——每格 CHAR_INFO 自带属性,高亮只是活动行几个格子的颜色位
// 换了,行文本不动、光标不动、stdout 零字节。档位门在 PlanInlineRepaint
// 的 native_rows(降级档不跑动画);这里只放纯函数。
// ---------------------------------------------------------------------------

// "这一拍不高亮"的记号(降级档/空标签/plain 主题)。
inline constexpr std::size_t kActivityHighlightNone = static_cast<std::size_t>(-1);

// 高亮位计算(纯函数,只算格位不碰布局):标签按显示格铺开,心拍每 200ms
// 前进一格,落在哪格就亮哪个字。宽字占双格——光走到宽字后半格的那一拍
// 亮的还是同一个字(宽字双格占两拍,那拍高亮位没变,调用方据此零落笔);
// 走到标签尽头按总格数回绕。零宽字(组合附标)不占格、永远轮不到亮。
// glyph_widths 为空返回 kActivityHighlightNone。
std::size_t WorkingHighlightGlyph(std::size_t beat, const std::vector<int>& glyph_widths);

// 活动行的重画判据(终端思考活动条单立档,扫光复活单加"高亮位"档):
// 秒数、阶段标签、中断态或高亮位任一变化,这一行才算变了。同一秒、同一
// 高亮位的闲拍(宽字第二格的回绕拍)据此直接收手,帧审计零新增落笔;
// 高亮位一档只在原生直写档有值,降级档恒 kActivityHighlightNone,天然
// 不构成变化源。纯函数,帧测试钉合同。
bool TurnActivityRowChanged(std::string_view old_label, long long old_seconds, bool old_interrupted,
                            std::size_t old_highlight,
                            std::string_view new_label, long long new_seconds, bool new_interrupted,
                            std::size_t new_highlight);

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
