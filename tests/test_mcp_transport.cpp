// M8:LineFramer 是纯函数式的增量分帧器(mcp/transport.hpp),不碰任何 IO——
// 单测只管喂字节、查吐出来的行,覆盖三种关键情形:一条消息被拆成好几段
// 喂进来、好几条消息挤在一次读到的数据里、残行留到下一次 Feed() 才凑齐。

#include <doctest/doctest.h>

#include "mcp/transport.hpp"

using namespace lubancode;

TEST_CASE("LineFramer: 一次 Feed 里一条完整的行") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("hello\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "hello");
}

TEST_CASE("LineFramer: 一次 Feed 里挤了好几条消息") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("line1\nline2\nline3\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "line1");
    CHECK(lines[1] == "line2");
    CHECK(lines[2] == "line3");
}

TEST_CASE("LineFramer: 一条消息被拆成好几段喂进来") {
    mcp::LineFramer framer;
    auto first = framer.Feed("hel");
    CHECK(first.empty());  // 还没凑齐一整行
    auto second = framer.Feed("lo wor");
    CHECK(second.empty());
    auto third = framer.Feed("ld\n");
    REQUIRE(third.size() == 1);
    CHECK(third[0] == "hello world");
}

TEST_CASE("LineFramer: 残行留在缓冲里,下一条完整消息带出上一次的残行") {
    mcp::LineFramer framer;
    auto first = framer.Feed("complete1\npartial-tai");
    REQUIRE(first.size() == 1);
    CHECK(first[0] == "complete1");

    auto second = framer.Feed("l\ncomplete2\n");
    REQUIRE(second.size() == 2);
    CHECK(second[0] == "partial-tail");
    CHECK(second[1] == "complete2");
}

TEST_CASE("LineFramer: \\r\\n 和裸 \\n 都认,统一去掉行尾 \\r") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("with-cr\r\nwithout-cr\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "with-cr");
    CHECK(lines[1] == "without-cr");
}

TEST_CASE("LineFramer: 空行(连续两个换行符)吐出一个空字符串") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("a\n\nb\n");
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "a");
    CHECK(lines[1].empty());
    CHECK(lines[2] == "b");
}

TEST_CASE("LineFramer: 没有换行符,什么都不吐,残留在缓冲里等下一次") {
    mcp::LineFramer framer;
    const auto lines = framer.Feed("no-newline-yet");
    CHECK(lines.empty());
    const auto more = framer.Feed("-still-nothing");
    CHECK(more.empty());
    const auto finally = framer.Feed("\n");
    REQUIRE(finally.size() == 1);
    CHECK(finally[0] == "no-newline-yet-still-nothing");
}
