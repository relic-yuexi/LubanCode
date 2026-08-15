#include <doctest/doctest.h>

#include <algorithm>

#include "config/provider_catalog.hpp"
#include "embedded_provider_catalog.hpp"

using namespace lubancode;

namespace {
constexpr const char* kCatalog = R"({
  "schema_version": 1,
  "revision": "2026-07-25",
  "providers": {
    "demo": {
      "name": "Demo",
      "wire": "chat_completions",
      "base_url": "https://api.example.test/v1",
      "key_env": "DEMO_API_KEY",
      "default_model": "demo-1",
      "model_reasoning_effort": "high",
      "stream_usage": true,
      "extra_headers": {"X-Key": "${LUBANCODE_API_KEY}"},
      "extra_body": {"tool_stream": true},
      "models": {
        "demo-1": {
          "name": "Demo One",
          "context_window": "1m",
          "max_output": 128000,
          "default_think": "high",
          "capabilities": {"reasoning": true, "tools": true},
          "variants": {
            "low": {"description": "快"},
            "high": {"description": "深", "extra_body": {"thinking": true}}
          }
        }
      }
    }
  }
})";
}

TEST_CASE("provider catalog: 严格解析 provider、模型与 variants") {
    const auto catalog = config::ParseProviderCatalogJson(kCatalog, "catalog.json");
    REQUIRE(catalog.has_value());
    CHECK(catalog->revision == "2026-07-25");
    const auto* preset = catalog->FindProvider("demo");
    REQUIRE(preset != nullptr);
    CHECK(preset->wire == config::Wire::ChatCompletions);
    CHECK(preset->stream_usage);  // Chat 流式 usage chunk 的 capability
    // preset -> 本地 provider 配置镜像到位。
    const auto provider = config::ProviderConfigFromPreset(*preset);
    CHECK(provider.stream_usage);
    const auto* model = preset->FindModel("demo-1");
    REQUIRE(model != nullptr);
    CHECK(model->context_window_tokens == 1000000);
    CHECK(model->max_output_tokens == 128000);
    CHECK(model->capabilities.at("reasoning"));
    REQUIRE(model->variants.size() == 2);
    const auto high = std::find_if(model->variants.begin(), model->variants.end(),
                                   [](const auto& variant) { return variant.id == "high"; });
    REQUIRE(high != model->variants.end());
    CHECK(high->extra_body["thinking"] == true);
}

TEST_CASE("provider catalog: schema、地址、默认模型和字段类型坏了都拒绝整份") {
    CHECK_FALSE(config::ParseProviderCatalogJson("{}", "p").has_value());
    CHECK_FALSE(config::ParseProviderCatalogJson(
        R"({"schema_version":2,"revision":"2026-07-25","providers":{}})", "p").has_value());
    std::string bad = kCatalog;
    bad.replace(bad.find("https://"), 8, "http://");
    CHECK_FALSE(config::ParseProviderCatalogJson(bad, "p").has_value());
    bad = kCatalog;
    bad.replace(bad.find("\"demo-1\"", bad.find("default_model")), 8, "\"missing\"");
    CHECK_FALSE(config::ParseProviderCatalogJson(bad, "p").has_value());
}

TEST_CASE("ProviderConfigFromPreset: 默认模型的窗口和思考档位落进本地配置") {
    const auto catalog = config::ParseProviderCatalogJson(kCatalog, "p");
    REQUIRE(catalog.has_value());
    const auto provider = config::ProviderConfigFromPreset(*catalog->FindProvider("demo"));
    CHECK(provider.name == "demo");
    CHECK(provider.model == "demo-1");
    CHECK(provider.context_window_tokens == 1000000);
    CHECK(provider.model_reasoning_effort == "high");
    CHECK(provider.extra_body["tool_stream"] == true);
}

TEST_CASE("ResolveProviderHeaderTemplates: 只在发请求前替换 key 占位符") {
    const auto headers = config::ResolveProviderHeaderTemplates(
        {{"x-api-key", "${LUBANCODE_API_KEY}"}, {"X-Mix", "a-${LUBANCODE_API_KEY}-b"}}, "secret");
    CHECK(headers.at("x-api-key") == "secret");
    CHECK(headers.at("X-Mix") == "a-secret-b");
}

TEST_CASE("内置 Claude 5 档位齐全，Sonnet 默认 medium、Opus 默认 high") {
    const auto catalog = config::ParseProviderCatalogJson(
        config::embedded::kProviderCatalogJson, "<embedded>");
    REQUIRE(catalog.has_value());
    const auto* anthropic = catalog->FindProvider("anthropic");
    REQUIRE(anthropic != nullptr);
    const auto* opus = anthropic->FindModel("claude-opus-5");
    REQUIRE(opus != nullptr);
    CHECK(opus->default_think == "high");
    std::vector<std::string> levels;
    for (const auto& variant : opus->variants) levels.push_back(variant.id);
    CHECK(levels == std::vector<std::string>{"low", "medium", "high", "extra", "max"});

    const auto* sonnet = anthropic->FindModel("claude-sonnet-5");
    REQUIRE(sonnet != nullptr);
    CHECK(sonnet->default_think == "medium");
    CHECK(sonnet->capabilities.at("reasoning"));
    levels.clear();
    for (const auto& variant : sonnet->variants) levels.push_back(variant.id);
    CHECK(levels == std::vector<std::string>{"low", "medium", "high", "extra", "max"});
}
