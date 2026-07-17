// 拿《Anthropic兼容-Messages.md》里"流式响应"一节的真实事件样例当 fixture,
// 验证 parse_event 映射到 StreamEvent 是否正确;顺带验证未知事件类型、
// 坏掉的 JSON 都不会崩。

#include <doctest/doctest.h>

#include <variant>

#include "api/anthropic/events.hpp"
#include "api/sse_framing.hpp"
#include "api/types.hpp"

using namespace lubancode::api;
using lubancode::api::anthropic::parse_event;

namespace {
SseFrame Frame(std::string data) {
    return SseFrame{"message", std::move(data)};
}
}  // namespace

TEST_CASE("message_start 映射出 MessageStart,带上 id 和 model") {
    auto event = parse_event(Frame(
        R"({"type":"message_start","message":{"id":"msg_xxx","type":"message","role":"assistant","model":"qwen3.7-plus","content":[],"usage":{"input_tokens":15,"output_tokens":0}}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageStart>(*event));
    const auto& start = std::get<MessageStart>(*event);
    CHECK(start.id == "msg_xxx");
    CHECK(start.model == "qwen3.7-plus");
}

TEST_CASE("content_block_start 类型是 text 时不产生事件,靠 delta 拼文本") {
    auto event = parse_event(Frame(R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("content_block_start 类型是 thinking 时也静默跳过") {
    auto event = parse_event(Frame(
        R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("content_block_start 类型是 tool_use 时映射出 ToolUseStart") {
    auto event = parse_event(Frame(
        R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"toolu_01","name":"get_weather","input":{}}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ToolUseStart>(*event));
    const auto& start = std::get<ToolUseStart>(*event);
    CHECK(start.index == 2);
    CHECK(start.id == "toolu_01");
    CHECK(start.name == "get_weather");
}

TEST_CASE("content_block_delta 的 text_delta 映射出 TextDelta") {
    auto event = parse_event(Frame(R"({"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"人工智能"}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<TextDelta>(*event));
    CHECK(std::get<TextDelta>(*event).text == "人工智能");
}

TEST_CASE("content_block_delta 的 thinking_delta 静默跳过") {
    auto event = parse_event(Frame(R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"分析一下"}})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("content_block_delta 的 signature_delta 静默跳过") {
    auto event = parse_event(Frame(R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":""}})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("content_block_delta 的 input_json_delta 映射出 ToolUseInputDelta") {
    auto event = parse_event(Frame(
        R"({"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\"city\":\"杭州\"}"}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ToolUseInputDelta>(*event));
    const auto& delta = std::get<ToolUseInputDelta>(*event);
    CHECK(delta.index == 2);
    CHECK(delta.partial_json == R"({"city":"杭州"})");
}

TEST_CASE("content_block_stop 映射出 ContentBlockDone") {
    auto event = parse_event(Frame(R"({"type":"content_block_stop","index":1})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ContentBlockDone>(*event));
    CHECK(std::get<ContentBlockDone>(*event).index == 1);
}

TEST_CASE("message_delta 映射出 MessageDone,带上 stop_reason 和完整 usage") {
    auto event = parse_event(Frame(
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"input_tokens":15,"output_tokens":1078,"cache_creation_input_tokens":0,"cache_read_input_tokens":0}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageDone>(*event));
    const auto& done = std::get<MessageDone>(*event);
    CHECK(done.stop_reason == "end_turn");
    CHECK(done.usage.input_tokens == 15);
    CHECK(done.usage.output_tokens == 1078);
}

TEST_CASE("message_stop 不重复产生事件,MessageDone 已经在 message_delta 时发过") {
    auto event = parse_event(Frame(R"({"type":"message_stop"})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("ping 心跳静默跳过") {
    auto event = parse_event(Frame(R"({"type":"ping"})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("error 事件映射出 StreamError") {
    auto event = parse_event(Frame(
        R"({"type":"error","error":{"type":"overloaded_error","message":"服务器繁忙,请稍后重试"}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<StreamError>(*event));
    CHECK(std::get<StreamError>(*event).message == "服务器繁忙,请稍后重试");
}

TEST_CASE("完整的一轮流式响应,按顺序喂给分帧器再解析,事件序列符合预期") {
    // 直接照抄文档里"流式响应示例"那一段(略去了 thinking 部分,只测
    // 文本回复这条主线),验证分帧器 + events 语义层串起来能用。
    SseFramer framer;
    const std::string raw =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_xxx\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"qwen3.7-plus\",\"content\":[],\"usage\":{\"input_tokens\":15,\"output_tokens\":0}}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"你好\"}}\n\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"input_tokens\":15,\"output_tokens\":3,\"cache_creation_input_tokens\":0,\"cache_read_input_tokens\":0}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";

    std::vector<StreamEvent> events;
    for (const SseFrame& frame : framer.feed(raw)) {
        if (auto event = parse_event(frame); event.has_value()) {
            events.push_back(*event);
        }
    }

    // message_start、text_delta、content_block_stop(->ContentBlockDone)、
    // message_delta(->MessageDone)各发一个事件;content_block_start(text
    // 类型)和 message_stop 都不单独发事件。
    REQUIRE(events.size() == 4);
    CHECK(std::holds_alternative<MessageStart>(events[0]));
    REQUIRE(std::holds_alternative<TextDelta>(events[1]));
    CHECK(std::get<TextDelta>(events[1]).text == "你好");
    REQUIRE(std::holds_alternative<ContentBlockDone>(events[2]));
    CHECK(std::get<ContentBlockDone>(events[2]).index == 0);
    REQUIRE(std::holds_alternative<MessageDone>(events[3]));
    CHECK(std::get<MessageDone>(events[3]).stop_reason == "end_turn");
}

TEST_CASE("未知事件类型静默跳过,不崩") {
    auto event = parse_event(Frame(R"({"type":"some_future_event","foo":"bar"})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("坏掉的 JSON 不会崩,只是跳过") {
    auto event = parse_event(Frame("{not valid json"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("data 里没有 type 字段,也不会崩") {
    auto event = parse_event(Frame(R"({"foo":"bar"})"));
    CHECK_FALSE(event.has_value());
}
