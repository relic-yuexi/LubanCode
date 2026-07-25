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
    CHECK(event.usage.input_tokens == 12);
    CHECK(event.usage.cache_read_tokens == 4);
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
