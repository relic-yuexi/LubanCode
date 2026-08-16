// api::ListModels 的 JSON 解析拆成两个纯函数(ParseAnthropicModelsResponse /
// ParseResponsesModelsResponse),这里只测这两个纯函数——不真发网络请求。

#include <doctest/doctest.h>

#include "api/models.hpp"

using namespace lubancode;

// ---------------------------------------------------------------------------
// ParseAnthropicModelsResponse:{"data":[{"id","display_name",...}]}
// ---------------------------------------------------------------------------

TEST_CASE("ParseAnthropicModelsResponse: 正常响应,带 display_name") {
    const std::string json = R"({
        "data": [
            {"id": "MiniMax-M3", "display_name": "MiniMax M3", "type": "model"},
            {"id": "MiniMax-M2.7", "display_name": "MiniMax M2.7"}
        ]
    })";

    const auto result = api::ParseAnthropicModelsResponse(json);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);
    CHECK((*result)[0].id == "MiniMax-M3");
    CHECK((*result)[0].display_name == "MiniMax M3");
    CHECK((*result)[1].id == "MiniMax-M2.7");
    CHECK((*result)[1].display_name == "MiniMax M2.7");
}

TEST_CASE("ParseAnthropicModelsResponse: 没有 display_name 字段时用 id 兜底") {
    const std::string json = R"({"data": [{"id": "only-id-model"}]})";

    const auto result = api::ParseAnthropicModelsResponse(json);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 1);
    CHECK((*result)[0].id == "only-id-model");
    CHECK((*result)[0].display_name == "only-id-model");
}

TEST_CASE("ParseAnthropicModelsResponse: data 是空数组,返回空列表(不是错误)") {
    const auto result = api::ParseAnthropicModelsResponse(R"({"data": []})");
    REQUIRE(result.has_value());
    CHECK(result->empty());
}

TEST_CASE("ParseAnthropicModelsResponse: 坏 JSON 报错") {
    const auto result = api::ParseAnthropicModelsResponse("{ not valid json ,, }");
    REQUIRE_FALSE(result.has_value());
    CHECK_FALSE(result.error().empty());
}

TEST_CASE("ParseAnthropicModelsResponse: 没有 data 字段报错") {
    const auto result = api::ParseAnthropicModelsResponse(R"({"models": []})");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseAnthropicModelsResponse: data 里有一条脏数据(没有 id),跳过它,别的照常解析") {
    const std::string json = R"({
        "data": [
            {"id": "good-model"},
            {"display_name": "缺了 id 的脏数据"},
            {"id": "another-good-model"}
        ]
    })";
    const auto result = api::ParseAnthropicModelsResponse(json);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);
    CHECK((*result)[0].id == "good-model");
    CHECK((*result)[1].id == "another-good-model");
}

// ---------------------------------------------------------------------------
// ParseResponsesModelsResponse:{"object":"list","data":[{"id",...}]}
// ---------------------------------------------------------------------------

TEST_CASE("ParseResponsesModelsResponse: 正常响应,没有 display_name,用 id 兜底") {
    const std::string json = R"({
        "object": "list",
        "data": [
            {"id": "MiniMax-M3", "object": "model"},
            {"id": "MiniMax-M2.5", "object": "model"}
        ]
    })";

    const auto result = api::ParseResponsesModelsResponse(json);
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 2);
    CHECK((*result)[0].id == "MiniMax-M3");
    CHECK((*result)[0].display_name == "MiniMax-M3");
    CHECK((*result)[1].id == "MiniMax-M2.5");
}

TEST_CASE("ParseResponsesModelsResponse: 空列表") {
    const auto result = api::ParseResponsesModelsResponse(R"({"object": "list", "data": []})");
    REQUIRE(result.has_value());
    CHECK(result->empty());
}

TEST_CASE("ParseResponsesModelsResponse: 坏 JSON 报错") {
    const auto result = api::ParseResponsesModelsResponse("not json at all");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseResponsesModelsResponse: 没有 data 字段报错") {
    const auto result = api::ParseResponsesModelsResponse(R"({"object": "list"})");
    REQUIRE_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// 向导重排单:模型探测 URL 的拼法与鉴权头三态。
// ---------------------------------------------------------------------------

TEST_CASE("ModelsUrl: anthropic 补 /v1/models,已带 /v1 不重复;OpenAI 系补 /models") {
    // anthropic:裸地址补 /v1/models。
    CHECK(api::ModelsUrl(config::Wire::Anthropic, "https://api.anthropic.com") ==
          "https://api.anthropic.com/v1/models");
    // anthropic:地址已带 /v1 结尾,不再拼出 /v1/v1/models。
    CHECK(api::ModelsUrl(config::Wire::Anthropic, "https://cc.example.test/v1") ==
          "https://cc.example.test/v1/models");
    // responses / chat_completions:用户负责带 /v1,客户端只补 /models。
    CHECK(api::ModelsUrl(config::Wire::Responses, "http://127.0.0.1:8000/v1") ==
          "http://127.0.0.1:8000/v1/models");
    CHECK(api::ModelsUrl(config::Wire::ChatCompletions, "http://127.0.0.1:8000/v1") ==
          "http://127.0.0.1:8000/v1/models");
}

TEST_CASE("ModelsRequestHeaders: 有 key 带 Bearer,无 key 彻底省头,extra 覆盖/删头照旧") {
    const auto with_key = api::ModelsRequestHeaders("sk-abc", {});
    CHECK(with_key.at("Authorization") == "Bearer sk-abc");

    // 无鉴权(auth=none)或 env 缺值:连 Authorization 这个头都不发,
    // 绝不发一枚空 Bearer 冒充。
    const auto without_key = api::ModelsRequestHeaders("", {});
    CHECK_FALSE(without_key.contains("Authorization"));

    // extra_headers:非空覆盖、空串删头(包括删 Authorization)。
    const auto overridden =
        api::ModelsRequestHeaders("sk-abc", {{"Authorization", "Bearer other"}, {"X-Custom", "v"}});
    CHECK(overridden.at("Authorization") == "Bearer other");
    CHECK(overridden.at("X-Custom") == "v");
    const auto erased = api::ModelsRequestHeaders("sk-abc", {{"Authorization", ""}});
    CHECK_FALSE(erased.contains("Authorization"));
}

TEST_CASE("RequestBaseHeaders: 三套正式 client 的基础头同款规矩") {
    const auto with_key = api::RequestBaseHeaders("sk-abc");
    CHECK(with_key.at("Content-Type") == "application/json");
    CHECK(with_key.at("Authorization") == "Bearer sk-abc");

    const auto without_key = api::RequestBaseHeaders("");
    CHECK(without_key.at("Content-Type") == "application/json");
    CHECK_FALSE(without_key.contains("Authorization"));
    CHECK(without_key.size() == 1);
}
