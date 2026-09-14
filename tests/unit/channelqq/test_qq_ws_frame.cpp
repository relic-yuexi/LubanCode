// WS 帧编解码册(QQ 机器人接入单 Q1):RFC 6455 客户端子集的纯函数测试。
// 半帧/粘帧/分片/控制帧/超帽/方向性(mask)逐项钉死——协议正确性不赌真机。
#include <doctest/doctest.h>

#include <cstring>
#include <string>

#include "channel/qq/ws_frame.hpp"

namespace lubancode::channel::qq {
namespace {

const std::uint8_t kMask[4] = {0x01, 0x02, 0x03, 0x04};

std::string FrameToText(const std::vector<std::byte>& frame) {
    return std::string(reinterpret_cast<const char*>(frame.data()), frame.size());
}

std::vector<std::byte> TextToFrame(const std::string& bytes) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(bytes.data()),
                                  reinterpret_cast<const std::byte*>(bytes.data()) +
                                      bytes.size());
}

// 服务端方向一帧 text 的手写期望(RFC 6455 §5.2 无 mask):
//   0x81(fin+text) len<126 直接跟载荷。
std::string ServerTextFrame(const std::string& payload) {
    std::string out(1, static_cast<char>(0x81));
    if (payload.size() <= 125) {
        out.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        out.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    out += payload;
    return out;
}

}  // namespace

TEST_CASE("ws_frame: 客户端帧编码带随机 mask,可被对端解开") {
    const std::string payload = "{\"op\":10}";
    const auto frame = EncodeClientFrame(WsOpcode::Text, payload, kMask);
    REQUIRE(frame.size() == 2 + 4 + payload.size());
    CHECK(frame[0] == static_cast<std::byte>(0x81));  // FIN + text
    CHECK(frame[1] == static_cast<std::byte>(0x80 | payload.size()));  // mask + len
    // mask key 原样在头里。
    CHECK(frame[2] == static_cast<std::byte>(kMask[0]));
    CHECK(frame[5] == static_cast<std::byte>(kMask[3]));
    // 载荷逐字节 XOR mask。
    CHECK(frame[6] == static_cast<std::byte>(payload[0] ^ kMask[0]));
}

TEST_CASE("ws_frame: 服务端帧编码不带 mask") {
    const auto frame = EncodeServerFrame(WsOpcode::Text, "hi");
    REQUIRE(frame.size() == 4);
    CHECK(frame[0] == static_cast<std::byte>(0x81));
    CHECK(frame[1] == static_cast<std::byte>(0x02));
    CHECK(frame[2] == static_cast<std::byte>('h'));
}

TEST_CASE("ws_frame: 完整消息一次解码") {
    WsFrameDecoder decoder;
    decoder.Feed(ServerTextFrame("{\"op\":0}"));
    const auto first = decoder.TryNext();
    REQUIRE(first.has_value());
    REQUIRE(first->has_value());
    CHECK((*first)->kind == WsFrameEvent::Kind::Message);
    CHECK((*first)->message_opcode == WsOpcode::Text);
    CHECK((*first)->payload == "{\"op\":0}");
    // 没有第二条:expected 本身成功,但值是 nullopt。
    const auto second = decoder.TryNext();
    REQUIRE(second.has_value());
    CHECK_FALSE(second->has_value());
}

TEST_CASE("ws_frame: 半帧——首字节不足时等待,补齐后解出") {
    WsFrameDecoder decoder;
    const std::string frame = ServerTextFrame("abcdef");
    // 只喂前 3 字节(opcode/len/2 字节载荷前 1 个)。
    decoder.Feed(frame.substr(0, 3));
    auto early = decoder.TryNext();
    REQUIRE(early.has_value());
    CHECK_FALSE(early->has_value());  // 字节不够
    decoder.Feed(frame.substr(3));
    const auto done = decoder.TryNext();
    REQUIRE(done.has_value());
    REQUIRE(done->has_value());
    CHECK((*done)->payload == "abcdef");
}

TEST_CASE("ws_frame: 粘帧——两条消息一次喂入,依次解出") {
    WsFrameDecoder decoder;
    decoder.Feed(ServerTextFrame("first"));
    decoder.Feed(ServerTextFrame("second"));
    const auto one = decoder.TryNext();
    const auto two = decoder.TryNext();
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    REQUIRE(one->has_value());
    REQUIRE(two->has_value());
    CHECK((*one)->payload == "first");
    CHECK((*two)->payload == "second");
}

TEST_CASE("ws_frame: 分片消息——fin=0 + continuation 拼装") {
    WsFrameDecoder decoder;
    // 手工构造:text 起始帧 fin=0 "abc",continuation fin=0 "de",
    // continuation fin=1 "f"。
    std::string stream;
    stream.push_back(static_cast<char>(0x01));  // fin=0, text
    stream.push_back(static_cast<char>(3));
    stream += "abc";
    stream.push_back(static_cast<char>(0x00));  // fin=0, continuation
    stream.push_back(static_cast<char>(2));
    stream += "de";
    stream.push_back(static_cast<char>(0x80));  // fin=1, continuation
    stream.push_back(static_cast<char>(1));
    stream += "f";
    decoder.Feed(stream);
    const auto message = decoder.TryNext();
    REQUIRE(message.has_value());
    REQUIRE(message->has_value());
    CHECK((*message)->kind == WsFrameEvent::Kind::Message);
    CHECK((*message)->payload == "abcdef");
}

TEST_CASE("ws_frame: continuation 无起点是协议错(sticky)") {
    WsFrameDecoder decoder;
    std::string stream;
    stream.push_back(static_cast<char>(0x80));  // fin=1, continuation 无起点
    stream.push_back(static_cast<char>(1));
    stream += "x";
    decoder.Feed(stream);
    const auto error = decoder.TryNext();
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().kind == WsFrameErrorKind::ProtocolError);
    // sticky:后续调用恒回同一错。
    const auto again = decoder.TryNext();
    REQUIRE_FALSE(again.has_value());
}

TEST_CASE("ws_frame: 服务端帧带 mask 一律协议错(RFC 6455 §5.1)") {
    WsFrameDecoder decoder;
    std::string stream;
    stream.push_back(static_cast<char>(0x81));
    stream.push_back(static_cast<char>(0x80 | 1));  // mask 位
    stream += std::string(4, '\0');
    stream += "x";
    decoder.Feed(stream);
    REQUIRE_FALSE(decoder.TryNext().has_value());
}

TEST_CASE("ws_frame: RSV 位非零协议错(未协商扩展)") {
    WsFrameDecoder decoder;
    std::string stream;
    stream.push_back(static_cast<char>(0x81 | 0x40));  // RSV1
    stream.push_back(static_cast<char>(1));
    stream += "x";
    decoder.Feed(stream);
    const auto error = decoder.TryNext();
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().kind == WsFrameErrorKind::ProtocolError);
}

TEST_CASE("ws_frame: 控制帧——ping/pong/close 单帧直出") {
    WsFrameDecoder decoder;
    // ping,载荷 4 字节。
    std::string ping_frame;
    ping_frame.push_back(static_cast<char>(0x89));
    ping_frame.push_back(static_cast<char>(4));
    ping_frame += "ping";
    decoder.Feed(ping_frame);
    auto event = decoder.TryNext();
    REQUIRE(event.has_value());
    REQUIRE(event->has_value());
    CHECK((*event)->kind == WsFrameEvent::Kind::Ping);
    CHECK((*event)->payload == "ping");

    // pong 空载荷。
    decoder.Feed(std::string{static_cast<char>(0x8A), static_cast<char>(0x00)});
    event = decoder.TryNext();
    REQUIRE(event.has_value());
    REQUIRE(event->has_value());
    CHECK((*event)->kind == WsFrameEvent::Kind::Pong);

    // close 带码 1000 + reason(载荷 = 2 字节码 + 3 字节 reason = 5)。
    std::string close_frame;
    close_frame.push_back(static_cast<char>(0x88));
    close_frame.push_back(static_cast<char>(5));
    close_frame.push_back(static_cast<char>(0x03));
    close_frame.push_back(static_cast<char>(0xE8));
    close_frame += "bye";
    decoder.Feed(close_frame);
    event = decoder.TryNext();
    REQUIRE(event.has_value());
    REQUIRE(event->has_value());
    CHECK((*event)->kind == WsFrameEvent::Kind::Close);
    CHECK((*event)->close_code == 1000);
    CHECK((*event)->close_reason == "bye");
}

TEST_CASE("ws_frame: close 无载荷按 1005;1 字节载荷协议错") {
    WsFrameDecoder decoder;
    decoder.Feed(std::string{static_cast<char>(0x88), static_cast<char>(0x00)});
    auto event = decoder.TryNext();
    REQUIRE(event.has_value());
    REQUIRE(event->has_value());
    CHECK((*event)->kind == WsFrameEvent::Kind::Close);
    CHECK((*event)->close_code == 1005);

    WsFrameDecoder bad;
    bad.Feed(std::string{static_cast<char>(0x88), static_cast<char>(0x01), 'x'});
    REQUIRE_FALSE(bad.TryNext().has_value());
}

TEST_CASE("ws_frame: 控制帧分片是协议错") {
    WsFrameDecoder decoder;
    decoder.Feed(std::string{static_cast<char>(0x09), static_cast<char>(2)});  // fin=0 ping
    REQUIRE_FALSE(decoder.TryNext().has_value());
}

TEST_CASE("ws_frame: 控制帧载荷超 125 是协议错") {
    WsFrameDecoder decoder;
    std::string stream;
    stream.push_back(static_cast<char>(0x89));  // ping
    stream.push_back(static_cast<char>(126));   // 16 位长度——控制帧不允许
    stream.push_back(static_cast<char>(0x00));
    stream.push_back(static_cast<char>(0x80));
    decoder.Feed(stream);
    const auto error = decoder.TryNext();
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().kind == WsFrameErrorKind::ProtocolError);
}

TEST_CASE("ws_frame: 声明超帽长度立即 MessageTooLarge(不等载荷)") {
    WsFrameDecoder decoder;
    std::string stream;
    stream.push_back(static_cast<char>(0x82));  // binary
    stream.push_back(static_cast<char>(127));   // 64 位长度
    const std::uint64_t huge = kWsMaxMessageBytes + 1;
    for (int shift = 56; shift >= 0; shift -= 8) {
        stream.push_back(static_cast<char>((huge >> shift) & 0xFF));
    }
    decoder.Feed(stream);
    const auto error = decoder.TryNext();
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().kind == WsFrameErrorKind::MessageTooLarge);
}

TEST_CASE("ws_frame: 拼装中消息累计超帽也 TooLarge") {
    WsFrameDecoder decoder;
    // 起始帧 fin=0,声明 65535(KiB 级)——单帧不超帽;循环喂 continuation
    // 累计到 8 MiB+ 才应该报。这里直接用两段 5 MiB 演示累计检查路径
    // (5 MiB 单帧本身 < 8 MiB 帽,合法)。
    const std::string chunk(5 * 1024 * 1024, 'a');
    std::string first_frame;
    first_frame.push_back(static_cast<char>(0x01));  // fin=0 text
    first_frame.push_back(static_cast<char>(127));
    for (int shift = 56; shift >= 0; shift -= 8) {
        first_frame.push_back(static_cast<char>((chunk.size() >> shift) & 0xFF));
    }
    first_frame += chunk;
    decoder.Feed(first_frame);
    auto pending = decoder.TryNext();
    REQUIRE(pending.has_value());
    CHECK_FALSE(pending->has_value());  // 分片进行中,没到 fin

    std::string second_frame;
    second_frame.push_back(static_cast<char>(0x80));  // fin=1 continuation
    second_frame.push_back(static_cast<char>(127));
    const std::size_t second_size = kWsMaxMessageBytes - chunk.size() + 1;
    for (int shift = 56; shift >= 0; shift -= 8) {
        second_frame.push_back(static_cast<char>((second_size >> shift) & 0xFF));
    }
    decoder.Feed(second_frame.substr(0, 20));  // 只喂头部,长度检查即触发
    const auto error = decoder.TryNext();
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().kind == WsFrameErrorKind::MessageTooLarge);
}

TEST_CASE("ws_frame: 分片进行中来新数据帧是协议错") {
    WsFrameDecoder decoder;
    std::string stream;
    stream.push_back(static_cast<char>(0x01));  // fin=0 text
    stream.push_back(static_cast<char>(1));
    stream += "a";
    stream.push_back(static_cast<char>(0x81));  // 新 text 帧(非法:拼装中)
    stream.push_back(static_cast<char>(1));
    stream += "b";
    decoder.Feed(stream);
    REQUIRE_FALSE(decoder.TryNext().has_value());
}

}  // namespace lubancode::channel::qq
