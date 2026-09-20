// 显式全局通知区(按代理状态投影单 P3):无法归属页面的诊断不落正文,
// 进底栏帧顶的独立通知区——有界、带过期、同文续命不叠条;会话边界清板。
// 这册钉纯逻辑(板本体);交互判定的分流(ReportDiagnosticLine 走不走板)
// 依赖终端能力,不在单测里断言,虚拟屏/真机验收管那半。

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "cli/global_notice.hpp"

using lubancode::cli::GlobalNoticeBoard;

TEST_CASE("全局通知区: 压条、过期自收、清板") {
    GlobalNoticeBoard board;
    CHECK_FALSE(board.HasActive());
    board.Push("  [hooks] 后台记录已归并  ");  // 首尾空白裁掉
    CHECK(board.HasActive());
    auto rows = board.ActiveRows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0] == "[hooks] 后台记录已归并");

    // 过期自收:1ms 的短命通知,睡过就收。
    board.Push("短命", std::chrono::milliseconds(1));
    CHECK(board.ActiveRows().size() == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(board.ActiveRows().size() == 1);
    CHECK(board.ActiveRows()[0] == "[hooks] 后台记录已归并");

    board.Clear();
    CHECK_FALSE(board.HasActive());
    CHECK(board.ActiveRows().empty());
}

TEST_CASE("全局通知区: 有界——超上限留最近三条;同文续命不叠条") {
    GlobalNoticeBoard board;
    board.Push("一号诊断");
    board.Push("二号诊断");
    board.Push("三号诊断");
    board.Push("四号诊断");  // 超上限:一号被挤掉
    auto rows = board.ActiveRows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == "二号诊断");
    CHECK(rows[2] == "四号诊断");

    // 同文重复(心跳异常一秒五拍那类):只续命,不叠条。
    board.Push("四号诊断");
    rows = board.ActiveRows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[2] == "四号诊断");
    CHECK(rows.size() == GlobalNoticeBoard::kMaxNotices);

    // 全空白的行没有信息量,不占板。
    board.Push("   ");
    CHECK(board.ActiveRows().size() == 3);
}
