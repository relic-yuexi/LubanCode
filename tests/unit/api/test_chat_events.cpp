#include <doctest/doctest.h>
#include "usage_body_fixture.hpp"

#include <variant>
#include <utility>
#include <vector>

#include "api/chat/events.hpp"
#include "api/assembler.hpp"
#include "api/usage_json.hpp"
#include "api_fixture.hpp"

using namespace lubancode;

namespace {
api::SseFrame Frame(std::string data) { return api::SseFrame{"", std::move(data)}; }
}

TEST_CASE("Chat events: 文本、usage 与结束原因") {
    api::chat::EventParser parser;
    auto first = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"chatcmpl_1","model":"qwen","choices":[{"delta":{"content":"你"},"finish_reason":null}]})"));
    REQUIRE(first.size() == 2);
    CHECK(std::holds_alternative<api::MessageStart>(first[0]));
    CHECK(std::get<api::TextDelta>(first[1]).text == "你");

    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[{"delta":{"content":"好"},"finish_reason":"stop"}],"usage":{"prompt_tokens":12,"completion_tokens":3,"prompt_tokens_details":{"cached_tokens":4}}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.stop_reason == "end_turn");
    // OpenAI/Qwen 风格:prompt_tokens 已含 cached_tokens,摊开后
    // input=12-4=8、cache_read=4,消费端不再加两遍。
    CHECK(event.usage.input_tokens == 8);
    CHECK(event.usage.cache_read_tokens == 4);
    CHECK(api::TotalInputTokens(event.usage) == 12);

    SUBCASE("structured duplicate keys retain the final body") {
        api::chat::EventParser duplicate_parser;
        const auto events = api::usage_fixture::ConsumeLegacyBody(duplicate_parser, Frame(
            R"({"id":"duplicate-body","model":"fixture-model","unused":{"a":[{"b":[1,true,null,18446744073709551615,1.25]}]},"unused":[{"c":{"d":["old"]}}],"unused":17,"unused":{"e":[[],{},[false]]},"unused":null,"choices":[{"delta":{"content":"old"}}],"choices":[{"delta":{"content":"last"}}]})"));
        REQUIRE(events.size() == 2);
        REQUIRE(std::holds_alternative<api::MessageStart>(events[0]));
        CHECK(std::get<api::MessageStart>(events[0]).id == "duplicate-body");
        CHECK(std::get<api::MessageStart>(events[0]).model == "fixture-model");
        REQUIRE(std::holds_alternative<api::TextDelta>(events[1]));
        CHECK(std::get<api::TextDelta>(events[1]).text == "last");
    }
    SUBCASE("deep extension JSON keeps the original body admission") {
        api::chat::EventParser deep_parser;
        std::string body = R"({"id":"deep-body","model":"fixture-model","choices":[{"delta":{"content":"deep"}}],"unused":)";
        body.append(512, '[');
        body += R"({"leaf":[null,true,-7,1.25,18446744073709551615]})";
        body.append(512, ']');
        body += '}';
        const auto events = api::usage_fixture::ConsumeLegacyBody(deep_parser, Frame(std::move(body)));
        REQUIRE(events.size() == 2);
        REQUIRE(std::holds_alternative<api::MessageStart>(events[0]));
        CHECK(std::get<api::MessageStart>(events[0]).id == "deep-body");
        CHECK(std::get<api::MessageStart>(events[0]).model == "fixture-model");
        REQUIRE(std::holds_alternative<api::TextDelta>(events[1]));
        CHECK(std::get<api::TextDelta>(events[1]).text == "deep");
    }
    SUBCASE("strict invalid input publishes no body") {
        api::chat::EventParser invalid_parser;
        CHECK(invalid_parser.Consume(Frame(R"({"choices":[{"delta":{"content":"hidden"}}]} true)")).empty());
        CHECK(invalid_parser.Consume(Frame(R"({"choices":[{"delta":{"content":"hidden"}}],"unused":{"a":[{"b":[1,2)")).empty());
        CHECK(invalid_parser.Consume(Frame("null")).empty());
    }
}

TEST_CASE("Chat events: 交错 tool_calls 先攒齐再按 index 吐出") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"tool_calls":[{"index":1,"id":"b","function":{"name":"write_file","arguments":"{\"p\":"}},{"index":0,"id":"a","function":{"name":"read_file","arguments":"{\"p\":\"a"}}]},"finish_reason":null}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"}"}},{"index":1,"function":{"arguments":"\"b\"}"}}]},"finish_reason":"tool_calls"}]})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 8);
    CHECK(std::get<api::ToolUseStart>(done[1]).id == "a");
    CHECK(std::get<api::ToolUseStart>(done[4]).id == "b");
    CHECK(std::get<api::MessageDone>(done[7]).stop_reason == "tool_use");
}

TEST_CASE("Chat events: API error 翻成 StreamError") {
    api::chat::EventParser parser;
    const auto events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(R"({"error":{"message":"bad key"}})"));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::StreamError>(events[0]).message == "bad key");
}

TEST_CASE("Chat events: reasoning_content 流式映射成 ThinkingDelta") {
    api::chat::EventParser parser;
    auto events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"chatcmpl_2","model":"deepseek","choices":[{"delta":{"reasoning_content":"先想"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<api::MessageStart>(events[0]));
    REQUIRE(std::holds_alternative<api::ThinkingDelta>(events[1]));
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "先想");

    events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[{"delta":{"reasoning_content":"想完"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::ThinkingDelta>(events[0]).text == "想完");

    // reasoning_content 过渡到 content:text 走 TextDelta
    events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
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
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"chatcmpl_ds","model":"deepseek-v4-pro","choices":[{"delta":{"content":"好"},"finish_reason":"stop"}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"prompt_tokens":50000,"completion_tokens":80,"prompt_cache_hit_tokens":49000,"prompt_cache_miss_tokens":1000}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
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
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[{"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":37,"completion_tokens":5}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.usage.input_tokens == 37);
    CHECK(event.usage.cache_read_tokens == 0);
    CHECK(event.usage_reported);
    CHECK_FALSE(event.cache_read_reported);
    CHECK(api::TotalInputTokens(event.usage) == 37);
}

TEST_CASE("Chat events: hit+miss 与 prompt_tokens 对不上——保留 hit/miss,不崩") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"prompt_tokens":60000,"completion_tokens":5,"prompt_cache_hit_tokens":49000,"prompt_cache_miss_tokens":1000}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    // 服务端账目不合时以缓存分项为准,保留原数,别拿 prompt_tokens 覆盖。
    CHECK(event.usage.input_tokens == 1000);
    CHECK(event.usage.cache_read_tokens == 49000);
    CHECK(api::TotalInputTokens(event.usage) == 50000);
}

TEST_CASE("Chat events: usage 在 finish chunk 与独立 chunk 各来一次,只认一份数") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}],"usage":{"prompt_tokens":100,"completion_tokens":10,"prompt_tokens_details":{"cached_tokens":60}}})"));
    // stream_options.include_usage=true 时 [DONE] 前还会再来一只空 choices 的
    // usage chunk——覆盖式记账,不得重复累计。
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"prompt_tokens":100,"completion_tokens":10,"prompt_tokens_details":{"cached_tokens":60}}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.usage.input_tokens == 40);
    CHECK(event.usage.cache_read_tokens == 60);
    CHECK(event.cache_read_reported);
    CHECK(event.usage.output_tokens == 10);
    CHECK(api::TotalInputTokens(event.usage) == 100);
}

TEST_CASE("Chat events: completion_tokens_details.reasoning_tokens 摊进 usage 拆账") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"r","choices":[{"delta":{"content":"答"},"finish_reason":"length"}],"usage":{"prompt_tokens":20,"completion_tokens":64,"completion_tokens_details":{"reasoning_tokens":60}}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    // reasoning 已含在 completion_tokens 总数里,不是另加的一笔。
    CHECK(event.usage.output_tokens == 64);
    CHECK(event.usage.output_reasoning_tokens == 60);
    CHECK(event.stop_reason == "max_tokens");  // finish_reason=length → 预算耗尽
}

TEST_CASE("Chat events: 顶层 reasoning_tokens 也认,没拆账就是 0") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"q","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}],"usage":{"prompt_tokens":9,"completion_tokens":2,"reasoning_tokens":7}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.usage.output_reasoning_tokens == 7);

    api::chat::EventParser bare;
    lubancode::api::usage_fixture::ConsumeLegacyBody(bare, Frame(
        R"({"id":"q2","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}],"usage":{"prompt_tokens":9,"completion_tokens":2}})"));
    const auto done2 = lubancode::api::usage_fixture::ConsumeLegacyBody(bare, Frame("[DONE]"));
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
    auto events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(frames[0].second));
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<api::MessageStart>(events[0]));

    // 2) delta.reasoning × N:思考逐块流出,每块立即成 ThinkingDelta。
    events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(frames[1].second));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::ThinkingDelta>(events[0]).text == "We");
    events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(frames[2].second));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::ThinkingDelta>(events[0]).text == " need");

    // 3) 正文 delta.content。
    events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(frames[3].second));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::TextDelta>(events[0]).text == "OK");

    // 4) finish_reason=stop 收口帧。
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(frames[4].second));

    // 5) usage-only chunk:choices 是空数组——usage 必须照收,不能因没有
    //    choices 便丢掉(根因三的现场:include_usage 开了才有这只帧)。
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(frames[5].second));

    // 6) [DONE]。
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(frames[6].second));
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
    auto events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"chatcmpl_ds","model":"deepseek","choices":[{"index":0,"delta":{"role":"assistant","reasoning_content":"先想"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "先想");
    events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[{"index":0,"delta":{"reasoning":"vLLM 风格"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 1);
    CHECK(std::get<api::ThinkingDelta>(events[0]).text == "vLLM 风格");
    events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[{"index":0,"delta":{"content":"答"},"finish_reason":"stop"}],"usage":{"prompt_tokens":5,"completion_tokens":1}})"));
    REQUIRE(events.size() == 1);
    CHECK(std::holds_alternative<api::TextDelta>(events[0]));
}

TEST_CASE("Chat events: 同一 chunk 两字段都有——去重,只吐一份") {
    // 镜像服务端会把 reasoning 同时写成两个字段:相等按一份算。
    api::chat::EventParser parser;
    auto events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"reasoning":"同一段","reasoning_content":"同一段"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "同一段");

    // 不相等:按固定优先级取 reasoning_content,另一份弃掉(诊断行点名),
    // 绝不拼成两段。
    api::chat::EventParser differ;
    events = lubancode::api::usage_fixture::ConsumeLegacyBody(differ, Frame(
        R"({"id":"y","choices":[{"delta":{"reasoning":"alias","reasoning_content":"canonical"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "canonical");

    // provider 声明了字段名:只认声明那个,另一个字段整个不看。
    api::chat::EventParser declared{"reasoning"};
    events = lubancode::api::usage_fixture::ConsumeLegacyBody(declared, Frame(
        R"({"id":"z","choices":[{"delta":{"reasoning":"声明优先","reasoning_content":"不该取这份"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::get<api::ThinkingDelta>(events[1]).text == "声明优先");
}

TEST_CASE("Chat events: reasoning-only + finish_reason=length——stop_reason=max_tokens,usage 照收") {
    api::chat::EventParser parser;
    // 思考吃满输出预算的现场(根因一/根因四):只有 reasoning,一个正文
    // 字都没有,finish_reason=length。解析层如实交账:ThinkingDelta 有、
    // TextDelta 无、stop_reason=max_tokens、usage 带拆账。
    auto events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"chatcmpl_len","model":"qwen3.8-27b","choices":[{"index":0,"delta":{"role":"assistant","reasoning":"先想"},"finish_reason":null}]})"));
    REQUIRE(events.size() == 2);
    CHECK(std::holds_alternative<api::MessageStart>(events[0]));
    CHECK(std::holds_alternative<api::ThinkingDelta>(events[1]));
    for (const char* piece : {"再想", "还在想"}) {
        events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(std::string(R"({"choices":[{"index":0,"delta":{"reasoning":")") + piece +
                                             R"("},"finish_reason":null}]})"));
        REQUIRE(events.size() == 1);
        CHECK(std::get<api::ThinkingDelta>(events[0]).text == piece);
    }
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[{"index":0,"delta":{},"finish_reason":"length"}],"usage":{"prompt_tokens":1200,"completion_tokens":4096,"completion_tokens_details":{"reasoning_tokens":4096}}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
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
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"chatcmpl_nousage","choices":[{"index":0,"delta":{"reasoning":"想"},"finish_reason":null}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[{"index":0,"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
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
    auto events = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"rd","choices":[{"index":0,"delta":{"reasoning_details":[{"type":"reasoning.text","text":"一块结构化思考"}]},"finish_reason":null}]})"));
    // 不吐 ThinkingDelta(未映射),也不吐 TextDelta(绝不混进正文)。
    for (const auto& event : events) {
        CHECK_FALSE(std::holds_alternative<api::ThinkingDelta>(event));
        CHECK_FALSE(std::holds_alternative<api::TextDelta>(event));
    }
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[{"index":0,"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    CHECK(parser.reasoning_details_blocks() == 1);
    CHECK(std::get<api::MessageDone>(done[0]).stop_reason == "end_turn");
}

// ---------------------------------------------------------------------------
// 缓存用量按 Wire 归一单 C4:双字段形状核对与异常账。DeepSeek 顶层 hit/miss
// 与 details.cached_tokens 同现只算一次;只报一项按明确规则推算/标异常,
// 不拿缺项零值造 100%;矛盾账(负数、cached>total、hit+miss 不符)原数保留、
// usage_anomaly 点名,绝不截零或截到 100%。
// ---------------------------------------------------------------------------

TEST_CASE("Chat events C4: DeepSeek 分项与 details.cached_tokens 同现——读取只算一次") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    // 兼容网关两副都回:读取按 DeepSeek 分项记,cached 不再加一遍。
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"prompt_tokens":12000,"completion_tokens":9,"prompt_cache_hit_tokens":8960,"prompt_cache_miss_tokens":3040,"prompt_tokens_details":{"cached_tokens":8960}}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.usage.cache_read_tokens == 8960);  // 只算一次
    CHECK(event.usage.input_tokens == 3040);
    CHECK(api::TotalInputTokens(event.usage) == 12000);
    CHECK(event.cache_read_reported);
    CHECK(event.usage_anomaly.empty());  // 两副一致:不算矛盾
}

TEST_CASE("Chat events C4: 只报 hit 不报 miss 也不报总量——不拿 0 补 miss 造 100%") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"completion_tokens":9,"prompt_cache_hit_tokens":8960}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    // 已知的只有 hit:数字照记,总量未知(anomaly 点名)——绝不 input=0
    // 凑出 8960/8960=100%。
    CHECK(event.usage.cache_read_tokens == 8960);
    CHECK(event.usage.input_tokens == 0);
    CHECK_FALSE(event.usage_anomaly.empty());
    CHECK(event.cache_read_reported);
}

TEST_CASE("Chat events C4: 只报 hit 但总量在场——miss 按总量推算并点名") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"prompt_tokens":12000,"completion_tokens":9,"prompt_cache_hit_tokens":8960}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    // miss = 12000-8960 = 3040,推算进 anomaly 说明,总输入保持 12000。
    CHECK(event.usage.input_tokens == 3040);
    CHECK(event.usage.cache_read_tokens == 8960);
    CHECK(api::TotalInputTokens(event.usage) == 12000);
    CHECK_FALSE(event.usage_anomaly.empty());
}

TEST_CASE("Chat events C4: hit+miss 与 prompt_tokens 冲突——原数保留并标异常") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"prompt_tokens":60000,"completion_tokens":5,"prompt_cache_hit_tokens":49000,"prompt_cache_miss_tokens":1000}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK(event.usage.input_tokens == 1000);
    CHECK(event.usage.cache_read_tokens == 49000);
    CHECK(api::TotalInputTokens(event.usage) == 50000);  // 原数,不被 60000 覆盖
    CHECK_FALSE(event.usage_anomaly.empty());             // 冲突点名
}

TEST_CASE("Chat events C4: cached_tokens 超过 prompt_tokens——input 为负保留,不截零") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"prompt_tokens":1000,"completion_tokens":5,"prompt_tokens_details":{"cached_tokens":1200}}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    // 旧实现会 max(total-cached,0) 截零:那是拿 0 掩盖矛盾。现在 input
    // 照记 -200,anomaly 点名,消费端把这笔排除出精确比例。
    CHECK(event.usage.input_tokens == -200);
    CHECK(event.usage.cache_read_tokens == 1200);
    CHECK(api::TotalInputTokens(event.usage) == 1000);  // 总输入仍是厂商原数
    CHECK_FALSE(event.usage_anomaly.empty());
}

TEST_CASE("Chat events C4: 负数字段——标异常,样本不冒充精确比例") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"prompt_tokens":100,"completion_tokens":5,"prompt_cache_hit_tokens":-10,"prompt_cache_miss_tokens":110}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    CHECK_FALSE(event.usage_anomaly.empty());  // 负数点名
    // 数字照记(hit 读不出时落 0,但 seen 旗标与 anomaly 都在)。
    CHECK(event.usage.input_tokens == 110);
    CHECK(event.cache_read_reported);  // 字段在场:明报位仍真
}

TEST_CASE("Chat events C4: 只报 miss 不报 hit——读取未知,不冒充已知零") {
    api::chat::EventParser parser;
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"id":"x","choices":[{"delta":{"content":"答"},"finish_reason":"stop"}]})"));
    lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame(
        R"({"choices":[],"usage":{"completion_tokens":5,"prompt_cache_miss_tokens":3040}})"));
    const auto done = lubancode::api::usage_fixture::ConsumeLegacyBody(parser, Frame("[DONE]"));
    REQUIRE(done.size() == 1);
    const auto& event = std::get<api::MessageDone>(done[0]);
    // 已知只有 miss:读取量未知(明报位 false + anomaly 点名),不写 0%
    // 也不写 100%。
    CHECK(event.usage.input_tokens == 3040);
    CHECK(event.usage.cache_read_tokens == 0);
    CHECK_FALSE(event.cache_read_reported);
    CHECK_FALSE(event.usage_anomaly.empty());
}


TEST_CASE("Five-field Chat details: malformed containers retain witnesses and block alias fallback") {
    namespace facts = lubancore::usage::v1;
    for (const auto& malformed : {nlohmann::json(nullptr), nlohmann::json(37),
                                  nlohmann::json("wrong"), nlohmann::json::array()}) {
        api::chat::EventParser parser;
        nlohmann::json wire{{"id", "real-chat"}, {"choices", nlohmann::json::array()},
            {"usage", {{"prompt_tokens", 100}, {"completion_tokens", 7},
                       {"prompt_tokens_details", malformed}, {"completion_tokens_details", malformed},
                       {"cache_write_tokens", 5}, {"reasoning_tokens", 2}}}};
        const auto events = parser.Consume(Frame(wire.dump()));
        const api::UsageSnapshot* snapshot = nullptr;
        for (const auto& event : events) if (const auto* value = std::get_if<api::UsageSnapshot>(&event)) snapshot = value;
        REQUIRE(snapshot); REQUIRE(snapshot->usage_observation);
        CHECK(snapshot->usage.output_tokens == 7);
        CHECK(snapshot->usage.cache_creation_tokens == 0); CHECK(snapshot->usage.output_reasoning_tokens == 0);
        const auto& observation = *snapshot->usage_observation;
        CHECK(observation.fields[2].presence == facts::Presence::Missing);
        CHECK(observation.fields[3].validity == facts::Validity::UnavailableOperands);
        CHECK(observation.fields[4].validity == facts::Validity::UnavailableOperands);
        bool parent = false, alias = false, conflict = false;
        for (const auto& raw : observation.raw_fields) {
            parent = parent || raw.path == "usage.prompt_tokens_details";
            alias = alias || (raw.path == "usage.cache_write_tokens" && raw.integer == 5);
        }
        for (const auto& anomaly : observation.anomalies)
            conflict = conflict || (anomaly.field == facts::Field::CacheCreation &&
                                    anomaly.code == facts::AnomalyCode::AliasConflict && anomaly.raw_field_count == 2);
        CHECK(parent); CHECK(alias); CHECK(conflict);
    }
}


TEST_CASE("Five-field aliases: equal summaries never prove equality of uncaptured values") {
    namespace facts = lubancore::usage::v1;
    const auto large = facts::kMaxMaterialBytes + 1;
    const std::vector<std::pair<nlohmann::json, nlohmann::json>> pairs{
        {std::string(large, 'a'), std::string(large, 'b')},
        {nlohmann::json{{"left", 1}}, nlohmann::json{{"right", 1}}},
        {nlohmann::json::array({1}), nlohmann::json::array({2})}};
    for (const auto& [canonical, alias] : pairs) {
        api::chat::EventParser parser;
        nlohmann::json wire{{"choices", nlohmann::json::array()},
            {"usage", {{"prompt_tokens", 100}, {"completion_tokens", 7},
                       {"prompt_tokens_details", {{"cache_write_tokens", canonical}}},
                       {"cache_write_tokens", alias}}}};
        const auto events = parser.Consume(Frame(wire.dump()));
        const api::UsageSnapshot* snapshot = nullptr;
        for (const auto& event : events) if (const auto* value = std::get_if<api::UsageSnapshot>(&event)) snapshot = value;
        REQUIRE(snapshot); REQUIRE(snapshot->usage_observation);
        const auto& observation = *snapshot->usage_observation;
        bool uncertain = false;
        for (const auto& anomaly : observation.anomalies)
            uncertain = uncertain || (anomaly.field == facts::Field::CacheCreation &&
                anomaly.code == facts::AnomalyCode::AliasUnverifiable && anomaly.raw_field_count == 2);
        CHECK(uncertain);
        const auto encoded = api::usage_json::Encode(observation, snapshot->usage);
        REQUIRE(encoded);
        const auto decoded = api::usage_json::Decode(*encoded, snapshot->usage);
        REQUIRE(decoded);
        bool restored_uncertainty = false;
        for (const auto& anomaly : decoded->anomalies)
            restored_uncertainty = restored_uncertainty || anomaly.code == facts::AnomalyCode::AliasUnverifiable;
        CHECK(restored_uncertainty);
        CHECK(observation.fields[3].validity == facts::Validity::InvalidType);
        CHECK(snapshot->usage.cache_creation_tokens == 0);
        for (const auto& raw : observation.raw_fields) {
            CHECK(raw.summary.size() <= facts::kMaxSummaryBytes);
            if (raw.kind == facts::RawKind::String) CHECK(raw.fingerprint.empty());
        }
    }
}


TEST_CASE("Five-field Chat terminal: text and tool completion preserve the observed material") {
    for (const bool tool : {false, true}) {
        api::chat::EventParser parser;
        api::MessageAssembler assembler;
        const auto body = tool
            ? R"({"tool_calls":[{"index":0,"id":"real-tool","function":{"name":"inspect","arguments":"{}"}}]})"
            : R"({"content":"answer"})";
        nlohmann::json wire{{"id", "real-provider"}, {"model", "fixture"},
            {"choices", nlohmann::json::array({{{"delta", nlohmann::json::parse(body)},
                                               {"finish_reason", tool ? "tool_calls" : "stop"}}})},
            {"usage", {{"prompt_tokens", 100}, {"completion_tokens", 7},
                       {"prompt_tokens_details", {{"cached_tokens", 11}, {"cache_write_tokens", 13}}},
                       {"completion_tokens_details", {{"reasoning_tokens", 2}}}}}};
        for (const auto& event : parser.Consume(Frame(wire.dump()))) assembler.Feed(event);
        REQUIRE(assembler.usage_observation());
        CHECK(assembler.stop_reason().empty());
        const auto terminal = parser.Finish();
        int completed = 0;
        for (const auto& event : terminal) {
            if (const auto* done = std::get_if<api::MessageDone>(&event)) {
                ++completed; REQUIRE(done->usage_observation);
                CHECK(done->provider_response_id == "real-provider");
            }
            assembler.Feed(event);
        }
        CHECK(completed == 1); REQUIRE(assembler.usage_observation());
        CHECK(assembler.usage().input_tokens == 76); CHECK(assembler.usage().output_tokens == 7);
        CHECK(assembler.usage().cache_read_tokens == 11); CHECK(assembler.usage().cache_creation_tokens == 13);
        CHECK(assembler.usage().output_reasoning_tokens == 2);
        CHECK(assembler.stop_reason() == (tool ? "tool_use" : "end_turn"));
        CHECK(parser.Finish().empty());
    }
}
