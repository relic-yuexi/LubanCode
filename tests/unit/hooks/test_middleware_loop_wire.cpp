// loop 的 PreRequest 接线缝(LuaHook 单 P0-B):on_pre_request_hooks 在每
// 次物理模型请求定形后、上 wire 前触发;放行照发,拦截整步明败;没配回调
// 一处不调(行为与从前逐字节一致)。快照由 runtime::BuildRequestSnapshotJson
// 投影——与 CLI/one-shot/app-server 三入口共用同一只函数。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <functional>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/types.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/middleware_runtime.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(与 test_step_lifecycle.cpp 同款最小面)。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::size_t calls = 0;
    std::vector<api::Request> sent;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        if (calls >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        sent.push_back(request);
        for (const auto& event : scripts[calls]) {
            on_event(event);
        }
        ++calls;
        return {};
    }
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

struct RecordedTurn {
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter;
    RecordedTurn() : adapter("test", ids) { adapter.Start(); }
};

}  // namespace

TEST_CASE("没配回调:一处不调,请求照发(与从前逐字节一致)") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("答")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    RecordedTurn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    REQUIRE(loop.Run("问", wiring).has_value());
    REQUIRE(backend.sent.size() == 1);
}

TEST_CASE("放行:每步一次,快照与预算齐;两步各得各的 step_id") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("答一")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    RecordedTurn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.turn_id = "turn-7";

    struct Call {
        std::string step_id;
        std::string turn_id;
        std::string model;
        std::string system;
        std::uint64_t window = 0;
        std::uint64_t reserve = 0;
    };
    std::vector<Call> calls;
    wiring.on_pre_request_hooks = [&calls](const std::string& step_id, const std::string& turn_id,
                                           const nlohmann::json& snapshot, std::uint64_t window,
                                           std::uint64_t reserve) {
        Call call;
        call.step_id = step_id;
        call.turn_id = turn_id;
        call.model = snapshot.value("model", std::string());
        call.system = snapshot.value("system", std::string());
        call.window = window;
        call.reserve = reserve;
        calls.push_back(std::move(call));
        return std::string();  // 放行
    };

    REQUIRE(loop.Run("问", wiring).has_value());
    REQUIRE(backend.sent.size() == 1);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].step_id == "step-1");
    CHECK(calls[0].turn_id == "turn-7");
    CHECK(calls[0].model == "test-model");
    CHECK(calls[0].system.find("system prompt") != std::string::npos);
    CHECK(calls[0].window > 0);   // 有效窗口透传(容量判断的预算)
    CHECK(calls[0].reserve > 0);  // 输出预留透传(§4.36 预算分开)
}

TEST_CASE("拦截:整步明败,请求不出门,理由透传") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("不该被看到")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    RecordedTurn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.on_pre_request_hooks = [](const std::string&, const std::string&, const nlohmann::json&,
                                     std::uint64_t, std::uint64_t) {
        return std::string("PreRequest 钩子拦下本次请求[recover]: 须压缩历史");
    };

    const auto outcome = loop.Run("问", wiring);
    REQUIRE(!outcome.has_value());
    CHECK(outcome.error().find("PreRequest 钩子拦下本次请求[recover]") != std::string::npos);
    CHECK(backend.sent.empty());  // 明拦不暗发
}
