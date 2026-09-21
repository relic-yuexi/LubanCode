// HC-08(补齐Backend包装层的预算映射与出站能力转发)合同册。
//
// 病灶(单子的账):SpinnerBackend/RebuildableBackend 只 override
// send_stream,预算映射与出站能力五口吃 Backend 基类默认——Serialize
// 空串、映射/PrepareWireRequest 不可得、有效上限回退 Request::max_tokens、
// Force no-op。真实终端/单发链(Agent 只认 Backend&,握的是壳)于是丢了
// 叶 client 合同:adapter 预算退内部估算、provider 覆盖读不着、收窄写不
// 进最终 wire。
//
// 本册四案:
//   1) 单层 Spinner(动画关):六口全落记账假内芯;
//   2) 双层 Spinner(Rebuildable(probe)):五口直达叶假内芯;
//   3) 真 Chat 叶装两层,与裸叶逐口比对(序列化/上限/映射/Prepare/Force);
//   4) 真 Agent 预算路径:真 Chat 叶两层包装 + 本机回环假 SSE 服务,
//      应急收窄值确实出现在最终出门体(真 POST body),不只
//      Request::max_tokens。
//
// 记账假内芯与 tests/unit/runtime/test_backend_stack.cpp 的同名件同款,
// 各册自持(仓库测试不跨文件共享夹具的惯例)。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "api/backend.hpp"
#include "api/chat/client.hpp"
#include "app/backend_stack.hpp"
#include "cli/spinner_backend.hpp"
#include "cli/theme.hpp"
#include "fake_http_server.hpp"
#include "tools/registry.hpp"

namespace {

// HC-08:能记账每个能力调用的假内芯(五口各留一笔账,返回值带印记)。
class CapabilityProbeBackend : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)cancel;
        ++send_calls;
        last_sent = request;
        on_event(lubancode::api::MessageStart{"probe-1", request.model});
        on_event(lubancode::api::MessageDone{"end_turn", lubancode::api::Usage{}});
        return {};
    }

    std::string SerializeForDiagnostics(const lubancode::api::Request& request) const override {
        (void)request;
        ++serialize_calls;
        return "PROBE-WIRE";
    }

    lubancode::api::PreparedWireRequest PrepareWireRequest(
        const lubancode::api::Request& request) const override {
        ++prepare_calls;
        lubancode::api::PreparedWireRequest prepared;
        prepared.body = nlohmann::json{{"probe", true}};
        lubancode::api::WireMessageMap map;
        map.container = "probe-messages";
        map.message_to_wire.assign(request.messages.size(), {0});
        map.wire_element_count = 1;
        prepared.wire_map = std::move(map);
        prepared.output_limit = 4242;
        prepared.output_limit_overridden = true;
        return prepared;
    }

    std::optional<lubancode::api::WireMessageMap> BuildWireMessageMap(
        const lubancode::api::Request& request) const override {
        (void)request;
        ++map_calls;
        lubancode::api::WireMessageMap map;
        map.container = "probe-messages";
        map.message_to_wire = {{0}};
        map.wire_element_count = 7;
        return map;
    }

    EffectiveOutputLimit GetEffectiveOutputLimit(const lubancode::api::Request& request) const override {
        (void)request;
        ++limit_calls;
        return {9973, true};
    }

    void ForceMaxOutputTokensOverride(lubancode::api::Request& request, int tokens) const override {
        ++force_calls;
        request.extra_body["max_tokens"] = tokens;
    }

    mutable int send_calls = 0;
    mutable int serialize_calls = 0;
    mutable int prepare_calls = 0;
    mutable int map_calls = 0;
    mutable int limit_calls = 0;
    mutable int force_calls = 0;
    lubancode::api::Request last_sent;
};

lubancode::api::Request ProbeRequest() {
    lubancode::api::Request request;
    request.model = "probe-model";
    request.max_tokens = 100;
    request.system = "sys";
    request.messages.push_back(lubancode::api::Message{});
    request.messages.back().role = lubancode::api::Role::User;
    request.messages.back().content.push_back(lubancode::api::TextBlock{"问"});
    return request;
}

}  // namespace

TEST_CASE("HC-08 单层:SpinnerBackend 六口全落内芯(动画关,纯透传)") {
    CapabilityProbeBackend inner;
    const lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    lubancode::cli::SpinnerBackend wrapped(inner, theme, /*spinner_enabled=*/false);

    lubancode::api::Request request = ProbeRequest();

    CHECK(wrapped.SerializeForDiagnostics(request) == "PROBE-WIRE");

    const auto prepared = wrapped.PrepareWireRequest(request);
    CHECK(prepared.body.at("probe") == true);
    REQUIRE(prepared.wire_map.has_value());
    CHECK(prepared.wire_map->container == "probe-messages");
    CHECK(prepared.output_limit == 4242);
    CHECK(prepared.output_limit_overridden);

    const auto map = wrapped.BuildWireMessageMap(request);
    REQUIRE(map.has_value());
    CHECK(map->container == "probe-messages");
    CHECK(map->wire_element_count == 7);

    const auto limit = wrapped.GetEffectiveOutputLimit(request);
    CHECK(limit.tokens == 9973);
    CHECK(limit.overridden);

    wrapped.ForceMaxOutputTokensOverride(request, 555);
    REQUIRE(request.extra_body.contains("max_tokens"));
    CHECK(request.extra_body.at("max_tokens") == 555);

    REQUIRE(wrapped.send_stream(request, [](const lubancode::api::StreamEvent&) {}).has_value());

    CHECK(inner.serialize_calls == 1);
    CHECK(inner.prepare_calls == 1);
    CHECK(inner.map_calls == 1);
    CHECK(inner.limit_calls == 1);
    CHECK(inner.force_calls == 1);
    CHECK(inner.send_calls == 1);
    CHECK(inner.last_sent.model == "probe-model");
}

TEST_CASE("HC-08 双层:Spinner(Rebuildable(probe)) 五口直达叶假内芯") {
    auto inner = std::make_shared<CapabilityProbeBackend>();
    lubancode::app::RebuildableBackend middle(inner);
    const lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    lubancode::cli::SpinnerBackend wrapped(middle, theme, /*spinner_enabled=*/false);

    lubancode::api::Request request = ProbeRequest();

    CHECK(wrapped.SerializeForDiagnostics(request) == "PROBE-WIRE");
    const auto prepared = wrapped.PrepareWireRequest(request);
    REQUIRE(prepared.wire_map.has_value());
    CHECK(prepared.output_limit == 4242);
    const auto map = wrapped.BuildWireMessageMap(request);
    REQUIRE(map.has_value());
    CHECK(map->wire_element_count == 7);
    const auto limit = wrapped.GetEffectiveOutputLimit(request);
    CHECK(limit.tokens == 9973);
    wrapped.ForceMaxOutputTokensOverride(request, 555);
    CHECK(request.extra_body.at("max_tokens") == 555);
    REQUIRE(wrapped.send_stream(request, [](const lubancode::api::StreamEvent&) {}).has_value());

    // 两层壳一枚不吞:内芯六口各恰好一笔。
    CHECK(inner->serialize_calls == 1);
    CHECK(inner->prepare_calls == 1);
    CHECK(inner->map_calls == 1);
    CHECK(inner->limit_calls == 1);
    CHECK(inner->force_calls == 1);
    CHECK(inner->send_calls == 1);
}

TEST_CASE("HC-08 真叶比对:Chat client 装两层包装,五口与裸叶一字不差(动画关)") {
    lubancode::config::Config config;
    config.wire = lubancode::config::Wire::ChatCompletions;
    config.base_url = "http://127.0.0.1:9";
    config.auth_token = "test-key";
    config.extra_body["max_tokens"] = 32800;  // provider 级宽覆盖(用户手笔)

    lubancode::app::RebuildableBackend rebuildable(config);  // BuildBackend 选出真 Chat 叶
    const lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    lubancode::cli::SpinnerBackend wrapped(rebuildable, theme, /*spinner_enabled=*/false);

    // 裸叶对照:同一副参数直造 ChatCompletionsBackend(不走网络,五口全是
    // 纯拼装)。
    const lubancode::api::chat::ChatCompletionsBackend leaf(
        config.base_url, config.auth_token, config.connect_timeout_ms, config.stream_idle_timeout_secs,
        config.extra_body);

    lubancode::api::Request request = ProbeRequest();

    // 序列化非空(病灶:包装默认空串,loop.cpp 的 adapter 预算判定翻假,
    // 退内部估算),且与裸叶一字不差。
    const std::string wrapped_wire = wrapped.SerializeForDiagnostics(request);
    CHECK_FALSE(wrapped_wire.empty());
    CHECK(wrapped_wire == leaf.SerializeForDiagnostics(request));

    // 有效上限:provider 覆盖后的 32800(病灶:包装默认回退 100)。
    const auto wrapped_limit = wrapped.GetEffectiveOutputLimit(request);
    REQUIRE(wrapped_limit.tokens.has_value());
    CHECK(*wrapped_limit.tokens == 32800);
    CHECK(wrapped_limit.overridden);
    const auto leaf_limit = leaf.GetEffectiveOutputLimit(request);
    CHECK(wrapped_limit.tokens == leaf_limit.tokens);
    CHECK(wrapped_limit.overridden == leaf_limit.overridden);

    // 拍平映射:与裸叶同源(病灶:包装默认 nullopt,prepared 账记不可得)。
    const auto wrapped_map = wrapped.BuildWireMessageMap(request);
    const auto leaf_map = leaf.BuildWireMessageMap(request);
    REQUIRE(wrapped_map.has_value());
    REQUIRE(leaf_map.has_value());
    CHECK(wrapped_map->container == leaf_map->container);
    CHECK(wrapped_map->wire_element_count == leaf_map->wire_element_count);
    CHECK(wrapped_map->message_to_wire == leaf_map->message_to_wire);

    // FD-02 窄口(PrepareWireRequest)同样穿透两层:出门体/上限/覆盖标志
    // 与裸叶逐字段相等。
    const auto wrapped_prepared = wrapped.PrepareWireRequest(request);
    const auto leaf_prepared = leaf.PrepareWireRequest(request);
    CHECK(wrapped_prepared.body == leaf_prepared.body);
    CHECK(wrapped_prepared.output_limit == leaf_prepared.output_limit);
    CHECK(wrapped_prepared.output_limit_overridden == leaf_prepared.output_limit_overridden);
    CHECK(wrapped_prepared.wire_map.has_value() == leaf_prepared.wire_map.has_value());

    // Force 写侧:窄值写进请求级覆盖位,与裸叶同笔;写完经两层读回,
    // 有效上限就是窄值(合并序最后压过 provider 级 32800)。
    lubancode::api::Request via_wrapped = ProbeRequest();
    lubancode::api::Request via_leaf = ProbeRequest();
    wrapped.ForceMaxOutputTokensOverride(via_wrapped, 2048);
    leaf.ForceMaxOutputTokensOverride(via_leaf, 2048);
    CHECK(via_wrapped.extra_body == via_leaf.extra_body);
    REQUIRE(via_wrapped.extra_body.contains("max_tokens"));
    CHECK(via_wrapped.extra_body.at("max_tokens") == 2048);
    const auto narrowed = wrapped.GetEffectiveOutputLimit(via_wrapped);
    REQUIRE(narrowed.tokens.has_value());
    CHECK(*narrowed.tokens == 2048);
    CHECK(narrowed.overridden);
}

TEST_CASE("HC-08 Agent 预算路径:真 Chat 叶两层包装,应急收窄值落在最终出门体") {
    // 本机回环假 SSE 服务当模型端,收真 POST body——出门体以此为准,不拿
    // Request 字段冒充。
    lubancode::test_support::FakeHttpServer server;
    const std::string sse =
        "data: {\"id\":\"hc08\",\"object\":\"chat.completion.chunk\",\"model\":\"m\","
        "\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"短交接\"},"
        "\"finish_reason\":null}]}\n\n"
        "data: {\"id\":\"hc08\",\"object\":\"chat.completion.chunk\",\"model\":\"m\","
        "\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":1}}\n\n"
        "data: [DONE]\n\n";
    lubancode::test_support::FakeHttpResponse response;
    response.status = 200;
    response.headers.emplace_back("Content-Type", "text/event-stream");
    response.body = sse;
    server.Enqueue(response);

    lubancode::config::Config config;
    config.wire = lubancode::config::Wire::ChatCompletions;
    config.base_url = "http://127.0.0.1:" + std::to_string(server.port());
    config.auth_token = "test-key";
    config.extra_body["max_tokens"] = 32800;  // provider 级宽覆盖

    lubancode::app::RebuildableBackend rebuildable(config);
    const lubancode::cli::Theme theme = lubancode::cli::BuiltinTheme("plain");
    lubancode::cli::SpinnerBackend wrapped(rebuildable, theme, /*spinner_enabled=*/false);

    // 预算读侧先证一次(病灶:包装默认 {回退,false},主循环读不着覆盖,
    // 永远进不了应急支)。两层之下读到的是 provider 覆盖后的 32800。
    lubancode::api::Request probe = ProbeRequest();
    const auto pre = wrapped.GetEffectiveOutputLimit(probe);
    REQUIRE(pre.tokens.has_value());
    REQUIRE(*pre.tokens == 32800);
    REQUIRE(pre.overridden);

    lubancode::tools::ToolRegistry registry;
    lubancode::agent::Agent agent(wrapped, registry,
                                  lubancode::agent::AgentProfile{
                                      .request{.model = "test-model"},
                                      .runtime{.max_steps_per_turn = 1, .context_window_tokens = 32768},
                                      .system_prompt = "sys"});

    const auto result = agent.Run("查一查", lubancode::agent::TurnWiring{});
    REQUIRE(result.has_value());  // 应急放行,同任务续跑

    // 收窄值的最终去处:真 POST body 上的 max_tokens == 应急预留 2048
    //(32768/16),不是 provider 级 32800,也不是只在 Request 字段里改改。
    const auto received = server.requests();
    REQUIRE(received.size() == 1);
    CHECK(received[0].method == "POST");
    const auto body = nlohmann::json::parse(received[0].body, nullptr, /*allow_exceptions=*/false);
    REQUIRE_FALSE(body.is_discarded());
    REQUIRE(body.contains("max_tokens"));
    CHECK(body.at("max_tokens") == 2048);
}
