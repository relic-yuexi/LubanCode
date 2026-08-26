// 请求改写层的性状测试(骨架拆解批四改版):五层 override 后端退役后,
// 会话级请求策略(model/effort/模型指令/魂/延迟索引)由 Agent 拼请求时
// 就地生效——这里对最终发出的 Request 做断言:叠层先后(索引段 -> 模型
// 指令段 -> 魂段,魂压轴)一错,系统提示的段序就错,这类回归编译期看不
// 出来,只能靠对最终 Request 断言。RebuildableBackend 的稳定引用断言照旧。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "app/backend_stack.hpp"
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

}  // namespace

using namespace lubancode::app;

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
    agent.SetSoul("NEW-SOUL");

    REQUIRE(agent.Run("再问", lubancode::agent::TurnWiring{}).has_value());
    CHECK(inner.captured.back().model == "glm-b");
    CHECK(inner.captured.back().reasoning_effort == "high");
    CHECK(inner.captured.back().system.find("NEW-INSTRUCTIONS") != std::string::npos);
    CHECK(inner.captured.back().system.find("NEW-SOUL") != std::string::npos);
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
