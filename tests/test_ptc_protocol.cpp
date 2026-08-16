// PTC 协议(帧 + 消息)纯逻辑单测:编解码往返、半截帧、坏负载、各消息型。

#include <doctest/doctest.h>

#include "ptc/protocol.hpp"

using namespace lubancode::ptc;

TEST_CASE("EncodeFrame/FrameDecoder: 编解码往返,长度头按小端收") {
    const std::string payload = R"({"type":"hello","protocol":1,"python":"3.11.8"})";
    const std::string frame = EncodeFrame(payload);
    REQUIRE(frame.size() == kFrameHeaderBytes + payload.size());
    // 小端长度头:低字节在前。
    const auto length = static_cast<std::uint32_t>(payload.size());
    CHECK(static_cast<unsigned char>(frame[0]) == static_cast<unsigned char>(length & 0xFFU));
    CHECK(static_cast<unsigned char>(frame[1]) == static_cast<unsigned char>((length >> 8U) & 0xFFU));
    CHECK(frame[2] == 0);
    CHECK(frame[3] == 0);

    FrameDecoder decoder;
    std::vector<std::string> frames;
    REQUIRE(decoder.Feed(frame, frames).has_value());
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == payload);
    CHECK(decoder.buffered() == 0);
}

TEST_CASE("FrameDecoder: 半截帧跨 Feed 攒齐,两帧连发一次吐") {
    const std::string one = EncodeFrame(R"({"type":"call","id":1,"tool":"a","input":{}})");
    const std::string two = EncodeFrame(R"({"type":"emit","value":[1,2,3]})");
    const std::string all = one + two;

    FrameDecoder decoder;
    std::vector<std::string> frames;
    // 先喂前一半(从中间劈开),再喂后一半。
    REQUIRE(decoder.Feed(std::string_view(all).substr(0, all.size() / 2), frames).has_value());
    CHECK(frames.empty());
    CHECK(decoder.buffered() > 0);
    REQUIRE(decoder.Feed(std::string_view(all).substr(all.size() / 2), frames).has_value());
    REQUIRE(frames.size() == 2);
    CHECK(ParseGuestMessage(frames[0]).value().kind == GuestMessage::Kind::Call);
    CHECK(ParseGuestMessage(frames[1]).value().kind == GuestMessage::Kind::Emit);
}

TEST_CASE("FrameDecoder: 超大长度头直接报错,不傻等") {
    FrameDecoder decoder;
    std::vector<std::string> frames;
    std::string evil(4, '\0');
    evil[0] = static_cast<char>(0xFF);
    evil[1] = static_cast<char>(0xFF);
    evil[2] = static_cast<char>(0xFF);
    evil[3] = static_cast<char>(0x7F);  // 0x7FFFFFFF > 32MiB 上限
    const auto result = decoder.Feed(evil, frames);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("超过协议上限") != std::string::npos);
}

TEST_CASE("ParseGuestMessage: hello/call/emit/fail/done 各字段") {
    const auto hello = ParseGuestMessage(R"({"type":"hello","protocol":1,"python":"3.12.1"})");
    REQUIRE(hello.has_value());
    CHECK(hello->kind == GuestMessage::Kind::Hello);
    CHECK(hello->protocol == 1);
    CHECK(hello->python == "3.12.1");

    const auto call = ParseGuestMessage(R"({"type":"call","id":42,"tool":"read_file","input":{"path":"中文路径/反斜杠\\引号\"引号"}})");
    REQUIRE(call.has_value());
    CHECK(call->kind == GuestMessage::Kind::Call);
    CHECK(call->id == 42);
    CHECK(call->tool == "read_file");
    CHECK(call->input.at("path").is_string());

    const auto emit = ParseGuestMessage(R"({"type":"emit","value":{"emoji":"🎉"}})");
    REQUIRE(emit.has_value());
    CHECK(emit->kind == GuestMessage::Kind::Emit);
    CHECK(emit->value.at("emoji") == "🎉");

    const auto fail = ParseGuestMessage(R"({"type":"fail","stage":"syntax","error":"SyntaxError: bad","traceback":"tb"})");
    REQUIRE(fail.has_value());
    CHECK(fail->kind == GuestMessage::Kind::Fail);
    CHECK(fail->stage == "syntax");

    const auto done = ParseGuestMessage(R"({"type":"done","captured_stdout":"print 了","calls":7})");
    REQUIRE(done.has_value());
    CHECK(done->kind == GuestMessage::Kind::Done);
    CHECK(done->captured_stdout == "print 了");
    CHECK(done->calls == 7);
}

TEST_CASE("ParseGuestMessage: 坏负载逐个拒") {
    CHECK_FALSE(ParseGuestMessage("not json").has_value());
    CHECK_FALSE(ParseGuestMessage("[1,2,3]").has_value());                  // 不是 object
    CHECK_FALSE(ParseGuestMessage(R"({"type":"call","tool":"x"})").has_value());     // 缺 id
    CHECK_FALSE(ParseGuestMessage(R"({"type":"call","id":"x","tool":"t","input":{}})").has_value()); // id 非数字
    CHECK_FALSE(ParseGuestMessage(R"({"type":"call","id":1,"tool":"t","input":[1]})").has_value());  // input 非对象
    CHECK_FALSE(ParseGuestMessage(R"({"type":"fail","stage":"whatever","error":"x"})").has_value()); // 非法 stage
    CHECK_FALSE(ParseGuestMessage(R"({"type":"mystery"})").has_value());    // 未知 type
    // type 缺席
    CHECK_FALSE(ParseGuestMessage(R"({"id":1})").has_value());
}

TEST_CASE("BuildResultPayload/BuildAbortPayload: 方向与字段") {
    const std::string ok_payload = BuildResultPayload(7, true, nlohmann::json{{"content", "hi"}}, "");
    const auto parsed_ok = nlohmann::json::parse(ok_payload);
    CHECK(parsed_ok.at("type") == "result");
    CHECK(parsed_ok.at("id") == 7);
    CHECK(parsed_ok.at("ok") == true);
    CHECK(parsed_ok.at("value").at("content") == "hi");
    CHECK_FALSE(parsed_ok.contains("error"));

    const std::string err_payload = BuildResultPayload(8, false, nlohmann::json::object(), "被 PreToolUse 钩子拦截");
    const auto parsed_err = nlohmann::json::parse(err_payload);
    CHECK(parsed_err.at("ok") == false);
    CHECK(parsed_err.at("error") == "被 PreToolUse 钩子拦截");
    CHECK_FALSE(parsed_err.contains("value"));

    const auto abort = nlohmann::json::parse(BuildAbortPayload("wall clock limit"));
    CHECK(abort.at("type") == "abort");
    CHECK(abort.at("reason") == "wall clock limit");
}
