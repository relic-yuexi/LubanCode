#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include "cli/terminal_frame.hpp"
#include "cli/terminal_port.hpp"
#include "platform/console.hpp"

using lubancode::cli::InlineFrame;
using lubancode::cli::InlineFrameRow;
using lubancode::cli::LayoutComposerRows;
using lubancode::cli::QueueInlineFrameDiff;
using lubancode::cli::BuildNativeRowCells;
using lubancode::cli::PaintInlineFrameNativeRows;
using lubancode::cli::TurnActivityRowChanged;
namespace native_bits = lubancode::platform;
using lubancode::platform::StdoutConsoleProbe;
using lubancode::platform::TerminalBatch;

TEST_CASE("composer layout: first row uses prompt width and continuations use full width") {
    const auto layout = LayoutComposerRows({U"abcdefghij"}, 0, 10, 4, 6);
    REQUIRE(layout.rows.size() == 2);
    CHECK(layout.rows[0].text == U"abcd");
    CHECK(layout.rows[1].text == U"efghij");
    CHECK(layout.cursor_row == 1);
    CHECK(layout.cursor_col == 6);
}

TEST_CASE("composer layout: explicit lines and wide characters keep cursor on the right row") {
    const auto layout = LayoutComposerRows({U"ab中d", U"第二行"}, 1, 2, 4, 5);
    REQUIRE(layout.rows.size() == 4);
    CHECK(layout.rows[0].text == U"ab中");
    CHECK(layout.rows[1].text == U"d");
    CHECK(layout.rows[2].text == U"第二");
    CHECK(layout.rows[3].text == U"行");
    CHECK(layout.cursor_row == 3);
    CHECK(layout.cursor_col == 0);
}

TEST_CASE("inline frame diff: unchanged rows are skipped and all changes share one batch") {
    InlineFrame previous{{InlineFrameRow{2, 8, false, "old"},
                          InlineFrameRow{0, 10, true, "rule"}},
                         5, 0};
    InlineFrame next{{InlineFrameRow{2, 8, false, "new"},
                      InlineFrameRow{0, 10, true, "rule"}},
                     5, 0};

    TerminalBatch batch(0, 0, /*synchronized_output=*/true);
    const auto stats = QueueInlineFrameDiff(batch, &previous, next, 7);
    const std::string bytes = batch.Finish();
    CHECK(stats.compared_rows == 2);
    CHECK(stats.changed_rows == 1);
    CHECK(stats.emitted);
    CHECK(bytes.find("old") == std::string::npos);
    CHECK(bytes.find("new") != std::string::npos);
    CHECK(bytes.find("\x1b[?25l") < bytes.find("new"));
    CHECK(bytes.find("\x1b[?25h") > bytes.find("new"));
    CHECK(bytes.find("\x1b[8;3H") != std::string::npos);  // origin 7 + row 0, x=2
    CHECK(bytes.find("\x1b[?2026h") == 0);
    CHECK(bytes.ends_with("\x1b[?2026l"));
}

TEST_CASE("inline frame diff: removed rows are cleared without repainting survivors") {
    InlineFrame previous{{InlineFrameRow{2, 8, false, "same"},
                          InlineFrameRow{0, 10, true, "gone"}},
                         2, 0};
    InlineFrame next{{InlineFrameRow{2, 8, false, "same"}}, 2, 0};

    TerminalBatch batch(0, 0, /*synchronized_output=*/true);
    const auto stats = QueueInlineFrameDiff(batch, &previous, next, 3);
    const std::string bytes = batch.Finish();
    CHECK(stats.changed_rows == 1);
    CHECK(bytes.find("gone") == std::string::npos);
    CHECK(bytes.find("\x1b[0m\x1b[5;1H\x1b[10X") != std::string::npos);
}

TEST_CASE("inline frame diff: a large frame repaints only the changed row") {
    InlineFrame previous;
    for (int i = 0; i < 1000; ++i) {
        previous.rows.push_back(InlineFrameRow{0, 120, false, "row " + std::to_string(i)});
    }
    previous.cursor_x = 4;
    previous.cursor_row = 999;
    InlineFrame next = previous;
    next.rows[500].text = "changed";

    TerminalBatch batch(0, 0, /*synchronized_output=*/true);
    const auto stats = QueueInlineFrameDiff(batch, &previous, next, 0);
    const std::string bytes = batch.Finish();
    CHECK(stats.compared_rows == 1000);
    CHECK(stats.changed_rows == 1);
    CHECK(bytes.find("changed") != std::string::npos);
    CHECK(bytes.find("row 499") == std::string::npos);
    CHECK(bytes.size() < 128);
}

TEST_CASE("terminal batch converts screen-buffer coordinates to viewport coordinates") {
    TerminalBatch batch(/*viewport_x=*/4, /*viewport_y=*/7, /*synchronized_output=*/false);
    batch.MoveTo(6, 9);
    const std::string bytes = batch.Finish();
    CHECK(bytes.find("\x1b[3;3H") != std::string::npos);
}

// ---- 终端思考活动条单(P0 治根):三档能力分开建模后的选路与输出合同 ----
// 同步输出是显式能力入参:没确认 DEC 2026 的宿主,批里一枚 2026 标记都
// 不许有;确认了才整批原子提交。

TEST_CASE("terminal batch: sync=true 整批包 2026,sync=false 一枚不包") {
    TerminalBatch sync_batch(0, 0, /*synchronized_output=*/true);
    sync_batch.Write("x");
    const std::string sync_bytes = sync_batch.Finish();
    CHECK(sync_bytes.find("\x1b[?2026h") == 0);
    CHECK(sync_bytes.ends_with("\x1b[?2026l"));

    TerminalBatch plain_batch(0, 0, /*synchronized_output=*/false);
    plain_batch.Write("x");
    const std::string plain_bytes = plain_batch.Finish();
    CHECK(plain_bytes.find("2026") == std::string::npos);
    CHECK(plain_bytes == "x");
}

TEST_CASE("inline repaint plan: 8.2 二轮重裁——Windows 恒原生直写,VT 批只剩 POSIX") {
    // 无 VT(老 conhost/管道):原生兜底路,谈不上同步输出。
    StdoutConsoleProbe none;
    none.is_console = true;
    none.vt_enabled = false;
    none.sync_output = false;
    const auto plan_none = lubancode::platform::PlanInlineRepaint(none);
    CHECK_FALSE(plan_none.vt_batch);
    CHECK_FALSE(plan_none.sync_output);
    // 扫光档(思考活动条扫光复活单):Windows 真 console 有缓冲区可直写;
    // POSIX 无原生路,恒关——降级档不跑动画。
#ifdef _WIN32
    CHECK(plan_none.native_rows);
#else
    CHECK_FALSE(plan_none.native_rows);
#endif

    // 普通 VT + 未确认 2026:Windows 恒原生直写行(8.1 实锤批内 CUP 搬
    // buffer 光标,2026 只缓冲文本渲染救不了);POSIX 无原生路,退 VT 批
    // 不包 2026(低频档,接受已知中间态)。
    StdoutConsoleProbe vt_only;
    vt_only.is_console = true;
    vt_only.vt_enabled = true;
    vt_only.sync_output = false;
    const auto plan_vt_only = lubancode::platform::PlanInlineRepaint(vt_only);
#ifdef _WIN32
    CHECK_FALSE(plan_vt_only.vt_batch);
    CHECK_FALSE(plan_vt_only.sync_output);
    CHECK(plan_vt_only.native_rows);
#else
    CHECK(plan_vt_only.vt_batch);
    CHECK_FALSE(plan_vt_only.sync_output);
    CHECK_FALSE(plan_vt_only.native_rows);
#endif

    // 普通 VT + 确认 2026:8.2 裁决"2026 保护光标"的前提已被实验否定——
    // Windows 上一律 native 写行(照样全 false);POSIX 才是 VT 批 + 同步
    // 输出(文本撕裂它还是防得住的)。
    StdoutConsoleProbe vt_sync;
    vt_sync.is_console = true;
    vt_sync.vt_enabled = true;
    vt_sync.sync_output = true;
    const auto plan_vt_sync = lubancode::platform::PlanInlineRepaint(vt_sync);
#ifdef _WIN32
    CHECK_FALSE(plan_vt_sync.vt_batch);
    CHECK_FALSE(plan_vt_sync.sync_output);
    CHECK(plan_vt_sync.native_rows);
#else
    CHECK(plan_vt_sync.vt_batch);
    CHECK(plan_vt_sync.sync_output);
    CHECK_FALSE(plan_vt_sync.native_rows);
#endif

    // 防御:VT 都不开却报 sync(矛盾输入),一律按无 VT 处理。
    StdoutConsoleProbe broken;
    broken.is_console = true;
    broken.vt_enabled = false;
    broken.sync_output = true;
    const auto plan_broken = lubancode::platform::PlanInlineRepaint(broken);
    CHECK_FALSE(plan_broken.vt_batch);
    CHECK_FALSE(plan_broken.sync_output);

    // 管道/重定向(is_console=false):没有屏幕缓冲区,Windows 上 vt_batch
    // 虽也恒 false,但原生直写同样落空——扫光档必须关,管道里不跑动画。
    StdoutConsoleProbe piped;
    piped.is_console = false;
    piped.vt_enabled = false;
    piped.sync_output = false;
    const auto plan_piped = lubancode::platform::PlanInlineRepaint(piped);
    CHECK_FALSE(plan_piped.vt_batch);
    CHECK_FALSE(plan_piped.sync_output);
    CHECK_FALSE(plan_piped.native_rows);
}

TEST_CASE("frame diff: 只有活动行变时,批的末笔光标恒归 composer 输入位") {
    // 活动行(行 0)秒数变、composer 各行与光标纹丝不动:批里画的最后一
    // 个 CUP 必须是 composer 的软换行坐标,其后只有 ShowCursor(同步档再
    // 跟 2026l),没有第二个光标恢复路。
    InlineFrame previous{{InlineFrameRow{0, 24, false, "activity (10s)"},
                          InlineFrameRow{0, 40, true, "rule"},
                          InlineFrameRow{2, 10, false, "> input"}},
                         4, 2};
    InlineFrame next = previous;
    next.rows[0].text = "activity (11s)";

    TerminalBatch batch(0, 0, /*synchronized_output=*/true);
    const auto stats = QueueInlineFrameDiff(batch, &previous, next, 7);
    CHECK(stats.changed_rows == 1);
    CHECK_FALSE(stats.cursor_changed);
    const std::string bytes = batch.Finish();
    // composer 光标:x=4,row=7+2=9 → CUP 10;5(1 基)。末笔合同:composer
    // 归位是最后一个定位命令,其后只有 ShowCursor 与同步输出收尾,没有
    // 第二条光标恢复路。
    const std::size_t cursor_cup = bytes.rfind("\x1b[10;5H");
    REQUIRE(cursor_cup != std::string::npos);
    const std::string tail = bytes.substr(cursor_cup);
    CHECK(tail == "\x1b[10;5H\x1b[?25h\x1b[?2026l");
}

TEST_CASE("turn activity row: 同一秒零变化,秒数/标签/中断态/高亮位变才重画") {
    using lubancode::cli::TurnActivityRowChanged;
    using lubancode::cli::kActivityHighlightNone;
    // 同一秒、同一标签、同一中断态、同一高亮位:不是变化,闲拍零落笔。
    CHECK_FALSE(TurnActivityRowChanged("思考中", 10, false, 0, "思考中", 10, false, 0));
    // 高亮位变(扫光移格):重画——扫光复活后活动行的新变化源。
    CHECK(TurnActivityRowChanged("思考中", 10, false, 0, "思考中", 10, false, 1));
    // 高亮位没变(宽字双格的第二拍):不算变化,帧账不进无变化帧。
    CHECK_FALSE(TurnActivityRowChanged("思考中", 10, false, 1, "思考中", 10, false, 1));
    // 有高亮 <-> 无高亮(降级档首拍/末拍):重画。
    CHECK(TurnActivityRowChanged("思考中", 10, false, kActivityHighlightNone, "思考中", 10, false, 0));
    CHECK(TurnActivityRowChanged("思考中", 10, false, 0, "思考中", 10, false, kActivityHighlightNone));
    CHECK_FALSE(TurnActivityRowChanged("思考中", 10, false, kActivityHighlightNone, "思考中", 10, false,
                                       kActivityHighlightNone));
    // 旧三档照旧:秒数/标签/中断态。
    CHECK(TurnActivityRowChanged("思考中", 10, false, 0, "思考中", 11, false, 0));
    CHECK(TurnActivityRowChanged("思考中", 10, false, 0, "Stopping...", 10, false, 0));
    CHECK(TurnActivityRowChanged("思考中", 10, false, 0, "思考中", 10, true, 0));
}

// ---- 思考活动条扫光复活单:高亮位轮换纯函数 --------------------------------
// 光按显示格走:宽字双格占两拍(第二拍同字,调用方零落笔),尽头回绕归零,
// 单字标签恒亮同一字(动画自然消失),空标签没字可亮。

TEST_CASE("working highlight: 按显示格逐拍轮换,宽字双格占两拍,回绕归零") {
    using lubancode::cli::WorkingHighlightGlyph;
    using lubancode::cli::kActivityHighlightNone;
    // "思考中":三个宽字 = 六个显示格。光走格:思(两格,两拍同亮)→考→中,
    // 第六格走完按总格数回绕回"思"。
    const std::vector<int> cn{2, 2, 2};
    CHECK(WorkingHighlightGlyph(0, cn) == 0);
    CHECK(WorkingHighlightGlyph(1, cn) == 0);  // 思的第二格:同字,占两拍
    CHECK(WorkingHighlightGlyph(2, cn) == 1);
    CHECK(WorkingHighlightGlyph(3, cn) == 1);
    CHECK(WorkingHighlightGlyph(4, cn) == 2);
    CHECK(WorkingHighlightGlyph(5, cn) == 2);
    CHECK(WorkingHighlightGlyph(6, cn) == 0);  // 边界回绕:第六拍回头亮第一字
    CHECK(WorkingHighlightGlyph(7, cn) == 0);
    CHECK(WorkingHighlightGlyph(12, cn) == 0);  // 两轮之后仍对齐

    // 混排 "a思b":1+2+1 = 4 格。
    const std::vector<int> mixed{1, 2, 1};
    CHECK(WorkingHighlightGlyph(0, mixed) == 0);
    CHECK(WorkingHighlightGlyph(1, mixed) == 1);
    CHECK(WorkingHighlightGlyph(2, mixed) == 1);  // 思的第二格
    CHECK(WorkingHighlightGlyph(3, mixed) == 2);
    CHECK(WorkingHighlightGlyph(4, mixed) == 0);  // 回绕

    // 标签长度 1(单字):每一拍都亮同一个字——高亮位永不变,零动画帧。
    const std::vector<int> single{2};
    CHECK(WorkingHighlightGlyph(0, single) == 0);
    CHECK(WorkingHighlightGlyph(1, single) == 0);
    CHECK(WorkingHighlightGlyph(9, single) == 0);

    // 单字窄标签同理。
    const std::vector<int> narrow{1};
    CHECK(WorkingHighlightGlyph(0, narrow) == 0);
    CHECK(WorkingHighlightGlyph(5, narrow) == 0);

    // 空标签:没字可亮。
    const std::vector<int> empty;
    CHECK(WorkingHighlightGlyph(0, empty) == kActivityHighlightNone);

    // 零宽字(组合附标):不占格,永远轮不到亮。
    const std::vector<int> zero_width{1, 0, 1};
    CHECK(WorkingHighlightGlyph(0, zero_width) == 0);
    CHECK(WorkingHighlightGlyph(1, zero_width) == 2);
    CHECK(WorkingHighlightGlyph(2, zero_width) == 0);
}

// ---- 单 2 二轮(8.2):原生行直写的纯函数合同 ----
// BuildNativeRowCells 是"落什么"的全部账:SGR 译 16 色位、宽字双格、尾部
// 铺默认空格、整字截断。WriteNativeRow 只管落盘,真 console 行为由 driver
// G0 高频轨迹幕钉(单测环境没有真控制台)。

TEST_CASE("native row cells: 无 SGR 的明文整行默认属性,尾部铺空格") {
    const auto cells = BuildNativeRowCells("ab", 5);
    REQUIRE(cells.size() == 5);
    CHECK(cells[0].ch == U'a');
    CHECK(cells[1].ch == U'b');
    CHECK(cells[2].ch == U' ');
    CHECK(cells[3].ch == U' ');
    CHECK(cells[4].ch == U' ');
    for (const auto& cell : cells) {
        CHECK(cell.attr == 0);  // 0 = 默认属性记号,WriteNativeRow 落盘时换控制台默认
    }
}

TEST_CASE("native row cells: 主题 SGR 译 16 色位,加粗补亮、压暗去亮") {
    using namespace lubancode::platform;
    const std::uint16_t fg_gray = kNativeFgRed | kNativeFgGreen | kNativeFgBlue;              // "2;37" 暗白
    const std::uint16_t fg_cyan = kNativeFgGreen | kNativeFgBlue;                             // "36" 青
    const std::uint16_t fg_bold_green = kNativeFgGreen | kNativeFgIntensity;                  // "1;32" 亮绿
    const std::uint16_t fg_bold_red = kNativeFgRed | kNativeFgIntensity;                      // "1;31" 亮红
    // dark 主题 stats("\x1b[2;37m") + spinner("\x1b[36m") + error("\x1b[1;31m") + 复位。
    const std::string row = std::string("\x1b[2;37m") + "灰" + "\x1b[36m" + "青" +
                            "\x1b[1;31m" + "红" + "\x1b[0m" + "默认";
    const auto cells = BuildNativeRowCells(row, 40);
    REQUIRE(cells.size() == 40);
    CHECK(cells[0].ch == U'灰');  // 灰(宽字前半格)
    CHECK(cells[0].attr == (fg_gray | kNativeCellLeading));
    CHECK(cells[1].attr == (fg_gray | kNativeCellTrailing));
    CHECK(cells[2].ch == U'青');  // 青
    CHECK(cells[2].attr == (fg_cyan | kNativeCellLeading));
    CHECK(cells[4].ch == U'红');  // 红
    CHECK(cells[4].attr == (fg_bold_red | kNativeCellLeading));
    // 复位之后回默认属性记号。
    CHECK(cells[6].ch == U'默');
    CHECK(cells[6].attr == kNativeCellLeading);
    // "1;32" 先加粗后给色:加粗在前景还是默认时记账不落位,色号一到补上
    // 强度——亮绿不许褪成暗绿。
    const auto bold_green = BuildNativeRowCells("\x1b[1;32mG", 3);
    CHECK(bold_green[0].attr == (kNativeFgGreen | kNativeFgIntensity));
    CHECK(fg_bold_green == (kNativeFgGreen | kNativeFgIntensity));
    // 90 系亮色自带强度,不被"未加粗"抹掉。
    const auto bright = BuildNativeRowCells("\x1b[91mX", 3);
    CHECK(bright[0].attr == (kNativeFgRed | kNativeFgIntensity));
}

TEST_CASE("native row cells: 宽字占双格带半格旗标,放不下整字就截断") {
    using namespace lubancode::platform;
    // "• 思考中" —— bullet(1 格)+ 空格 + 三个宽字(各 2 格)。
    const std::string bullet = "\xe2\x80\xa2 \xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad";  // bullet + 空格 + 思考中
    const auto full = BuildNativeRowCells(bullet, 12);
    REQUIRE(full.size() == 12);
    CHECK(full[0].ch == 0x2022);
    CHECK(full[0].attr == 0);
    CHECK(full[2].ch == 0x601d);  // 思
    CHECK(full[2].attr == kNativeCellLeading);
    CHECK(full[3].attr == kNativeCellTrailing);
    CHECK(full[6].ch == 0x4e2d);  // 中:第三个宽字,占 [6][7] 双格
    CHECK(full[7].attr == kNativeCellTrailing);
    CHECK(full[8].ch == U' ');
    CHECK(full[11].ch == U' ');
    // 只有 3 格:bullet + 空格之后放不下整个"思"(2 格),整字截断补空格。
    const auto cut = BuildNativeRowCells(bullet, 3);
    REQUIRE(cut.size() == 3);
    CHECK(cut[0].ch == 0x2022);
    CHECK(cut[1].ch == U' ');
    CHECK(cut[2].ch == U' ');
}

TEST_CASE("native row cells: 256 色近似到最近 16 色,非 SGR 的 CSI 序列直接跳过") {
    using namespace lubancode::platform;
    // 38;5;196 = 纯红 -> 亮红(9 号)。
    const auto red = BuildNativeRowCells("\x1b[38;5;196mR", 2);
    CHECK(red[0].attr == (kNativeFgRed | kNativeFgIntensity));
    // 38;2;255;0;0 同为纯红,殊途同归。
    const auto rgb = BuildNativeRowCells("\x1b[38;2;255;0;0mR", 2);
    CHECK(rgb[0].attr == (kNativeFgRed | kNativeFgIntensity));
    // 夹杂非 SGR 的 CSI(光标类):不产格、不动色,只吃字节。
    const auto csi = BuildNativeRowCells("\x1b[2J\x1b[36mC", 4);
    REQUIRE(csi.size() == 4);
    CHECK(csi[0].ch == U'C');
    CHECK(csi[0].attr == (kNativeFgGreen | kNativeFgBlue));
}

TEST_CASE("native frame paint: 管道拒直写、越界行无笔可落,真写路不碰光标") {
    InlineFrame previous{{InlineFrameRow{0, 10, false, "old"}}, 2, 0};
    InlineFrame next{{InlineFrameRow{0, 10, false, "new"}}, 2, 0};
    if (lubancode::platform::ProbeStdoutConsole().is_console) {
        // 手工在真控制台直跑测试本体的情形:屏内行不许写(单测不涂屏),
        // 用越界行验"无笔可落不算失败"与脏行记账。
        std::size_t rows = 0;
        CHECK(PaintInlineFrameNativeRows(nullptr, next, 1000000, &rows));
        CHECK(rows == 1);
    } else {
        // CTest/管道环境(常态):WriteNativeRow 拿不到控制台缓冲区,
        // PaintInlineFrameNativeRows 必须如实返回 false 让调用方退
        // PaintInlineFrameLegacy——直写路写失败时装死才是真祸。POSIX 无
        // 原生路,同样恒 false。
        std::size_t painted_rows = 99;
        CHECK_FALSE(PaintInlineFrameNativeRows(&previous, next, 5, &painted_rows));
        CHECK(painted_rows == 0);
    }
    // "写行不动光标"的正面合同在真 console 上由 driver G0 高频轨迹幕钉
    //(几十 kHz 采样,任何 CUP 中间态现形);纯函数侧能钉的半边是:构建
    // 器产物只有"字符+属性",压根不存在光标语义。
}

// ---- 帧账"保锚可见"决策(多智能体真机回归单):纯函数钉死两套控制台形态的账 ----
// 长缓冲(conhost 9001 行/驱动器 120×400):窗口底下有余量,只平移视口,
// 内容与锚点一个不动;贴缓冲底(WT/ConPTY 常态):全靠滚内容,锚点要对账。

TEST_CASE("viewport reveal: 已在可视区,一笔不动") {
    const auto plan = lubancode::cli::ComputeViewportReveal(/*buffer_height=*/400, /*viewport_y=*/0,
                                                            /*viewport_height=*/30, /*top_row=*/10,
                                                            /*rows_needed=*/8);
    CHECK(plan.pan_rows == 0);
    CHECK(plan.scroll_rows == 0);
}

TEST_CASE("viewport reveal: 长缓冲窗口底下有余量,全走平移视口") {
    // 窗口 0..29,帧 25..34 伸出 5 行;缓冲 400 行,底下 370 行余量。
    const auto plan = lubancode::cli::ComputeViewportReveal(400, 0, 30, 25, 10);
    CHECK(plan.pan_rows == 5);
    CHECK(plan.scroll_rows == 0);
}

TEST_CASE("viewport reveal: 视口贴缓冲底(WT/ConPTY 形态),全走滚内容") {
    // 缓冲即窗口(30 行),帧 25..34 伸出 5 行——平移没余量,只能滚。
    const auto plan = lubancode::cli::ComputeViewportReveal(30, 0, 30, 25, 10);
    CHECK(plan.pan_rows == 0);
    CHECK(plan.scroll_rows == 5);
}

TEST_CASE("viewport reveal: 余量不够时先平移再滚剩余") {
    // 缓冲 32 行,窗口 0..29,帧 25..34 伸 5 行——底下只有 2 行,平 2 滚 3。
    const auto plan = lubancode::cli::ComputeViewportReveal(32, 0, 30, 25, 10);
    CHECK(plan.pan_rows == 2);
    CHECK(plan.scroll_rows == 3);
}

TEST_CASE("viewport reveal: 窗口不在缓冲顶(用户上滚过)按窗口实位算") {
    // 窗口 100..129,帧 95..102 已在区内不动;帧 128..135 伸 6 行平 6。
    const auto in_view = lubancode::cli::ComputeViewportReveal(400, 100, 30, 95, 8);
    CHECK(in_view.pan_rows == 0);
    CHECK(in_view.scroll_rows == 0);
    const auto spill = lubancode::cli::ComputeViewportReveal(400, 100, 30, 128, 8);
    CHECK(spill.pan_rows == 6);
    CHECK(spill.scroll_rows == 0);
}

TEST_CASE("viewport reveal: 退化入参不炸——零行需求/零高缓冲/窗口报得比缓冲长") {
    const auto none = lubancode::cli::ComputeViewportReveal(400, 0, 30, 25, 0);
    CHECK(none.pan_rows == 0);
    CHECK(none.scroll_rows == 0);
    const auto no_buffer = lubancode::cli::ComputeViewportReveal(0, 0, 30, 25, 8);
    CHECK(no_buffer.pan_rows == 0);
    CHECK(no_buffer.scroll_rows == 0);
    // viewport_height 报 0(未知):按缓冲高兜底,贴底形态走滚。
    const auto fallback = lubancode::cli::ComputeViewportReveal(30, 0, 0, 25, 10);
    CHECK(fallback.pan_rows == 0);
    CHECK(fallback.scroll_rows == 5);
    // 窗口报得比缓冲还长:夹回缓冲底,不出现负平移。
    const auto clamped = lubancode::cli::ComputeViewportReveal(30, 10, 30, 25, 10);
    CHECK(clamped.pan_rows == 0);
    CHECK(clamped.scroll_rows == 5);
}

TEST_CASE("footer resize:经典控制台未 reflow 时沿用旧绝对坐标") {
    const std::vector<int> rows{18, 119, 12, 119, 90};
    const auto plan = lubancode::cli::ComputeFooterResizeRecovery(
        /*previous_top_row=*/20, /*previous_input_row=*/22, /*current_cursor_row=*/22,
        rows, /*input_row_index=*/2, /*input_cursor_column=*/12, /*current_width=*/80);
    CHECK(plan.top_row == 20);
    CHECK(plan.rows_to_clear == 5);
    CHECK_FALSE(plan.cursor_reflowed);
}

TEST_CASE("footer resize:Windows Terminal reflow 后从输入光标反推旧框") {
    // 120 -> 80:上横线、下横线、状态行各折成两行。输入行由 22 移到 23，
    // 反推后旧框仍从 20 起，共占 8 行；不能丢锚后另画一份。
    const std::vector<int> rows{18, 119, 12, 119, 90};
    const auto plan = lubancode::cli::ComputeFooterResizeRecovery(
        /*previous_top_row=*/20, /*previous_input_row=*/22, /*current_cursor_row=*/23,
        rows, /*input_row_index=*/2, /*input_cursor_column=*/12, /*current_width=*/80);
    CHECK(plan.top_row == 20);
    CHECK(plan.rows_to_clear == 8);
    CHECK(plan.cursor_reflowed);
}

TEST_CASE("footer resize:放宽时连同上方正文位移反推新顶行") {
    const std::vector<int> rows{18, 79, 12, 79, 70};
    const auto plan = lubancode::cli::ComputeFooterResizeRecovery(
        /*previous_top_row=*/20, /*previous_input_row=*/22, /*current_cursor_row=*/18,
        rows, /*input_row_index=*/2, /*input_cursor_column=*/12, /*current_width=*/120);
    CHECK(plan.top_row == 16);
    CHECK(plan.rows_to_clear == 5);
    CHECK(plan.cursor_reflowed);
}

TEST_CASE("footer resize:输入光标自身折行也计入反推") {
    const std::vector<int> rows{20, 99, 95, 99, 80};
    const auto plan = lubancode::cli::ComputeFooterResizeRecovery(
        /*previous_top_row=*/10, /*previous_input_row=*/12, /*current_cursor_row=*/14,
        rows, /*input_row_index=*/2, /*input_cursor_column=*/95, /*current_width=*/60);
    CHECK(plan.top_row == 10);  // 前两行占 1+2，输入光标又折 1 行：14-3-1
    CHECK(plan.rows_to_clear == 9);
    CHECK(plan.cursor_reflowed);
}

// ---------------------------------------------------------------------------
// Unicode emoji 治理单 P1(§9.2):字素级 composer 折行 + native 行 cells 的
// 编码单元映射。断言的是"实际 UTF-16 合法性",不只断言 cell 数。
// ---------------------------------------------------------------------------

namespace {

// 按 WriteNativeRow(console_win.cpp,禁改文件)的现行语义把 cells 重组回
// UTF-16 码元流:BMP 码点一格一码元;非 BMP 码点按 trailing 旗标拆高/低
// 代理。这是落盘字节的真实形状,合法性在这里验,不验 cell 计数。
std::u16string CellsToUtf16(const std::vector<native_bits::NativeRowCell>& cells) {
    std::u16string out;
    for (const auto& cell : cells) {
        if (cell.ch >= 0x10000) {
            const std::uint32_t v = static_cast<std::uint32_t>(cell.ch) - 0x10000;
            const bool trailing = (cell.attr & native_bits::kNativeCellTrailing) != 0;
            out.push_back(static_cast<char16_t>(trailing ? (0xDC00 + (v & 0x3FF))
                                                         : (0xD800 + (v >> 10))));
        } else {
            out.push_back(static_cast<char16_t>(cell.ch));
        }
    }
    return out;
}

// UTF-16 合法性:高代理必紧跟低代理,低代理必有高代理在前(无孤立代理)。
bool Utf16WellFormed(const std::u16string& text) {
    bool expect_low = false;
    for (const char16_t unit : text) {
        if (expect_low) {
            if (unit < 0xDC00 || unit > 0xDFFF) {
                return false;  // 高代理后没跟低代理
            }
            expect_low = false;
        } else if (unit >= 0xD800 && unit <= 0xDBFF) {
            expect_low = true;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            return false;  // 凭空出现低代理
        }
    }
    return !expect_low;
}

}  // namespace

TEST_CASE("LayoutComposerRows: ZWJ 序列整簇装行,不拆") {
    // "a" + 👩‍💻 + "b":首行宽 3 恰好装 a+整簇,行尾 b 另起。
    const std::u32string tech = {0x1F469, 0x200D, 0x1F4BB};
    const auto layout = LayoutComposerRows({U"a" + tech + U"b"}, 0, 5, 3, 5);
    REQUIRE(layout.rows.size() == 2);
    CHECK(layout.rows[0].text == U"a" + tech);
    CHECK(layout.rows[0].display_width == 3);
    CHECK(layout.rows[1].text == U"b");
}

TEST_CASE("LayoutComposerRows: 恰满一行后的零宽附标不挤丢(§9.2 第三条)") {
    // capacity=2:第一行恰装"中"(2 列);第二簇"中+重音"整簇两列,重音跟
    // 随基础字进同一物理行,不会在容量到点时被丢掉或挤成下一行孤儿。
    const std::u32string line = {U'中', U'中', 0x0301};
    const auto layout = LayoutComposerRows({line}, 0, 3, 2, 2);
    REQUIRE(layout.rows.size() == 2);
    CHECK(layout.rows[0].text == U"中");
    CHECK(layout.rows[1].text == std::u32string{U'中', 0x0301});
    CHECK(layout.rows[1].display_width == 2);
}

TEST_CASE("LayoutComposerRows: 旗帜与家庭组合按整簇宽度折行") {
    const std::u32string flag = {0x1F1E8, 0x1F1F3};
    const std::u32string family = {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466};
    // 旗帜(2)+家庭(2):首行宽 4 装两簇;第三个家庭另起。
    const std::u32string line = flag + family + family;
    const auto layout = LayoutComposerRows({line}, 0, line.size(), 4, 4);
    REQUIRE(layout.rows.size() == 2);
    CHECK(layout.rows[0].text == flag + family);
    CHECK(layout.rows[0].display_width == 4);
    CHECK(layout.rows[1].text == family);
}

TEST_CASE("native row cells: 旗帜双格打代理对,落盘 UTF-16 合法(§9.2 缺口)") {
    using namespace lubancode::platform;
    // 旧实现把每枚区域指示符量成一列:各占单格、无 trailing 旗标,
    // WriteNativeRow 每格只写高代理 → 落盘两枚孤立高代理,UTF-16 非法。
    // 现在旗帜整簇两格、ch=簇首指示符、leading/trailing 打一对代理——
    // 合法配对(第二枚指示符不占独立格,是多码点簇在 cell 模型里的已知
    // 显示降级,lossy 出参告发,调用方退 legacy 字节流路保真)。
    const auto cells = BuildNativeRowCells("\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3", 2);  // 🇨🇳
    REQUIRE(cells.size() == 2);
    CHECK((cells[0].attr & kNativeCellLeading) != 0);
    CHECK((cells[1].attr & kNativeCellTrailing) != 0);
    const std::u16string utf16 = CellsToUtf16(cells);
    CHECK(Utf16WellFormed(utf16));
    REQUIRE(utf16.size() == 2);  // 恰一对代理:高 D83C + 低 DDE8
    CHECK(utf16[0] == 0xD83C);
    CHECK(utf16[1] == 0xDDE8);
    // 多码点簇(旗帜两码点)如实报 lossy。
    bool lossy = false;
    (void)BuildNativeRowCells("\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3", 2, &lossy);
    CHECK(lossy);
}

TEST_CASE("native row cells: 窄非 BMP 也双格,不再只写高代理") {
    // U+1D11E(音乐记谱,量宽一列):编码上仍是代理对,CHAR_INFO 单格装
    // 不下,原生路恒双格——这是 cell 模型的编码硬约束,不是量宽策略。
    const auto cells = BuildNativeRowCells("\xF0\x9D\x84\x9E", 2);
    REQUIRE(cells.size() == 2);
    CHECK(CellsToUtf16(cells) == std::u16string{0xD834, 0xDD1E});
    CHECK(Utf16WellFormed(CellsToUtf16(cells)));
}

TEST_CASE("native row cells: emoji 簇与混排整行,UTF-16 全程合法") {
    using namespace lubancode::platform;
    // "x" + 👩‍💻 + "中" + 😀,铺 12 格:簇宽账 1+2+2+2 = 7,尾部空格补齐。
    const std::string row = "x\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB\xE4\xB8\xAD\xF0\x9F\x98\x80";
    const auto cells = BuildNativeRowCells(row, 12);
    REQUIRE(cells.size() == 12);
    CHECK(cells[0].ch == U'x');
    CHECK(cells[1].ch == 0x1F469);  // 👩 簇首占格(簇跟随者报 lossy,见下)
    CHECK((cells[1].attr & kNativeCellLeading) != 0);
    CHECK(cells[3].ch == U'中');
    CHECK(cells[5].ch == 0x1F600);
    CHECK(cells[7].ch == U' ');
    CHECK(Utf16WellFormed(CellsToUtf16(cells)));
}

TEST_CASE("native row cells: 多码点簇报 lossy,不静默丢附标(§9.2)") {
    using namespace lubancode::platform;
    // e+组合重音:单格装不下重音,lossy 告发(调用方退 legacy 字节流路)。
    bool lossy = true;
    const auto accent = BuildNativeRowCells("e\xCC\x81", 4, &lossy);
    CHECK(lossy);
    CHECK(accent[0].ch == U'e');
    CHECK(Utf16WellFormed(CellsToUtf16(accent)));
    // 肤色修饰同样装不下。
    lossy = false;
    const auto skin = BuildNativeRowCells("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", 2, &lossy);  // 👍🏽
    CHECK(lossy);
    CHECK(skin[0].ch == 0x1F44D);
    CHECK(Utf16WellFormed(CellsToUtf16(skin)));
    // 纯 ASCII/单码点字不 lossy。
    lossy = true;
    (void)BuildNativeRowCells("ab", 4, &lossy);
    CHECK_FALSE(lossy);
    lossy = true;
    (void)BuildNativeRowCells("\xF0\x9F\x98\x80", 2, &lossy);  // 单码点 😀
    CHECK_FALSE(lossy);
    // 空行(清行路)永不 lossy。
    lossy = true;
    (void)BuildNativeRowCells(std::string_view(), 4, &lossy);
    CHECK_FALSE(lossy);
}

TEST_CASE("native row cells: 孤立零宽与坏代理的可见回退,UTF-16 仍合法") {
    using namespace lubancode::platform;
    // 孤立组合重音:不静默丢——占一格报 lossy。
    bool lossy = false;
    const auto lone = BuildNativeRowCells("\xCC\x81", 2, &lossy);
    CHECK(lossy);
    CHECK(lone[0].ch == 0x0301);
    CHECK(Utf16WellFormed(CellsToUtf16(lone)));
    // UTF-8 编出的孤立代理(ED A0 80 = U+D800):替换 U+FFFD,不落非法对。
    lossy = false;
    const auto bad = BuildNativeRowCells("\xED\xA0\x80", 2, &lossy);
    CHECK(lossy);
    CHECK(bad[0].ch == 0xFFFD);
    CHECK(Utf16WellFormed(CellsToUtf16(bad)));
}

// ---------------------------------------------------------------------------
// conhost 原生几何分叉单:逻辑列宽 vs 物理格数分离账 + 字节回退的帧级判定
// ---------------------------------------------------------------------------

TEST_CASE("native row cells: 逻辑预算按 VT 口径,窄非 BMP 的编码开销不劈行尾") {
    using namespace lubancode::platform;
    // "ab" + U+1D11E(量宽一列的非 BMP)+ "c":逻辑 4 列、物理 5 格。旧账
    // 把 cell_count 当物理帽,行尾 'c' 会被编码开销挤掉;分离账后逻辑预算
    // 装得下就一个不丢,超出的格数恰等于窄非 BMP 的个数。
    const std::string row = "ab\xF0\x9D\x84\x9E"
                            "c";
    const auto cells = BuildNativeRowCells(row, 4);
    REQUIRE(cells.size() == 5);
    CHECK(cells[0].ch == U'a');
    CHECK(cells[1].ch == U'b');
    CHECK(cells[2].ch == 0x1D11E);
    CHECK((cells[2].attr & kNativeCellLeading) != 0);
    CHECK(cells[3].ch == 0x1D11E);
    CHECK((cells[3].attr & kNativeCellTrailing) != 0);
    CHECK(cells[4].ch == U'c');
    CHECK(Utf16WellFormed(CellsToUtf16(cells)));

    // 逻辑未满时尾部照铺空格到逻辑预算(清写区一发盖满)。
    const auto padded = BuildNativeRowCells("\xF0\x9D\x84\x9E", 4);
    REQUIRE(padded.size() == 4);
    CHECK((padded[1].attr & kNativeCellTrailing) != 0);
    CHECK(padded[2].ch == U' ');
    CHECK(padded[3].ch == U' ');
}

TEST_CASE("native row cells: 整簇截断按逻辑账判,与 VT 折行同一把尺") {
    using namespace lubancode::platform;
    // 预算 3:a + 𝄞(逻辑 1)+ c 逻辑恰满,物理 4 格全落;
    // 预算 2:'c' 的逻辑装不下(2+1 > 2)才截——不是被编码开销挤掉。
    const std::string row = "a\xF0\x9D\x84\x9E"
                            "c";
    const auto full = BuildNativeRowCells(row, 3);
    REQUIRE(full.size() == 4);
    CHECK(full[3].ch == U'c');
    const auto cut = BuildNativeRowCells(row, 2);
    REQUIRE(cut.size() == 3);  // a(1 格)+ 𝄞(代理对 2 格);逻辑 2 恰满
    CHECK(cut[0].ch == U'a');
    CHECK(cut[2].ch == 0x1D11E);
    CHECK((cut[2].attr & kNativeCellTrailing) != 0);
}

TEST_CASE("native row needs byte fallback: 多码点簇要字节路,窄非 BMP 不要") {
    using lubancode::cli::NativeRowNeedsByteFallback;
    CHECK(NativeRowNeedsByteFallback("e\xCC\x81"));                          // 组合重音
    CHECK(NativeRowNeedsByteFallback("\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3"));  // 旗帜(两码点簇)
    CHECK(NativeRowNeedsByteFallback("\xCC\x81"));                           // 孤立零宽
    CHECK(NativeRowNeedsByteFallback("\xED\xA0\x80"));                       // 坏代理
    CHECK_FALSE(NativeRowNeedsByteFallback("ab\xF0\x9D\x84\x9E"));           // 窄非 BMP:代理对装得下
    CHECK_FALSE(NativeRowNeedsByteFallback("\xF0\x9F\x98\x80"));             // 单码点 emoji
    CHECK_FALSE(NativeRowNeedsByteFallback("\x1b[36mC\x1b[0m"));             // 纯单码点带配色
    CHECK_FALSE(NativeRowNeedsByteFallback(std::string_view()));
}

TEST_CASE("native column for logical: 两路末态光标钉在同一文字位置") {
    using lubancode::cli::NativeColumnForLogical;
    // 无窄非 BMP 的文本,原生路列号与 VT 路逻辑列必须恒等值(折行与末态
    // 光标一致的册);宽字双格 = 逻辑两列,不产生差。
    CHECK(NativeColumnForLogical("a\xE4\xB8\xAD"
                                 "b",
                                 4) == 4);
    CHECK(NativeColumnForLogical("abc", 3) == 3);
    CHECK(NativeColumnForLogical("abc", 0) == 0);
    CHECK(NativeColumnForLogical("abc", 99) == 3);  // 超出按物理末尾(防御)
    // 窄非 BMP 各多占一格:光标钉在与 VT 路相同的文字位置,列号带差。
    CHECK(NativeColumnForLogical("\xF0\x9D\x84\x9E", 1) == 2);
    CHECK(NativeColumnForLogical("a\xF0\x9D\x84\x9E"
                                 "b",
                                 2) == 3);  // a 与 𝄞 之后
    CHECK(NativeColumnForLogical("a\xF0\x9D\x84\x9E"
                                 "b",
                                 1) == 1);  // 恰在 𝄞 之前
    CHECK(NativeColumnForLogical("\x1b[36m\xF0\x9D\x84\x9E\x1b[0m", 1) == 2);  // 配色段不占列
}

TEST_CASE("几何分叉册: 同输入两路折行一致,末态光标同一文字位置") {
    // 折行账两路共用 LayoutComposerRows(逻辑宽断行,不变式);光标账:
    // VT 路列号 = 逻辑列,原生路 = NativeColumnForLogical 的折算列,差值
    // 恰等于光标前窄非 BMP 的个数——同一文字位置,两种列号。
    const std::u32string line = {U'a', U'b', 0x1D11E, U'c'};
    const auto layout = LayoutComposerRows({line}, 0, line.size(), 3, 3);
    REQUIRE(layout.rows.size() == 2);  // 逻辑 4 断成 3 + 1
    CHECK(layout.rows[0].text == std::u32string{U'a', U'b', 0x1D11E});
    CHECK(layout.rows[0].display_width == 3);
    CHECK(layout.rows[1].text == std::u32string{U'c'});
    // 行 0 逻辑满宽处:VT 列 3,原生物理列 4(𝄞 的代理对多占一格)。
    const std::string row0 = "ab\xF0\x9D\x84\x9E";
    CHECK(lubancode::cli::NativeColumnForLogical(row0, 3) == 4);
    // 行 1("c")无窄非 BMP:两路末态光标列同为 1。
    CHECK(lubancode::cli::NativeColumnForLogical("c", 1) == 1);
}

TEST_CASE("native frame plan: emoji 常驻而该行不脏,不整帧退字节路(计数型断言)") {
    using lubancode::cli::PlanInlineFrameNativePaint;
    InlineFrame previous{{InlineFrameRow{0, 24, false, "activity (10s)"},
                          InlineFrameRow{0, 40, true, "rule"},
                          InlineFrameRow{2, 10, false, "> ok"}},
                         4, 2};
    // 拍 A:只有活动行变——分路账全原生,零字节回退。
    InlineFrame next_a = previous;
    next_a.rows[0].text = "activity (11s)";
    const auto plan_a = PlanInlineFrameNativePaint(&previous, next_a);
    CHECK(plan_a.compared_rows == 3);
    CHECK(plan_a.changed_rows == 1);
    CHECK(plan_a.native_rows == 1);
    CHECK(plan_a.byte_fallback_rows == 0);

    // 拍 B:多码点簇进了输入行——只有那一行退字节路,同拍变脏的活动行
    // 仍直写;不再整帧全量。
    InlineFrame emoji = previous;
    emoji.rows[0].text = "activity (12s)";
    emoji.rows[2].text = "> \xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB";  // 👩‍💻
    const auto plan_b = PlanInlineFrameNativePaint(&previous, emoji);
    CHECK(plan_b.changed_rows == 2);
    CHECK(plan_b.byte_fallback_rows == 1);
    CHECK(plan_b.native_rows == 1);

    // 拍 C:emoji 常驻屏上、输入行纹丝不动,秒数照跳——字节回退为零。
    InlineFrame next_c = emoji;
    next_c.rows[0].text = "activity (13s)";
    const auto plan_c = PlanInlineFrameNativePaint(&emoji, next_c);
    CHECK(plan_c.changed_rows == 1);
    CHECK(plan_c.byte_fallback_rows == 0);
    CHECK(plan_c.native_rows == 1);
}

TEST_CASE("native frame paint: 非真 console 一字节不写整帧交 legacy,不落两遍") {
    // 帧级判定先于落笔:管道环境(CTest 常态)GetScreenInfo 探不到控制台,
    // 含字节回退行的帧直接 false,调用方退 PaintInlineFrameLegacy 单写一遍
    // ——本函数不许先写半个帧再退。真 console 跑测试本体的情形用越界行
    // 验"字节回退行落笔"的账(不涂屏,正文走改道流)。
    InlineFrame previous{{InlineFrameRow{0, 10, false, "old"}}, 2, 0};
    InlineFrame next{{InlineFrameRow{0, 10, false, "> \xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB"}}, 2, 0};
    std::size_t painted = 99;
    std::size_t byte_rows = 99;
    if (lubancode::platform::ProbeStdoutConsole().is_console) {
        std::ostringstream captured;
        lubancode::cli::TermPort().Redirect(&captured, nullptr);
        CHECK(PaintInlineFrameNativeRows(&previous, next, 1000000, &painted, &byte_rows));
        lubancode::cli::TermPort().Reset();
        CHECK(painted == 1);
        CHECK(byte_rows == 1);
        CHECK(captured.str() == next.rows[0].text);
    } else {
        std::ostringstream captured;
        lubancode::cli::TermPort().Redirect(&captured, nullptr);
        CHECK_FALSE(PaintInlineFrameNativeRows(&previous, next, 5, &painted, &byte_rows));
        lubancode::cli::TermPort().Reset();
        CHECK(painted == 0);
        CHECK(byte_rows == 0);
        CHECK(captured.str().empty());
    }
}
