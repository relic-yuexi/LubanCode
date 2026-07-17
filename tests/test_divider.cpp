// BuildDividerLine:纯函数,算分界线该长什么样——宽度公式
// min(console_width - 1, 80)、plain 主题下退化成 "-"、宽度探测失败/异常值
// (<=0)时的兜底行为。

#include <doctest/doctest.h>

#include "cli/divider.hpp"

using lubancode::cli::BuildDividerLine;

namespace {
// 一个 UTF-8 "─" (U+2500) 占 3 字节。
constexpr std::size_t kUnitBytes = 3;
}  // namespace

TEST_CASE("BuildDividerLine: 控制台够宽(120 列)时,按 80 列上限截断") {
    const std::string line = BuildDividerLine(120, /*plain=*/false);
    CHECK(line.size() == kUnitBytes * 80);
}

TEST_CASE("BuildDividerLine: 控制台比 80 列窄时,按 console_width - 1 来") {
    const std::string line = BuildDividerLine(50, /*plain=*/false);
    CHECK(line.size() == kUnitBytes * 49);
}

TEST_CASE("BuildDividerLine: console_width <= 0(探测失败)按 80 列兜底") {
    const std::string line_zero = BuildDividerLine(0, /*plain=*/false);
    const std::string line_neg = BuildDividerLine(-1, /*plain=*/false);
    CHECK(line_zero.size() == kUnitBytes * 80);
    CHECK(line_neg.size() == kUnitBytes * 80);
}

TEST_CASE("BuildDividerLine: 极窄控制台(1 列)算出来 <= 0,返回空串") {
    const std::string line = BuildDividerLine(1, /*plain=*/false);
    CHECK(line.empty());
}

TEST_CASE("BuildDividerLine: plain=true 时用半角 \"-\" 代替 box-drawing 字符") {
    const std::string line = BuildDividerLine(21, /*plain=*/true);
    CHECK(line.size() == 20);
    for (char c : line) {
        CHECK(c == '-');
    }
}

TEST_CASE("BuildDividerLine: max_width 参数放宽上限(0.17.0 输入框横线传 100)") {
    CHECK(BuildDividerLine(120, /*plain=*/false, /*max_width=*/100).size() == kUnitBytes * 100);
    CHECK(BuildDividerLine(80, /*plain=*/false, /*max_width=*/100).size() == kUnitBytes * 79);
    CHECK(BuildDividerLine(0, /*plain=*/true, /*max_width=*/100).size() == 100);  // 探测失败按 max_width 兜底
}

TEST_CASE("BuildDividerLine: 非 plain 时,内容确实是重复的 U+2500 字符") {
    const std::string line = BuildDividerLine(6, /*plain=*/false);
    REQUIRE(line.size() == kUnitBytes * 5);
    for (std::size_t i = 0; i < line.size(); i += kUnitBytes) {
        CHECK(line[i] == static_cast<char>(0xE2));
        CHECK(line[i + 1] == static_cast<char>(0x94));
        CHECK(line[i + 2] == static_cast<char>(0x80));
    }
}
