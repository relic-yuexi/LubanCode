// 请求级恢复经 AgentLoop 的合同测试(监督器单 P0-1 验收线):本地假后端
// 造各路故障——连接复位(connect reset)、半截正文断流、半截 tool JSON 断流、
// 工具结果已提交后下一请求断流——每路都按合同收口:
//   * 首字节前/未提交断流可安全重发:从同一提交边界重来,attempt 连号;
//   * 半截 text 不拼两段正文:history 里只有成功那份完整消息;
//   * 半截 tool JSON 零执行:assembler 没收口,工具一个不跑;
//   * 已提交的 ToolResult 绝不重放:工具只执行一次,重发的只是模型请求;
//   * 重试用尽:结构化收口,文案如实写"已自动重试 N 次"。
// 主路与子路共用这一环(两路都经 AgentLoop::Run),这里钉主路;子路台账
// 回滚在 test_agent_progress 钉。

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 故障注入的假后端:每调一次 send_stream 按脚本走;脚本项 = (事件序列, 收场)。
// 收场为 nullopt = 流式成功;有值 = 返回那枚错误(半截断流:事件先吐,再断)。
class FlakyBackend : public api::Backend {
public:
    struct Take {
        std::vector<api::StreamEvent> events;
        std::optional<api::Error> error;
    };
    std::vector<Take> script;
    int calls = 0;
    std::vector<std::size_t> message_counts;  // 每次请求带的消息条数(重发边界对账)

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        message_counts.push_back(request.messages.size());
        const std::size_t idx = static_cast<std::size_t>(calls++);
        if (idx >= script.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FlakyBackend: 脚本用完了", 0});
        }
        for (const auto& event : script[idx].events) {
            on_event(event);
        }
        if (script[idx].error.has_value()) {
            return std::unexpected(*script[idx].error);
        }
        return {};
    }
};

class CountingTool : public tools::Tool {
public:
    std::string name() const override { return "counting_tool"; }
    std::string description() const override { return "counts calls"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json&) override {
        ++call_count;
        return {"第 " + std::to_string(call_count) + " 次执行", false};
    }
    int call_count = 0;
};

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolScript(const std::string& tool_id) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, "counting_tool"},
        api::ToolUseInputDelta{0, "{\"n\":1}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

api::Error ConnectReset() { return api::Error{api::ErrorKind::Network, "Connection reset by peer", 0}; }

struct Turn {
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter;
    Turn() : adapter("test", ids) { adapter.Start(); }
};

}  // namespace

TEST_CASE("断流恢复:首字节都没到就断,重发后整轮跑完,attempt 连号") {
    FlakyBackend backend;
    backend.script = {
        {/*空*/},
        {TextScript("网络回来后的完整回答"), std::nullopt},
    };
    backend.script[0].error = ConnectReset();
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    const auto result = loop.Run("问一句", wiring);
    REQUIRE(result.has_value());
    CHECK(backend.calls == 2);
    // 两次请求同一条消息(1 条 user):同一提交边界重发。
    REQUIRE(backend.message_counts.size() == 2);
    CHECK(backend.message_counts[0] == backend.message_counts[1]);
    REQUIRE(loop.History().size() == 2);
    bool saw_text = false;
    for (const auto& block : loop.History().back().content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            saw_text = true;
            CHECK(text->text == "网络回来后的完整回答");  // 半截的不算,只有完整那份
        }
    }
    CHECK(saw_text);
}

TEST_CASE("断流恢复:半截正文断流,history 只留成功那份,不拼两段") {
    FlakyBackend backend;
    backend.script = {
        {std::vector<api::StreamEvent>{
             api::MessageStart{"msg", "model"},
             api::TextDelta{"半截的话还没说完"},
         },
         ConnectReset()},
        {TextScript("重取后的完整回答"), std::nullopt},
    };
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    const auto result = loop.Run("继续说", wiring);
    REQUIRE(result.has_value());
    CHECK(backend.calls == 2);
    for (const auto& block : loop.History().back().content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block)) {
            CHECK(text->text.find("半截的话") == std::string::npos);  // 断流那份不落地
        }
    }
}

TEST_CASE("断流恢复:半截 tool JSON 断流,工具零执行;重发后完整消息才跑工具") {
    FlakyBackend backend;
    backend.script = {
        {std::vector<api::StreamEvent>{
             api::MessageStart{"msg", "model"},
             api::ToolUseStart{0, "toolu_1", "counting_tool"},
             api::ToolUseInputDelta{0, "{\"n\""},  // JSON 劈在半道
         },
         ConnectReset()},
        {ToolScript("toolu_2"), std::nullopt},
        {TextScript("工具跑完,收口"), std::nullopt},
    };
    tools::ToolRegistry registry;
    auto tool = std::make_unique<CountingTool>();
    CountingTool* counting = tool.get();
    registry.Register(std::move(tool));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    const auto result = loop.Run("跑个工具", wiring);
    REQUIRE(result.has_value());
    CHECK(counting->call_count == 1);  // 半截那次零执行,重发后完整消息跑一次
    CHECK(backend.calls == 3);
}

TEST_CASE("断流恢复:工具结果已提交,下一请求断流重试不重跑工具") {
    FlakyBackend backend;
    backend.script = {
        {ToolScript("toolu_1"), std::nullopt},   // 工具执行,结果入 history
        {/*空*/},                                 // 下一请求:连接复位
        {TextScript("重试后的最终结论"), std::nullopt},
    };
    backend.script[1].error = ConnectReset();
    tools::ToolRegistry registry;
    auto tool = std::make_unique<CountingTool>();
    CountingTool* counting = tool.get();
    registry.Register(std::move(tool));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    const auto result = loop.Run("跑完再说", wiring);
    REQUIRE(result.has_value());
    CHECK(counting->call_count == 1);  // 已提交的 ToolResult 绝不重放
    // 重发的第二次请求带着工具结果(3 条消息:1 user + 1 assistant + 1 tool_result)。
    REQUIRE(backend.message_counts.size() == 3);
    CHECK(backend.message_counts[1] == 3);
    CHECK(backend.message_counts[2] == 3);
}

TEST_CASE("断流恢复:重试用尽,结构化收口并如实写重试次数") {
    FlakyBackend backend;
    backend.script = {
        {/*空*/},
        {/*空*/},
        {/*空*/},
    };
    for (auto& take : backend.script) {
        take.error = ConnectReset();
    }
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    const auto result = loop.Run("必失败", wiring);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("已自动重试 2 次仍失败") != std::string::npos);
    CHECK(backend.calls == 3);  // 首发 + 2 次重试
}

TEST_CASE("恢复账:尝试相位从环里流出,started 连号、retrying 带稳定码") {
    FlakyBackend backend;
    backend.script = {
        {/*空*/},
        {TextScript("ok"), std::nullopt},
    };
    backend.script[0].error = ConnectReset();
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system"});
    Turn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    std::vector<std::pair<int, api::RequestAttemptPhase>> phases;
    std::string retry_reason;
    wiring.on_request_attempt = [&phases, &retry_reason](const api::ModelRequestAttempt& attempt,
                                                          api::RequestAttemptPhase phase) {
        phases.emplace_back(attempt.attempt, phase);
        if (phase == api::RequestAttemptPhase::Retrying) {
            retry_reason = attempt.error_code;
        }
    };
    REQUIRE(loop.Run("记个账", wiring).has_value());
    REQUIRE(phases.size() == 4);  // started/retrying + started/succeeded
    CHECK(phases[0].first == 1);
    CHECK(phases[0].second == api::RequestAttemptPhase::Started);
    CHECK(phases[1].second == api::RequestAttemptPhase::Retrying);
    CHECK(phases[2].first == 2);
    CHECK(phases[3].second == api::RequestAttemptPhase::Succeeded);
    CHECK(retry_reason == "network.error");
}
