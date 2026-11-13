// 工具入参兜底(开源模型坏 JSON 单)的 api 层:宽松解析纯函数各病例
//(围栏、尾逗号、单引号、截断补齐成功/拒绝、本来合法的不动),以及
// assembler 收尾时的接线——修补带标记、修不成带 input_error、长度截断
// 一律改判截断错误、修补过的块不进流式提前档。

#include <doctest/doctest.h>

#include <string>
#include <variant>

#include "api/assembler.hpp"
#include "api/tool_args_repair.hpp"
#include "api/types.hpp"

using namespace lubancode::api;

namespace {

bool HasRepair(const ToolArgsRepairResult& result, const std::string& needle) {
    for (const std::string& repair : result.repairs) {
        if (repair.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// 单枚 tool_use 喂进 assembler,按给定 stop_reason 收口,交回那枚块。
ToolUseBlock AssembleOne(MessageAssembler& assembler, const std::string& raw, const std::string& stop_reason) {
    assembler.Feed(MessageStart{"msg", "model"});
    assembler.Feed(ToolUseStart{0, "toolu_1", "write_file"});
    assembler.Feed(ToolUseInputDelta{0, raw});
    assembler.Feed(ContentBlockDone{0});
    assembler.Feed(MessageDone{stop_reason, Usage{}});
    const Message message = assembler.BuildMessage();
    REQUIRE(message.content.size() == 1);
    REQUIRE(std::holds_alternative<ToolUseBlock>(message.content[0]));
    return std::get<ToolUseBlock>(message.content[0]);
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------

TEST_CASE("入参修补:本来合法的原样通过,不带修补标记") {
    const auto result = RepairToolArgs(R"({"path":"a.txt","n":3,"tags":["x","y"]})");
    REQUIRE(result.ok);
    CHECK_FALSE(result.repaired);
    CHECK(result.repairs.empty());
    CHECK_FALSE(result.closed_truncation);
    CHECK(result.value.at("path") == "a.txt");
    CHECK(result.value.at("n") == 3);
}

TEST_CASE("入参修补:空串与全空白按无参调用给 {},不算修补") {
    for (const char* raw : {"", "   ", "\n\t"}) {
        const auto result = RepairToolArgs(raw);
        REQUIRE(result.ok);
        CHECK_FALSE(result.repaired);
        CHECK(result.value.is_object());
        CHECK(result.value.empty());
    }
}

TEST_CASE("入参修补:剥 ```json 围栏,有尾围栏没尾围栏都救得回") {
    {
        const auto result = RepairToolArgs("```json\n{\"path\":\"a.txt\"}\n```");
        REQUIRE(result.ok);
        CHECK(result.repaired);
        CHECK(HasRepair(result, "围栏"));
        CHECK_FALSE(result.closed_truncation);
        CHECK(result.value.at("path") == "a.txt");
    }
    {
        const auto result = RepairToolArgs("```\n{\"path\":\"b.txt\"}");
        REQUIRE(result.ok);
        CHECK(result.value.at("path") == "b.txt");
    }
}

TEST_CASE("入参修补:去对象与数组末尾多余逗号,字符串里的逗号不动") {
    const auto result = RepairToolArgs(R"({"a":[1,2,],"s":"x,}","b":true,})");
    REQUIRE(result.ok);
    CHECK(result.repaired);
    CHECK(HasRepair(result, "逗号"));
    CHECK(result.value.at("a").size() == 2);
    CHECK(result.value.at("s") == "x,}");
    CHECK(result.value.at("b") == true);
}

TEST_CASE("入参修补:通篇单引号改双引号,\\' 还原成裸单引号") {
    const auto result = RepairToolArgs(R"({'path': 'a.txt', 'msg': 'it\'s ok'})");
    REQUIRE(result.ok);
    CHECK(result.repaired);
    CHECK(HasRepair(result, "单引号"));
    CHECK(result.value.at("path") == "a.txt");
    CHECK(result.value.at("msg") == "it's ok");
}

TEST_CASE("入参修补:单双引号混写不碰单引号那味") {
    const auto result = RepairToolArgs(R"({"path": 'a.txt'})");
    CHECK_FALSE(result.ok);
    CHECK_FALSE(HasRepair(result, "单引号"));
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("入参修补:末尾未闭合的字符串与括号补齐成 object,置截断嫌疑") {
    const auto result = RepairToolArgs(R"({"path":"a.txt","content":"line1\nline2)");
    REQUIRE(result.ok);
    CHECK(result.repaired);
    CHECK(result.closed_truncation);
    CHECK(HasRepair(result, "截断"));
    CHECK(result.value.at("content") == "line1\nline2");

    const auto nested = RepairToolArgs(R"({"a":{"b":[1,2,)");
    REQUIRE(nested.ok);
    CHECK(nested.closed_truncation);
    CHECK(nested.value.at("a").at("b").size() == 2);
}

TEST_CASE("入参修补:补齐救不回的照拒——悬在冒号上、键没值、括号错配、非 object") {
    CHECK_FALSE(RepairToolArgs(R"({"path":)").ok);
    CHECK_FALSE(RepairToolArgs(R"({"path":"a","conte)").ok);
    CHECK_FALSE(RepairToolArgs(R"({"a":[1,2})").ok);
    CHECK_FALSE(RepairToolArgs(R"([1,2)").ok);  // 补完是数组,不收
    const auto broken = RepairToolArgs("{not valid json");
    CHECK_FALSE(broken.ok);
    CHECK_FALSE(broken.error.empty());
}

TEST_CASE("入参修补:双重编码的字符串套壳拆一层") {
    const auto result = RepairToolArgs(R"("{\"path\":\"a.txt\"}")");
    REQUIRE(result.ok);
    CHECK(result.repaired);
    CHECK(result.value.at("path") == "a.txt");
}

TEST_CASE("入参片段:超 200 字节截断带总长,UTF-8 不劈半个字") {
    std::string raw;
    for (int i = 0; i < 100; ++i) {
        raw += "中";  // 3 字节一个
    }
    const std::string snippet = ToolArgsSnippet(raw);
    CHECK(snippet.find("共 300 字节") != std::string::npos);
    CHECK(snippet.rfind("中", 0) == 0);
    CHECK(ToolArgsSnippet("short") == "short");
    CHECK(IsLengthStopReason("max_tokens"));
    CHECK(IsLengthStopReason("length"));
    CHECK_FALSE(IsLengthStopReason("tool_use"));
}

// ---------------------------------------------------------------------------
// assembler 接线
// ---------------------------------------------------------------------------

TEST_CASE("assembler 入参兜底:尾逗号修补成功,块带 input_repair,不进流式提前档") {
    MessageAssembler assembler;
    const ToolUseBlock block = AssembleOne(assembler, R"({"path":"a.txt",})", "tool_use");
    CHECK(block.input_error.empty());
    CHECK_FALSE(block.input_repair.empty());
    CHECK(block.input.at("path") == "a.txt");
    CHECK_FALSE(assembler.has_parse_error());
    CHECK(assembler.completed_tool_uses().empty());
}

TEST_CASE("assembler 入参兜底:严格合法照进流式提前档,不带任何兜底标记") {
    MessageAssembler assembler;
    const ToolUseBlock block = AssembleOne(assembler, R"({"path":"a.txt"})", "max_tokens");
    CHECK(block.input_error.empty());
    CHECK(block.input_repair.empty());
    CHECK(assembler.completed_tool_uses().size() == 1);
}

TEST_CASE("assembler 入参兜底:修不成给 {} 与 invalid_json 人话,含工具名与原文片段") {
    MessageAssembler assembler;
    const ToolUseBlock block = AssembleOne(assembler, "{path: a.txt}", "tool_use");
    CHECK(block.input.is_object());
    CHECK(block.input.empty());
    CHECK(block.input_error_code == kErrToolInputInvalidJson);
    CHECK(block.input_error.find("write_file") != std::string::npos);
    CHECK(block.input_error.find("{path: a.txt}") != std::string::npos);
    CHECK(block.input_error.find("JSON") != std::string::npos);
    CHECK(assembler.has_parse_error());
    CHECK(assembler.completed_tool_uses().empty());
}

TEST_CASE("assembler 入参兜底:非截断收口时补齐救回的照用,标修补") {
    MessageAssembler assembler;
    const ToolUseBlock block = AssembleOne(assembler, R"({"path":"a.txt","content":"abc)", "tool_use");
    CHECK(block.input_error.empty());
    CHECK(block.input.at("content") == "abc");
    CHECK(block.input_repair.find("截断") != std::string::npos);
}

TEST_CASE("assembler 入参兜底:max_tokens 截断的半截入参不补齐,改判截断错误") {
    MessageAssembler assembler;
    const ToolUseBlock block = AssembleOne(assembler, R"({"path":"a.txt","content":"abc)", "max_tokens");
    CHECK(block.input.is_object());
    CHECK(block.input.empty());  // 半截内容不进入参
    CHECK(block.input_repair.empty());
    CHECK(block.input_error_code == kErrToolInputTruncated);
    CHECK(block.input_error.find("截断") != std::string::npos);
    CHECK(block.input_error.find("拆小") != std::string::npos);
    CHECK(assembler.has_parse_error());
}

TEST_CASE("assembler 入参兜底:length 截断且修不成,同样按截断报") {
    MessageAssembler assembler;
    const ToolUseBlock block = AssembleOne(assembler, R"({"path":)", "length");
    CHECK(block.input_error_code == kErrToolInputTruncated);
}

TEST_CASE("assembler 入参兜底:max_tokens 收口但入参仅剥了围栏(结构完整),照常可用") {
    MessageAssembler assembler;
    const ToolUseBlock block = AssembleOne(assembler, "```json\n{\"path\":\"a.txt\"}\n```", "max_tokens");
    CHECK(block.input_error.empty());
    CHECK(block.input.at("path") == "a.txt");
}
