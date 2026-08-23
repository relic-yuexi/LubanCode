// 脚本化 StreamEvent 序列,验证 MessageAssembler 攒出的 Message 对不对:
// 纯 text、单 tool_use、text+tool_use 混合、input JSON 劈多段、input 非法
// JSON 报错但不崩。

#include <doctest/doctest.h>

#include <variant>

#include "api/assembler.hpp"
#include "api/types.hpp"

using namespace lubancode::api;

TEST_CASE("纯 text:多段 TextDelta 拼成一个 TextBlock") {
    MessageAssembler assembler;
    assembler.Feed(MessageStart{"msg_1", "some-model"});
    assembler.Feed(TextDelta{"你好,"});
    assembler.Feed(TextDelta{"世界"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"end_turn", Usage{10, 5}});

    CHECK_FALSE(assembler.has_parse_error());
    CHECK(assembler.stop_reason() == "end_turn");
    CHECK(assembler.usage().input_tokens == 10);
    CHECK(assembler.usage().output_tokens == 5);

    const Message message = assembler.BuildMessage();
    REQUIRE(message.role == Role::Assistant);
    REQUIRE(message.content.size() == 1);
    REQUIRE(std::holds_alternative<TextBlock>(message.content[0]));
    CHECK(std::get<TextBlock>(message.content[0]).text == "你好,世界");
}

TEST_CASE("单 tool_use:input JSON 一次性喂完,攒出 ToolUseBlock") {
    MessageAssembler assembler;
    assembler.Feed(MessageStart{"msg_2", "some-model"});
    assembler.Feed(ToolUseStart{0, "toolu_01", "get_weather"});
    assembler.Feed(ToolUseInputDelta{0, R"({"city":"杭州"})"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    CHECK_FALSE(assembler.has_parse_error());
    CHECK(assembler.stop_reason() == "tool_use");

    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 1);
    REQUIRE(std::holds_alternative<ToolUseBlock>(message.content[0]));
    const auto& block = std::get<ToolUseBlock>(message.content[0]);
    CHECK(block.id == "toolu_01");
    CHECK(block.name == "get_weather");
    CHECK(block.input.at("city").get<std::string>() == "杭州");
}

TEST_CASE("text + tool_use 混合:先文本后工具调用,两个块顺序不错") {
    MessageAssembler assembler;
    assembler.Feed(MessageStart{"msg_3", "some-model"});
    assembler.Feed(TextDelta{"我来查一下天气。"});
    // 没有显式的 ContentBlockDone 也没关系:ToolUseStart 自己会把前一个块收尾。
    assembler.Feed(ToolUseStart{1, "toolu_02", "get_weather"});
    assembler.Feed(ToolUseInputDelta{1, R"({"city":)"});
    assembler.Feed(ToolUseInputDelta{1, R"("北京"})"});
    assembler.Feed(ContentBlockDone{1});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 2);
    REQUIRE(std::holds_alternative<TextBlock>(message.content[0]));
    CHECK(std::get<TextBlock>(message.content[0]).text == "我来查一下天气。");
    REQUIRE(std::holds_alternative<ToolUseBlock>(message.content[1]));
    const auto& tool_block = std::get<ToolUseBlock>(message.content[1]);
    CHECK(tool_block.name == "get_weather");
    CHECK(tool_block.input.at("city").get<std::string>() == "北京");
}

TEST_CASE("input JSON 劈成好几段,拼起来还是对的") {
    MessageAssembler assembler;
    assembler.Feed(ToolUseStart{0, "toolu_03", "read_file"});
    assembler.Feed(ToolUseInputDelta{0, R"({"pa)"});
    assembler.Feed(ToolUseInputDelta{0, R"(th":"a)"});
    assembler.Feed(ToolUseInputDelta{0, R"(.txt","limit":)"});
    assembler.Feed(ToolUseInputDelta{0, R"(10})"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    CHECK_FALSE(assembler.has_parse_error());
    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 1);
    const auto& block = std::get<ToolUseBlock>(message.content[0]);
    CHECK(block.input.at("path").get<std::string>() == "a.txt");
    CHECK(block.input.at("limit").get<int>() == 10);
}

TEST_CASE("input 非法 JSON:报错但不崩,依旧攒出一个可用的 Message") {
    MessageAssembler assembler;
    assembler.Feed(ToolUseStart{0, "toolu_04", "read_file"});
    assembler.Feed(ToolUseInputDelta{0, "{not valid json"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    CHECK(assembler.has_parse_error());
    CHECK_FALSE(assembler.parse_error().empty());

    // 就算解析失败,BuildMessage() 也不能崩、也不能丢块——input 退化成空对象。
    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 1);
    REQUIRE(std::holds_alternative<ToolUseBlock>(message.content[0]));
    const auto& block = std::get<ToolUseBlock>(message.content[0]);
    CHECK(block.id == "toolu_04");
    CHECK(block.name == "read_file");
    CHECK(block.input.is_object());
    CHECK(block.input.empty());
}

TEST_CASE("空的 tool_use(没有任何 ToolUseInputDelta)攒出空对象 input,不报错") {
    MessageAssembler assembler;
    assembler.Feed(ToolUseStart{0, "toolu_05", "no_args_tool"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"tool_use", Usage{}});

    CHECK_FALSE(assembler.has_parse_error());
    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 1);
    const auto& block = std::get<ToolUseBlock>(message.content[0]);
    CHECK(block.input.is_object());
    CHECK(block.input.empty());
}

TEST_CASE("thinking + text:ThinkingDelta 攒成 ThinkingBlock(signature 也拼上)") {
    MessageAssembler assembler;
    assembler.Feed(ThinkingDelta{"分析", ""});
    assembler.Feed(ThinkingDelta{"一下", ""});
    assembler.Feed(ThinkingDelta{"", "sig_abc"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(TextDelta{"答案是 42"});
    assembler.Feed(ContentBlockDone{1});
    assembler.Feed(MessageDone{"end_turn", Usage{}});

    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 2);
    REQUIRE(std::holds_alternative<ThinkingBlock>(message.content[0]));
    const auto& thinking = std::get<ThinkingBlock>(message.content[0]);
    CHECK(thinking.text == "分析一下");
    CHECK(thinking.signature == "sig_abc");
    REQUIRE(std::holds_alternative<TextBlock>(message.content[1]));
    CHECK(std::get<TextBlock>(message.content[1]).text == "答案是 42");
}

TEST_CASE("chat wire 过渡:thinking 后接 text,没有 ContentBlockDone 也能自动收尾") {
    MessageAssembler assembler;
    assembler.Feed(ThinkingDelta{"先想想", ""});
    assembler.Feed(TextDelta{"再回答"});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{"end_turn", Usage{}});

    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 2);
    REQUIRE(std::holds_alternative<ThinkingBlock>(message.content[0]));
    CHECK(std::get<ThinkingBlock>(message.content[0]).text == "先想想");
    REQUIRE(std::holds_alternative<TextBlock>(message.content[1]));
    CHECK(std::get<TextBlock>(message.content[1]).text == "再回答");
}
