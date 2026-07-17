// 拿《OpenAI兼容-Responses.md》里"流式输出"一节的真实事件样例(以及按同一
// 协议家族外推出的 function_call 系列事件——文档只把这套事件流程完整展示在
// MCP/内置工具上,自定义 function 工具复用同一套 response.output_item.added
// /response.function_call_arguments.delta/response.output_item.done 骨架)
// 当 fixture,验证 parse_event 映射到 StreamEvent 是否正确;顺带验证未知
// 事件类型、坏掉的 JSON 都不会崩。

#include <doctest/doctest.h>

#include <variant>

#include "api/responses/events.hpp"
#include "api/sse_framing.hpp"
#include "api/types.hpp"

using namespace lubancode::api;
using lubancode::api::responses::parse_event;

namespace {
SseFrame Frame(std::string data) {
    return SseFrame{"message", std::move(data)};
}
}  // namespace

TEST_CASE("response.created 不产生事件") {
    auto event = parse_event(Frame(
        R"({"response":{"id":"428c90e9-9cd6-90a6-9726-c02b08ebexxx","created_at":1769082930,"object":"response","status":"queued"},"sequence_number":0,"type":"response.created"})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("response.in_progress 不产生事件") {
    auto event = parse_event(Frame(
        R"({"response":{"id":"428c90e9-9cd6-90a6-9726-c02b08ebexxx","status":"in_progress"},"sequence_number":1,"type":"response.in_progress"})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("response.output_item.added 类型是 message 时不产生事件,靠 delta 拼文本") {
    auto event = parse_event(Frame(
        R"({"item":{"id":"msg_bcb45d66-fc34-46a2-bb56-714a51e8exxx","content":[],"role":"assistant","status":"in_progress","type":"message"},"output_index":0,"sequence_number":2,"type":"response.output_item.added"})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("response.output_item.added 类型是 reasoning 时静默跳过") {
    auto event = parse_event(Frame(
        R"({"sequence_number":2,"item":{"summary":[],"type":"reasoning","id":"msg_5bd0c6df-19b8-4a04-bc00-8042a224exxx"},"output_index":0,"type":"response.output_item.added"})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("response.output_item.added 类型是 function_call 时映射出 ToolUseStart") {
    auto event = parse_event(Frame(
        R"({"item":{"id":"msg_fc1","name":"read_file","arguments":"","call_id":"call_abc123","status":"in_progress","type":"function_call"},"output_index":1,"sequence_number":11,"type":"response.output_item.added"})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ToolUseStart>(*event));
    const auto& start = std::get<ToolUseStart>(*event);
    CHECK(start.index == 1);
    CHECK(start.id == "call_abc123");
    CHECK(start.name == "read_file");
}

TEST_CASE("response.output_text.delta 映射出 TextDelta") {
    auto event = parse_event(Frame(
        R"({"content_index":0,"delta":"人工智能","item_id":"msg_bcb45d66-fc34-46a2-bb56-714a51e8exxx","logprobs":[],"output_index":0,"sequence_number":4,"type":"response.output_text.delta"})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<TextDelta>(*event));
    CHECK(std::get<TextDelta>(*event).text == "人工智能");
}

TEST_CASE("response.function_call_arguments.delta 映射出 ToolUseInputDelta") {
    auto event = parse_event(Frame(
        R"({"delta":"{\"path\": \"vcpkg","sequence_number":12,"output_index":1,"type":"response.function_call_arguments.delta","item_id":"msg_fc1"})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ToolUseInputDelta>(*event));
    const auto& delta = std::get<ToolUseInputDelta>(*event);
    CHECK(delta.index == 1);
    CHECK(delta.partial_json == R"({"path": "vcpkg)");
}

TEST_CASE("response.function_call_arguments.done 不单独产生事件(delta 已经拼完了)") {
    auto event = parse_event(Frame(
        R"({"arguments":"{\"path\": \"vcpkg.json\"}","sequence_number":13,"output_index":1,"type":"response.function_call_arguments.done","item_id":"msg_fc1"})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("response.output_item.done 类型是 message 时映射出 ContentBlockDone") {
    auto event = parse_event(Frame(
        R"({"item":{"id":"msg_bcb45d66-fc34-46a2-bb56-714a51e8exxx","content":[{"annotations":[],"text":"你好","type":"output_text"}],"role":"assistant","status":"completed","type":"message"},"output_index":0,"sequence_number":55,"type":"response.output_item.done"})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ContentBlockDone>(*event));
    CHECK(std::get<ContentBlockDone>(*event).index == 0);
}

TEST_CASE("response.output_item.done 类型是 function_call 时映射出 ContentBlockDone") {
    auto event = parse_event(Frame(
        R"({"item":{"id":"msg_fc1","name":"read_file","arguments":"{\"path\": \"vcpkg.json\"}","call_id":"call_abc123","status":"completed","type":"function_call"},"output_index":1,"sequence_number":14,"type":"response.output_item.done"})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ContentBlockDone>(*event));
    CHECK(std::get<ContentBlockDone>(*event).index == 1);
}

TEST_CASE("response.output_item.done 类型是 reasoning 时静默跳过(没有对应的开块)") {
    auto event = parse_event(Frame(
        R"({"sequence_number":14,"item":{"summary":[{"type":"summary_text","text":"..."}],"type":"reasoning","id":"msg_5bd0c6df-19b8-4a04-bc00-8042a224exxx"},"output_index":1,"type":"response.output_item.done"})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("response.reasoning_summary_text.delta / done 静默跳过") {
    auto delta_event = parse_event(Frame(
        R"({"delta":"用户想要...","sequence_number":3,"output_index":0,"type":"response.reasoning_summary_text.delta","item_id":"msg_xxx","summary_index":0})"));
    CHECK_FALSE(delta_event.has_value());

    auto done_event = parse_event(Frame(
        R"({"sequence_number":13,"text":"用户想要...","output_index":0,"type":"response.reasoning_summary_text.done","item_id":"msg_xxx","summary_index":0})"));
    CHECK_FALSE(done_event.has_value());
}

TEST_CASE("response.completed:纯文本回复,output 里没有 function_call,stop_reason 是 end_turn") {
    auto event = parse_event(Frame(
        R"({"type":"response.completed","response":{"id":"428c90e9-9cd6-90a6-9726-c02b08ebexxx","status":"completed","output":[{"type":"message","id":"msg_1","role":"assistant","status":"completed","content":[{"type":"output_text","text":"你好"}]}],"usage":{"input_tokens":37,"output_tokens":243,"total_tokens":280}},"sequence_number":56})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageDone>(*event));
    const auto& done = std::get<MessageDone>(*event);
    CHECK(done.stop_reason == "end_turn");
    CHECK(done.usage.input_tokens == 37);
    CHECK(done.usage.output_tokens == 243);
}

TEST_CASE("response.completed:usage.input_tokens_details.cached_tokens 映射进 cache_read_tokens") {
    auto event = parse_event(Frame(
        R"({"type":"response.completed","response":{"id":"resp_cache","status":"completed","output":[{"type":"message","id":"msg_1","role":"assistant","status":"completed","content":[{"type":"output_text","text":"你好"}]}],"usage":{"input_tokens":1578,"output_tokens":83,"total_tokens":1661,"input_tokens_details":{"cached_tokens":128}}},"sequence_number":56})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageDone>(*event));
    const auto& done = std::get<MessageDone>(*event);
    CHECK(done.usage.input_tokens == 1578);
    CHECK(done.usage.output_tokens == 83);
    CHECK(done.usage.cache_read_tokens == 128);
    CHECK(done.usage.cache_creation_tokens == 0);  // responses wire 没有缓存写入这个概念
}

TEST_CASE("response.completed:没有 input_tokens_details 字段时,cache_read_tokens 落 0,不崩") {
    auto event = parse_event(Frame(
        R"({"type":"response.completed","response":{"id":"resp_nocache","status":"completed","output":[],"usage":{"input_tokens":37,"output_tokens":243,"total_tokens":280}},"sequence_number":56})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageDone>(*event));
    CHECK(std::get<MessageDone>(*event).usage.cache_read_tokens == 0);
}

TEST_CASE("response.completed:output 里带 function_call,stop_reason 相当于 tool_use") {
    auto event = parse_event(Frame(
        R"({"type":"response.completed","response":{"id":"resp_1","status":"completed","output":[{"type":"function_call","id":"msg_fc1","name":"read_file","arguments":"{\"path\":\"vcpkg.json\"}","call_id":"call_abc123","status":"completed"}],"usage":{"input_tokens":50,"output_tokens":20,"total_tokens":70}},"sequence_number":9})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageDone>(*event));
    CHECK(std::get<MessageDone>(*event).stop_reason == "tool_use");
}

TEST_CASE("response.completed:status 是 incomplete 时 stop_reason 是 max_tokens") {
    auto event = parse_event(Frame(
        R"({"type":"response.completed","response":{"id":"resp_2","status":"incomplete","output":[{"type":"message","id":"msg_1","role":"assistant","status":"incomplete","content":[{"type":"output_text","text":"没说完"}]}],"usage":{"input_tokens":10,"output_tokens":4096,"total_tokens":4106}},"sequence_number":9})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageDone>(*event));
    CHECK(std::get<MessageDone>(*event).stop_reason == "max_tokens");
}

TEST_CASE("response.failed 映射出 StreamError") {
    auto event = parse_event(Frame(
        R"({"type":"response.failed","response":{"id":"resp_3","status":"failed","error":{"message":"服务器繁忙,请稍后重试"}}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<StreamError>(*event));
    CHECK(std::get<StreamError>(*event).message == "服务器繁忙,请稍后重试");
}

TEST_CASE("error 事件映射出 StreamError") {
    auto event = parse_event(Frame(R"({"type":"error","error":{"message":"参数不合法"}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<StreamError>(*event));
    CHECK(std::get<StreamError>(*event).message == "参数不合法");
}

TEST_CASE("未知事件类型静默跳过,不崩") {
    auto event = parse_event(Frame(R"({"type":"response.web_search_call.in_progress","output_index":1})"));
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

TEST_CASE("完整的纯文本流式响应,按顺序喂给分帧器再解析,事件序列符合预期") {
    SseFramer framer;
    const std::string raw =
        "data: {\"type\":\"response.created\",\"response\":{\"status\":\"queued\"},\"sequence_number\":0}\n\n"
        "data: {\"type\":\"response.in_progress\",\"response\":{\"status\":\"in_progress\"},\"sequence_number\":1}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_1\",\"content\":[],\"role\":\"assistant\",\"status\":\"in_progress\",\"type\":\"message\"},\"output_index\":0,\"sequence_number\":2}\n\n"
        "data: {\"type\":\"response.content_part.added\",\"content_index\":0,\"item_id\":\"msg_1\",\"output_index\":0,\"part\":{\"annotations\":[],\"text\":\"\",\"type\":\"output_text\"},\"sequence_number\":3}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"content_index\":0,\"delta\":\"你好\",\"item_id\":\"msg_1\",\"output_index\":0,\"sequence_number\":4}\n\n"
        "data: {\"type\":\"response.output_text.done\",\"content_index\":0,\"item_id\":\"msg_1\",\"output_index\":0,\"text\":\"你好\",\"sequence_number\":5}\n\n"
        "data: {\"type\":\"response.content_part.done\",\"content_index\":0,\"item_id\":\"msg_1\",\"output_index\":0,\"part\":{\"annotations\":[],\"text\":\"你好\",\"type\":\"output_text\"},\"sequence_number\":6}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"msg_1\",\"content\":[{\"annotations\":[],\"text\":\"你好\",\"type\":\"output_text\"}],\"role\":\"assistant\",\"status\":\"completed\",\"type\":\"message\"},\"output_index\":0,\"sequence_number\":7}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_x\",\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"id\":\"msg_1\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"你好\"}]}],\"usage\":{\"input_tokens\":10,\"output_tokens\":2,\"total_tokens\":12}},\"sequence_number\":8}\n\n";

    std::vector<StreamEvent> events;
    for (const SseFrame& frame : framer.feed(raw)) {
        if (auto event = parse_event(frame); event.has_value()) {
            events.push_back(*event);
        }
    }

    // response.created / in_progress / output_item.added(message) / content_part.* /
    // output_text.done 都不产生事件;只剩 output_text.delta、output_item.done、
    // response.completed 三个。
    REQUIRE(events.size() == 3);
    REQUIRE(std::holds_alternative<TextDelta>(events[0]));
    CHECK(std::get<TextDelta>(events[0]).text == "你好");
    REQUIRE(std::holds_alternative<ContentBlockDone>(events[1]));
    CHECK(std::get<ContentBlockDone>(events[1]).index == 0);
    REQUIRE(std::holds_alternative<MessageDone>(events[2]));
    CHECK(std::get<MessageDone>(events[2]).stop_reason == "end_turn");
}

TEST_CASE("完整的 function_call 流式响应,arguments 劈成好几段,事件序列符合预期") {
    SseFramer framer;
    const std::string raw =
        "data: {\"type\":\"response.created\",\"response\":{\"status\":\"queued\"},\"sequence_number\":0}\n\n"
        "data: {\"type\":\"response.in_progress\",\"response\":{\"status\":\"in_progress\"},\"sequence_number\":1}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_fc1\",\"name\":\"read_file\",\"arguments\":\"\",\"call_id\":\"call_abc123\",\"status\":\"in_progress\",\"type\":\"function_call\"},\"output_index\":0,\"sequence_number\":2}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\",\"delta\":\"{\\\"path\\\":\",\"output_index\":0,\"item_id\":\"msg_fc1\",\"sequence_number\":3}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\",\"delta\":\"\\\"vcpkg.json\\\"}\",\"output_index\":0,\"item_id\":\"msg_fc1\",\"sequence_number\":4}\n\n"
        "data: {\"type\":\"response.function_call_arguments.done\",\"arguments\":\"{\\\"path\\\":\\\"vcpkg.json\\\"}\",\"output_index\":0,\"item_id\":\"msg_fc1\",\"sequence_number\":5}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"msg_fc1\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"vcpkg.json\\\"}\",\"call_id\":\"call_abc123\",\"status\":\"completed\",\"type\":\"function_call\"},\"output_index\":0,\"sequence_number\":6}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_y\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\",\"id\":\"msg_fc1\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"vcpkg.json\\\"}\",\"call_id\":\"call_abc123\",\"status\":\"completed\"}],\"usage\":{\"input_tokens\":50,\"output_tokens\":20,\"total_tokens\":70}},\"sequence_number\":7}\n\n";

    std::vector<StreamEvent> events;
    for (const SseFrame& frame : framer.feed(raw)) {
        if (auto event = parse_event(frame); event.has_value()) {
            events.push_back(*event);
        }
    }

    REQUIRE(events.size() == 5);
    REQUIRE(std::holds_alternative<ToolUseStart>(events[0]));
    CHECK(std::get<ToolUseStart>(events[0]).id == "call_abc123");
    CHECK(std::get<ToolUseStart>(events[0]).name == "read_file");

    REQUIRE(std::holds_alternative<ToolUseInputDelta>(events[1]));
    REQUIRE(std::holds_alternative<ToolUseInputDelta>(events[2]));
    const std::string joined = std::get<ToolUseInputDelta>(events[1]).partial_json + std::get<ToolUseInputDelta>(events[2]).partial_json;
    CHECK(joined == R"({"path":"vcpkg.json"})");

    REQUIRE(std::holds_alternative<ContentBlockDone>(events[3]));

    REQUIRE(std::holds_alternative<MessageDone>(events[4]));
    CHECK(std::get<MessageDone>(events[4]).stop_reason == "tool_use");
}

TEST_CASE("字段类型不对(type_error 一族)不抛异常、不崩,当坏帧跳过") {
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"response.output_text.delta","delta":42})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"response.output_item.added","output_index":"x","item":{"type":"function_call","call_id":[],"name":9}})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"response.function_call_arguments.delta","output_index":0,"delta":{}})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"response.output_item.done","output_index":[],"item":{"type":"message"}})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"response.completed","response":{"status":123,"usage":{"input_tokens":"a"}}})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"error","error":{"message":[]}})")));

    auto bad = parse_event(Frame(R"({"type":"response.output_text.delta","delta":42})"));
    CHECK_FALSE(bad.has_value());
}
