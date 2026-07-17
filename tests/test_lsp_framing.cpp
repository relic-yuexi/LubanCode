// LSP Content-Length 分帧器(lsp/transport.hpp 的 ContentLengthFramer)是
// 纯函数式的增量分帧器,不碰任何 IO——单测只管喂字节、查吐出来的消息,
// 覆盖:劈包(头劈开/正文劈开/头和正文分两次到)、挤包(一次 Feed 好几条
// 消息)、大体积正文、多头部行、坏头跳过这些情形。

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "lsp/transport.hpp"

using namespace lubancode;

namespace {

std::string Frame(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

}  // namespace

TEST_CASE("ContentLengthFramer: 一次 Feed 里一条完整消息") {
    lsp::ContentLengthFramer framer;
    const auto messages = framer.Feed(Frame("{\"a\":1}"));
    REQUIRE(messages.size() == 1);
    CHECK(messages[0] == "{\"a\":1}");
}

TEST_CASE("ContentLengthFramer: 一次 Feed 里挤了好几条消息") {
    lsp::ContentLengthFramer framer;
    const auto messages = framer.Feed(Frame("first") + Frame("second") + Frame("third"));
    REQUIRE(messages.size() == 3);
    CHECK(messages[0] == "first");
    CHECK(messages[1] == "second");
    CHECK(messages[2] == "third");
}

TEST_CASE("ContentLengthFramer: 头被劈成好几段喂进来") {
    lsp::ContentLengthFramer framer;
    CHECK(framer.Feed("Content-Le").empty());
    CHECK(framer.Feed("ngth: 5\r\n").empty());
    CHECK(framer.Feed("\r").empty());
    const auto messages = framer.Feed("\nhello");
    REQUIRE(messages.size() == 1);
    CHECK(messages[0] == "hello");
}

TEST_CASE("ContentLengthFramer: 正文被劈成好几段,残包留缓冲") {
    lsp::ContentLengthFramer framer;
    CHECK(framer.Feed("Content-Length: 10\r\n\r\nhel").empty());
    CHECK(framer.Feed("lo wo").empty());
    const auto messages = framer.Feed("rld");
    REQUIRE(messages.size() == 1);
    CHECK(messages[0] == "hello worl");  // 正好 10 字节,"d" 留在缓冲里
}

TEST_CASE("ContentLengthFramer: 一条完整消息带出下一条的残头") {
    lsp::ContentLengthFramer framer;
    const auto first = framer.Feed(Frame("one") + "Content-Len");
    REQUIRE(first.size() == 1);
    CHECK(first[0] == "one");
    const auto second = framer.Feed("gth: 3\r\n\r\ntwo");
    REQUIRE(second.size() == 1);
    CHECK(second[0] == "two");
}

TEST_CASE("ContentLengthFramer: 大体积正文(256KB)一样能凑齐") {
    lsp::ContentLengthFramer framer;
    std::string big(256 * 1024, 'x');
    big[0] = '{';
    big[big.size() - 1] = '}';
    const std::string framed = Frame(big);
    // 按 4KB 一段慢慢喂,模拟管道读取的真实节奏。
    std::vector<std::string> collected;
    for (std::size_t offset = 0; offset < framed.size(); offset += 4096) {
        const std::size_t len = std::min<std::size_t>(4096, framed.size() - offset);
        auto out = framer.Feed(std::string_view(framed).substr(offset, len));
        for (auto& m : out) {
            collected.push_back(std::move(m));
        }
    }
    REQUIRE(collected.size() == 1);
    CHECK(collected[0] == big);
}

TEST_CASE("ContentLengthFramer: 头部块里有多行头(Content-Type),照样只认 Content-Length") {
    lsp::ContentLengthFramer framer;
    const auto messages =
        framer.Feed("Content-Length: 4\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\nbody");
    REQUIRE(messages.size() == 1);
    CHECK(messages[0] == "body");
}

TEST_CASE("ContentLengthFramer: 头名大小写不敏感") {
    lsp::ContentLengthFramer framer;
    const auto messages = framer.Feed("content-length: 2\r\n\r\nok");
    REQUIRE(messages.size() == 1);
    CHECK(messages[0] == "ok");
}

TEST_CASE("ContentLengthFramer: 坏头(没有 Content-Length)整块丢掉,不把流搞死") {
    lsp::ContentLengthFramer framer;
    const auto bad = framer.Feed("X-Whatever: 3\r\n\r\n");
    CHECK(bad.empty());
    const auto good = framer.Feed(Frame("ok"));
    REQUIRE(good.size() == 1);
    CHECK(good[0] == "ok");
}

TEST_CASE("ContentLengthFramer: 长度为 0 的消息吐出空字符串正文") {
    lsp::ContentLengthFramer framer;
    const auto messages = framer.Feed(Frame("") + Frame("next"));
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].empty());
    CHECK(messages[1] == "next");
}
