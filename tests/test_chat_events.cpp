#include <doctest/doctest.h>

#include <variant>

#include "api/chat/events.hpp"

using namespace lubancode;

namespace {
api::SseFrame Frame(std::string data) { return api::SseFrame{"", std::move(data)}; }
}

TEST_CASE("Chat events: 文本、usage 与结束原因") {
    api::chat::EventParser parser;
    auto first = parser.Consume(Frame(
        R"({"id":"chatcmpl_1","model":"qwen","choices":[{"delta":{"content":"你"},"finish_reason":null}]})"));
    REQUIRE(first.size() == 2);
    CHECK(std::holds_alternative<api::MessageStart>(first[0]));
    CHECK(std::get<api::TextDelta>(first[1]).text == "你");

    parser.Consume(Frame(
        R"({"choices":[{"delta":{"content":"好"},"finish_reason":"stop"}],"usage":{"prompt_tokens":12,"completion_tokens":3,"prompt_tokens_details":{"cached_tokens":4}}})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.stop_reason == "end_turn");
    // OpenAI/Qwen 风格:prompt_tokens 已含 cached_tokens,摊开后
    // input=12-4=8、cache_read=4,消费端不再加两遍。
    CHECK(event.usage.input_tokens == 8);
    CHECK(event.usage.cache_read_tokens == 4);
    CHECK(api::TotalInputTokens(event.usage) == 12);
}

TEST_CASE("Chat events: 交错 tool_calls 先攒齐再按 index 吐出") {
    api::chat::EventParser parser;
    parser.Consume(Frame(
        R"({"id":"x","choices":[{"delta":{"tool_calls":[{"index":1,"id":"b","function":{"name":"write_file","arguments":"{\"p\":"}},{"index":0,"id":"a","function":{"name":"read_file","arguments":"{\"p\":\"a"}}]},"finish_reason":null}]})"));
    parser.Consume(Frame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"}"}},{"index":1,"function":{"arguments":"\"b\"}"}}]},"finish_reason":"tool_calls"}]})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    REQUIRE(done.size() == 8);
    CHECK(std::get<api::ToolUseStart>(done[1]).id == "a");
    CHECK(std::get<api::ToolUseStart>(done[4]).id == "b");
    CHECK(std::get<api::MessageDone>(done[7]).stop_reason == "tool_use");
}

TEST_CASE("Chat events: API error 翻成 StreamError") {
    api::chat::EventParser parser;
    const auto events = parser.Consume(Frame(R"({"error":{"message":"bad key"}})"));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::StreamError>(events[0]).message == "bad key");
}

TEST_CASE("Chat events: reasoning_content 流式映射成 ThinkingDelta") {
    api::chat::EventParser parser;
    auto events = parser.Consume(Frame(
        R"({"id":"chatcmpl_2","model":"deepseek","choices":[{"delta":{"reasoning_content":"先想"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<api::MessageStart>(events[0]));
    REQUIRE(std::holds_alternative<api::ThinkingDelta>(events[1]));
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "先想");

    events = parser.Consume(Frame(
        R"({"choices":[{"delta":{"reasoning_content":"想完"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::ThinkingDelta>(events[0]).text == "想完");

    // reasoning_content 过渡到 content:text 走 TextDelta
    events = parser.Consume(Frame(
        R"({"choices":[{"delta":{"content":"答案是"},"finish_reason":"stop"}]})"));
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<api::TextDelta>(events[0]));
}

// ---------------------------------------------------------------------------
// usage 统一口径(前缀缓存守恒单第一期):DeepSeek 顶层 hit/miss、光杆
// prompt_tokens、hit+miss 对不上、独立 usage chunk、重复 usage 不重复累计。
// ---------------------------------------------------------------------------

TEST_CASE("Chat events: DeepSeek 顶层 prompt_cache_hit/miss_tokens 摊成统一口径") {
    api::chat::EventParser parser;
    // 官方文档形状:49k 命中 + 1k 未命中,总 50k。choices 为空的独立 usage
    // chunk(stream_options.include_usage 开了才会在 [DONE] 前来这一只)。
    parser.Consume(Frame(
        R"({"id":"chatcmpl_ds","model":"deepseek-v4-pro","choices":[{"delta":{"content":"好"},"finish_reason":"stop"}]})"));
    parser.Consume(Frame(
        R"({"choices":[],"usage":{"prompt_tokens":50000,"completion_tokens":80,"prompt_cache_hit_tokens":49000,"prompt_cache_miss_tokens":1000}})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.usage.input_tokens == 1000);
    CHECK(event.usage.cache_read_tokens == 49000);
    CHECK(event.usage.cache_creation_tokens == 0);
    CHECK(api::TotalInputTokens(event.usage) == 50000);
    // 命中率按 token 算:49000/50000 = 98%。
    CHECK(event.usage.cache_read_tokens * 100 / api::TotalInputTokens(event.usage) == 98);
}

TEST_CASE("Chat events: 只有 prompt_tokens、没有 cache 字段——input=total,cache_read=0") {
    api::chat::EventParser parser;
    parser.Consume(Frame(
        R"({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":37,"completion_tokens":5}})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.usage.input_tokens == 37);
    CHECK(event.usage.cache_read_tokens == 0);
    CHECK(api::TotalInputTokens(event.usage) == 37);
}

TEST_CASE("Chat events: hit+miss 与 prompt_tokens 对不上——保留 hit/miss,不崩") {
    api::chat::EventParser parser;
    parser.Consume(Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    parser.Consume(Frame(
        R"({"choices":[],"usage":{"prompt_tokens":60000,"completion_tokens":5,"prompt_cache_hit_tokens":49000,"prompt_cache_miss_tokens":1000}})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    // 服务端账目不合时以缓存分项为准,保留原数,别拿 prompt_tokens 覆盖。
    CHECK(event.usage.input_tokens == 1000);
    CHECK(event.usage.cache_read_tokens == 49000);
    CHECK(api::TotalInputTokens(event.usage) == 50000);
}

TEST_CASE("Chat events: usage 在 finish chunk 与独立 chunk 各来一次,只认一份数") {
    api::chat::EventParser parser;
    parser.Consume(Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}],"usage":{"prompt_tokens":100,"completion_tokens":10,"prompt_tokens_details":{"cached_tokens":60}}})"));
    // stream_options.include_usage=true 时 [DONE] 前还会再来一只空 choices 的
    // usage chunk——覆盖式记账,不得重复累计。
    parser.Consume(Frame(
        R"({"choices":[],"usage":{"prompt_tokens":100,"completion_tokens":10,"prompt_tokens_details":{"cached_tokens":60}}})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.usage.input_tokens == 40);
    CHECK(event.usage.cache_read_tokens == 60);
    CHECK(event.usage.output_tokens == 10);
    CHECK(api::TotalInputTokens(event.usage) == 100);
}

TEST_CASE("Chat events: completion_tokens_details.reasoning_tokens 摊进 usage 拆账") {
    api::chat::EventParser parser;
    parser.Consume(Frame(
        R"({"id":"r","choices":[{"delta":{"content":"答"},"finish_reason":"length"}],"usage":{"prompt_tokens":20,"completion_tokens":64,"completion_tokens_details":{"reasoning_tokens":60}}})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    // reasoning 已含在 completion_tokens 总数里,不是另加的一笔。
    CHECK(event.usage.output_tokens == 64);
    CHECK(event.usage.output_reasoning_tokens == 60);
    CHECK(event.stop_reason == "max_tokens");  // finish_reason=length → 预算耗尽
}

TEST_CASE("Chat events: 顶层 reasoning_tokens 也认,没拆账就是 0") {
    api::chat::EventParser parser;
    parser.Consume(Frame(
        R"({"id":"q","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}],"usage":{"prompt_tokens":9,"completion_tokens":2,"reasoning_tokens":7}})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.usage.output_reasoning_tokens == 7);

    api::chat::EventParser bare;
    bare.Consume(Frame(
        R"({"id":"q2","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}],"usage":{"prompt_tokens":9,"completion_tokens":2}})"));
    const auto done2 = bare.Consume(Frame("[DONE]"));
    CHECK(std::get<api::MessageDone>(done2[0]).usage.output_reasoning_tokens == 0);
}
