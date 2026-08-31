// google-generate-content wire 的请求体构建:system/历史/工具/思考档位/
// extra_body 透传,全部对着 BuildRequestJson 这只纯函数钉。

#include <doctest/doctest.h>

#include "api/gemini/request.hpp"
#include <memory>
#include <set>

#include "tools/registry.hpp"
#include "tools/tool_search.hpp"

using namespace lubancode;

namespace {

// 一份带齐各类内容块的三轮历史:assistant 文本+工具调用、user 工具结果,
// 供多个用例共用。
api::Request SampleConversation() {
    api::Request request;
    request.model = "gemini-2.5-pro";
    request.system = "守规矩";
    request.max_tokens = 1024;
    request.tools.push_back({"read_file", "读文件", nlohmann::json{{"type", "object"}}});

    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"看图"});
    user.content.push_back(api::ImageBlock{"image/png", "AAAA", "x.png", 1, 1});
    request.messages.push_back(user);

    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"想一想", "sig"});
    assistant.content.push_back(api::TextBlock{"我来读"});
    assistant.content.push_back(api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a.cpp"}}});
    request.messages.push_back(assistant);

    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{"call_1", "正文", false});
    request.messages.push_back(result);
    return request;
}

}  // namespace

TEST_CASE("Gemini request: system 走 systemInstruction,不进 contents") {
    api::Request request;
    request.model = "gemini-2.5-pro";
    request.system = "你是助手";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"你好"});
    request.messages.push_back(user);

    const auto body = api::gemini::BuildRequestJson(request);
    CHECK(body["systemInstruction"]["parts"][0]["text"] == "你是助手");
    REQUIRE(body["contents"].size() == 1);
    CHECK(body["contents"][0]["role"] == "user");
    CHECK(body["contents"][0]["parts"][0]["text"] == "你好");
}

TEST_CASE("Gemini request: 历史翻 contents,工具调用/结果各成一条") {
    const auto body = api::gemini::BuildRequestJson(SampleConversation());
    REQUIRE(body["contents"].size() == 4);

    // user 文本+图片同框成一条。
    CHECK(body["contents"][0]["role"] == "user");
    CHECK(body["contents"][0]["parts"][0]["text"] == "看图");
    CHECK(body["contents"][0]["parts"][1]["inlineData"]["mimeType"] == "image/png");
    CHECK(body["contents"][0]["parts"][1]["inlineData"]["data"] == "AAAA");

    // assistant:文本一条(思考块不回传),工具调用单独一条 role=model。
    CHECK(body["contents"][1]["role"] == "model");
    CHECK(body["contents"][1]["parts"][0]["text"] == "我来读");
    CHECK(body["contents"][2]["role"] == "model");
    CHECK(body["contents"][2]["parts"][0]["functionCall"]["name"] == "read_file");
    CHECK(body["contents"][2]["parts"][0]["functionCall"]["args"]["path"] == "a.cpp");

    // 工具结果:role=user,functionResponse 对回函数名(不是 tool_use_id),
    // 字符串正文包成 {"result": ...}。
    CHECK(body["contents"][3]["role"] == "user");
    CHECK(body["contents"][3]["parts"][0]["functionResponse"]["name"] == "read_file");
    CHECK(body["contents"][3]["parts"][0]["functionResponse"]["response"]["result"] == "正文");
}

TEST_CASE("Gemini request: 工具结果正文是 JSON object 就原样用,is_error 换 error 键") {
    api::Request request;
    request.model = "m";
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"c1", "run", nlohmann::json{{"cmd", "ls"}}});
    assistant.content.push_back(api::ToolUseBlock{"c2", "run", nlohmann::json{{"cmd", "rm"}}});
    request.messages.push_back(assistant);

    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{"c1", R"({"rows":[1,2]})", false});
    result.content.push_back(api::ToolResultBlock{"c2", "炸了", true});
    request.messages.push_back(result);

    const auto body = api::gemini::BuildRequestJson(request);
    // 两条 assistant 工具调用 + 两条 user 工具结果,共 4 条 contents。
    REQUIRE(body["contents"].size() == 4);
    const auto& ok = body["contents"][2]["parts"][0]["functionResponse"];
    CHECK(ok["name"] == "run");
    CHECK(ok["response"]["rows"] == nlohmann::json::array({1, 2}));
    const auto& failed = body["contents"][3]["parts"][0]["functionResponse"];
    CHECK(failed["name"] == "run");
    CHECK(failed["response"]["error"] == "炸了");
}

TEST_CASE("Gemini request: 富工具结果——structuredContent 走原生对象,图片块降级投影文本(MCP 富结果单 P0.6)") {
    api::Request request;
    request.model = "m";
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"c1", "browser_screenshot", nlohmann::json::object()});
    request.messages.push_back(assistant);

    api::Message result_message;
    result_message.role = api::Role::User;
    api::ToolResultBlock rich;
    rich.tool_use_id = "c1";
    rich.is_error = false;
    rich.content = "[图片 art-00112233.png image/png 640x480 2048字节 artifact=mcp-artifacts/art-00112233.png]";
    lubancode::tools::ImageContent image;
    image.mime_type = "image/png";
    image.width = 640;
    image.height = 480;
    image.bytes = 2048;
    image.artifact.filename = "art-00112233.png";
    image.artifact.path = "mcp-artifacts/art-00112233.png";
    image.artifact.stored = true;
    rich.blocks.push_back(std::move(image));
    rich.structured_content = nlohmann::json{{"sha256", "ab"}, {"full_page", false}};
    result_message.content.push_back(rich);
    request.messages.push_back(result_message);

    const auto body = api::gemini::BuildRequestJson(request);
    REQUIRE(body["contents"].size() == 2);
    const auto& response = body["contents"][1]["parts"][0]["functionResponse"];
    // structuredContent 原生对象,不绕投影再 parse 一圈。
    CHECK(response["response"]["sha256"] == "ab");
    CHECK(response["response"]["full_page"] == false);
    // 图片本体不假定 inlineData:投影文本里是 artifact 短句(明确降级)。
    CHECK(response.dump().find("artifact=mcp-artifacts") == std::string::npos);
}

namespace {

// 带重灌图片的工具结果消息(gemini 用例共用)。
api::Message ImageResultMessage() {
    api::Message result_message;
    result_message.role = api::Role::User;
    api::ToolResultBlock rich;
    rich.tool_use_id = "c1";
    rich.content = "已截图 640x480";
    lubancode::tools::ImageContent image;
    image.mime_type = "image/png";
    image.width = 640;
    image.height = 480;
    image.bytes = 9;
    image.wire_base64 = "iVBORw0KGgo=";
    image.artifact.filename = "art-00112233.png";
    image.artifact.path = "mcp-artifacts/art-00112233.png";
    image.artifact.stored = true;
    rich.blocks.push_back(std::move(image));
    result_message.content.push_back(rich);
    return result_message;
}

}  // namespace

TEST_CASE("工具结果图片回喂: Gemini 3+ 上 functionResponse.parts 嵌 inlineData 真发(工具结果图片回喂单)") {
    for (const char* model : {"gemini-3-pro-preview", "gemini-3.5-flash", "models/gemini-3.1-pro"}) {
        api::Request request;
        request.model = model;
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::ToolUseBlock{"c1", "browser_screenshot", nlohmann::json::object()});
        request.messages.push_back(assistant);
        request.messages.push_back(ImageResultMessage());

        const auto body = api::gemini::BuildRequestJson(request);
        const auto& response = body["contents"][1]["parts"][0]["functionResponse"];
        // parts 数组:inlineData 带 displayName/mimeType/base64 data。
        REQUIRE(response.contains("parts"));
        REQUIRE(response["parts"].size() == 1);
        const auto& inline_data = response["parts"][0]["inlineData"];
        CHECK(inline_data["displayName"] == "art-00112233.png");
        CHECK(inline_data["mimeType"] == "image/png");
        CHECK(inline_data["data"] == "iVBORw0KGgo=");
        // 投影文本照旧进 response.result,不带降级附注。
        CHECK(response["response"]["result"] == "已截图 640x480");
    }
}

TEST_CASE("工具结果图片回喂: Gemini 3 之前/认不出的模型名明降级,不带 parts") {
    for (const char* model : {"gemini-2.5-pro", "gemini-flash-latest", "some-other-model"}) {
        api::Request request;
        request.model = model;
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::ToolUseBlock{"c1", "browser_screenshot", nlohmann::json::object()});
        request.messages.push_back(assistant);
        request.messages.push_back(ImageResultMessage());

        const auto body = api::gemini::BuildRequestJson(request);
        const auto& response = body["contents"][1]["parts"][0]["functionResponse"];
        // 无 parts(多模态 functionResponse 是 Gemini 3+ 的事),字节不出门。
        CHECK_FALSE(response.contains("parts"));
        CHECK(body.dump().find("iVBORw0KGgo=") == std::string::npos);
        // 投影后追明降级附注:点名张数与落盘文件名。
        const std::string result_text = response["response"]["result"].get<std::string>();
        CHECK(result_text.find("已截图 640x480") != std::string::npos);
        CHECK(result_text.find("[wire 降级] 该 wire 不支持工具结果图片,1 张未随行,字节已存盘: art-00112233.png") !=
              std::string::npos);
    }
}

TEST_CASE("工具结果图片回喂: Gemini 多模态 MIME 表外(png/jpeg/webp 之外)的图不硬发") {
    api::Request request;
    request.model = "gemini-3-pro";
    api::Message result_message = ImageResultMessage();
    auto& rich = std::get<api::ToolResultBlock>(result_message.content[0]);
    std::get<lubancode::tools::ImageContent>(rich.blocks[0]).mime_type = "image/gif";  // 文档表外
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ToolUseBlock{"c1", "shot", nlohmann::json::object()});
    request.messages.push_back(assistant);
    request.messages.push_back(result_message);

    const auto body = api::gemini::BuildRequestJson(request);
    CHECK(body.dump().find("inlineData") == std::string::npos);
}

TEST_CASE("Gemini request: 工具定义收进 functionDeclarations") {
    const auto body = api::gemini::BuildRequestJson(SampleConversation());
    REQUIRE(body["tools"].size() == 1);
    REQUIRE(body["tools"][0]["functionDeclarations"].size() == 1);
    const auto& decl = body["tools"][0]["functionDeclarations"][0];
    CHECK(decl["name"] == "read_file");
    CHECK(decl["description"] == "读文件");
    CHECK(decl["parameters"]["type"] == "object");
}

TEST_CASE("Gemini request: max_tokens 与思考档位进 generationConfig") {
    const auto body = api::gemini::BuildRequestJson(SampleConversation());
    CHECK(body["generationConfig"]["maxOutputTokens"] == 1024);
    // reasoning_effort 没写:不带 thinkingConfig(字段整个缺席,不偷偷默认)。
    CHECK_FALSE(body["generationConfig"].contains("thinkingConfig"));

    api::Request on = SampleConversation();
    on.reasoning_effort = "high";
    CHECK(api::gemini::BuildRequestJson(on)["generationConfig"]["thinkingConfig"]["includeThoughts"] == true);

    api::Request off = SampleConversation();
    off.reasoning_effort = "none";
    CHECK(api::gemini::BuildRequestJson(off)["generationConfig"]["thinkingConfig"]["includeThoughts"] == false);
}

TEST_CASE("Gemini request: effort/budget 档案分别写 thinkingLevel/thinkingBudget") {
    api::Request effort = SampleConversation();
    effort.reasoning_effort = "high";
    effort.reasoning.supports_effort = true;
    effort.reasoning.wire_dialect = "effort";
    const auto effort_body = api::gemini::BuildRequestJson(effort);
    CHECK(effort_body["generationConfig"]["thinkingConfig"]["thinkingLevel"] == "high");

    api::Request budget = SampleConversation();
    budget.reasoning_effort = "auto";
    budget.reasoning.supports_toggle = true;
    budget.reasoning.budget_min = 128;
    budget.reasoning.budget_max = 32768;
    budget.reasoning.wire_dialect = "budget";
    const auto budget_body = api::gemini::BuildRequestJson(budget);
    CHECK(budget_body["generationConfig"]["thinkingConfig"]["thinkingBudget"] == -1);
    CHECK(budget.extra_body.empty());
}

TEST_CASE("Gemini request: max_tokens 未设且档位为空时整个不带 generationConfig") {
    api::Request request;
    request.model = "m";
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"问"});
    request.messages.push_back(user);
    const auto body = api::gemini::BuildRequestJson(request);
    CHECK_FALSE(body.contains("generationConfig"));
}

TEST_CASE("Gemini request: extra_body 顶层覆盖,generationConfig 深一层合并") {
    api::Request request = SampleConversation();
    request.reasoning_effort = "high";

    const nlohmann::json extra = nlohmann::json{
        {"safetySettings", nlohmann::json::array()},
        {"generationConfig", nlohmann::json{{"thinkingConfig",
                                             nlohmann::json{{"includeThoughts", true}, {"thinkingBudget", 8192}}},
                                            {"temperature", 0.2}}},
    };
    const auto body = api::gemini::BuildRequestJson(request, extra);
    // 顶层照抄。
    CHECK(body.contains("safetySettings"));
    // generationConfig:内置 maxOutputTokens 保得住,extra 的子键叠上来。
    CHECK(body["generationConfig"]["maxOutputTokens"] == 1024);
    CHECK(body["generationConfig"]["temperature"] == 0.2);
    CHECK(body["generationConfig"]["thinkingConfig"]["thinkingBudget"] == 8192);
    CHECK(body["generationConfig"]["thinkingConfig"]["includeThoughts"] == true);
}

TEST_CASE("Gemini request: 模型 variant 的 extra_body 压过 provider 级同名子键") {
    api::Request request = SampleConversation();
    request.extra_body = nlohmann::json{{"generationConfig", nlohmann::json{{"maxOutputTokens", 2048}}}};
    const nlohmann::json provider_extra =
        nlohmann::json{{"generationConfig", nlohmann::json{{"maxOutputTokens", 512}, {"temperature", 0.5}}}};
    const auto body = api::gemini::BuildRequestJson(request, provider_extra);
    CHECK(body["generationConfig"]["maxOutputTokens"] == 2048);  // variant 压过 provider
    CHECK(body["generationConfig"]["temperature"] == 0.5);       // provider 的别的子键还在
}

TEST_CASE("Gemini request: StreamUrl 拼 v1beta 端点,剥 models/ 前缀与尾斜杠") {
    CHECK(api::gemini::StreamUrl("https://generativelanguage.googleapis.com", "gemini-2.5-pro") ==
          "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-pro:streamGenerateContent?alt=sse");
    CHECK(api::gemini::StreamUrl("https://generativelanguage.googleapis.com", "models/gemini-2.5-flash") ==
          "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:streamGenerateContent?alt=sse");
    CHECK(api::gemini::StreamUrl("https://proxy.example.test", "gemini-3-pro/") ==
          "https://proxy.example.test/v1beta/models/gemini-3-pro:streamGenerateContent?alt=sse");
}

// 工具 schema 归一化:空 schema 出门前兑成最小合法壳(缘起与四家共用的
// 兑法见 api/types.hpp 的 ToolSchemaForWire 注释)。
TEST_CASE("Gemini request: 工具空 schema 兑成 type=object,合规的原样放行") {
    api::Request request;
    request.model = "gemini-2.5-pro";
    request.tools.push_back({"list_sessions", "列会话", nlohmann::json::object()});
    const auto good = nlohmann::json{{"type", "object"},
                                     {"properties", {{"path", {{"type", "string"}}}}},
                                     {"required", nlohmann::json::array({"path"})}};
    request.tools.push_back({"read_file", "读文件", good});

    const auto body = api::gemini::BuildRequestJson(request);
    const auto& decls = body["tools"][0]["functionDeclarations"];
    CHECK(decls[0]["parameters"]["type"] == "object");
    CHECK(decls[0]["parameters"]["properties"].is_object());
    CHECK(decls[1]["parameters"] == good);
}


TEST_CASE("ModelImageBlock 重放:翻 text part 短标记,不带 base64") {
    api::Request request;
    request.model = "gemini-2.5-pro";
    api::ModelImageBlock ref;
    ref.id = "ig_1";
    ref.filename = "img-abcd12.png";
    ref.path = "images/img-abcd12.png";
    ref.width = 512;
    ref.height = 512;
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(ref);
    request.messages.push_back(assistant);

    const auto body = api::gemini::BuildRequestJson(request, nlohmann::json::object());
    const std::string dumped = body.dump();
    CHECK(dumped.find("[模型已生成图片: img-abcd12.png (512x512)]") != std::string::npos);
    CHECK(dumped.find("images/img-abcd12.png") == std::string::npos);
    CHECK(dumped.find("inlineData") == std::string::npos);
}

// 动态工具 PromptCache 守恒单 P1:Gemini wire 的 proxy_reference replay。
// 顶层只见固定的 tool_search + tool_invoke;functionCall.args 是原生对象,
// 往返逐字段无损。
TEST_CASE("Gemini proxy replay: tool_search+tool_invoke 定义恒在,代理调用 args 无损") {
    tools::ToolRegistry dummy_registry;
    auto loaded = std::make_shared<std::set<std::string>>();
    tools::ToolSearchTool search(dummy_registry, loaded);
    tools::ToolInvokeTool invoke;

    api::Request request;
    request.model = "gemini-test";
    request.tools.push_back({"tool_search", search.description(), search.input_schema()});
    request.tools.push_back({"tool_invoke", invoke.description(), invoke.input_schema()});

    api::Message search_result;
    search_result.role = api::Role::User;
    const std::string discovery_json =
        nlohmann::json{{"catalog_revision", "sha256:abc"},
                       {"matches", nlohmann::json::array({nlohmann::json{
                                       {"tool_ref", "dt_0123456789_1"},
                                       {"name", "mcp__github__search_issues"},
                                       {"schema_digest", "sha256:def"},
                                       {"input_schema", nlohmann::json{{"type", "object"}}}}})}}
            .dump();
    search_result.content.push_back(api::ToolResultBlock{"call_search", discovery_json, false});

    api::Message proxy_call;
    proxy_call.role = api::Role::Assistant;
    proxy_call.content.push_back(api::ToolUseBlock{
        "call_invoke", "tool_invoke",
        nlohmann::json{{"tool_ref", "dt_0123456789_1"},
                       {"arguments", nlohmann::json{{"query", "repo:lubancode cache"}, {"per_page", 50}}}}});
    api::Message proxy_result;
    proxy_result.role = api::Role::User;
    proxy_result.content.push_back(api::ToolResultBlock{"call_invoke", "issue #42", false});
    request.messages.push_back(search_result);
    request.messages.push_back(proxy_call);
    request.messages.push_back(proxy_result);

    const auto body = api::gemini::BuildRequestJson(request, nlohmann::json::object());
    REQUIRE(body["tools"].size() == 1);
    const auto& declarations = body["tools"][0]["functionDeclarations"];
    REQUIRE(declarations.size() == 2);
    CHECK(declarations[0]["name"] == "tool_search");
    CHECK(declarations[1]["name"] == "tool_invoke");
    CHECK(declarations[1]["parameters"]["additionalProperties"] == false);

    bool saw_discovery = false;
    bool saw_proxy = false;
    for (const auto& content : body["contents"]) {
        for (const auto& part : content["parts"]) {
            if (part.contains("functionCall")) {
                const auto& call = part["functionCall"];
                if (call["name"] == "tool_invoke") {
                    saw_proxy = true;
                    const auto& args = call["args"];
                    CHECK(args["tool_ref"] == "dt_0123456789_1");
                    CHECK(args["arguments"]["query"] == "repo:lubancode cache");
                    CHECK(args["arguments"]["per_page"] == 50);
                }
            } else if (part.contains("functionResponse")) {
                const auto& response = part["functionResponse"];
                if (response.value("name", std::string()) == "tool_search" ||
                    response.dump().find("call_search") != std::string::npos) {
                    saw_discovery = response.dump().find("dt_0123456789_1") != std::string::npos;
                }
            }
        }
    }
    CHECK(saw_proxy);
    CHECK(saw_discovery);
}
