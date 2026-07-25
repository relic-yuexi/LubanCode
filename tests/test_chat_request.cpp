#include <doctest/doctest.h>

#include "api/chat/request.hpp"

using namespace lubancode;

TEST_CASE("Chat request: system、图片、工具调用和工具结果翻成兼容消息") {
    api::Request request;
    request.model = "glm-5.2";
    request.system = "守规矩";
    request.reasoning_effort = "high";
    request.tools.push_back({"read_file", "读文件", nlohmann::json{{"type", "object"}}});

    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"看图"});
    user.content.push_back(api::ImageBlock{"image/png", "AAAA", "x.png", 1, 1});
    request.messages.push_back(user);

    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::TextBlock{"我来读"});
    assistant.content.push_back(api::ToolUseBlock{"call_1", "read_file", nlohmann::json{{"path", "a.cpp"}}});
    request.messages.push_back(assistant);

    api::Message result;
    result.role = api::Role::User;
    result.content.push_back(api::ToolResultBlock{"call_1", "正文", false});
    request.messages.push_back(result);

    const auto body = api::chat::BuildRequestJson(request, nlohmann::json{{"tool_stream", true}});
    CHECK(body["model"] == "glm-5.2");
    CHECK(body["reasoning_effort"] == "high");
    CHECK(body["tool_stream"] == true);
    REQUIRE(body["messages"].size() == 4);
    CHECK(body["messages"][0]["role"] == "system");
    CHECK(body["messages"][1]["content"][1]["type"] == "image_url");
    CHECK(body["messages"][2]["tool_calls"][0]["function"]["name"] == "read_file");
    CHECK(body["messages"][3]["role"] == "tool");
    CHECK(body["tools"][0]["function"]["parameters"]["type"] == "object");
}

TEST_CASE("Chat request: extra_body 最后覆盖内置字段") {
    api::Request request;
    request.model = "m";
    request.extra_body = nlohmann::json{{"max_tokens", 77}};
    const auto body = api::chat::BuildRequestJson(
        request, nlohmann::json{{"max_tokens", 99}, {"stream_options", nullptr}});
    CHECK(body["max_tokens"] == 77);  // 模型 variant 压过 provider 默认
    CHECK(body["stream_options"].is_null());
}
