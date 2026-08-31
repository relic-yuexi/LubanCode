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

TEST_CASE("response.reasoning_summary_text.delta 映射出 ThinkingDelta,done 静默跳过") {
    auto delta_event = parse_event(Frame(
        R"({"delta":"用户想要...","sequence_number":3,"output_index":0,"type":"response.reasoning_summary_text.delta","item_id":"msg_xxx","summary_index":0})"));
    REQUIRE(delta_event.has_value());
    REQUIRE(std::holds_alternative<ThinkingDelta>(*delta_event));
    CHECK(std::get<ThinkingDelta>(*delta_event).text == "用户想要...");

    auto done_event = parse_event(Frame(
        R"({"sequence_number":13,"text":"用户想要...","output_index":0,"type":"response.reasoning_summary_text.done","item_id":"msg_xxx","summary_index":0})"));
    CHECK_FALSE(done_event.has_value());
}

// vLLM 本地模型四 wire 支持勘察单 P0:思考正文走 reasoning_text 系
// (vLLM 扩展,OpenAI 新版 API 同名),与 summary 系(服务端摘要)不同源。
// 落地前 parser 只认 summary 系,reasoning_text.delta 帧帧静默跳过,
// 思考整段丢——这两册就是那道回归钉。
TEST_CASE("response.reasoning_text.delta 映射出 ThinkingDelta,done/part 系静默跳过") {
    auto delta_event = parse_event(Frame(
        R"({"delta":"We","item_id":"rs_aeb964886f47be44","output_index":0,"type":"response.reasoning_text.delta"})"));
    REQUIRE(delta_event.has_value());
    REQUIRE(std::holds_alternative<ThinkingDelta>(*delta_event));
    CHECK(std::get<ThinkingDelta>(*delta_event).text == "We");

    auto done_event = parse_event(Frame(
        R"({"text":"We need answer user: 2+2=4.","item_id":"rs_aeb964886f47be44","output_index":0,"type":"response.reasoning_text.done"})"));
    CHECK_FALSE(done_event.has_value());

    auto part_added = parse_event(Frame(
        R"({"item_id":"rs_aeb964886f47be44","output_index":0,"part":{"text":"","type":"reasoning_text"},"type":"response.reasoning_part.added"})"));
    CHECK_FALSE(part_added.has_value());

    auto part_done = parse_event(Frame(
        R"({"item_id":"rs_aeb964886f47be44","output_index":0,"part":{"text":"We need answer user: 2+2=4.","type":"reasoning_text"},"type":"response.reasoning_part.done"})"));
    CHECK_FALSE(part_done.has_value());
}

TEST_CASE("summary 系与 reasoning_text 系并存不串线:两只事件名同帧流各吐各的 ThinkingDelta") {
    auto summary_delta = parse_event(Frame(
        R"({"delta":"摘要一片","output_index":0,"type":"response.reasoning_summary_text.delta","item_id":"rs_x","summary_index":0})"));
    REQUIRE(summary_delta.has_value());
    CHECK(std::get<ThinkingDelta>(*summary_delta).text == "摘要一片");

    auto text_delta = parse_event(Frame(
        R"({"delta":"思考一片","output_index":0,"type":"response.reasoning_text.delta","item_id":"rs_y"})"));
    REQUIRE(text_delta.has_value());
    CHECK(std::get<ThinkingDelta>(*text_delta).text == "思考一片");

    // 空 delta 不发空事件(与 summary 系同一道闸)。
    auto empty_delta = parse_event(Frame(
        R"({"delta":"","output_index":0,"type":"response.reasoning_text.delta","item_id":"rs_y"})"));
    CHECK_FALSE(empty_delta.has_value());
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
    // 统一口径(前缀缓存守恒单):cached_tokens 已含在 input_tokens 总数里,
    // 摊开成 input=1578-128=1450、cache_read=128——消费端(TotalInputTokens)
    // 加回去才是 1578,不再把 cached 算两遍。
    CHECK(done.usage.input_tokens == 1450);
    CHECK(done.usage.output_tokens == 83);
    CHECK(done.usage.cache_read_tokens == 128);
    CHECK(TotalInputTokens(done.usage) == 1578);
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

TEST_CASE("response.completed:output 里的 image_generation_call 不再当错误,照常 MessageDone") {
    // 巡检单 P0 起图片接线:completed 里的图片条目不是失败信号,收尾判定
    // 里当普通非工具条目看(stop_reason 仍 end_turn);正文的唯一来路是
    // output_item.done(见下),宿主按 item id 去重。
    auto event = parse_event(Frame(
        R"({"type":"response.completed","response":{"id":"resp_image","status":"completed","output":[{"type":"image_generation_call","id":"ig_1","status":"completed","output_format":"png","result":"iVBORw0KGgoAAA_TEST_SENTINEL"},{"type":"message","id":"msg_1","role":"assistant","status":"completed","content":[{"type":"output_text","text":"图片已生成"}]}],"usage":{"input_tokens":25,"output_tokens":3,"total_tokens":28}},"sequence_number":17})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageDone>(*event));
    CHECK(std::get<MessageDone>(*event).stop_reason == "end_turn");
}

TEST_CASE("response.output_item.added/done 类型是 image_generation_call:开卡 + ImageOutput") {
    const auto start = parse_event(Frame(
        R"({"type":"response.output_item.added","item":{"id":"ig_1","type":"image_generation_call","status":"in_progress"},"output_index":0,"sequence_number":2})"));
    REQUIRE(start.has_value());
    REQUIRE(std::holds_alternative<BuiltinToolStart>(*start));
    CHECK(std::get<BuiltinToolStart>(*start).id == "ig_1");
    CHECK(std::get<BuiltinToolStart>(*start).name == "image_generation");

    const auto done = parse_event(Frame(
        R"({"type":"response.output_item.done","item":{"id":"ig_1","type":"image_generation_call","status":"completed","action":"generate","output_format":"png","result":"iVBORw0KGgoAAA_TEST_SENTINEL","size":"1024x1024"},"output_index":0,"sequence_number":6})"));
    REQUIRE(done.has_value());
    REQUIRE(std::holds_alternative<ImageOutput>(*done));
    CHECK(std::get<ImageOutput>(*done).id == "ig_1");
    CHECK(std::get<ImageOutput>(*done).base64 == "iVBORw0KGgoAAA_TEST_SENTINEL");
}

TEST_CASE("response.output_item.done 类型是 image_generation_call 但 result 为空:不发 ImageOutput") {
    // 服务端生成失败(status=failed / result 缺席)或被掐:没有正文可救,
    // 不发事件,不造半张图。
    auto failed = parse_event(Frame(
        R"({"type":"response.output_item.done","item":{"id":"ig_bad","type":"image_generation_call","status":"failed"},"output_index":0,"sequence_number":6})"));
    CHECK_FALSE(failed.has_value());

    auto empty = parse_event(Frame(
        R"({"type":"response.output_item.done","item":{"id":"ig_empty","type":"image_generation_call","status":"completed","result":""},"output_index":0,"sequence_number":7})"));
    CHECK_FALSE(empty.has_value());
}

TEST_CASE("多图流:两张 image_generation_call 各自开卡、各带一份正文,文本夹在中间不乱") {
    SseFramer framer;
    const std::string raw =
        "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"ig_a\",\"type\":\"image_generation_call\",\"status\":\"in_progress\"},\"output_index\":0,\"sequence_number\":1}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"ig_a\",\"type\":\"image_generation_call\",\"status\":\"completed\",\"result\":\"AAAA_aaaa\"},\"output_index\":0,\"sequence_number\":2}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"status\":\"in_progress\",\"content\":[]},\"output_index\":1,\"sequence_number\":3}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_1\",\"output_index\":1,\"content_index\":0,\"delta\":\"两张图\",\"sequence_number\":4}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"两张图\"}]},\"output_index\":1,\"sequence_number\":5}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"ig_b\",\"type\":\"image_generation_call\",\"status\":\"in_progress\"},\"output_index\":2,\"sequence_number\":6}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"ig_b\",\"type\":\"image_generation_call\",\"status\":\"completed\",\"result\":\"BBBB_bbbb\"},\"output_index\":2,\"sequence_number\":7}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_multi\",\"status\":\"completed\",\"output\":[{\"type\":\"image_generation_call\",\"id\":\"ig_a\",\"result\":\"AAAA_aaaa\"},{\"type\":\"message\",\"id\":\"msg_1\",\"content\":[{\"type\":\"output_text\",\"text\":\"两张图\"}]},{\"type\":\"image_generation_call\",\"id\":\"ig_b\",\"result\":\"BBBB_bbbb\"}],\"usage\":{\"input_tokens\":30,\"output_tokens\":5}},\"sequence_number\":8}\n\n";

    std::vector<StreamEvent> events;
    for (const SseFrame& frame : framer.feed(raw)) {
        if (auto event = parse_event(frame); event.has_value()) {
            events.push_back(*event);
        }
    }
    REQUIRE(events.size() == 7);
    REQUIRE(std::holds_alternative<BuiltinToolStart>(events[0]));
    CHECK(std::get<BuiltinToolStart>(events[0]).id == "ig_a");
    REQUIRE(std::holds_alternative<ImageOutput>(events[1]));
    CHECK(std::get<ImageOutput>(events[1]).id == "ig_a");
    CHECK(std::get<ImageOutput>(events[1]).base64 == "AAAA_aaaa");
    REQUIRE(std::holds_alternative<TextDelta>(events[2]));
    REQUIRE(std::holds_alternative<ContentBlockDone>(events[3]));
    REQUIRE(std::holds_alternative<BuiltinToolStart>(events[4]));
    CHECK(std::get<BuiltinToolStart>(events[4]).id == "ig_b");
    REQUIRE(std::holds_alternative<ImageOutput>(events[5]));
    CHECK(std::get<ImageOutput>(events[5]).id == "ig_b");
    CHECK(std::get<ImageOutput>(events[5]).base64 == "BBBB_bbbb");
    REQUIRE(std::holds_alternative<MessageDone>(events[6]));
}

TEST_CASE("response.failed / error 事件:type 与 code 随 message 带上,指得上路") {
    auto failed = parse_event(Frame(
        R"({"type":"response.failed","response":{"id":"resp_f","status":"failed","error":{"message":"Upstream request failed","type":"upstream_error","code":"route_miss"}}})"));
    REQUIRE(failed.has_value());
    REQUIRE(std::holds_alternative<StreamError>(*failed));
    const std::string& message = std::get<StreamError>(*failed).message;
    CHECK(message.find("Upstream request failed") != std::string::npos);
    CHECK(message.find("type=upstream_error") != std::string::npos);
    CHECK(message.find("code=route_miss") != std::string::npos);

    auto errored = parse_event(Frame(
        R"({"type":"error","error":{"message":"参数不合法","code":"invalid_request"}})"));
    REQUIRE(errored.has_value());
    REQUIRE(std::holds_alternative<StreamError>(*errored));
    CHECK(std::get<StreamError>(*errored).message.find("code=invalid_request") != std::string::npos);
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

// M12(原生 web_search):模型自己联网搜索时,output 里会多出一个
// type:"web_search_call" 的条目(带 status/id/action,可选 results/
// sources)。真正的搜索结果由模型消化后写进最终 message 的 output_text
// 里(带 url_citation 标注)。客户端不再把轨迹吞掉，而是翻成只展示、不执行
// 的 BuiltinToolStart/Done；正文仍照旧从 output_text 拼。
TEST_CASE("response.output_item.added 类型是 web_search_call 时发展示起点") {
    auto event = parse_event(Frame(
        R"({"item":{"id":"ws_1","status":"in_progress","type":"web_search_call","action":{"type":"search","query":"今天天气"}},"output_index":0,"sequence_number":2,"type":"response.output_item.added"})"));
    REQUIRE(event.has_value());
    CHECK(std::holds_alternative<BuiltinToolStart>(*event));
}

TEST_CASE("response.output_item.done 类型是 web_search_call 时发展示终点") {
    auto event = parse_event(Frame(
        R"({"item":{"id":"ws_1","status":"completed","type":"web_search_call","action":{"type":"search","query":"今天天气"},"results":[{"url":"https://example.com","title":"今天天气预报"}]},"output_index":0,"sequence_number":5,"type":"response.output_item.done"})"));
    REQUIRE(event.has_value());
    CHECK(std::holds_alternative<BuiltinToolDone>(*event));
}

TEST_CASE("完整的原生 web_search 流式响应:web_search_call 条目穿插在文本前后,不崩,最终文本正常拼出") {
    SseFramer framer;
    const std::string raw =
        "data: {\"type\":\"response.created\",\"response\":{\"status\":\"queued\"},\"sequence_number\":0}\n\n"
        "data: {\"type\":\"response.in_progress\",\"response\":{\"status\":\"in_progress\"},\"sequence_number\":1}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"ws_1\",\"status\":\"in_progress\",\"type\":\"web_search_call\",\"action\":{\"type\":\"search\",\"query\":\"今天天气\"}},\"output_index\":0,\"sequence_number\":2}\n\n"
        "data: {\"type\":\"response.web_search_call.in_progress\",\"output_index\":0,\"item_id\":\"ws_1\",\"sequence_number\":3}\n\n"
        "data: {\"type\":\"response.web_search_call.searching\",\"output_index\":0,\"item_id\":\"ws_1\",\"sequence_number\":4}\n\n"
        "data: {\"type\":\"response.web_search_call.completed\",\"output_index\":0,\"item_id\":\"ws_1\",\"sequence_number\":5}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"ws_1\",\"status\":\"completed\",\"type\":\"web_search_call\",\"action\":{\"type\":\"search\",\"query\":\"今天天气\"}},\"output_index\":0,\"sequence_number\":6}\n\n"
        "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_1\",\"content\":[],\"role\":\"assistant\",\"status\":\"in_progress\",\"type\":\"message\"},\"output_index\":1,\"sequence_number\":7}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"content_index\":0,\"delta\":\"今天\",\"item_id\":\"msg_1\",\"output_index\":1,\"sequence_number\":8}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"content_index\":0,\"delta\":\"晴,25度\",\"item_id\":\"msg_1\",\"output_index\":1,\"sequence_number\":9}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"msg_1\",\"content\":[{\"annotations\":[{\"type\":\"url_citation\",\"url\":\"https://example.com\",\"title\":\"今天天气预报\"}],\"text\":\"今天晴,25度\",\"type\":\"output_text\"}],\"role\":\"assistant\",\"status\":\"completed\",\"type\":\"message\"},\"output_index\":1,\"sequence_number\":10}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_ws\",\"status\":\"completed\",\"output\":[{\"type\":\"web_search_call\",\"id\":\"ws_1\",\"status\":\"completed\"},{\"type\":\"message\",\"id\":\"msg_1\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":\"今天晴,25度\"}]}],\"usage\":{\"input_tokens\":30,\"output_tokens\":8,\"total_tokens\":38}},\"sequence_number\":11}\n\n";

    std::vector<StreamEvent> events;
    for (const SseFrame& frame : framer.feed(raw)) {
        if (auto event = parse_event(frame); event.has_value()) {
            events.push_back(*event);
        }
    }

    // added/done 各产一枚展示事件；三个过程子事件仍跳过。
    REQUIRE(events.size() == 6);
    REQUIRE(std::holds_alternative<BuiltinToolStart>(events[0]));
    REQUIRE(std::holds_alternative<BuiltinToolDone>(events[1]));
    REQUIRE(std::holds_alternative<TextDelta>(events[2]));
    CHECK(std::get<TextDelta>(events[2]).text == "今天");
    REQUIRE(std::holds_alternative<TextDelta>(events[3]));
    CHECK(std::get<TextDelta>(events[3]).text == "晴,25度");
    REQUIRE(std::holds_alternative<ContentBlockDone>(events[4]));
    CHECK(std::get<ContentBlockDone>(events[4]).index == 1);
    REQUIRE(std::holds_alternative<MessageDone>(events[5]));
    const auto& done = std::get<MessageDone>(events[5]);
    CHECK(done.stop_reason == "end_turn");  // output 里 web_search_call 不是 function_call,不当 tool_use
    CHECK(done.usage.input_tokens == 30);
    CHECK(done.usage.output_tokens == 8);
}

TEST_CASE("SummarizeErrorBodyForUser:抽 message/type/code、打码密钥、截短长文") {
    // 标准 JSON 错误体:三字段拼一行。
    CHECK(SummarizeErrorBodyForUser(R"({"error":{"message":"Upstream request failed","type":"upstream_error"}})") ==
          "Upstream request failed (type=upstream_error)");
    // 非 JSON 原文走同一道打码截短。
    const std::string masked = SummarizeErrorBodyForUser("key sk-abcdef1234567890abcdef rejected");
    CHECK(masked.find("sk-abcdef1234567890") == std::string::npos);
    CHECK(masked.find("sk-abcdef...") != std::string::npos);
    // 超长文本截短并标省略。
    const std::string long_text(600, 'x');
    const std::string truncated = SummarizeErrorBodyForUser(long_text);
    CHECK(truncated.size() < long_text.size());
    CHECK(truncated.find("截短") != std::string::npos);
}

TEST_CASE("SummarizeErrorBodyForUser:provider 错报 500 时仍分清上下文与图片能力") {
    const std::string context = SummarizeErrorBodyForUser(
        R"({"error":{"message":"maximum context length is 32768: input 24577 + output 8192","type":"internal_error"}})");
    CHECK(context.find("上下文超出模型窗口") != std::string::npos);
    CHECK(context.find("internal_error") != std::string::npos);

    const std::string image = SummarizeErrorBodyForUser(
        R"({"error":{"message":"MiniCPM5-1B is not a multimodal model","type":"internal_error"}})");
    CHECK(image.find("当前模型不支持图片输入") != std::string::npos);
    CHECK(image.find("MiniCPM5-1B") != std::string::npos);
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

TEST_CASE("Responses 内置 web_search_call 翻成只展示、不本地执行的起止事件") {
    const auto start = parse_event(Frame(
        R"({"type":"response.output_item.added","output_index":0,"item":{"type":"web_search_call","id":"ws_1","status":"in_progress","action":{"type":"search","query":"台风"}}})"));
    REQUIRE(start.has_value());
    REQUIRE(std::holds_alternative<BuiltinToolStart>(*start));
    CHECK(std::get<BuiltinToolStart>(*start).name == "web_search");
    CHECK(std::get<BuiltinToolStart>(*start).input["query"] == "台风");

    const auto done = parse_event(Frame(
        R"({"type":"response.output_item.done","output_index":0,"item":{"type":"web_search_call","id":"ws_1","status":"completed","action":{"type":"search","query":"台风"}}})"));
    REQUIRE(done.has_value());
    REQUIRE(std::holds_alternative<BuiltinToolDone>(*done));
    CHECK(std::get<BuiltinToolDone>(*done).summary.find("台风") != std::string::npos);
    CHECK(std::get<BuiltinToolDone>(*done).input["query"] == "台风");
}

TEST_CASE("Responses 内置搜索起点缺 action 时，终点参数仍可回填标题") {
    const auto start = parse_event(Frame(
        R"({"type":"response.output_item.added","output_index":0,"item":{"type":"web_search_call","id":"ws_sparse","status":"in_progress"}})"));
    REQUIRE(start.has_value());
    REQUIRE(std::holds_alternative<BuiltinToolStart>(*start));
    CHECK(std::get<BuiltinToolStart>(*start).input.empty());

    const auto done = parse_event(Frame(
        R"({"type":"response.output_item.done","output_index":0,"item":{"type":"web_search_call","id":"ws_sparse","status":"completed","action":{"type":"search","query":"OPD 强化学习"}}})"));
    REQUIRE(done.has_value());
    REQUIRE(std::holds_alternative<BuiltinToolDone>(*done));
    CHECK(std::get<BuiltinToolDone>(*done).input["query"] == "OPD 强化学习");
}

// vLLM 本地模型勘察单 P2:非流式响应体(POST /responses 不带 stream,一次
// JSON 对象)的展开路径。思考原文在 output[].content[].reasoning_text
//(vLLM 扩展;OpenAI 官方是 summary[].summary_text,这台端 summary 恒空),
// 与流式路 reasoning_text.delta 同源——两路翻出的中立事件同一形状。
TEST_CASE("ExpandNonStreamResponse: reasoning 项 + message 项展开成中立事件") {
    const auto events = responses::ExpandNonStreamResponse(
        R"({"id":"resp_aeb964886f47be44","object":"response","status":"completed","model":"qwen3.8-27b",)"
        R"("output":[{"id":"rs_aeb964886f47be44","type":"reasoning","summary":[],)"
        R"("content":[{"text":"We need answer user: 2+2=4.","type":"reasoning_text"}],"encrypted_content":null},)"
        R"({"id":"msg_5c31e0a2f8b7d902","type":"message","role":"assistant","status":"completed",)"
        R"("content":[{"type":"output_text","text":"\n\n2"}]}],)"
        R"("usage":{"input_tokens":100,"output_tokens":38,"output_tokens_details":{"reasoning_tokens":0},)"
        R"("input_tokens_details":{"cached_tokens":40}}})");
    REQUIRE(events.size() == 4);
    REQUIRE(std::holds_alternative<ThinkingDelta>(events[0]));
    CHECK(std::get<ThinkingDelta>(events[0]).text == "We need answer user: 2+2=4.");
    REQUIRE(std::holds_alternative<TextDelta>(events[1]));
    CHECK(std::get<TextDelta>(events[1]).text == "\n\n2");
    REQUIRE(std::holds_alternative<ContentBlockDone>(events[2]));
    CHECK(std::get<ContentBlockDone>(events[2]).index == 1);
    REQUIRE(std::holds_alternative<MessageDone>(events[3]));
    const auto& done = std::get<MessageDone>(events[3]);
    CHECK(done.stop_reason == "end_turn");
    CHECK(done.usage_reported);
    // usage 摊法与 response.completed 同一口径:input=100-40、cache_read=40。
    CHECK(done.usage.input_tokens == 60);
    CHECK(done.usage.cache_read_tokens == 40);
    CHECK(done.usage.output_tokens == 38);
    CHECK(TotalInputTokens(done.usage) == 100);
}

TEST_CASE("ExpandNonStreamResponse: function_call 项带双 id,认 call_id,整段 arguments 一枚入参增量") {
    const auto events = responses::ExpandNonStreamResponse(
        R"({"id":"resp_fc","status":"completed","output":[)"
        R"({"id":"fc_5d81c0aa92f3e70b","type":"function_call","call_id":"chatcmpl-tool-a0d9a4d1bebbd50a",)"
        R"("name":"get_weather","arguments":"{\"city\": \"北京\"}","status":"completed"}],)"
        R"("usage":{"input_tokens":59,"output_tokens":38}})");
    REQUIRE(events.size() == 4);
    REQUIRE(std::holds_alternative<ToolUseStart>(events[0]));
    const auto& start = std::get<ToolUseStart>(events[0]);
    CHECK(start.id == "chatcmpl-tool-a0d9a4d1bebbd50a");  // 双 id 取 call_id
    CHECK(start.name == "get_weather");
    CHECK(start.index == 0);
    REQUIRE(std::holds_alternative<ToolUseInputDelta>(events[1]));
    CHECK(std::get<ToolUseInputDelta>(events[1]).partial_json == R"({"city": "北京"})");
    REQUIRE(std::holds_alternative<ContentBlockDone>(events[2]));
    REQUIRE(std::holds_alternative<MessageDone>(events[3]));
    CHECK(std::get<MessageDone>(events[3]).stop_reason == "tool_use");
}

TEST_CASE("ExpandNonStreamResponse: OpenAI 官方 summary 系也能展开(vLLM 端 summary 恒空,两者不同源)") {
    const auto events = responses::ExpandNonStreamResponse(
        R"({"id":"resp_sum","status":"completed","output":[)"
        R"({"id":"rs_1","type":"reasoning","summary":[{"type":"summary_text","text":"摘要一片"}]},)"
        R"({"id":"msg_1","type":"message","role":"assistant","content":[{"type":"output_text","text":"答"}]})"
        R"(]})");
    REQUIRE(events.size() == 4);
    REQUIRE(std::holds_alternative<ThinkingDelta>(events[0]));
    CHECK(std::get<ThinkingDelta>(events[0]).text == "摘要一片");
    REQUIRE(std::holds_alternative<TextDelta>(events[1]));
    REQUIRE(std::holds_alternative<ContentBlockDone>(events[2]));
    REQUIRE(std::holds_alternative<MessageDone>(events[3]));
    CHECK_FALSE(std::get<MessageDone>(events[3]).usage_reported);  // 没 usage 对象就不冒充
}

TEST_CASE("ExpandNonStreamResponse: 坏 JSON / 缺 output / 空数组,不抛不崩") {
    CHECK(responses::ExpandNonStreamResponse("not json {").empty());
    CHECK(responses::ExpandNonStreamResponse(R"({"status":"completed"})").empty());  // 没有 output 数组
    CHECK(responses::ExpandNonStreamResponse(R"({"output":[]})").size() == 1);  // 只有 MessageDone
    CHECK_NOTHROW(responses::ExpandNonStreamResponse(R"({"output":[{"type":"reasoning","content":42}]})"));
}

TEST_CASE("ExpandNonStreamResponse: incomplete 状态映射 max_tokens,与流式 completed 同口径") {
    const auto events = responses::ExpandNonStreamResponse(
        R"({"id":"resp_cut","status":"incomplete","incomplete_details":{"reason":"max_output_tokens"},)"
        R"("output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"半截"}]}]})");
    REQUIRE(events.size() == 3);
    REQUIRE(std::holds_alternative<MessageDone>(events[2]));
    CHECK(std::get<MessageDone>(events[2]).stop_reason == "max_tokens");
}
