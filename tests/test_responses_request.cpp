// 中立 Request -> Responses JSON 的映射逐项验证:BuildRequestJson 是纯函数,
// 不碰网络,这里直接调用、断言拼出来的 JSON 长什么样。

#include <doctest/doctest.h>

#include "api/responses/request.hpp"
#include "api/types.hpp"

using namespace lubancode::api;
using lubancode::api::responses::BuildRequestJson;

TEST_CASE("system 映射成 instructions") {
    Request request;
    request.model = "MiniMax-M3";
    request.system = "你是一个有用的助手。";

    const auto body = BuildRequestJson(request);

    CHECK(body.at("instructions") == "你是一个有用的助手。");
    CHECK(body.at("model") == "MiniMax-M3");
}

TEST_CASE("system 为空时不写 instructions 字段") {
    Request request;
    request.model = "MiniMax-M3";

    const auto body = BuildRequestJson(request);

    CHECK_FALSE(body.contains("instructions"));
}

TEST_CASE("stream 恒为 true,store 恒为 false") {
    Request request;
    const auto body = BuildRequestJson(request);

    CHECK(body.at("stream") == true);
    CHECK(body.at("store") == false);
}

TEST_CASE("max_tokens 映射成 max_output_tokens") {
    Request request;
    request.max_tokens = 2048;

    const auto body = BuildRequestJson(request);

    CHECK(body.at("max_output_tokens") == 2048);
    CHECK_FALSE(body.contains("max_tokens"));
}

TEST_CASE("user 的纯文本消息映射成 message item,content 用 input_text") {
    Request request;
    Message user_msg;
    user_msg.role = Role::User;
    user_msg.content.push_back(TextBlock{"你好"});
    request.messages.push_back(user_msg);

    const auto body = BuildRequestJson(request);

    REQUIRE(body.at("input").size() == 1);
    const auto& item = body.at("input")[0];
    CHECK(item.at("type") == "message");
    CHECK(item.at("role") == "user");
    REQUIRE(item.at("content").size() == 1);
    CHECK(item.at("content")[0].at("type") == "input_text");
    CHECK(item.at("content")[0].at("text") == "你好");
}

TEST_CASE("assistant 的纯文本消息映射成 message item,content 用 output_text") {
    Request request;
    Message assistant_msg;
    assistant_msg.role = Role::Assistant;
    assistant_msg.content.push_back(TextBlock{"我来帮你查一下。"});
    request.messages.push_back(assistant_msg);

    const auto body = BuildRequestJson(request);

    REQUIRE(body.at("input").size() == 1);
    const auto& item = body.at("input")[0];
    CHECK(item.at("type") == "message");
    CHECK(item.at("role") == "assistant");
    CHECK(item.at("content")[0].at("type") == "output_text");
    CHECK(item.at("content")[0].at("text") == "我来帮你查一下。");
}

TEST_CASE("assistant 的 tool_use 块映射成 function_call item,call_id 原样带上 id") {
    Request request;
    Message assistant_msg;
    assistant_msg.role = Role::Assistant;
    assistant_msg.content.push_back(ToolUseBlock{"call_abc123", "read_file", nlohmann::json{{"path", "vcpkg.json"}}});
    request.messages.push_back(assistant_msg);

    const auto body = BuildRequestJson(request);

    REQUIRE(body.at("input").size() == 1);
    const auto& item = body.at("input")[0];
    CHECK(item.at("type") == "function_call");
    CHECK(item.at("call_id") == "call_abc123");
    CHECK(item.at("name") == "read_file");
    // arguments 是 JSON 字符串,不是内嵌对象
    REQUIRE(item.at("arguments").is_string());
    const auto parsed_args = nlohmann::json::parse(item.at("arguments").get<std::string>());
    CHECK(parsed_args.at("path") == "vcpkg.json");
}

TEST_CASE("user 的 tool_result 块映射成 function_call_output item,call_id 对应 tool_use_id") {
    Request request;
    Message user_msg;
    user_msg.role = Role::User;
    user_msg.content.push_back(ToolResultBlock{"call_abc123", "依赖: fmt, cpr", false});
    request.messages.push_back(user_msg);

    const auto body = BuildRequestJson(request);

    REQUIRE(body.at("input").size() == 1);
    const auto& item = body.at("input")[0];
    CHECK(item.at("type") == "function_call_output");
    CHECK(item.at("call_id") == "call_abc123");
    CHECK(item.at("output") == "依赖: fmt, cpr");
}

TEST_CASE("一条 assistant 消息里 text + tool_use 混合,拆成两个 input item") {
    Request request;
    Message assistant_msg;
    assistant_msg.role = Role::Assistant;
    assistant_msg.content.push_back(TextBlock{"我来查一下。"});
    assistant_msg.content.push_back(ToolUseBlock{"call_1", "get_weather", nlohmann::json::object()});
    request.messages.push_back(assistant_msg);

    const auto body = BuildRequestJson(request);

    REQUIRE(body.at("input").size() == 2);
    CHECK(body.at("input")[0].at("type") == "message");
    CHECK(body.at("input")[1].at("type") == "function_call");
}

TEST_CASE("完整的 tool_use -> tool_result 一来一回,历史 messages 顺序不错") {
    Request request;

    Message user_ask;
    user_ask.role = Role::User;
    user_ask.content.push_back(TextBlock{"读一下 vcpkg.json"});
    request.messages.push_back(user_ask);

    Message assistant_call;
    assistant_call.role = Role::Assistant;
    assistant_call.content.push_back(ToolUseBlock{"call_xyz", "read_file", nlohmann::json{{"path", "vcpkg.json"}}});
    request.messages.push_back(assistant_call);

    Message tool_result;
    tool_result.role = Role::User;
    tool_result.content.push_back(ToolResultBlock{"call_xyz", "{\"dependencies\":[\"fmt\"]}", false});
    request.messages.push_back(tool_result);

    const auto body = BuildRequestJson(request);

    REQUIRE(body.at("input").size() == 3);
    CHECK(body.at("input")[0].at("type") == "message");
    CHECK(body.at("input")[1].at("type") == "function_call");
    CHECK(body.at("input")[1].at("call_id") == "call_xyz");
    CHECK(body.at("input")[2].at("type") == "function_call_output");
    CHECK(body.at("input")[2].at("call_id") == "call_xyz");
}

TEST_CASE("工具定义映射成 Responses 的 function 类型工具数组") {
    Request request;
    ToolDefinition def;
    def.name = "read_file";
    def.description = "读一个文件的内容";
    def.input_schema = nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json{{"path", nlohmann::json{{"type", "string"}}}}},
        {"required", nlohmann::json::array({"path"})},
    };
    request.tools.push_back(def);

    const auto body = BuildRequestJson(request);

    REQUIRE(body.at("tools").size() == 1);
    const auto& tool = body.at("tools")[0];
    CHECK(tool.at("type") == "function");
    CHECK(tool.at("name") == "read_file");
    CHECK(tool.at("description") == "读一个文件的内容");
    CHECK(tool.at("parameters") == def.input_schema);
}

TEST_CASE("没有工具时不写 tools 字段") {
    Request request;
    const auto body = BuildRequestJson(request);
    CHECK_FALSE(body.contains("tools"));
}
