// 中立 Request -> Responses JSON 的映射逐项验证:BuildRequestJson 是纯函数,
// 不碰网络,这里直接调用、断言拼出来的 JSON 长什么样。

#include <doctest/doctest.h>

#include "api/responses/client.hpp"
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

TEST_CASE("reasoning_effort 为空串时不写 reasoning 字段") {
    Request request;
    request.reasoning_effort = "";
    const auto body = BuildRequestJson(request);
    CHECK_FALSE(body.contains("reasoning"));
}

TEST_CASE("reasoning_effort 非空时映射成 reasoning.effort") {
    Request request;
    request.reasoning_effort = "high";
    const auto body = BuildRequestJson(request);
    REQUIRE(body.contains("reasoning"));
    CHECK(body.at("reasoning").at("effort") == "high");
}

TEST_CASE("reasoning_effort 支持 none/low/medium/high 四档,原样透传不做限制") {
    for (const std::string level : {"none", "low", "medium", "high"}) {  // 按值:初始化列表元素是 const char*,引用绑临时会招 -Wrange-loop-construct
        Request request;
        request.reasoning_effort = level;
        const auto body = BuildRequestJson(request);
        CHECK(body.at("reasoning").at("effort") == level);
    }
}

// M10:档位放开成任意字符串——responses 这边压根不认字典,服务商定的档位
// 名字原样递过去,哪怕是 anthropic 那套映射表里没有的名字(xhigh)、或者
// 两边协议都没听过的生造词,都不在这层拦。
TEST_CASE("reasoning_effort 是任意字符串(含 anthropic 专属档位名、生造词)都原样透传") {
    for (const std::string level : {"xhigh", "max", "ultra-think-9000"}) {  // 同上,按值
        Request request;
        request.reasoning_effort = level;
        const auto body = BuildRequestJson(request);
        REQUIRE(body.contains("reasoning"));
        CHECK(body.at("reasoning").at("effort") == level);
    }
}

// M12(原生 web_search):provider 开了 native_web_search 时,tools 数组里
// 要追加 {"type":"web_search"} 一项——服务端原生联网搜索声明,跟本地
// function 工具是两码事。

TEST_CASE("native_web_search=false(默认)时不写 web_search 声明,行为跟现状一致") {
    Request request;
    const auto body = BuildRequestJson(request);
    CHECK_FALSE(body.contains("tools"));

    const auto body_explicit_false = BuildRequestJson(request, /*native_web_search=*/false);
    CHECK_FALSE(body_explicit_false.contains("tools"));
}

TEST_CASE("native_web_search=true 且本地工具表为空时,tools 数组只有 web_search 一项") {
    Request request;
    const auto body = BuildRequestJson(request, /*native_web_search=*/true);
    REQUIRE(body.contains("tools"));
    REQUIRE(body.at("tools").size() == 1);
    CHECK(body.at("tools")[0].at("type") == "web_search");
    CHECK_FALSE(body.at("tools")[0].contains("name"));
}

TEST_CASE("native_web_search=true 且本地工具表非空时,web_search 追加在本地工具后面") {
    Request request;
    ToolDefinition def;
    def.name = "read_file";
    def.description = "读一个文件的内容";
    def.input_schema = nlohmann::json{{"type", "object"}};
    request.tools.push_back(def);

    const auto body = BuildRequestJson(request, /*native_web_search=*/true);
    REQUIRE(body.contains("tools"));
    REQUIRE(body.at("tools").size() == 2);
    CHECK(body.at("tools")[0].at("type") == "function");
    CHECK(body.at("tools")[0].at("name") == "read_file");
    CHECK(body.at("tools")[1].at("type") == "web_search");
}

// ---------------------------------------------------------------------------
// extra_body:同 anthropic 那边的规则,merge 点在所有内置逻辑拼完之后、
// 返回之前,浅合并、键冲突整段覆盖,不做深合并。
// ---------------------------------------------------------------------------

TEST_CASE("extra_body 缺省(默认空 object)时,请求体跟不传这个参数一模一样") {
    Request request;
    request.reasoning_effort = "medium";
    const auto body_default = BuildRequestJson(request);
    const auto body_explicit_empty = BuildRequestJson(request, /*native_web_search=*/false, nlohmann::json::object());
    CHECK(body_default == body_explicit_empty);
}

TEST_CASE("extra_body 里的新键原样加到请求体顶层") {
    Request request;
    const auto extra_body = nlohmann::json::parse(R"({"reasoning_effort":"max"})");
    const auto body = BuildRequestJson(request, /*native_web_search=*/false, extra_body);
    CHECK(body.at("reasoning_effort") == "max");
}

TEST_CASE("extra_body 跟内置字段(reasoning)同名时,extra_body 的值整个覆盖内置算出来的值") {
    Request request;
    request.reasoning_effort = "high";  // 内置逻辑会算出 reasoning.effort=high
    const auto extra_body = nlohmann::json::parse(R"({"reasoning":{"effort":"max"}})");
    const auto body = BuildRequestJson(request, /*native_web_search=*/false, extra_body);
    CHECK(body.at("reasoning").at("effort") == "max");
}

TEST_CASE("extra_body 也能覆盖 native_web_search 拼出来的 tools 数组") {
    Request request;
    const auto extra_body = nlohmann::json::parse(R"({"tools":[]})");
    const auto body = BuildRequestJson(request, /*native_web_search=*/true, extra_body);
    CHECK(body.at("tools").empty());  // 内置拼出来的 web_search 声明被整段覆盖掉了
}

// ---------------------------------------------------------------------------
// ApplyExtraHeaders(responses 协议自己那份,逻辑跟 anthropic 那份一样,各自
// 小巧不共用):同名覆盖(含 Authorization),不同名追加。
// ---------------------------------------------------------------------------

TEST_CASE("responses::ApplyExtraHeaders: 为空时基础头原样不变,新头追加,同名覆盖") {
    const std::map<std::string, std::string> base{{"Content-Type", "application/json"},
                                                    {"Authorization", "Bearer old-token"}};

    const auto unchanged = lubancode::api::responses::ApplyExtraHeaders(base, {});
    CHECK(unchanged == base);

    const auto appended = lubancode::api::responses::ApplyExtraHeaders(base, {{"X-Api-Version", "2024-06-01"}});
    CHECK(appended.at("Content-Type") == "application/json");
    CHECK(appended.at("X-Api-Version") == "2024-06-01");

    const auto overridden = lubancode::api::responses::ApplyExtraHeaders(base, {{"Authorization", "Bearer new-token"}});
    CHECK(overridden.at("Authorization") == "Bearer new-token");
    CHECK(overridden.at("Content-Type") == "application/json");
}

TEST_CASE("用户图片映射成 Responses input_image data URL") {
    Request request;
    Message user;
    user.role = Role::User;
    user.content.push_back(TextBlock{"看看报错"});
    user.content.push_back(ImageBlock{"image/png", "aGVsbG8=", "error.png", 640, 480});
    request.messages.push_back(user);

    const auto body = BuildRequestJson(request);
    REQUIRE(body.at("input").size() == 1);
    const auto& content = body.at("input").at(0).at("content");
    REQUIRE(content.size() == 2);
    CHECK(content.at(0).at("type") == "input_text");
    CHECK(content.at(1).at("type") == "input_image");
    CHECK(content.at(1).at("image_url") == "data:image/png;base64,aGVsbG8=");
}
