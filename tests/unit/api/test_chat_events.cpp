#include <doctest/doctest.h>

#include <variant>

#include "api/chat/events.hpp"
#include "api_fixture.hpp"

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
    CHECK(event.usage_reported);
    CHECK_FALSE(event.cache_reported);
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
    CHECK(event.cache_reported);
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

// ---------------------------------------------------------------------------
// delta.reasoning(vLLM 0.27.1 + qwen3.8-27b 真机实录缩成的 fixture,规格
// "子代理与 MainAgent 同级"根因二):vLLM/Qwen 系用 delta.reasoning 送思考,
// 不是 LubanCode 原先只认的 reasoning_content。六组流:真机实录、
// reasoning_content 对照、两字段同现去重、reasoning-only + length、
// usage 缺席、provider 声明字段。
// 实录字节已迁 tests/fixtures/api/openai_chat/vllm_qwen_reasoning_delta
// (P0 行为不改):这里经 loader 取帧,断言原样。
// ---------------------------------------------------------------------------

TEST_CASE("Chat events: vLLM 真机实录——delta.reasoning 走 ThinkingDelta,usage-only chunk 的空 choices 不丢 usage") {
    const auto loaded = lubancode_test::LoadApiFixture("openai_chat", "vllm_qwen_reasoning_delta");
    REQUIRE(loaded.has_value());
    const auto frames = loaded->SseFrames();
    REQUIRE(frames.size() == 7);  // 开场 + reasoning×2 + 正文 + stop + usage-only + [DONE]

    api::chat::EventParser parser;
    // 1) delta.role 开场帧:只有 role,没有内容——MessageStart 有,无增量。
    auto events = parser.Consume(Frame(frames[0].second));
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<api::MessageStart>(events[0]));

    // 2) delta.reasoning × N:思考逐块流出,每块立即成 ThinkingDelta。
    events = parser.Consume(Frame(frames[1].second));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::ThinkingDelta>(events[0]).text == "We");
    events = parser.Consume(Frame(frames[2].second));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::ThinkingDelta>(events[0]).text == " need");

    // 3) 正文 delta.content。
    events = parser.Consume(Frame(frames[3].second));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::TextDelta>(events[0]).text == "OK");

    // 4) finish_reason=stop 收口帧。
    parser.Consume(Frame(frames[4].second));

    // 5) usage-only chunk:choices 是空数组——usage 必须照收,不能因没有
    //    choices 便丢掉(根因三的现场:include_usage 开了才有这只帧)。
    parser.Consume(Frame(frames[5].second));

    // 6) [DONE]。
    const auto done = parser.Consume(Frame(frames[6].second));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.stop_reason == "end_turn");
    CHECK(event.usage.input_tokens == 15);
    CHECK(event.usage.output_tokens == 2);
    CHECK(event.usage.cache_read_tokens == 0);
    CHECK(api::TotalInputTokens(event.usage) == 15);
}

TEST_CASE("Chat events: reasoning_content 对照——同一链路,不因字段名分家") {
    api::chat::EventParser parser;
    auto events = parser.Consume(Frame(
        R"({"id":"chatcmpl_ds","model":"deepseek","choices":[{"index":0,"delta":{"role":"assistant","reasoning_content":"先想"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "先想");
    events = parser.Consume(Frame(
        R"({"choices":[{"index":0,"delta":{"reasoning":"vLLM 风格"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::ThinkingDelta>(events[0]).text == "vLLM 风格");
    events = parser.Consume(Frame(
        R"({"choices":[{"index":0,"delta":{"content":"答"},"finish_reason":"stop"}],"usage":{"prompt_tokens":5,"completion_tokens":1}})"));
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<api::TextDelta>(events[0]));
}

TEST_CASE("Chat events: 同一 chunk 两字段都有——去重,只吐一份") {
    // 镜像服务端会把 reasoning 同时写成两个字段:相等按一份算。
    api::chat::EventParser parser;
    auto events = parser.Consume(Frame(
        R"({"id":"x","choices":[{"delta":{"reasoning":"同一段","reasoning_content":"同一段"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "同一段");

    // 不相等:按固定优先级取 reasoning_content,另一份弃掉(诊断行点名),
    // 绝不拼成两段。
    api::chat::EventParser differ;
    events = differ.Consume(Frame(
        R"({"id":"y","choices":[{"delta":{"reasoning":"alias","reasoning_content":"canonical"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "canonical");

    // provider 声明了字段名:只认声明那个,另一个字段整个不看。
    api::chat::EventParser declared{"reasoning"};
    events = declared.Consume(Frame(
        R"({"id":"z","choices":[{"delta":{"reasoning":"声明优先","reasoning_content":"不该取这份"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "声明优先");
}

TEST_CASE("Chat events: reasoning-only + finish_reason=length——stop_reason=max_tokens,usage 照收") {
    api::chat::EventParser parser;
    // 思考吃满输出预算的现场(根因一/根因四):只有 reasoning,一个正文
    // 字都没有,finish_reason=length。解析层如实交账:ThinkingDelta 有、
    // TextDelta 无、stop_reason=max_tokens、usage 带拆账。
    auto events = parser.Consume(Frame(
        R"({"id":"chatcmpl_len","model":"qwen3.8-27b","choices":[{"index":0,"delta":{"role":"assistant","reasoning":"先想"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<api::MessageStart>(events[0]));
    CHECK(std::holds_alternative<api::ThinkingDelta>(events[1]));
    for (const char* piece : {"再想", "还在想"}) {
        events = parser.Consume(Frame(std::string(R"({"choices":[{"index":0,"delta":{"reasoning":")") + piece +
                                             R"("},"finish_reason":null}]})"));
        REQUIRE(events.size() == 1);
        CHECK(std::get<api::ThinkingDelta>(events[0]).text == piece);
    }
    parser.Consume(Frame(
        R"({"choices":[{"index":0,"delta":{},"finish_reason":"length"}],"usage":{"prompt_tokens":1200,"completion_tokens":4096,"completion_tokens_details":{"reasoning_tokens":4096}}})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.stop_reason == "max_tokens");
    CHECK(event.usage.output_tokens == 4096);
    CHECK(event.usage.output_reasoning_tokens == 4096);
}

TEST_CASE("Chat events: usage 缺席——不拿零冒充,Reported 语义留给上层") {
    api::chat::EventParser parser;
    // stream_usage 没开的兼容端:全程没有 usage 帧。解析层交回的 usage
    // 五项全零——"未报告"这层语义由 UsageReport::reported() 判,这里钉住
    // 零值原样透传、不崩、不编造。
    parser.Consume(Frame(
        R"({"id":"chatcmpl_nousage","choices":[{"index":0,"delta":{"reasoning":"想"},"finish_reason":null}]})"));
    parser.Consume(Frame(
        R"({"choices":[{"index":0,"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.stop_reason == "end_turn");
    CHECK(event.usage.input_tokens == 0);
    CHECK(event.usage.output_tokens == 0);
    CHECK(event.usage.cache_read_tokens == 0);
    CHECK(event.usage.output_reasoning_tokens == 0);
}

TEST_CASE("Chat events: 结构化 reasoning_details——不映射也不静默吞,计数留账") {
    api::chat::EventParser parser;
    // 原始兼容 fixture(形状照 OpenAI 结构化思考抄,内容是编的):当前
    // 版本不映射成 ThinkingDelta(映射另定),但计了数、Finish 时有诊断。
    auto events = parser.Consume(Frame(
        R"({"id":"rd","choices":[{"index":0,"delta":{"reasoning_details":[{"type":"reasoning.text","text":"一块结构化思考"}]},"finish_reason":null}]})"));
    // 不吐 ThinkingDelta(未映射),也不吐 TextDelta(绝不混进正文)。
    for (const auto& event : events) {
        CHECK_FALSE(std::holds_alternative<api::ThinkingDelta>(event));
        CHECK_FALSE(std::holds_alternative<api::TextDelta>(event));
    }
    parser.Consume(Frame(
        R"({"choices":[{"index":0,"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    const auto done = parser.Consume(Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    CHECK(parser.reasoning_details_blocks() == 1);
    CHECK(std::get<api::MessageDone>(done[0]).stop_reason == "end_turn");
}
