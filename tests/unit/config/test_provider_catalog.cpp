#include <doctest/doctest.h>

#include <algorithm>

#include "api/chat/request.hpp"
#include "config/provider_catalog.hpp"
#include "embedded_provider_catalog.hpp"

using namespace lubancode;

namespace {
constexpr const char* kCatalog = R"({
  "schema_version": 2,
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
      "reasoning_replay": "tool_episode",
      "extra_headers": {"X-Key": "${LUBANCODE_API_KEY}"},
      "extra_body": {"tool_stream": true},
      "models": {
        "demo-1": {
          "name": "Demo One",
          "context_window": "1m",
          "max_output": 128000,
          "default_think": "high",
          "capabilities": {"reasoning": true, "tools": true},
          "reasoning": {
            "controls": [{"kind": "effort", "values": ["low", "high"]}, {"kind": "toggle"}],
            "supportedEfforts": ["low", "high"]
          }
        }
      }
    }
  }
})";
}

TEST_CASE("provider catalog: 严格解析 provider、模型与 reasoning") {
    const auto catalog = config::ParseProviderCatalogJson(kCatalog, "catalog.json");
    REQUIRE(catalog.has_value());
    CHECK(catalog->revision == "2026-07-25");
    const auto* preset = catalog->FindProvider("demo");
    REQUIRE(preset != nullptr);
    CHECK(preset->wire == config::Wire::ChatCompletions);
    CHECK(preset->stream_usage);  // Chat 流式 usage chunk 的 capability
    CHECK(preset->reasoning_replay == "tool_episode");  // 工具段思考回传策略
    // preset -> 本地 provider 配置镜像到位。
    const auto provider = config::ProviderConfigFromPreset(*preset);
    CHECK(provider.stream_usage);
    CHECK(provider.reasoning_replay == "tool_episode");
    const auto* model = preset->FindModel("demo-1");
    REQUIRE(model != nullptr);
    CHECK(model->context_window_tokens == 1000000);
    CHECK(model->max_output_tokens == 128000);
    CHECK(model->capabilities.at("reasoning"));
    CHECK(model->reasoning.supports_effort);
    CHECK(model->reasoning.supports_toggle);
    CHECK(model->reasoning.supported_efforts == std::vector<std::string>{"low", "high"});
}

TEST_CASE("provider catalog: schema、地址、默认模型和字段类型坏了都拒绝整份") {
    CHECK_FALSE(config::ParseProviderCatalogJson("{}", "p").has_value());
    CHECK_FALSE(config::ParseProviderCatalogJson(
        R"({"schema_version":1,"revision":"2026-07-25","providers":{}})", "p").has_value());
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

TEST_CASE("内置 Claude 5 档位直接写在模型 reasoning 里") {
    const auto catalog = config::ParseProviderCatalogJson(
        config::embedded::kProviderCatalogJson, "<embedded>");
    REQUIRE(catalog.has_value());
    const auto* anthropic = catalog->FindProvider("anthropic");
    REQUIRE(anthropic != nullptr);
    const auto* opus = anthropic->FindModel("claude-opus-5");
    REQUIRE(opus != nullptr);
    CHECK(opus->default_think == "high");
    CHECK(opus->reasoning.supported_efforts ==
          std::vector<std::string>{"low", "medium", "high", "xhigh", "max"});

    const auto* sonnet = anthropic->FindModel("claude-sonnet-5");
    REQUIRE(sonnet != nullptr);
    CHECK(sonnet->default_think == "medium");
    CHECK(sonnet->capabilities.at("reasoning"));
    CHECK(sonnet->reasoning.supported_efforts ==
          std::vector<std::string>{"low", "medium", "high", "xhigh", "max", "none"});
}

TEST_CASE("内置 GPT 5.6 与 GLM 5.3 各自声明不同 effort") {
    const auto catalog = config::ParseProviderCatalogJson(
        config::embedded::kProviderCatalogJson, "<embedded>");
    REQUIRE(catalog.has_value());
    const auto* terra = catalog->FindProvider("openai")->FindModel("gpt-5.6-terra");
    REQUIRE(terra != nullptr);
    CHECK(terra->reasoning.supported_efforts ==
          std::vector<std::string>{"none", "low", "medium", "high", "xhigh", "max"});
    const auto* glm = catalog->FindProvider("zai")->FindModel("glm-5.3");
    REQUIRE(glm != nullptr);
    CHECK(glm->reasoning.supported_efforts == std::vector<std::string>{"low", "high", "max"});
}

// ---------------------------------------------------------------------------
// Kimi 保留式思考单 P0:Moonshot 四枚模型不可混作一家(官方契约表逐行)。
// K3/K2.7 固定 always,K2.6 默认 tool_episode,K2.5 固定 never;controls
// 与 efforts 一道校正——K2.6 不认 reasoning_effort,K3 不认 none 与
// thinking 开关,K2.7 什么都不用发。
// ---------------------------------------------------------------------------

TEST_CASE("内置 Moonshot 四枚模型:controls/efforts/dialect/replay 逐行对官方契约") {
    const auto catalog = config::ParseProviderCatalogJson(
        config::embedded::kProviderCatalogJson, "<embedded>");
    REQUIRE(catalog.has_value());
    const auto* moonshot = catalog->FindProvider("moonshot");
    REQUIRE(moonshot != nullptr);

    const auto* k3 = moonshot->FindModel("kimi-k3");
    REQUIRE(k3 != nullptr);
    CHECK(k3->reasoning.supports_effort);
    CHECK_FALSE(k3->reasoning.supports_toggle);  // 始终思考,不该发送 thinking
    CHECK(k3->reasoning.supported_efforts == std::vector<std::string>{"low", "high", "max"});
    CHECK(k3->reasoning.dialect.toggle == "none");
    CHECK(k3->reasoning.dialect.replay == "always");
    CHECK(k3->reasoning.dialect.verified);

    for (const char* id : {"kimi-k2.7-code", "kimi-k2.7-code-highspeed"}) {
        const auto* code = moonshot->FindModel(id);
        REQUIRE(code != nullptr);  // 两枚各自核:找不着 model 当场红
        CHECK_FALSE(code->reasoning.supports_effort);
        CHECK_FALSE(code->reasoning.supports_toggle);
        CHECK_FALSE(code->reasoning.budget_min.has_value());
        CHECK_FALSE(code->reasoning.budget_max.has_value());
        // 什么都不用发:服务端固定思考,请求里不带 thinking/effort/budget。
        CHECK(code->reasoning.dialect.toggle == "none");
        CHECK(code->reasoning.dialect.effort_path.empty());
        CHECK(code->reasoning.dialect.budget_path.empty());
        CHECK(code->reasoning.dialect.replay == "always");
        CHECK(code->reasoning.dialect.verified);
    }

    const auto* k26 = moonshot->FindModel("kimi-k2.6");
    REQUIRE(k26 != nullptr);
    CHECK(k26->reasoning.supports_toggle);       // thinking.type enabled/disabled
    CHECK_FALSE(k26->reasoning.supports_effort);  // 官方不认 reasoning_effort
    CHECK_FALSE(k26->reasoning.budget_min.has_value());
    CHECK_FALSE(k26->reasoning.budget_max.has_value());
    CHECK(k26->reasoning.supported_efforts.empty());
    CHECK(k26->reasoning.dialect.toggle == "thinking_type");
    CHECK(k26->reasoning.dialect.effort_path.empty());
    CHECK(k26->reasoning.dialect.replay == "tool_episode");  // 本 Turn 工具循环回传
    CHECK(k26->reasoning.dialect.verified);
    CHECK(k26->default_think.empty());  // 不再默认 high,免得顶层发 reasoning_effort

    const auto* k25 = moonshot->FindModel("kimi-k2.5");
    REQUIRE(k25 != nullptr);
    CHECK(k25->reasoning.supports_toggle);
    CHECK_FALSE(k25->reasoning.supports_effort);
    CHECK_FALSE(k25->reasoning.budget_min.has_value());
    CHECK(k25->reasoning.dialect.replay == "never");  // 不支持 Preserved Thinking
    CHECK(k25->reasoning.dialect.verified);
}

TEST_CASE("Moonshot 四枚模型的请求 golden:该发的发,不该发的一概不发") {
    const auto catalog = config::ParseProviderCatalogJson(
        config::embedded::kProviderCatalogJson, "<embedded>");
    REQUIRE(catalog.has_value());
    const auto* moonshot = catalog->FindProvider("moonshot");
    REQUIRE(moonshot != nullptr);

    // 造一份"上一轮带思考的 assistant + 本轮 user"的历史,四枚模型共用。
    const auto build_request = [&moonshot](const char* model_id, const std::string& effort) {
        api::Request request;
        const auto* model = moonshot->FindModel(model_id);
        REQUIRE(model != nullptr);
        request.model = model_id;
        request.reasoning = model->reasoning;
        request.reasoning_effort = effort;
        api::Message user;
        user.role = api::Role::User;
        user.content.push_back(api::TextBlock{"你好"});
        request.messages.push_back(user);
        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::ThinkingBlock{"上一轮的思考", ""});
        assistant.content.push_back(api::TextBlock{"上一轮的答"});
        request.messages.push_back(assistant);
        api::Message next;
        next.role = api::Role::User;
        next.content.push_back(api::TextBlock{"再问"});
        request.messages.push_back(next);
        return request;
    };

    // K3:reasoning_effort 落顶层;thinking 整个不发;历史 reasoning 原样在。
    const auto k3_body = api::chat::BuildRequestJson(build_request("kimi-k3", "high"));
    CHECK(k3_body["reasoning_effort"] == "high");
    CHECK_FALSE(k3_body.contains("thinking"));
    CHECK_FALSE(k3_body.contains("thinking_budget"));
    CHECK(k3_body["messages"][1]["reasoning_content"] == "上一轮的思考");

    // K2.7 Code:服务端固定思考——effort/toggle/budget 三样全不发;历史
    // reasoning 照回(Preserved Thinking 是客户端责任,与请求参数无关)。
    const auto k27_body = api::chat::BuildRequestJson(build_request("kimi-k2.7-code", "high"));
    CHECK_FALSE(k27_body.contains("reasoning_effort"));
    CHECK_FALSE(k27_body.contains("thinking"));
    CHECK_FALSE(k27_body.contains("thinking_budget"));
    CHECK(k27_body["messages"][1]["reasoning_content"] == "上一轮的思考");

    // K2.6:只发 thinking.type;官方不认的 reasoning_effort/budget 不发。
    // 纯对话段的思考不回传(tool_episode 只认工具段)。
    const auto k26_body = api::chat::BuildRequestJson(build_request("kimi-k2.6", "high"));
    CHECK(k26_body["thinking"]["type"] == "enabled");
    CHECK_FALSE(k26_body.contains("reasoning_effort"));
    CHECK_FALSE(k26_body.contains("thinking_budget"));
    CHECK_FALSE(k26_body["messages"][1].contains("reasoning_content"));
    // none 档:关思考的请求照发(生效与否服务端说了算)。
    const auto k26_off = api::chat::BuildRequestJson(build_request("kimi-k2.6", "none"));
    CHECK(k26_off["thinking"]["type"] == "disabled");

    // K2.5:thinking.type 可开关;不支持 Preserved Thinking——历史 reasoning
    // 不回传,thinking.keep 一概不发。
    const auto k25_body = api::chat::BuildRequestJson(build_request("kimi-k2.5", "high"));
    CHECK(k25_body["thinking"]["type"] == "enabled");
    CHECK_FALSE(k25_body["thinking"].contains("keep"));
    CHECK_FALSE(k25_body["messages"][1].contains("reasoning_content"));
}

// ---------------------------------------------------------------------------
// Kimi 保留式思考单 P1:跨轮保留的服务端历史控制(history_control)。
// K2.6 声明 thinking_keep(可选跨轮保留);K3/K2.7/K2.5 显式 none(同
// provider 里与 K2.6 分家的那道闸)。history all 的请求 golden:keep 与
// type 同发、历史 reasoning 原字节回传。
// ---------------------------------------------------------------------------

TEST_CASE("Moonshot history_control: K2.6 可选保留,其余三枚显式不清位") {
    const auto catalog = config::ParseProviderCatalogJson(
        config::embedded::kProviderCatalogJson, "<embedded>");
    REQUIRE(catalog.has_value());
    const auto* moonshot = catalog->FindProvider("moonshot");
    REQUIRE(moonshot != nullptr);

    const auto* k26 = moonshot->FindModel("kimi-k2.6");
    REQUIRE(k26 != nullptr);
    CHECK(k26->reasoning.dialect.history_control == "thinking_keep");
    CHECK(k26->reasoning.dialect.history_all_value.empty());  // 空 = "all"(官方唯一值)
    CHECK(api::ReasoningHistorySupportFor(k26->reasoning) ==
          api::ReasoningHistorySupport::RequestControl);

    for (const char* id : {"kimi-k3", "kimi-k2.7-code", "kimi-k2.7-code-highspeed"}) {
        const auto* model = moonshot->FindModel(id);
        REQUIRE(model != nullptr);
        // 显式 none 清位:就算 provider 级将来声明了 thinking_keep,这几枚
        // 也不继承。
        CHECK(model->reasoning.dialect.history_control.empty());
        CHECK(api::ReasoningHistorySupportFor(model->reasoning) ==
              api::ReasoningHistorySupport::ServerFixed);
    }

    const auto* k25 = moonshot->FindModel("kimi-k2.5");
    REQUIRE(k25 != nullptr);
    CHECK(k25->reasoning.dialect.history_control.empty());
    CHECK(api::ReasoningHistorySupportFor(k25->reasoning) ==
          api::ReasoningHistorySupport::None);  // replay=never,不认保留
}

TEST_CASE("Moonshot K2.6 history all 请求 golden: keep 与历史 reasoning 同发") {
    const auto catalog = config::ParseProviderCatalogJson(
        config::embedded::kProviderCatalogJson, "<embedded>");
    REQUIRE(catalog.has_value());
    const auto* moonshot = catalog->FindProvider("moonshot");
    REQUIRE(moonshot != nullptr);
    const auto* k26 = moonshot->FindModel("kimi-k2.6");
    REQUIRE(k26 != nullptr);

    api::Request request;
    request.model = "kimi-k2.6";
    request.reasoning = k26->reasoning;
    request.reasoning_effort = "high";
    request.reasoning_history = api::ReasoningHistoryMode::All;
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{"你好"});
    request.messages.push_back(user);
    api::Message assistant;
    assistant.role = api::Role::Assistant;
    assistant.content.push_back(api::ThinkingBlock{"上一轮的思考", ""});
    assistant.content.push_back(api::TextBlock{"上一轮的答"});
    request.messages.push_back(assistant);
    api::Message next;
    next.role = api::Role::User;
    next.content.push_back(api::TextBlock{"再问"});
    request.messages.push_back(next);

    const auto body = api::chat::BuildRequestJson(request);
    // 只发 keep、不带历史,或只带历史、不发 keep,都不算跨轮 Preserved
    // Thinking 已启用——两者必须同时在场。
    CHECK(body["thinking"]["type"] == "enabled");
    CHECK(body["thinking"]["keep"] == "all");
    CHECK(body["messages"][1]["reasoning_content"] == "上一轮的思考");
    CHECK_FALSE(body.contains("reasoning_effort"));
}

TEST_CASE("方言 history_control 的解析边界:拼错枚举、错 wire、模型级清继承") {
    // 拼错的枚举当场报错。
    CHECK_FALSE(config::ParseProviderCatalogJson(R"({
      "schema_version": 2, "revision": "2026-08-30",
      "providers": {"d": {"name": "D", "wire": "chat_completions",
        "base_url": "https://a.test/v1", "key_env": "D_KEY", "default_model": "m",
        "reasoning_dialect": {"history_control": "keep_all"},
        "models": {"m": {"name": "M"}}}}})", "p").has_value());
    // 认不得的方言字段报错(schema additionalProperties=false 同一口径)。
    CHECK_FALSE(config::ParseProviderCatalogJson(R"({
      "schema_version": 2, "revision": "2026-08-30",
      "providers": {"d": {"name": "D", "wire": "chat_completions",
        "base_url": "https://a.test/v1", "key_env": "D_KEY", "default_model": "m",
        "reasoning_dialect": {"history_kontroll": "thinking_keep"},
        "models": {"m": {"name": "M"}}}}})", "p").has_value());
    // 非 chat 家不认 thinking_keep:catalog parse 阶段就拒,不留到运行时。
    CHECK_FALSE(config::ParseProviderCatalogJson(R"({
      "schema_version": 2, "revision": "2026-08-30",
      "providers": {"d": {"name": "D", "wire": "anthropic_messages",
        "base_url": "https://a.test/v1", "key_env": "D_KEY", "default_model": "m",
        "reasoning_dialect": {"history_control": "thinking_keep"},
        "models": {"m": {"name": "M"}}}}})", "p").has_value());
    // history_all_value 空串不是合法声明。
    CHECK_FALSE(config::ParseProviderCatalogJson(R"({
      "schema_version": 2, "revision": "2026-08-30",
      "providers": {"d": {"name": "D", "wire": "chat_completions",
        "base_url": "https://a.test/v1", "key_env": "D_KEY", "default_model": "m",
        "reasoning_dialect": {"history_control": "thinking_keep", "history_all_value": ""},
        "models": {"m": {"name": "M"}}}}})", "p").has_value());

    // provider 级声明 thinking_keep,模型级 none 清掉继承(P1 分家闸)。
    const auto cleared = config::ParseProviderCatalogJson(R"({
      "schema_version": 2, "revision": "2026-08-30",
      "providers": {"d": {"name": "D", "wire": "chat_completions",
        "base_url": "https://a.test/v1", "key_env": "D_KEY", "default_model": "keeps",
        "reasoning_dialect": {"history_control": "thinking_keep", "replay": "tool_episode"},
        "models": {
          "keeps": {"name": "K", "reasoning": {"controls": [{"kind": "toggle"}]}},
          "clears": {"name": "C", "reasoning": {"dialect": {"history_control": "none"}}}}}}})",
        "p");
    REQUIRE_MESSAGE(cleared.has_value(), cleared.error_or(std::string()));
    const auto* keeps = cleared->FindProvider("d")->FindModel("keeps");
    REQUIRE(keeps != nullptr);
    CHECK(keeps->reasoning.dialect.history_control == "thinking_keep");  // 继承
    const auto* clears = cleared->FindProvider("d")->FindModel("clears");
    REQUIRE(clears != nullptr);
    CHECK(clears->reasoning.dialect.history_control.empty());  // 显式清位
}
