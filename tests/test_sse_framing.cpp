// 钉死 SseFramer 的分帧行为:一条事件劈成多段喂、多条事件挤一包、
// CRLF/LF 混用、多行 data 拼接、注释行、data: 后有无空格、空 event 名
// 默认 "message"。

#include <doctest/doctest.h>

#include "api/sse_framing.hpp"

using lubancode::api::SseFrame;
using lubancode::api::SseFramer;

TEST_CASE("一条事件劈成三段喂入,仍能正确拼出一帧") {
    SseFramer framer;

    auto f1 = framer.feed("event: content_block_delta\ndata: {\"a\":");
    CHECK(f1.empty());

    auto f2 = framer.feed("1,\"b\":2}");
    CHECK(f2.empty());

    auto f3 = framer.feed("\n\n");
    REQUIRE(f3.size() == 1);
    CHECK(f3[0].event == "content_block_delta");
    CHECK(f3[0].data == "{\"a\":1,\"b\":2}");
}

TEST_CASE("一条事件按字节逐个字符喂入,依然能拼对") {
    SseFramer framer;
    const std::string raw = "event: ping\ndata: {}\n\n";

    std::vector<SseFrame> frames;
    for (char c : raw) {
        auto batch = framer.feed(std::string_view(&c, 1));
        frames.insert(frames.end(), batch.begin(), batch.end());
    }

    REQUIRE(frames.size() == 1);
    CHECK(frames[0].event == "ping");
    CHECK(frames[0].data == "{}");
}

TEST_CASE("两条事件挤在一个 chunk 里,能拆成两帧") {
    SseFramer framer;
    auto frames = framer.feed(
        "event: ping\ndata: {}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n");

    REQUIRE(frames.size() == 2);
    CHECK(frames[0].event == "ping");
    CHECK(frames[0].data == "{}");
    CHECK(frames[1].event == "message_stop");
    CHECK(frames[1].data == "{\"type\":\"message_stop\"}");
}

TEST_CASE("CRLF 和 LF 混用都能正确分行") {
    SseFramer framer;
    auto frames = framer.feed("event: a\r\ndata: 1\r\n\r\nevent: b\ndata: 2\n\n");

    REQUIRE(frames.size() == 2);
    CHECK(frames[0].event == "a");
    CHECK(frames[0].data == "1");
    CHECK(frames[1].event == "b");
    CHECK(frames[1].data == "2");
}

TEST_CASE("多行 data 按换行拼接") {
    SseFramer framer;
    auto frames = framer.feed("data: line1\ndata: line2\ndata: line3\n\n");

    REQUIRE(frames.size() == 1);
    CHECK(frames[0].event == "message");  // 没写 event: 字段,默认 message
    CHECK(frames[0].data == "line1\nline2\nline3");
}

TEST_CASE("以冒号开头的注释行要跳过,不当字段处理") {
    SseFramer framer;
    auto frames = framer.feed(": 这是一条注释,啥也不做\ndata: hello\n\n");

    REQUIRE(frames.size() == 1);
    CHECK(frames[0].data == "hello");
}

TEST_CASE("data: 后有没有空格,都能取到正确的值") {
    SseFramer framer;
    auto frames = framer.feed("data:no-space\n\ndata: with-space\n\n");

    REQUIRE(frames.size() == 2);
    CHECK(frames[0].data == "no-space");
    CHECK(frames[1].data == "with-space");
}

TEST_CASE("只有 event 没有 data 的空事件不产生帧") {
    SseFramer framer;
    auto frames = framer.feed("event: ping\n\n");
    CHECK(frames.empty());
}

TEST_CASE("data 值可以是空串,只要 data 字段出现过就该派发") {
    SseFramer framer;
    auto frames = framer.feed("event: foo\ndata:\n\n");

    REQUIRE(frames.size() == 1);
    CHECK(frames[0].event == "foo");
    CHECK(frames[0].data.empty());
}

TEST_CASE("连续多帧之间状态不串味") {
    SseFramer framer;
    auto frames = framer.feed(
        "event: first\ndata: one\n\n"
        "data: two\n\n"
        "event: third\ndata: three\n\n");

    REQUIRE(frames.size() == 3);
    CHECK(frames[0].event == "first");
    CHECK(frames[0].data == "one");
    CHECK(frames[1].event == "message");  // 第二帧没写 event,不该沿用上一帧的 "first"
    CHECK(frames[1].data == "two");
    CHECK(frames[2].event == "third");
    CHECK(frames[2].data == "three");
}

TEST_CASE("帧长上限:残行/单帧 data 超过 8MB 时报废,不再吐帧,不无限吃内存") {
    SseFramer framer;

    // 一行 data 迟迟不见换行,累积超过上限。
    const std::string chunk(1024 * 1024, 'x');
    framer.feed("data: ");
    for (int i = 0; i < 9; ++i) {
        framer.feed(chunk);
    }
    CHECK(framer.overflowed());

    // 报废之后,再喂正常事件也不吐帧了(调用方看 overflowed() 断连)。
    auto frames = framer.feed("data: {}\n\n");
    CHECK(frames.empty());
}

TEST_CASE("帧长上限:超限前已凑齐的完整帧照常交出") {
    SseFramer framer;
    std::string input = "data: ok\n\n";
    input += "data: ";
    input += std::string(9 * 1024 * 1024, 'y');  // 超限的残行
    const auto frames = framer.feed(input);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].data == "ok");
    CHECK(framer.overflowed());
}

TEST_CASE("正常大小的流水线不触发 overflowed") {
    SseFramer framer;
    for (int i = 0; i < 1000; ++i) {
        framer.feed("data: {\"n\":" + std::to_string(i) + "}\n\n");
    }
    CHECK_FALSE(framer.overflowed());
}
