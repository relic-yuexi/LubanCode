// 脚本化 StreamEvent 序列,验证 MessageAssembler 攒出的 Message 对不对:
// 纯 text、单 tool_use、text+tool_use 混合、input JSON 劈多段、input 非法
// JSON 报错但不崩。

#include <doctest/doctest.h>

#include <variant>

#include "api/assembler.hpp"
#include "api/types.hpp"
#include "platform/text_encoding.hpp"  // IsValidUtf8:清洗结果断言

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

TEST_CASE("SanitizeMessage:合法内容原样不动,坏字节按 U+FFFD 清洗") {
    // 合法 UTF-8:一字不动。
    Message clean;
    clean.role = Role::User;
    clean.content.push_back(TextBlock{"你好,世界"});
    clean.content.push_back(ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a.txt"}}});
    clean.content.push_back(ToolResultBlock{"call_1", "工具输出:正常内容", false});
    clean.content.push_back(ThinkingBlock{"思考一下", "sig_01"});
    Message clean_copy = clean;
    SanitizeMessage(clean);
    // 合法内容清洗后必须一字不动。
    REQUIRE(clean.content.size() == clean_copy.content.size());
    for (std::size_t i = 0; i < clean.content.size(); ++i) {
        const auto& a = clean.content[i];
        const auto& b = clean_copy.content[i];
        REQUIRE(a.index() == b.index());
        std::visit(
            [&](const auto& av) {
                using T = std::decay_t<decltype(av)>;
                const auto& bv = std::get<T>(b);
                if constexpr (std::is_same_v<T, TextBlock>) {
                    CHECK(av.text == bv.text);
                } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                    CHECK(av.id == bv.id);
                    CHECK(av.name == bv.name);
                    CHECK(av.input == bv.input);
                } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                    CHECK(av.tool_use_id == bv.tool_use_id);
                    CHECK(av.content == bv.content);
                    CHECK(av.is_error == bv.is_error);
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    CHECK(av.text == bv.text);
                    CHECK(av.signature == bv.signature);
                }
            },
            a);
    }

    // 非法 UTF-8(夹着 0xE4 开头的残序列):清洗后必须是合法 UTF-8,
    // 且合法片段保留、坏字节变成 U+FFFD。
    const std::string bad = "前\xE4\xB8后";  // \xE4\xB8 是"中"的前两字节,缺第三字节
    REQUIRE_FALSE(lubancode::platform::IsValidUtf8(bad));
    Message dirty;
    dirty.role = Role::User;
    dirty.content.push_back(TextBlock{bad});
    dirty.content.push_back(ToolUseBlock{"call_2", "search", nlohmann::json{{"path", bad}, {"pattern", "ok"}}});
    dirty.content.push_back(ToolResultBlock{"call_2", bad, false});
    dirty.content.push_back(ThinkingBlock{bad, bad});

    SanitizeMessage(dirty);
    for (const auto& block : dirty.content) {
        std::visit(
            [](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, TextBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.text));
                    CHECK(b.text.find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD
                    CHECK(b.text.find("后") != std::string::npos);            // 合法片段保留
                } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(
                        b.input.at("path").template get<std::string>()));
                } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.content));
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.text));
                    CHECK(lubancode::platform::IsValidUtf8(b.signature));
                }
            },
            block);
    }
}

TEST_CASE("SanitizeRequest:所有 wire 字符串出口一并清洗") {
    const std::string bad = "前\xE4\xB8后";
    REQUIRE_FALSE(lubancode::platform::IsValidUtf8(bad));

    Request request;
    request.model = bad;
    request.system = bad;
    request.reasoning_effort = bad;
    request.messages.push_back(Message{
        Role::User,
        {TextBlock{bad}, ToolUseBlock{bad, bad, nlohmann::json{{"path", bad}}},
         ToolResultBlock{bad, bad, false}, ThinkingBlock{bad, bad}}});
    request.tools.push_back(ToolDefinition{bad, bad, nlohmann::json{{"type", "object"}, {"title", bad}}});
    request.extra_body = nlohmann::json{{"vendor", nlohmann::json{{"note", bad}}}};

    SanitizeRequest(request);

    CHECK(lubancode::platform::IsValidUtf8(request.model));
    CHECK(lubancode::platform::IsValidUtf8(request.system));
    CHECK(lubancode::platform::IsValidUtf8(request.reasoning_effort));
    for (const auto& block : request.messages[0].content) {
        std::visit(
            [](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, TextBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.text));
                } else if constexpr (std::is_same_v<T, ToolUseBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.id));
                    CHECK(lubancode::platform::IsValidUtf8(b.name));
                    CHECK(lubancode::platform::IsValidUtf8(
                        b.input.at("path").template get<std::string>()));
                } else if constexpr (std::is_same_v<T, ToolResultBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.tool_use_id));
                    CHECK(lubancode::platform::IsValidUtf8(b.content));
                } else if constexpr (std::is_same_v<T, ThinkingBlock>) {
                    CHECK(lubancode::platform::IsValidUtf8(b.text));
                    CHECK(lubancode::platform::IsValidUtf8(b.signature));
                }
            },
            block);
    }
    CHECK(lubancode::platform::IsValidUtf8(request.tools[0].name));
    CHECK(lubancode::platform::IsValidUtf8(request.tools[0].description));
    CHECK(lubancode::platform::IsValidUtf8(request.tools[0].input_schema.at("title").get<std::string>()));
    CHECK(lubancode::platform::IsValidUtf8(request.extra_body["vendor"]["note"].get<std::string>()));
}
