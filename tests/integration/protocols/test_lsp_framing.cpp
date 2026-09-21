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

// ---- SV-04 资源合同:超长头/超限声明/十进制溢出/不闭合正文的资源边界 ----
// 合同三件事(见 lsp/transport.hpp):头部块 ≤ 8KB、声明正文 ≤ 8MB,恰好
// 上限通过、超一字节拒绝;Feed 怎么切不影响结论;超限即报废、坏头只跳过。

namespace {

// 拼一个头部块(含结尾 \r\n\r\n)恰为 header_bytes 字节的帧:Content-Length
// 行 + 一行超长 X-Pad 垫尺寸。调用处自检定界符恰在头部块末尾。
std::string FrameWithHeaderBytes(std::size_t header_bytes, const std::string& body) {
    std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n";
    const std::size_t pad_line = header_bytes - frame.size() - 2;  // 减去结尾空行 \r\n
    frame += "X-Pad: " + std::string(pad_line - 9, 'p') + "\r\n";  // "X-Pad: "(7)+\r\n(2)+凑数
    frame += "\r\n";
    frame += body;
    return frame;
}

}  // namespace

TEST_CASE("ContentLengthFramer: 头部块恰在上限通过——整片/劈在边界结论一致") {
    const std::size_t cap = lsp::ContentLengthFramer::kMaxHeaderBytes;
    const std::string framed = FrameWithHeaderBytes(cap, "body");
    REQUIRE(framed.find("\r\n\r\n") == cap - 4);  // 自检:定界恰在头部块末尾

    // 整片喂入。
    {
        lsp::ContentLengthFramer framer;
        const auto messages = framer.Feed(framed);
        REQUIRE(messages.size() == 1);
        CHECK(messages[0] == "body");
        CHECK_FALSE(framer.overflowed());
    }
    // 劈在边界:喂到上限前一字节(定界不完整,攒头恰在上限内),再补最后一字节。
    {
        lsp::ContentLengthFramer framer;
        CHECK(framer.Feed(framed.substr(0, cap - 1)).empty());
        CHECK_FALSE(framer.overflowed());
        const auto messages = framer.Feed(framed.substr(cap - 1));
        REQUIRE(messages.size() == 1);
        CHECK(messages[0] == "body");
        CHECK_FALSE(framer.overflowed());
    }
}

TEST_CASE("ContentLengthFramer: 头部块超上限拒绝——整片与逐段攒头判死一致") {
    const std::size_t cap = lsp::ContentLengthFramer::kMaxHeaderBytes;
    const std::string framed = FrameWithHeaderBytes(cap + 1, "body");
    REQUIRE(framed.find("\r\n\r\n") == cap - 3);

    // 整片喂入:凑齐的头块本身越帽。
    {
        lsp::ContentLengthFramer framer;
        CHECK(framer.Feed(framed).empty());
        CHECK(framer.overflowed());
    }
    // 逐段攒头:恰在上限不误判,再进一字节即判死。
    {
        lsp::ContentLengthFramer framer;
        CHECK(framer.Feed(framed.substr(0, cap)).empty());
        CHECK_FALSE(framer.overflowed());
        CHECK(framer.Feed(framed.substr(cap, 1)).empty());
        CHECK(framer.overflowed());
        CHECK(framer.Feed(framed.substr(cap + 1)).empty());  // 报废后不再吐
    }
}

TEST_CASE("ContentLengthFramer: 攒头无定界越帽即判死——无界增长封死") {
    const std::size_t cap = lsp::ContentLengthFramer::kMaxHeaderBytes;
    lsp::ContentLengthFramer framer;
    CHECK(framer.Feed(std::string(cap, 'j')).empty());  // 恰在上限:还在等定界,不判死
    CHECK_FALSE(framer.overflowed());
    CHECK(framer.Feed("j").empty());  // +1 字节仍无 \r\n\r\n:判死
    CHECK(framer.overflowed());
    CHECK(framer.Feed(Frame("ok")).empty());  // 报废后不再吐消息
}

TEST_CASE("ContentLengthFramer: 声明正文超上限,头凑齐即判死,不等正文") {
    lsp::ContentLengthFramer framer;
    const std::string header =
        "Content-Length: " + std::to_string(lsp::ContentLengthFramer::kMaxBodyBytes + 1) + "\r\n\r\n";
    CHECK(framer.Feed(header).empty());
    CHECK(framer.overflowed());
    CHECK(framer.Feed("body-bytes").empty());  // 报废后喂啥都不吐
}

TEST_CASE("ContentLengthFramer: 几十位十进制声明不把累积乘爆,按超上限判死") {
    lsp::ContentLengthFramer framer;
    CHECK(framer.Feed("Content-Length: 999999999999999999999999999999\r\n\r\n").empty());
    CHECK(framer.overflowed());
}

TEST_CASE("ContentLengthFramer: 声明恰在上限(8MB)的正文照常凑齐") {
    lsp::ContentLengthFramer framer;
    const std::string body(lsp::ContentLengthFramer::kMaxBodyBytes, 'x');
    const std::string framed = Frame(body);
    // 1MB 一段喂,模拟管道读取节奏。
    std::vector<std::string> collected;
    for (std::size_t offset = 0; offset < framed.size(); offset += 1024 * 1024) {
        const std::size_t len = std::min<std::size_t>(1024 * 1024, framed.size() - offset);
        auto out = framer.Feed(std::string_view(framed).substr(offset, len));
        for (auto& m : out) {
            collected.push_back(std::move(m));
        }
    }
    REQUIRE(collected.size() == 1);
    CHECK(collected[0] == body);
    CHECK_FALSE(framer.overflowed());
}

TEST_CASE("ContentLengthFramer: 多条合规大消息同批到达全部解出——合计不是帽") {
    lsp::ContentLengthFramer framer;
    const std::string body(3 * 1024 * 1024, 'x');  // 单条 3MB,合规
    std::string batch;
    for (int i = 0; i < 3; ++i) {
        batch += Frame(body);  // 合计 9MB,超过单条上限,但不是总量帽
    }
    const auto messages = framer.Feed(batch);
    REQUIRE(messages.size() == 3);
    CHECK_FALSE(framer.overflowed());
}

TEST_CASE("ContentLengthFramer: 声明合规但正文迟迟不闭合——不判死,有界等待") {
    lsp::ContentLengthFramer framer;
    const std::string head =
        "Content-Length: " + std::to_string(lsp::ContentLengthFramer::kMaxBodyBytes) + "\r\n\r\n";
    CHECK(framer.Feed(head + std::string(64 * 1024, 'y')).empty());
    CHECK_FALSE(framer.overflowed());  // 不是协议违规:等待属正常,缓冲上界 = 声明值
    // 对端死活归传输层断连与客户端超时收线,分帧器只管内存有界。
}

TEST_CASE("ContentLengthFramer: 坏头跳过不算资源超限,overflowed 为假") {
    lsp::ContentLengthFramer framer;
    CHECK(framer.Feed("X-Whatever: 3\r\n\r\n").empty());
    CHECK_FALSE(framer.overflowed());
    const auto good = framer.Feed(Frame("ok"));
    REQUIRE(good.size() == 1);
    CHECK(good[0] == "ok");
    CHECK_FALSE(framer.overflowed());
}

TEST_CASE("ContentLengthFramer: 同批里超限声明只判死它自己,先前合规消息照常交出") {
    lsp::ContentLengthFramer framer;
    const std::string batch = Frame("fine") + "Content-Length: 999999999\r\n\r\njunk-body";
    const auto messages = framer.Feed(batch);
    REQUIRE(messages.size() == 1);
    CHECK(messages[0] == "fine");
    CHECK(framer.overflowed());
    CHECK(framer.Feed(Frame("late")).empty());  // 报废后不再吐消息
}
