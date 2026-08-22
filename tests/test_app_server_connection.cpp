// app-server 协议骨架单:分帧(LineFramer,照搬 mcp 的测试手法)与
// 有界出站队列(溢出通报的类型、终态不丢)。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "app_server/framing.hpp"
#include "app_server/outbox.hpp"
#include "app_server/protocol.hpp"

using namespace lubancode::app_server;

// ---------------------------------------------------------------------------
// LineFramer(手法照 test_mcp_transport.cpp)
// ---------------------------------------------------------------------------

TEST_CASE("app_server LineFramer: 一次 Feed 里一条完整的行") {
    LineFramer framer;
    const auto lines = framer.Feed("hello\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "hello");
}

TEST_CASE("app_server LineFramer: 挤包、劈包、残行") {
    LineFramer framer;
    auto lines = framer.Feed("line1\nline2\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "line2");

    lines = framer.Feed("hel");
    CHECK(lines.empty());
    lines = framer.Feed("lo\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "hello");
}

TEST_CASE("app_server LineFramer: \\r\\n 与裸 \\n 都认,行尾 \\r 统一剥掉") {
    LineFramer framer;
    const auto lines = framer.Feed("with-cr\r\nwithout-cr\n");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "with-cr");
    CHECK(lines[1] == "without-cr");
}

TEST_CASE("app_server LineFramer: 超长单行置 overflowed、报废、不再吐行") {
    LineFramer framer;
    const std::string big(1024 * 1024, 'x');
    for (int i = 0; i < 9; ++i) {
        framer.Feed(big); // 9MB,不换行
    }
    CHECK(framer.overflowed());
    CHECK(framer.Feed("late\n").empty());
}

TEST_CASE("app_server LineFramer: 溢出前已凑齐的完整行照常交出") {
    LineFramer framer;
    std::string chunk = "complete\n";
    chunk.append(LineFramer::kMaxLineBytes + 1, 'x');
    const auto lines = framer.Feed(chunk);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "complete");
    CHECK(framer.overflowed());
}

// ---------------------------------------------------------------------------
// BoundedOutbox
// ---------------------------------------------------------------------------

TEST_CASE("BoundedOutbox: 常规入队出队,次序保持") {
    BoundedOutbox outbox(4);
    CHECK(outbox.Push("a"));
    CHECK(outbox.Push("b"));
    CHECK(outbox.Push("c"));
    CHECK(outbox.Pop() == "a");
    CHECK(outbox.Pop() == "b");
    CHECK(outbox.Pop() == "c");
    CHECK_FALSE(outbox.Pop().has_value());
    CHECK(outbox.dropped() == 0);
}

TEST_CASE("BoundedOutbox: 满了丢可丢事件,记溢出账,绝不阻塞") {
    BoundedOutbox outbox(2);
    CHECK(outbox.Push("delta-1"));
    CHECK(outbox.Push("delta-2"));
    CHECK(outbox.full());
    // 第三条可丢事件:丢弃,不阻塞。
    CHECK_FALSE(outbox.Push("delta-3"));
    CHECK(outbox.dropped() == 1);
    CHECK(outbox.size() == 2);
    // 队里两条还是先前的。
    CHECK(outbox.Pop() == "delta-1");
    CHECK(outbox.Pop() == "delta-2");
}

TEST_CASE("BoundedOutbox: must_keep 事件撞满时挤掉可丢的,自己不丢") {
    BoundedOutbox outbox(2);
    CHECK(outbox.Push("delta-1", /*must_keep=*/false));
    CHECK(outbox.Push("delta-2", /*must_keep=*/false));
    // 终态类:必须保——丢一个可丢的腾位置。
    CHECK(outbox.Push("turn/completed", /*must_keep=*/true));
    CHECK(outbox.dropped() == 1);
    CHECK(outbox.size() == 2);
    CHECK(outbox.Pop() == "delta-2");
    CHECK(outbox.Pop() == "turn/completed");
    CHECK_FALSE(outbox.Pop().has_value());
}

TEST_CASE("BoundedOutbox: 全满 must_keep 时连必保也丢(记账,不堆不堵)") {
    BoundedOutbox outbox(1);
    CHECK(outbox.Push("turn/completed", /*must_keep=*/true));
    CHECK(outbox.full());
    // 队里已有一条必保,容量 1:第二条必保丢掉并记账。
    CHECK_FALSE(outbox.Push("turn/completed", /*must_keep=*/true));
    CHECK(outbox.dropped() == 1);
    // 原先那条还在。
    CHECK(outbox.Pop() == "turn/completed");
}

TEST_CASE("BoundedOutbox: PopAll 一次倒空且清队") {
    BoundedOutbox outbox(8);
    for (int i = 0; i < 5; ++i) {
        outbox.Push("e" + std::to_string(i));
    }
    const std::vector<std::string> all = outbox.PopAll();
    REQUIRE(all.size() == 5);
    CHECK(all.front() == "e0");
    CHECK(all.back() == "e4");
    CHECK(outbox.size() == 0);
}

TEST_CASE("EventMustKeep: 终态与审批保,delta 不保") {
    CHECK(EventMustKeep(kEventTurnCompleted));
    CHECK(EventMustKeep(kEventQueueOverflow));
    CHECK(EventMustKeep(kMethodPermissionRequest));
    CHECK(EventMustKeep(kMethodUserAsk));
    CHECK_FALSE(EventMustKeep(kEventItemDelta));
    CHECK_FALSE(EventMustKeep(kEventItemStarted));
    CHECK_FALSE(EventMustKeep(kEventThreadStarted));
}

// ---------------------------------------------------------------------------
// 稳定错误码不许漂移(冻结前的锚)
// ---------------------------------------------------------------------------

TEST_CASE("错误码锚:标准段与服务器段各就各位") {
    CHECK(kErrParseError == -32700);
    CHECK(kErrInvalidRequest == -32600);
    CHECK(kErrMethodNotFound == -32601);
    CHECK(kErrInvalidParams == -32602);
    CHECK(kErrInternalError == -32603);
    CHECK(kErrServerBusy == -32000);
    CHECK(kErrNotInitialized == -32002);
    CHECK(kErrTurnAlreadyRunning == -32004);
}
