// 自部署端点窗口探测(Terminal-Bench 自部署单)的解析纯函数:
// api::ParseModelContextWindowFromModelsResponse。只测 JSON 解析,不发网络
// (ProbeModelContextWindow 的网络部分不写真网测试)。

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>

#include "api/models.hpp"

using namespace lubancode;

TEST_CASE("窗口探测解析: vLLM /v1/models 样例读 max_model_len") {
    const std::string body = R"({
        "object": "list",
        "data": [
            {"id": "Qwen3.8-27B", "object": "model", "created": 1760000000,
             "owned_by": "vllm", "root": "/models/Qwen3.8-27B", "parent": null,
             "max_model_len": 131072,
             "permission": [{"id": "modelperm-1", "object": "model_permission"}]}
        ]
    })";
    const auto window = api::ParseModelContextWindowFromModelsResponse(body, "Qwen3.8-27B");
    REQUIRE(window.has_value());
    CHECK(*window == 131072);
}

TEST_CASE("窗口探测解析: 多模型按 id 挑本场那只") {
    const std::string body = R"({"data": [
        {"id": "other-model", "max_model_len": 8192},
        {"id": "target-model", "max_model_len": 32768},
        {"id": "third", "max_model_len": 65536}
    ]})";
    const auto window = api::ParseModelContextWindowFromModelsResponse(body, "target-model");
    REQUIRE(window.has_value());
    CHECK(*window == 32768);
}

TEST_CASE("窗口探测解析: 精确 id 压过大小写不敏感的近似 id") {
    const std::string body = R"({"data": [
        {"id": "qwen-local", "max_model_len": 4096},
        {"id": "Qwen-Local", "max_model_len": 16384}
    ]})";
    const auto exact = api::ParseModelContextWindowFromModelsResponse(body, "Qwen-Local");
    REQUIRE(exact.has_value());
    CHECK(*exact == 16384);
    // 没有精确匹配时退到忽略大小写的那只。
    const auto loose = api::ParseModelContextWindowFromModelsResponse(body, "QWEN-LOCAL");
    REQUIRE(loose.has_value());
    CHECK(*loose == 4096);
}

TEST_CASE("窗口探测解析: 其他实现的字段名 context_length/context_window/max_context_length") {
    CHECK(api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"m","context_length":40960}]})", "m") ==
          std::optional<std::size_t>(40960));
    CHECK(api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"m","context_window":20000}]})", "m") ==
          std::optional<std::size_t>(20000));
    CHECK(api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"m","max_context_length":8192}]})",
                                                         "m") == std::optional<std::size_t>(8192));
    // max_model_len 优先于其余字段。
    CHECK(api::ParseModelContextWindowFromModelsResponse(
              R"({"data":[{"id":"m","context_length":999999,"max_model_len":65536}]})", "m") ==
          std::optional<std::size_t>(65536));
}

TEST_CASE("窗口探测解析: 字段缺失或找不到模型给空") {
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"m","owned_by":"ollama"}]})", "m")
                    .has_value());
    CHECK_FALSE(
        api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"x","max_model_len":8192}]})", "m")
            .has_value());
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse(R"({"data":[]})", "m").has_value());
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse(R"({"object":"list"})", "m").has_value());
    // 模型名为空不匹配任何条目。
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"","max_model_len":8192}]})", "")
                    .has_value());
}

TEST_CASE("窗口探测解析: 非正数、浮点、字符串都不采用,退到下一枚字段") {
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"m","max_model_len":0}]})", "m")
                    .has_value());
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"m","max_model_len":-1}]})", "m")
                    .has_value());
    CHECK_FALSE(
        api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"m","max_model_len":"8192"}]})", "m")
            .has_value());
    CHECK_FALSE(
        api::ParseModelContextWindowFromModelsResponse(R"({"data":[{"id":"m","max_model_len":8192.5}]})", "m")
            .has_value());
    CHECK(api::ParseModelContextWindowFromModelsResponse(
              R"({"data":[{"id":"m","max_model_len":0,"context_length":32768}]})", "m") ==
          std::optional<std::size_t>(32768));
}

TEST_CASE("窗口探测解析: 非对象、坏 JSON、data 非数组、条目非对象都给空") {
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse("{ not json", "m").has_value());
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse("[]", "m").has_value());
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse("\"text\"", "m").has_value());
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse(R"({"data":{"id":"m","max_model_len":1}})", "m")
                    .has_value());
    CHECK_FALSE(api::ParseModelContextWindowFromModelsResponse(R"({"data":["m", 42, null]})", "m").has_value());
    // 脏条目跳过,后面的正常条目照认。
    CHECK(api::ParseModelContextWindowFromModelsResponse(
              R"({"data":["m", {"id": 7}, {"id":"m","max_model_len":2048}]})", "m") ==
          std::optional<std::size_t>(2048));
}

TEST_CASE("窗口探测: 非 OpenAI 兼容 wire 与空参数不发请求直接给空") {
    // 这几条在发 GET 之前就收口,不碰网络。
    CHECK_FALSE(api::ProbeModelContextWindow(config::Wire::Anthropic, "http://127.0.0.1:1", "", "m").has_value());
    CHECK_FALSE(
        api::ProbeModelContextWindow(config::Wire::GoogleGenerateContent, "http://127.0.0.1:1", "", "m").has_value());
    CHECK_FALSE(api::ProbeModelContextWindow(config::Wire::ChatCompletions, "", "", "m").has_value());
    CHECK_FALSE(api::ProbeModelContextWindow(config::Wire::ChatCompletions, "http://127.0.0.1:1", "", "").has_value());
}
