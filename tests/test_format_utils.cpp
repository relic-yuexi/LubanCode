// cli/format_utils:FormatTokenCount 的 k/M 化边界、状态行文本拼装。全是
// 纯函数,断言直接钉字符串。

#include <doctest/doctest.h>

#include "cli/format_utils.hpp"

using lubancode::cli::BuildStatusLineText;
using lubancode::cli::ConfirmMode;
using lubancode::cli::FormatTokenCount;
using lubancode::cli::StatusLineInfoSegment;
using lubancode::cli::StatusLineModeSegment;

TEST_CASE("FormatTokenCount: 10000 以下原样十进制") {
    CHECK(FormatTokenCount(0) == "0");
    CHECK(FormatTokenCount(1) == "1");
    CHECK(FormatTokenCount(999) == "999");
    CHECK(FormatTokenCount(9999) == "9999");
}

TEST_CASE("FormatTokenCount: 10000 起一位小数 k,尾随 .0 省略") {
    CHECK(FormatTokenCount(10000) == "10k");    // 10.0k -> 10k
    CHECK(FormatTokenCount(10500) == "10.5k");
    CHECK(FormatTokenCount(12300) == "12.3k");
    CHECK(FormatTokenCount(99950) == "100k");   // 四舍五入后尾随 .0 照样省
    CHECK(FormatTokenCount(200000) == "200k");
}

TEST_CASE("FormatTokenCount: 一位小数按四舍五入取") {
    CHECK(FormatTokenCount(10449) == "10.4k");
    CHECK(FormatTokenCount(10450) == "10.5k");
}

TEST_CASE("FormatTokenCount: 1000000 起两位小数 M,尾随 0 逐位省略") {
    CHECK(FormatTokenCount(1000000) == "1M");     // 1.00M -> 1M
    CHECK(FormatTokenCount(1049999) == "1.05M");  // 四舍五入到两位小数
    CHECK(FormatTokenCount(1050000) == "1.05M");
    CHECK(FormatTokenCount(1500000) == "1.5M");   // 1.50M -> 1.5M
    CHECK(FormatTokenCount(2340000) == "2.34M");
}

TEST_CASE("FormatTokenCount: 负数不猜,原样十进制") {
    CHECK(FormatTokenCount(-5) == "-5");
}

TEST_CASE("StatusLineModeSegment: 三档各有一个展示词,提示 shift+tab 切换") {
    const std::string confirm = StatusLineModeSegment(ConfirmMode::Confirm);
    CHECK(confirm.find("确认模式") != std::string::npos);
    CHECK(confirm.find("shift+tab") != std::string::npos);
    CHECK(StatusLineModeSegment(ConfirmMode::Auto).find("auto") != std::string::npos);
    CHECK(StatusLineModeSegment(ConfirmMode::Yolo).find("yolo") != std::string::npos);
}

TEST_CASE("StatusLineInfoSegment: 模型名 + context 百分比,token 数 k 化") {
    const std::string seg = StatusLineInfoSegment("MiniMax-M3", 8, 16400, 200000);
    CHECK(seg.find("MiniMax-M3") != std::string::npos);
    CHECK(seg.find("context 8%") != std::string::npos);
    CHECK(seg.find("16.4k/200k") != std::string::npos);
}

TEST_CASE("StatusLineInfoSegment: 还没有用量(0 tokens)时不摆括号数字") {
    const std::string seg = StatusLineInfoSegment("MiniMax-M3", 0, 0, 200000);
    CHECK(seg.find("context 0%") != std::string::npos);
    CHECK(seg.find("(") == std::string::npos);
}

TEST_CASE("StatusLineInfoSegment: 模型名为空就跳过那一节") {
    const std::string seg = StatusLineInfoSegment("", 3, 0, 0);
    CHECK(seg.find(" ·  · ") == std::string::npos);
    CHECK(seg.find("context 3%") != std::string::npos);
}

TEST_CASE("BuildStatusLineText: 整行 = 模式段 + 信息段") {
    const std::string line = BuildStatusLineText(ConfirmMode::Confirm, "MiniMax-M3", 8, 16400, 200000);
    CHECK(line ==
          StatusLineModeSegment(ConfirmMode::Confirm) + StatusLineInfoSegment("MiniMax-M3", 8, 16400, 200000));
    CHECK(line.find("⏵⏵") != std::string::npos);
}
