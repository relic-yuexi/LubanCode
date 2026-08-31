// 拿《Anthropic兼容-Messages.md》里"流式响应"一节的真实事件样例当 fixture,
// 验证 parse_event 映射到 StreamEvent 是否正确;顺带验证未知事件类型、
// 坏掉的 JSON 都不会崩。

#include <doctest/doctest.h>

#include <algorithm>
#include <variant>
#include <vector>

#include "api/anthropic/events.hpp"
#include "api/assembler.hpp"
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

TEST_CASE("content_block_delta 的 thinking_delta 映射出 ThinkingDelta") {
    auto event = parse_event(Frame(R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"分析一下"}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ThinkingDelta>(*event));
    CHECK(std::get<ThinkingDelta>(*event).text == "分析一下");
    CHECK(std::get<ThinkingDelta>(*event).signature.empty());
}

TEST_CASE("content_block_delta 的 signature_delta 映射出 ThinkingDelta(只带 signature)") {
    auto event = parse_event(Frame(R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig_abc"}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ThinkingDelta>(*event));
    const auto& delta = std::get<ThinkingDelta>(*event);
    CHECK(delta.text.empty());
    CHECK(delta.signature == "sig_abc");
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

TEST_CASE("message_delta 的 usage 带 cache_read_input_tokens/cache_creation_input_tokens 时映射进 Usage") {
    auto event = parse_event(Frame(
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"input_tokens":1578,"output_tokens":83,"cache_creation_input_tokens":50,"cache_read_input_tokens":128}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageDone>(*event));
    const auto& done = std::get<MessageDone>(*event);
    CHECK(done.usage.input_tokens == 1578);
    CHECK(done.usage.output_tokens == 83);
    CHECK(done.usage.cache_read_tokens == 128);
    CHECK(done.usage.cache_creation_tokens == 50);
}

TEST_CASE("message_delta 的 usage 没有 cache 字段时,cache_read_tokens/cache_creation_tokens 落 0,不崩") {
    auto event = parse_event(Frame(
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"input_tokens":10,"output_tokens":5}})"));

    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<MessageDone>(*event));
    const auto& done = std::get<MessageDone>(*event);
    CHECK(done.usage.cache_read_tokens == 0);
    CHECK(done.usage.cache_creation_tokens == 0);
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

// ---------------------------------------------------------------------------
// M12(anthropic 协议接原生 web_search):server tool 完全在服务端跑完,
// 客户端不需要执行任何东西——响应里会多出 server_tool_use(工具调用本身)
// 和 web_search_tool_result(搜索结果)两种 content_block 类型,都不等于
// "tool_use",HandleContentBlockStart 现有的 `if (type != "tool_use")` 分支
// 天然把它们当成"text/thinking 同类"静默跳过,不会被误当成需要客户端执行
// 的本地工具调用、也不会崩。这里用一段带 server_tool_use/
// web_search_tool_result 块的模拟完整流实锤验证:解析不崩,且不产生
// ToolUseStart 事件(不会被上层当成待执行的本地函数工具),文本正常拼出。
// ---------------------------------------------------------------------------

TEST_CASE("content_block_start 类型是 server_tool_use 时静默跳过,不产生 ToolUseStart") {
    auto event = parse_event(Frame(
        R"({"type":"content_block_start","index":1,"content_block":{"type":"server_tool_use","id":"srvtoolu_01","name":"web_search","input":{}}})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("content_block_start 类型是 web_search_tool_result 时静默跳过") {
    auto event = parse_event(Frame(
        R"({"type":"content_block_start","index":2,"content_block":{"type":"web_search_tool_result","tool_use_id":"srvtoolu_01","content":[{"type":"web_search_result","url":"https://example.com","title":"示例","encrypted_content":"abc","page_age":"2026-01-01"}]}})"));
    CHECK_FALSE(event.has_value());
}

TEST_CASE("完整一轮夹带 server_tool_use/web_search_tool_result 的流式响应:不崩,不产生 ToolUseStart,文本正常拼出") {
    SseFramer framer;
    const std::string raw =
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_xxx\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-sonnet\",\"content\":[],\"usage\":{\"input_tokens\":20,\"output_tokens\":0}}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"我查一下最新消息。\"}}\n\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"server_tool_use\",\"id\":\"srvtoolu_01\",\"name\":\"web_search\",\"input\":{}}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"query\\\":\\\"今天天气\\\"}\"}}\n\n"
        "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"web_search_tool_result\",\"tool_use_id\":\"srvtoolu_01\",\"content\":[{\"type\":\"web_search_result\",\"url\":\"https://example.com\",\"title\":\"示例\",\"encrypted_content\":\"abc\",\"page_age\":\"2026-01-01\"}]}}\n\n"
        "data: {\"type\":\"content_block_stop\",\"index\":2}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":3,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":3,\"delta\":{\"type\":\"text_delta\",\"text\":\"根据搜索结果,今天晴。\"}}\n\n"
        "data: {\"type\":\"content_block_stop\",\"index\":3}\n\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\",\"stop_sequence\":null},\"usage\":{\"input_tokens\":20,\"output_tokens\":30,\"cache_creation_input_tokens\":0,\"cache_read_input_tokens\":0}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n";

    std::vector<StreamEvent> events;
    CHECK_NOTHROW({
        for (const SseFrame& frame : framer.feed(raw)) {
            if (auto event = parse_event(frame); event.has_value()) {
                events.push_back(*event);
            }
        }
    });

    // server_tool_use / web_search_tool_result 两个块的 start/delta/stop
    // 全部被静默吞掉(server_tool_use 的 content_block_stop 例外——那条
    // ContentBlockDone 不区分块类型,始终会发,上层按 index 匹配,匹配不到
    // 对应的 ToolUseStart 时是安全的空操作)。真正需要断言的是:不出现
    // ToolUseStart(不会被误当成待执行的本地工具),TextDelta 正常拼出
    // 两段文本。
    for (const StreamEvent& event : events) {
        CHECK_FALSE(std::holds_alternative<ToolUseStart>(event));
    }

    std::string assembled_text;
    for (const StreamEvent& event : events) {
        if (std::holds_alternative<TextDelta>(event)) {
            assembled_text += std::get<TextDelta>(event).text;
        }
    }
    CHECK(assembled_text == "我查一下最新消息。根据搜索结果,今天晴。");

    // message_delta 仍然正常映射出 MessageDone,流没有因为中间夹了 server
    // tool 块就被打断。
    bool saw_message_done = false;
    for (const StreamEvent& event : events) {
        if (std::holds_alternative<MessageDone>(event)) {
            saw_message_done = true;
            CHECK(std::get<MessageDone>(event).stop_reason == "end_turn");
        }
    }
    CHECK(saw_message_done);
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

TEST_CASE("字段类型不对(type_error 一族)不抛异常、不崩,当坏帧跳过") {
    // 这些帧 JSON 合法、type 字段也认得,但内层字段类型全是错的——以前
    // .value()/.get() 会抛 type_error 穿透进 libcurl 回调栈,直接崩。
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"message_start","message":{"id":123,"model":[]}})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"content_block_start","index":"x","content_block":{"type":"tool_use","id":[],"name":7}})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":42}})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"content_block_stop","index":"oops"})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"message_delta","delta":{"stop_reason":[]},"usage":{"input_tokens":"a"}})")));
    CHECK_NOTHROW(parse_event(Frame(R"({"type":"error","error":{"message":123}})")));

    // 坏帧的结果是"跳过",不是半个歪事件。
    auto bad = parse_event(Frame(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":42}})"));
    CHECK_FALSE(bad.has_value());
}

TEST_CASE("工具续轮兼容:think 开闭标签横跨 delta 时隔离思考,只留正文") {
    anthropic::EventParser parser(/*recover_tagged_thinking=*/true);
    std::vector<StreamEvent> events;
    const std::vector<std::string> frames = {
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"<thi"}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"nk>先查"}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"清楚</thi"}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"nk>\n\n答案"}})",
        R"({"type":"content_block_stop","index":0})",
    };
    for (const auto& raw : frames) {
        for (auto& event : parser.Consume(Frame(raw))) events.push_back(std::move(event));
    }

    std::string thinking;
    std::string text;
    for (const auto& event : events) {
        if (const auto* delta = std::get_if<ThinkingDelta>(&event)) thinking += delta->text;
        if (const auto* delta = std::get_if<TextDelta>(&event)) text += delta->text;
    }
    CHECK(thinking.empty());
    CHECK(text == "\n\n答案");
    CHECK(parser.recovered_tagged_thinking());
}

TEST_CASE("工具续轮兼容:普通正文与正文中间的 think 标签一字不改") {
    anthropic::EventParser parser(/*recover_tagged_thinking=*/true);
    const auto first = parser.Consume(Frame(
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"XML 示例: <think>保留</think>"}})"));
    REQUIRE(first.size() == 1);
    REQUIRE(std::holds_alternative<TextDelta>(first[0]));
    CHECK(std::get<TextDelta>(first[0]).text == "XML 示例: <think>保留</think>");
}

TEST_CASE("工具续轮兼容:正文从 think 标签开头但没有空行后续时原样保留") {
    anthropic::EventParser parser(/*recover_tagged_thinking=*/true);
    std::vector<StreamEvent> events;
    for (auto& event : parser.Consume(Frame(
             R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"<think>XML 字面量</think>直接说明"}})"))) {
        events.push_back(std::move(event));
    }
    for (auto& event : parser.Consume(Frame(R"({"type":"content_block_stop","index":0})"))) {
        events.push_back(std::move(event));
    }
    std::string text;
    for (const auto& event : events) {
        if (const auto* delta = std::get_if<TextDelta>(&event)) text += delta->text;
    }
    CHECK(text == "<think>XML 字面量</think>直接说明");
    CHECK_FALSE(parser.recovered_tagged_thinking());
}

TEST_CASE("工具续轮兼容:think 标签没闭合时报协议错,不漏进正文") {
    anthropic::EventParser parser(/*recover_tagged_thinking=*/true);
    std::vector<StreamEvent> events;
    for (auto& event : parser.Consume(Frame(
             R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"<think>半截思考"}})"))) {
        events.push_back(std::move(event));
    }
    for (auto& event : parser.Consume(Frame(R"({"type":"content_block_stop","index":0})"))) {
        events.push_back(std::move(event));
    }

    CHECK(std::none_of(events.begin(), events.end(), [](const StreamEvent& event) {
        const auto* text = std::get_if<TextDelta>(&event);
        return text != nullptr && text->text.find("<think>") != std::string::npos;
    }));
    CHECK(std::any_of(events.begin(), events.end(),
                      [](const StreamEvent& event) { return std::holds_alternative<StreamError>(event); }));
}

// ---------------------------------------------------------------------------
// 动态工具 P3(Claude NativeReference·§7.2):服务端工具搜索的原生块解析。
// 门(parse_server_tool_search)只对本请求自己声明过 server_tool_search 的
// 流开:开了门,server_tool_use(名字是我们声明的那两枚搜索工具)与
// tool_search_tool_result 解析成中性事件,由 assembler 攒成原生事实块;
// 门没开(默认),两类块照旧行为静默跳过——web_search 等其他 server tool
// 即便开门也照旧跳过,旧路一字不动。流样例照官方文档 Streaming 一节。
// ---------------------------------------------------------------------------

TEST_CASE("P3 开门: tool_search 的 server_tool_use 映射出 ServerToolUseStart,caller 照带") {
    auto event = parse_event(Frame(
        R"({"type":"content_block_start","index":1,"content_block":{"type":"server_tool_use","id":"srvtoolu_xyz789","name":"tool_search_tool_regex"}})"),
        /*parse_server_tool_search=*/true);
    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ServerToolUseStart>(*event));
    const auto& start = std::get<ServerToolUseStart>(*event);
    CHECK(start.index == 1);
    CHECK(start.id == "srvtoolu_xyz789");
    CHECK(start.name == "tool_search_tool_regex");

    auto bm25 = parse_event(Frame(
        R"({"type":"content_block_start","index":1,"content_block":{"type":"server_tool_use","id":"srvtoolu_2","name":"tool_search_tool_bm25"}})"),
        /*parse_server_tool_search=*/true);
    REQUIRE(bm25.has_value());
    CHECK(std::get<ServerToolUseStart>(*bm25).name == "tool_search_tool_bm25");
}

TEST_CASE("P3 开门: tool_search_tool_result 整块到齐,嵌套 content 无损") {
    auto event = parse_event(Frame(
        R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_search_tool_result","tool_use_id":"srvtoolu_xyz789","content":{"type":"tool_search_tool_search_result","tool_references":[{"type":"tool_reference","tool_name":"get_weather"}]}}})"),
        /*parse_server_tool_search=*/true);
    REQUIRE(event.has_value());
    REQUIRE(std::holds_alternative<ServerToolResult>(*event));
    const auto& result = std::get<ServerToolResult>(*event);
    CHECK(result.index == 2);
    CHECK(result.tool_use_id == "srvtoolu_xyz789");
    CHECK(result.content.at("type") == "tool_search_tool_search_result");
    REQUIRE(result.content.at("tool_references").is_array());
    CHECK(result.content.at("tool_references").at(0).at("tool_name") == "get_weather");
}

TEST_CASE("P3 关门(默认): 搜索的 server 块照旧静默跳过,旧行为一字不动") {
    CHECK_FALSE(parse_event(Frame(
        R"({"type":"content_block_start","index":1,"content_block":{"type":"server_tool_use","id":"srvtoolu_1","name":"tool_search_tool_regex"}})")).has_value());
    CHECK_FALSE(parse_event(Frame(
        R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_search_tool_result","tool_use_id":"srvtoolu_1","content":{"type":"tool_search_tool_search_result","tool_references":[]}}})")).has_value());
}

TEST_CASE("P3 开门也只认搜索工具: web_search 的 server_tool_use 照旧跳过") {
    CHECK_FALSE(parse_event(Frame(
        R"({"type":"content_block_start","index":1,"content_block":{"type":"server_tool_use","id":"srvtoolu_9","name":"web_search"}})"),
        /*parse_server_tool_search=*/true).has_value());
}

TEST_CASE("P3 完整一轮官方流样例: 搜索 -> 结果 -> 直调真实工具,assembler 攒出原生块") {
    // 样例照官方 tool search 文档 Streaming 一节:server_tool_use 的入参走
    // input_json_delta 增量;tool_search_tool_result 整块随 content_block_start
    // 到齐;随后模型直调发现的工具(普通 tool_use,本地照走执行正门)。
    anthropic::EventParser parser(/*recover_tagged_thinking=*/false, /*parse_server_tool_search=*/true);
    MessageAssembler assembler;
    const std::vector<std::string> frames = {
        R"({"type":"message_start","message":{"id":"msg_1","model":"claude-opus-5","content":[]}})",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"我搜一下天气工具。"}})",
        R"({"type":"content_block_stop","index":0})",
        R"({"type":"content_block_start","index":1,"content_block":{"type":"server_tool_use","id":"srvtoolu_xyz789","name":"tool_search_tool_regex"}})",
        R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"pattern\":\"weather\",\"limit\":10}"}})",
        R"({"type":"content_block_stop","index":1})",
        R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_search_tool_result","tool_use_id":"srvtoolu_xyz789","content":{"type":"tool_search_tool_search_result","tool_references":[{"type":"tool_reference","tool_name":"get_weather"}]}}})",
        R"({"type":"content_block_stop","index":2})",
        R"({"type":"content_block_start","index":3,"content_block":{"type":"tool_use","id":"toolu_01XYZ789","name":"get_weather","input":{}}})",
        R"({"type":"content_block_delta","index":3,"delta":{"type":"input_json_delta","partial_json":"{\"location\":\"San Francisco\"}"}})",
        R"({"type":"content_block_stop","index":3})",
        R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"input_tokens":20,"output_tokens":30}})",
    };
    for (const auto& raw : frames) {
        for (auto& event : parser.Consume(Frame(raw))) {
            assembler.Feed(event);
        }
    }

    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 4);
    REQUIRE(std::holds_alternative<TextBlock>(message.content[0]));
    CHECK(std::get<TextBlock>(message.content[0]).text == "我搜一下天气工具。");

    // server_tool_use:provider 执行的搜索事实,不是本地待执行的调用。
    REQUIRE(std::holds_alternative<ServerToolUseBlock>(message.content[1]));
    const auto& server_use = std::get<ServerToolUseBlock>(message.content[1]);
    CHECK(server_use.id == "srvtoolu_xyz789");
    CHECK(server_use.name == "tool_search_tool_regex");
    CHECK(server_use.input.at("pattern") == "weather");
    CHECK(server_use.input.at("limit") == 10);

    // 搜索结果:嵌套 tool_reference 无损。
    REQUIRE(std::holds_alternative<ServerToolResultBlock>(message.content[2]));
    const auto& server_result = std::get<ServerToolResultBlock>(message.content[2]);
    CHECK(server_result.tool_use_id == "srvtoolu_xyz789");
    CHECK(server_result.content.at("tool_references").at(0).at("tool_name") == "get_weather");

    // 模型直调真实工具:普通 ToolUseBlock,本地照走 RunOneTool 正门。
    REQUIRE(std::holds_alternative<ToolUseBlock>(message.content[3]));
    const auto& real_call = std::get<ToolUseBlock>(message.content[3]);
    CHECK(real_call.id == "toolu_01XYZ789");
    CHECK(real_call.name == "get_weather");
    CHECK(real_call.input.at("location") == "San Francisco");
    CHECK(assembler.stop_reason() == "tool_use");
}

TEST_CASE("P3 搜索失败流: tool_search_tool_result_error 的 error_code/message 无损保存") {
    anthropic::EventParser parser(/*recover_tagged_thinking=*/false, /*parse_server_tool_search=*/true);
    MessageAssembler assembler;
    const std::vector<std::string> frames = {
        R"({"type":"content_block_start","index":0,"content_block":{"type":"server_tool_use","id":"srvtoolu_err","name":"tool_search_tool_regex"}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"pattern\":\"weather(\"}"}})",
        R"({"type":"content_block_stop","index":0})",
        R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_search_tool_result","tool_use_id":"srvtoolu_err","content":{"type":"tool_search_tool_result_error","error_code":"invalid_tool_input","error_message":"Invalid regular expression pattern: missing ) at position 1"}}})",
        R"({"type":"content_block_stop","index":1})",
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"input_tokens":9,"output_tokens":9}})",
    };
    for (const auto& raw : frames) {
        for (auto& event : parser.Consume(Frame(raw))) {
            assembler.Feed(event);
        }
    }
    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 2);
    REQUIRE(std::holds_alternative<ServerToolResultBlock>(message.content[1]));
    const auto& result = std::get<ServerToolResultBlock>(message.content[1]);
    CHECK(result.content.at("type") == "tool_search_tool_result_error");
    CHECK(result.content.at("error_code") == "invalid_tool_input");
    CHECK(result.content.at("error_message").get<std::string>().find("missing )") != std::string::npos);
}
