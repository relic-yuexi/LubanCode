#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "cli/terminal_frame.hpp"
#include "platform/console.hpp"

using lubancode::cli::InlineFrame;
using lubancode::cli::InlineFrameRow;
using lubancode::cli::LayoutComposerRows;
using lubancode::cli::QueueInlineFrameDiff;
using lubancode::cli::TurnActivityRowChanged;
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

TEST_CASE("inline repaint plan: VT×sync 四档选路") {
    // 无 VT(老 conhost/管道):原生兜底路,谈不上同步输出。
    StdoutConsoleProbe none;
    none.is_console = true;
    none.vt_enabled = false;
    none.sync_output = false;
    const auto plan_none = lubancode::platform::PlanInlineRepaint(none);
    CHECK_FALSE(plan_none.vt_batch);
    CHECK_FALSE(plan_none.sync_output);

    // 普通 VT + 未确认 2026:Windows 退原生控制台 API(不搬实体光标的屏
    // 幕写入),不再发会漏中间态的 CUP 串;POSIX 无原生路,退 VT 批不包
    // 2026(动画已撤,只在真状态变化时单笔落帧)。
    StdoutConsoleProbe vt_only;
    vt_only.is_console = true;
    vt_only.vt_enabled = true;
    vt_only.sync_output = false;
    const auto plan_vt_only = lubancode::platform::PlanInlineRepaint(vt_only);
#ifdef _WIN32
    CHECK_FALSE(plan_vt_only.vt_batch);
#else
    CHECK(plan_vt_only.vt_batch);
#endif
    CHECK_FALSE(plan_vt_only.sync_output);

    // 普通 VT + 确认 2026:VT 批 + 同步输出,帧原子提交。
    StdoutConsoleProbe vt_sync;
    vt_sync.is_console = true;
    vt_sync.vt_enabled = true;
    vt_sync.sync_output = true;
    const auto plan_vt_sync = lubancode::platform::PlanInlineRepaint(vt_sync);
    CHECK(plan_vt_sync.vt_batch);
    CHECK(plan_vt_sync.sync_output);

    // 防御:VT 都不开却报 sync(矛盾输入),一律按无 VT 处理。
    StdoutConsoleProbe broken;
    broken.is_console = true;
    broken.vt_enabled = false;
    broken.sync_output = true;
    const auto plan_broken = lubancode::platform::PlanInlineRepaint(broken);
    CHECK_FALSE(plan_broken.vt_batch);
    CHECK_FALSE(plan_broken.sync_output);
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

TEST_CASE("turn activity row: 同一秒零变化,秒数/标签/中断态变才重画") {
    using lubancode::cli::TurnActivityRowChanged;
    // 同一秒、同一标签、同一中断态:不是变化,闲拍零落笔。
    CHECK_FALSE(TurnActivityRowChanged("思考中", 10, false, "思考中", 10, false));
    // 秒数进一:重画。
    CHECK(TurnActivityRowChanged("思考中", 10, false, "思考中", 11, false));
    // 阶段换词(Begin/Stopping):重画。
    CHECK(TurnActivityRowChanged("思考中", 10, false, "Stopping...", 10, false));
    // 中断置位(圆点换 error 色):重画。
    CHECK(TurnActivityRowChanged("思考中", 10, false, "思考中", 10, true));
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
