// google-generate-content wire 的 SSE 事件解析:每帧 data 是一只完整的
// GenerateContentResponse,没有 event: 名字段——对着 EventParser 这只纯
// 函数钉文本/思考/工具调用/用量/finishReason 与断流容错。

#include <doctest/doctest.h>

#include <variant>

#include "api/gemini/events.hpp"

using namespace lubancode;

namespace {
api::SseFrame Frame(std::string data) { return api::SseFrame{"", std::move(data)}; }
}  // namespace

TEST_CASE("Gemini events: 文本流 + 收尾帧翻 MessageStart/TextDelta/MessageDone") {
    api::gemini::EventParser parser;
    auto first = parser.Consume(Frame(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"你"}]}}],"modelVersion":"gemini-2.5-pro"})"));
    REQUIRE(first.size() == 2);
    const auto& start = std::get<api::MessageStart>(first[0]);
    CHECK(start.model == "gemini-2.5-pro");
    CHECK(std::get<api::TextDelta>(first[1]).text == "你");

    const auto done = parser.Consume(Frame(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"好"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":37,"candidatesTokenCount":5,"totalTokenCount":42}})"));
    // 收尾帧:先吐这帧的 TextDelta,再落锤 MessageDone。
    REQUIRE(done.size() == 2);
    CHECK(std::get<api::TextDelta>(done[0]).text == "好");
    const auto& message_done = std::get<api::MessageDone>(done[1]);
    CHECK(message_done.stop_reason == "end_turn");
    CHECK(message_done.usage.input_tokens == 37);
    CHECK(message_done.usage.output_tokens == 5);
    CHECK(message_done.usage.cache_read_tokens == 0);
    // 收尾之后再喂帧(协议上不该有)不再重复落锤。
    CHECK(parser.Consume(Frame(R"({"candidates":[{"finishReason":"STOP"}]})")).empty());
    CHECK(parser.finished());
}

TEST_CASE("Gemini events: thought part 翻 ThinkingDelta") {
    api::gemini::EventParser parser;
    const auto events = parser.Consume(Frame(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"先想","thought":true},{"text":"答"}]}}],"modelVersion":"gemini-3-pro"})"));
    REQUIRE(events.size() == 3);
    CHECK(std::holds_alternative<api::MessageStart>(events[0]));
    REQUIRE(std::holds_alternative<api::ThinkingDelta>(events[1]));
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "先想");
    CHECK(std::get<api::TextDelta>(events[2]).text == "答");
}

TEST_CASE("Gemini events: functionCall 攒齐,finishReason 落锤时按次序吐出") {
    api::gemini::EventParser parser;
    parser.Consume(Frame(
        R"({"candidates":[{"content":{"role":"model","parts":[{"text":"我来"}]}}],"modelVersion":"gemini-2.5-pro"})"));
    const auto done = parser.Consume(Frame(
        R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"read_file","args":{"path":"a.cpp"}}},{"functionCall":{"name":"write_file","args":{"path":"b.cpp"}}}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":4}})"));
    REQUIRE(done.size() == 8);
    // ContentBlockDone{0} 收文本块的尾,然后每只调用 Start/Delta/Done 三连。
    CHECK(std::get<api::ContentBlockDone>(done[0]).index == 0);
    const auto& first_start = std::get<api::ToolUseStart>(done[1]);
    CHECK(first_start.index == 0);
    CHECK(first_start.name == "read_file");
    CHECK(!first_start.id.empty());
    CHECK(std::get<api::ToolUseInputDelta>(done[2]).partial_json == R"({"path":"a.cpp"})");
    CHECK(std::get<api::ToolUseStart>(done[4]).name == "write_file");
    CHECK(std::get<api::ToolUseInputDelta>(done[5]).partial_json == R"({"path":"b.cpp"})");
    const auto& message_done = std::get<api::MessageDone>(done[7]);
    CHECK(message_done.stop_reason == "tool_use");
}

TEST_CASE("Gemini events: usageMetadata 摊成统一口径") {
    api::gemini::EventParser parser;
    // prompt 含缓存命中:input = prompt - cached,cache_read = cached;
    // candidates 已含思考(官方 API 口径),thoughts 只拆账不叠加。
    const auto done = parser.Consume(Frame(
        R"({"candidates":[{"content":{"parts":[{"text":"答"}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":50000,"candidatesTokenCount":80,"thoughtsTokenCount":20,"cachedContentTokenCount":49000,"totalTokenCount":50080}})"));
    REQUIRE(done.size() == 2);
    const auto& message_done = std::get<api::MessageDone>(done[1]);
    CHECK(message_done.usage.input_tokens == 1000);
    CHECK(message_done.usage.cache_read_tokens == 49000);
    CHECK(message_done.usage.cache_creation_tokens == 0);
    CHECK(message_done.usage.output_tokens == 80);
    CHECK(message_done.usage.output_reasoning_tokens == 20);
    CHECK(api::TotalInputTokens(message_done.usage) == 50000);
}

TEST_CASE("Gemini events: MAX_TOKENS 翻 max_tokens;SAFETY 类截断翻 end_turn") {
    api::gemini::EventParser parser;
    const auto done = parser.Consume(Frame(
        R"({"candidates":[{"content":{"parts":[{"text":"写到一半"}]},"finishReason":"MAX_TOKENS"}],"modelVersion":"gemini-2.5-pro"})"));
    REQUIRE(done.size() == 3);
    CHECK(std::get<api::MessageDone>(done[2]).stop_reason == "max_tokens");

    api::gemini::EventParser safety;
    const auto events = safety.Consume(Frame(
        R"({"candidates":[{"content":{"parts":[{"text":"拒绝"}]},"finishReason":"SAFETY"}],"modelVersion":"gemini-2.5-pro"})"));
    CHECK(std::get<api::MessageDone>(events.back()).stop_reason == "end_turn");
}

TEST_CASE("Gemini events: error 帧翻 StreamError") {
    api::gemini::EventParser parser;
    const auto events = parser.Consume(Frame(R"({"error":{"code":429,"message":"resource exhausted","status":"RESOURCE_EXHAUSTED"}})"));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::StreamError>(events[0]).message == "resource exhausted");
}

TEST_CASE("Gemini events: 坏 JSON 帧、空 candidates 帧都跳过不崩") {
    api::gemini::EventParser parser;
    CHECK(parser.Consume(Frame("not json")).empty());
    CHECK(parser.Consume(Frame(R"({"candidates":[]})")).empty());
    CHECK(parser.Consume(Frame(R"([1,2])")).empty());
    CHECK_FALSE(parser.finished());
}

TEST_CASE("Gemini events: 断流(没等到 finishReason)由 Finish 兜底落锤") {
    api::gemini::EventParser parser;
    parser.Consume(Frame(
        R"({"candidates":[{"content":{"parts":[{"text":"半截"}]}}],"modelVersion":"gemini-2.5-pro"})"));
    REQUIRE_FALSE(parser.finished());
    const auto done = parser.Finish();
    REQUIRE(done.size() == 1);
    CHECK(std::get<api::MessageDone>(done[0]).stop_reason == "end_turn");
    // Finish 只落一次锤。
    CHECK(parser.Finish().empty());
}

TEST_CASE("Gemini events: 一只载荷都没有的流 Finish 不硬造 MessageDone") {
    api::gemini::EventParser parser;
    CHECK(parser.Finish().empty());
}

TEST_CASE("Gemini events: 字段类型不对的坏帧当没看见,不崩解析器") {
    api::gemini::EventParser parser;
    // modelVersion 给了个数字(协议里是字符串):.value() 抛 type_error,整帧
    // 作废不崩;后面的好帧照常解析。
    CHECK(parser.Consume(Frame(R"({"candidates":[{"finishReason":"STOP"}],"modelVersion":7})")).empty());
    const auto events = parser.Consume(Frame(
        R"({"candidates":[{"content":{"parts":[{"text":"好"}]},"finishReason":"STOP"}]})"));
    CHECK(std::holds_alternative<api::TextDelta>(events[0]));
    CHECK(std::get<api::MessageDone>(events[1]).stop_reason == "end_turn");
}
