// 请求改写层的性状测试(骨架拆解批四改版):五层 override 后端退役后,
// 会话级请求策略(model/effort/模型指令/魂/延迟索引)由 Agent 拼请求时
// 就地生效——这里对最终发出的 Request 做断言:叠层先后(索引段 -> 模型
// 指令段 -> 魂段,魂压轴)一错,系统提示的段序就错,这类回归编译期看不
// 出来,只能靠对最终 Request 断言。RebuildableBackend 的稳定引用断言照旧。
// HC-08 追加:包装层(RebuildableBackend)预算映射与出站能力转发的单层
// 合同——六口全落内芯、未装配按接口默认给不可得、换血不撕在途流。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "runtime/assembly/backend.hpp"
#include "tools/registry.hpp"

namespace {

// 假内芯:记录收到的 Request,回一条极简 end_turn 流,不当真发网络。
class CapturingBackend : public lubancode::api::Backend {
public:
    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)cancel;
        captured.push_back(request);
        using lubancode::api::StreamEvent;
        on_event(lubancode::api::MessageStart{"m-1", request.model});
        on_event(lubancode::api::TextDelta{"好"});
        on_event(lubancode::api::ContentBlockDone{0});
        on_event(lubancode::api::MessageDone{"end_turn", lubancode::api::Usage{}});
        return {};
    }

    std::vector<lubancode::api::Request> captured;
};

// HC-08:能记账每个能力调用的假内芯。五口(send 之外的那五枚)各留一笔
// 账,返回值带印记——包装层哪一口没转发、转发串了型号,当场对不上。
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

    // const 查询也要记账,计数全 mutable。
    mutable int send_calls = 0;
    mutable int serialize_calls = 0;
    mutable int prepare_calls = 0;
    mutable int map_calls = 0;
    mutable int limit_calls = 0;
    mutable int force_calls = 0;
    lubancode::api::Request last_sent;
};

}  // namespace

using namespace lubancode::runtime::assembly;

TEST_CASE("叠层次序:索引段 -> 模型指令段 -> 魂段依次追加,魂压轴") {
    CapturingBackend inner;
    lubancode::tools::ToolRegistry registry;

    lubancode::agent::AgentProfile profile;
    profile.request.model = "glm-x";
    profile.request.reasoning_effort = "high";
    profile.system_prompt = "BASE-SYSTEM";
    profile.model_instructions = "MODEL-ONLY-INSTRUCTIONS";
    profile.soul = "<!-- 说明:给人看的,不注入 -->\nSOUL-BODY";
    profile.deferred_index_provider = [] { return std::string("INDEX-SEGMENT"); };
    lubancode::agent::Agent agent(inner, registry, std::move(profile));

    REQUIRE(agent.Run("问", lubancode::agent::TurnWiring{}).has_value());
    REQUIRE(inner.captured.size() == 1);
    const auto& sent = inner.captured.front();
    CHECK(sent.model == "glm-x");
    CHECK(sent.reasoning_effort == "high");

    const std::size_t base_pos = sent.system.find("BASE-SYSTEM");
    const std::size_t index_pos = sent.system.find("INDEX-SEGMENT");
    const std::size_t instructions_pos = sent.system.find("MODEL-ONLY-INSTRUCTIONS");
    const std::size_t soul_pos = sent.system.find("SOUL-BODY");
    REQUIRE(base_pos != std::string::npos);
    REQUIRE(index_pos != std::string::npos);
    REQUIRE(instructions_pos != std::string::npos);
    REQUIRE(soul_pos != std::string::npos);
    CHECK(base_pos < index_pos);
    CHECK(index_pos < instructions_pos);
    CHECK(instructions_pos < soul_pos);
}

TEST_CASE("全空透传:空指令/索引段与纯注释的魂不改动 system") {
    CapturingBackend inner;
    lubancode::tools::ToolRegistry registry;

    lubancode::agent::AgentProfile profile;
    profile.request.model = "glm-x";
    profile.request.reasoning_effort = "";
    profile.system_prompt = "BASE-SYSTEM";
    profile.model_instructions = "";
    profile.soul = "<!-- 默认魂:整个文件只有一行注释 -->";
    profile.deferred_index_provider = [] { return std::string(); };
    lubancode::agent::Agent agent(inner, registry, std::move(profile));

    REQUIRE(agent.Run("问", lubancode::agent::TurnWiring{}).has_value());

    CHECK(inner.captured.front().system == "BASE-SYSTEM");
    CHECK(inner.captured.front().reasoning_effort.empty());
    CHECK(inner.captured.front().model == "glm-x");
    CHECK(inner.captured.front().extra_body.empty());
}

TEST_CASE("会话中改皮上的活字段,下一次请求立即生效") {
    CapturingBackend inner;
    lubancode::tools::ToolRegistry registry;

    lubancode::agent::AgentProfile profile;
    profile.request.model = "glm-a";
    profile.request.reasoning_effort = "low";
    profile.system_prompt = "BASE-SYSTEM";
    lubancode::agent::Agent agent(inner, registry, std::move(profile));

    REQUIRE(agent.Run("问", lubancode::agent::TurnWiring{}).has_value());
    CHECK(inner.captured.front().model == "glm-a");
    CHECK(inner.captured.front().reasoning_effort == "low");

    // /model、/think 走的正门:SetRequestProfile 整份换(批四:五层后端
    // 退役后 shared_ptr<string> 旁路拆掉,同一份即时生效)。
    lubancode::api::RequestProfile request;
    request.model = "glm-b";
    request.reasoning_effort = "high";
    agent.SetRequestProfile(std::move(request));
    agent.SetModelInstructions("NEW-INSTRUCTIONS");
    // Soul 会话冻结单 P0(§5.1):首请求已锁定,SetSoul 被拒——魂的
    //"中途改完下一请求立即生效"旧合同随锁定边界废除,system 不再换魂。
    CHECK_FALSE(agent.SetSoul("NEW-SOUL"));

    REQUIRE(agent.Run("再问", lubancode::agent::TurnWiring{}).has_value());
    CHECK(inner.captured.back().model == "glm-b");
    CHECK(inner.captured.back().reasoning_effort == "high");
    CHECK(inner.captured.back().system.find("NEW-INSTRUCTIONS") != std::string::npos);
    CHECK(inner.captured.back().system.find("NEW-SOUL") == std::string::npos);
}

TEST_CASE("RebuildableBackend:构造/重建/析构不崩,对外引用地址不变") {
    lubancode::config::Config config;
    config.wire = lubancode::config::Wire::Anthropic;
    config.base_url = "http://127.0.0.1:9";
    config.auth_token = "test-key";

    auto rebuildable = std::make_unique<RebuildableBackend>(config);
    lubancode::api::Backend* address = rebuildable.get();
    rebuildable->Rebuild(config);
    rebuildable->Rebuild(config);
    CHECK(rebuildable.get() == address);
    rebuildable.reset();  // 真析构
}

// ---------------------------------------------------------------------------
// HC-08:包装层(RebuildableBackend)单层合同——预算映射与出站能力转发
// ---------------------------------------------------------------------------

namespace {

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

TEST_CASE("HC-08 单层合同:RebuildableBackend 六口全落内芯,返回值印记不丢") {
    auto inner = std::make_shared<CapabilityProbeBackend>();
    RebuildableBackend wrapped(inner);

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

    CHECK(inner->serialize_calls == 1);
    CHECK(inner->prepare_calls == 1);
    CHECK(inner->map_calls == 1);
    CHECK(inner->limit_calls == 1);
    CHECK(inner->force_calls == 1);
    CHECK(inner->send_calls == 1);
    CHECK(inner->last_sent.model == "probe-model");
}

TEST_CASE("HC-08 未装配合同:空内芯按接口默认给不可得/回退,send 报错不崩") {
    RebuildableBackend wrapped(std::shared_ptr<lubancode::api::Backend>{});

    lubancode::api::Request request = ProbeRequest();

    CHECK(wrapped.SerializeForDiagnostics(request).empty());
    const auto prepared = wrapped.PrepareWireRequest(request);
    CHECK(prepared.body.is_null());
    CHECK_FALSE(prepared.wire_map.has_value());
    CHECK(prepared.output_limit == 100);  // 回退 Request::max_tokens 原值(基类默认)
    CHECK_FALSE(prepared.output_limit_overridden);
    CHECK_FALSE(wrapped.BuildWireMessageMap(request).has_value());
    const auto limit = wrapped.GetEffectiveOutputLimit(request);
    CHECK(limit.tokens == 100);
    CHECK_FALSE(limit.overridden);
    wrapped.ForceMaxOutputTokensOverride(request, 64);  // no-op:不造键
    CHECK(request.extra_body.empty());
    REQUIRE_FALSE(wrapped.send_stream(request, [](const lubancode::api::StreamEvent&) {}).has_value());
}

// ---------------------------------------------------------------------------
// HC-08:换血不撕在途流——send_stream 挂门闩,Rebuild 照常返回;老流跑完
// 落旧内芯,之后的查询落新内芯。若转发持锁跨流式调用,Rebuild 会死等
// 在途流,这册当场挂到超时。
// ---------------------------------------------------------------------------

namespace {

class GateBackend : public lubancode::api::Backend {
public:
    explicit GateBackend(std::string tag)
        : tag_(std::move(tag)), release_gate_(release_.get_future()) {}

    const std::string& tag() const { return tag_; }

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)cancel;
        on_event(lubancode::api::MessageStart{tag_, request.model});
        started_.set_value();  // 到店打点:内芯已进 send_stream
        release_gate_.wait();  // 等放行令,模拟一段在途流
        on_event(lubancode::api::MessageDone{"end_turn", lubancode::api::Usage{}});
        return {};
    }

    std::string SerializeForDiagnostics(const lubancode::api::Request& request) const override {
        (void)request;
        return tag_;
    }

    void Release() { release_.set_value(); }
    void WaitStarted() { started_.get_future().wait(); }

private:
    std::string tag_;
    std::promise<void> started_;
    std::promise<void> release_;
    std::shared_future<void> release_gate_;
};

}  // namespace

TEST_CASE("HC-08 换血:在途流持旧内芯跑完,Rebuild 不等流,后续查询落新内芯") {
    auto old_inner = std::make_shared<GateBackend>("OLD-INNER");
    RebuildableBackend wrapped(old_inner);

    bool stream_ok = false;
    std::thread in_flight([&] {
        const auto sent = wrapped.send_stream(ProbeRequest(), [](const lubancode::api::StreamEvent&) {});
        stream_ok = sent.has_value();
    });
    old_inner->WaitStarted();  // 旧内芯已进流,还挂在门闩上

    // 换血:在途流未完,Rebuild 必须立刻返回——send_stream 的转发不许持锁。
    lubancode::config::Config config;
    config.wire = lubancode::config::Wire::Anthropic;
    config.base_url = "http://127.0.0.1:9";
    config.auth_token = "test-key";
    wrapped.Rebuild(config);

    old_inner->Release();
    in_flight.join();
    CHECK(stream_ok);  // 在途调用保住旧内芯,跑到完

    // 下一笔(此处用 const 查询代替,免真发网络)落新内芯:新叶给真
    // anthropic wire JSON,不再是 GateBackend 的 tag 印记。
    const std::string wire = wrapped.SerializeForDiagnostics(ProbeRequest());
    CHECK_FALSE(wire.empty());
    CHECK(wire != "OLD-INNER");
}
