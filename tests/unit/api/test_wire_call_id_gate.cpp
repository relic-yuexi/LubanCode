// 子代理空轨迹单 P0-F(5.1):空 call id 挡在 provider 边界。
//   - Responses 流式:早帧(output_item.added)缺 call_id、终帧(output_item.
//     done)补 id——assembler 按 output index 合并终帧身份,最终 id 正确。
//   - added/done 都缺 id:不产出可执行 ToolUseBlock,不伪造 provider id。
//   - ESC 截断(硬收尾)仍无 id:同样不产出 ToolUseBlock,不合成局部结果。
//   - Chat wire:不再本地造 "chat_tool_N" 假 id;id 缺席如实透传交 assembler 挡。
//   - 非流式 Responses:function_call 缺 call_id 同门。
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/assembler.hpp"
#include "api/chat/events.hpp"
#include "api/responses/events.hpp"
#include "api/sse_framing.hpp"
#include "api/types.hpp"

using namespace lubancode;
using namespace lubancode::api;

namespace {

const api::ToolUseBlock* FirstToolUse(const api::Message& message) {
    for (const auto& block : message.content) {
        if (const auto* call = std::get_if<api::ToolUseBlock>(&block)) {
            return call;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("P0-F assembler:早帧缺 id、终帧补 id——按 output index 合并终帧身份") {
    MessageAssembler assembler;
    assembler.Feed(api::MessageStart{"msg", "model"});
    assembler.Feed(api::ToolUseStart{1, "", "read_file"});  // added 帧缺 call_id
    assembler.Feed(api::ToolUseInputDelta{1, R"({"path":"README.md"})"});
    api::ContentBlockDone done;
    done.index = 1;
    done.tool_use_id = "call_late_001";  // done 帧补齐身份
    assembler.Feed(done);
    assembler.Feed(api::MessageDone{"tool_use", api::Usage{}});

    const api::Message message = assembler.BuildMessage();
    const api::ToolUseBlock* call = FirstToolUse(message);
    REQUIRE(call != nullptr);
    CHECK(call->id == "call_late_001");  // 最终 id 正确,不是空也不是假 id
    CHECK(call->name == "read_file");
    CHECK(call->input.value("path", std::string()) == "README.md");
    CHECK(assembler.idless_tool_calls_dropped() == 0);
}

TEST_CASE("P0-F assembler:added/done 都缺 id——不产出可执行 ToolUseBlock") {
    MessageAssembler assembler;
    assembler.Feed(api::MessageStart{"msg", "model"});
    assembler.Feed(api::TextDelta{"先想想。"});
    assembler.Feed(api::ToolUseStart{1, "", "read_file"});
    assembler.Feed(api::ToolUseInputDelta{1, "{}"});
    api::ContentBlockDone done;  // done 帧也没有 id
    done.index = 1;
    assembler.Feed(done);
    assembler.Feed(api::MessageDone{"tool_use", api::Usage{}});

    const api::Message message = assembler.BuildMessage();
    CHECK(FirstToolUse(message) == nullptr);   // 不进 ToolUseBlock,不执行
    CHECK(assembler.idless_tool_calls_dropped() == 1);  // 消费端按畸形收口的凭据
    // 文本块照常保留。
    bool has_text = false;
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr && !text->text.empty()) {
            has_text = true;
        }
    }
    CHECK(has_text);
}

TEST_CASE("P0-F assembler:ESC 硬收尾仍无 id——丢弃,不合成局部工具结果") {
    MessageAssembler assembler;
    assembler.Feed(api::MessageStart{"msg", "model"});
    assembler.Feed(api::ToolUseStart{0, "", "run_command"});
    assembler.Feed(api::ToolUseInputDelta{0, R"({"command":"ping"})"});
    // 流被掐断:ContentBlockDone/MessageDone 永远不来,宿主硬收尾。
    assembler.FinalizeOpenBlock();
    const api::Message message = assembler.BuildMessage();
    CHECK(FirstToolUse(message) == nullptr);
    CHECK(assembler.idless_tool_calls_dropped() == 1);
}

TEST_CASE("P0-F Responses 流式:added 缺 call_id、done 补 call_id 的真帧形状") {
    // added 帧:item 有 id 字段但没有 call_id(兼容端实测形状)。
    const std::string added = R"({"type":"response.output_item.added","output_index":2,
        "item":{"type":"function_call","id":"fc_01","call_id":"","name":"search","arguments":""}})";
    const std::string delta =
        R"({"type":"response.function_call_arguments.delta","output_index":2,"delta":"{\"q\":\"a\"}"})";
    const std::string done = R"({"type":"response.output_item.done","output_index":2,
        "item":{"type":"function_call","id":"fc_01","call_id":"call_abc123","name":"search","arguments":"{\"q\":\"a\"}"}})";
    const std::string completed = R"({"type":"response.completed",
        "response":{"id":"resp_1","status":"completed","output":[],"usage":{}}})";

    MessageAssembler assembler;
    for (const std::string& data : {added, delta, done, completed}) {
        const auto event = responses::parse_event(api::SseFrame{"message", data});
        REQUIRE(event.has_value());
        assembler.Feed(*event);
    }
    const api::Message message = assembler.BuildMessage();
    const api::ToolUseBlock* call = FirstToolUse(message);
    REQUIRE(call != nullptr);
    CHECK(call->id == "call_abc123");
    CHECK(assembler.idless_tool_calls_dropped() == 0);
}

TEST_CASE("P0-F Responses 流式:终帧也无 call_id——可执行块不诞生") {
    const std::string added = R"({"type":"response.output_item.added","output_index":0,
        "item":{"type":"function_call","id":"fc_02","call_id":"","name":"read_file","arguments":""}})";
    const std::string done = R"({"type":"response.output_item.done","output_index":0,
        "item":{"type":"function_call","id":"fc_02","call_id":"","name":"read_file","arguments":"{}"}})";
    const std::string completed = R"({"type":"response.completed",
        "response":{"id":"resp_2","status":"completed","output":[],"usage":{}}})";

    MessageAssembler assembler;
    for (const std::string& data : {added, done, completed}) {
        if (const auto event = responses::parse_event(api::SseFrame{"message", data})) {
            assembler.Feed(*event);
        }
    }
    CHECK(FirstToolUse(assembler.BuildMessage()) == nullptr);
    CHECK(assembler.idless_tool_calls_dropped() == 1);
}

TEST_CASE("P0-F Responses 非流式:function_call 缺 call_id 同门") {
    const std::string body = R"({"id":"resp_3","status":"completed","output":[
        {"type":"function_call","id":"fc_03","call_id":"","name":"read_file","arguments":"{}"}],
        "usage":{"input_tokens":10,"output_tokens":5}})";
    const auto events = responses::ExpandNonStreamResponse(body);
    MessageAssembler assembler;
    for (const auto& event : events) {
        assembler.Feed(event);
    }
    CHECK(FirstToolUse(assembler.BuildMessage()) == nullptr);
    CHECK(assembler.idless_tool_calls_dropped() == 1);
    // 有 id 的非流式调用照常保留(回归钉)。
    const std::string ok_body = R"({"id":"resp_4","status":"completed","output":[
        {"type":"function_call","id":"fc_04","call_id":"call_ok_9","name":"read_file","arguments":"{}"}],
        "usage":{"input_tokens":10,"output_tokens":5}})";
    MessageAssembler ok_assembler;
    for (const auto& event : responses::ExpandNonStreamResponse(ok_body)) {
        ok_assembler.Feed(event);
    }
    const api::Message ok_message = ok_assembler.BuildMessage();  // 具名:指针不悬垂
    const api::ToolUseBlock* call = FirstToolUse(ok_message);
    REQUIRE(call != nullptr);
    CHECK(call->id == "call_ok_9");
    CHECK(ok_assembler.idless_tool_calls_dropped() == 0);
}

TEST_CASE("P0-F Chat wire:工具调用缺 id 不再造 chat_tool_N 假 id") {
    // delta 不带 id 字段(坏兼容端形状);Finish 时不再合成。
    const std::string no_id_chunk = R"({"id":"cmpl-1","model":"m","choices":[{"index":0,
        "delta":{"tool_calls":[{"index":0,"function":{"name":"read_file","arguments":"{}"}}]}}]})";
    const std::string finish_chunk =
        R"({"id":"cmpl-1","model":"m","choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}]})";

    chat::EventParser parser;
    MessageAssembler assembler;
    for (const std::string& data : {no_id_chunk, finish_chunk}) {
        for (const auto& event : parser.Consume(api::SseFrame{"message", data})) {
            assembler.Feed(event);
        }
    }
    for (const auto& event : parser.Finish()) {
        assembler.Feed(event);
    }
    const api::Message message = assembler.BuildMessage();
    const api::ToolUseBlock* call = FirstToolUse(message);
    CHECK(call == nullptr);  // 不伪造 provider 认可不了的 id
    CHECK(assembler.idless_tool_calls_dropped() == 1);

    // 带 id 的正常路(回归钉):id 原样透传。
    const std::string id_chunk = R"({"id":"cmpl-2","model":"m","choices":[{"index":0,
        "delta":{"tool_calls":[{"index":0,"id":"call_good_7","function":{"name":"read_file","arguments":"{}"}}]}}]})";
    chat::EventParser ok_parser;
    MessageAssembler ok_assembler;
    for (const auto& event : ok_parser.Consume(api::SseFrame{"message", id_chunk})) {
        ok_assembler.Feed(event);
    }
    for (const auto& event : ok_parser.Finish()) {
        ok_assembler.Feed(event);
    }
    const api::Message ok_message = ok_assembler.BuildMessage();  // 具名:指针不悬垂
    const api::ToolUseBlock* ok_call = FirstToolUse(ok_message);
    REQUIRE(ok_call != nullptr);
    CHECK(ok_call->id == "call_good_7");
    CHECK(ok_assembler.idless_tool_calls_dropped() == 0);
}

TEST_CASE("P0-F Anthropic wire:content_block_start 缺 id——由 assembler 同门挡下") {
    // anthropic 的解析层不造 id,空 id 原样透传(assembler 统一门)。
    MessageAssembler assembler;
    assembler.Feed(api::MessageStart{"msg", "claude"});
    assembler.Feed(api::ToolUseStart{1, "", "read_file"});
    assembler.Feed(api::ToolUseInputDelta{1, "{}"});
    assembler.Feed(api::ContentBlockDone{1});
    assembler.Feed(api::MessageDone{"tool_use", api::Usage{}});
    CHECK(FirstToolUse(assembler.BuildMessage()) == nullptr);
    CHECK(assembler.idless_tool_calls_dropped() == 1);
}
