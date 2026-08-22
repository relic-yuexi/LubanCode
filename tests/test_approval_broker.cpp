// 异步审批通道(P2:显示系统剥离单"Broker 先行")的 AgentLoop 侧测试。
//
// 钉的是 RunOneTool/Run 的新回落规则与四态收账:
//   1. 只设 async:走 future,Accept/AcceptForSession 放行、Decline/Cancel 拒;
//   2. async 返回的 future 悬空收口(nullopt)= 拒绝,缺省文案不冒充用户拒绝,
//      on_tool_denial_text 照旧管文案;
//   3. 只设旧同步 on_tool_confirm:路径一字不变(子代理/PTC 转发、单测、
//      后台"没人可问"的短路全在这条路上);
//   4. 两个都设:async 优先;async 为空函数(不设)回落同步。
// 全部用 FakeBackend + 假工具,不碰终端。

#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

class FakeTool : public tools::Tool {
public:
    FakeTool(std::string name, tools::Tool::Result result, bool needs_confirm_flag)
        : name_(std::move(name)), result_(std::move(result)), needs_confirm_flag_(needs_confirm_flag) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return needs_confirm_flag_; }

    tools::Tool::Result execute(const nlohmann::json&) override {
        ++call_count;
        return result_;
    }

    int call_count = 0;

private:
    std::string name_;
    tools::Tool::Result result_;
    bool needs_confirm_flag_;
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 就绪 future:构造时带答案,WaitApproval 立即返回(终端"当场问完"的实现
// 形状)。
class ReadyFuture final : public agent::InteractionFuture {
public:
    explicit ReadyFuture(std::optional<agent::ApprovalResponse> response) : response_(std::move(response)) {}

    std::optional<agent::ApprovalResponse> WaitApproval() override { return response_; }

private:
    std::optional<agent::ApprovalResponse> response_;
};

}  // namespace

TEST_CASE("异步审批:Accept 放行,工具真执行") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_a", "dangerous_tool"),
        TextOnlyScript("办完了"),
    };
    tools::ToolRegistry registry;
    auto* tool = new FakeTool("dangerous_tool", tools::Tool::Result{"结果", false}, true);
    registry.Register(std::unique_ptr<FakeTool>(tool));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    int async_asked = 0;
    int sync_asked = 0;
    callbacks.on_tool_confirm_async = [&async_asked](const agent::ApprovalRequest& request) {
        ++async_asked;
        REQUIRE(request.tool_name == "dangerous_tool");
        agent::ApprovalResponse response;
        response.decision = agent::ApprovalDecision::Accept;
        return std::make_shared<ReadyFuture>(response);
    };
    callbacks.on_tool_confirm = [&sync_asked](const std::string&, const std::string&, const nlohmann::json&) {
        ++sync_asked;
        return true;
    };

    REQUIRE(loop.Run("去办", callbacks).has_value());
    CHECK(async_asked == 1);
    CHECK(sync_asked == 0);  // async 优先,同步回落路没走
    CHECK(tool->call_count == 1);
}

TEST_CASE("异步审批:Decline 拒绝,tool_result 是 is_error 且用缺省拒绝文案") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_d", "dangerous_tool"),
        TextOnlyScript("好,不办了"),
    };
    tools::ToolRegistry registry;
    auto* tool = new FakeTool("dangerous_tool", tools::Tool::Result{"不该被看到的结果", false}, true);
    registry.Register(std::unique_ptr<FakeTool>(tool));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    callbacks.on_tool_confirm_async = [](const agent::ApprovalRequest&) {
        agent::ApprovalResponse response;
        response.decision = agent::ApprovalDecision::Decline;
        response.reason = "用户看过了,不让";
        return std::make_shared<ReadyFuture>(response);
    };

    REQUIRE(loop.Run("去办", callbacks).has_value());
    CHECK(tool->call_count == 0);

    REQUIRE(loop.history().size() == 4);
    const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content == "用户拒绝执行该工具");
}

TEST_CASE("异步审批:悬空收口(nullopt)= 拒绝,缺省文案不冒充用户拒绝") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_c", "dangerous_tool"),
        TextOnlyScript("收到"),
    };
    tools::ToolRegistry registry;
    auto* tool = new FakeTool("dangerous_tool", tools::Tool::Result{"不该被看到的结果", false}, true);
    registry.Register(std::unique_ptr<FakeTool>(tool));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    // future 悬空收口:断开/打断/超时之后 Broker 统一按 cancel 收口的形状。
    callbacks.on_tool_confirm_async = [](const agent::ApprovalRequest&) {
        return std::make_shared<ReadyFuture>(std::nullopt);
    };

    REQUIRE(loop.Run("去办", callbacks).has_value());
    CHECK(tool->call_count == 0);

    const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content.find("悬空收口") != std::string::npos);
    CHECK(tool_result.content.find("用户拒绝") == std::string::npos);
}

TEST_CASE("异步审批:悬空收口的文案可由 on_tool_denial_text 接管") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_ct", "dangerous_tool"),
        TextOnlyScript("收到"),
    };
    tools::ToolRegistry registry;
    registry.Register(
        std::make_unique<FakeTool>("dangerous_tool", tools::Tool::Result{"不该被看到的结果", false}, true));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    callbacks.on_tool_confirm_async = [](const agent::ApprovalRequest&) {
        return std::make_shared<ReadyFuture>(std::nullopt);
    };
    callbacks.on_tool_denial_text = [](const std::string&, const std::string& name) {
        return "连接断开," + name + " 未等到回答,已按取消收口。";
    };

    REQUIRE(loop.Run("去办", callbacks).has_value());
    const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content == "连接断开,dangerous_tool 未等到回答,已按取消收口。");
}

TEST_CASE("异步审批:AcceptForSession 放行,Cancel 拒(四态各归各位)") {
    {
        FakeBackend backend;
        backend.scripts = {ToolUseScript("toolu_s1", "dangerous_tool"), TextOnlyScript("好")};
        tools::ToolRegistry registry;
        auto* tool = new FakeTool("dangerous_tool", tools::Tool::Result{"ok", false}, true);
        registry.Register(std::unique_ptr<FakeTool>(tool));
        agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
        agent::Callbacks callbacks;
        callbacks.on_tool_confirm_async = [](const agent::ApprovalRequest&) {
            agent::ApprovalResponse response;
            response.decision = agent::ApprovalDecision::AcceptForSession;
            return std::make_shared<ReadyFuture>(response);
        };
        REQUIRE(loop.Run("去办", callbacks).has_value());
        CHECK(tool->call_count == 1);
    }
    {
        FakeBackend backend;
        backend.scripts = {ToolUseScript("toolu_s2", "dangerous_tool"), TextOnlyScript("好")};
        tools::ToolRegistry registry;
        auto* tool = new FakeTool("dangerous_tool", tools::Tool::Result{"ok", false}, true);
        registry.Register(std::unique_ptr<FakeTool>(tool));
        agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
        agent::Callbacks callbacks;
        callbacks.on_tool_confirm_async = [](const agent::ApprovalRequest&) {
            agent::ApprovalResponse response;
            response.decision = agent::ApprovalDecision::Cancel;
            return std::make_shared<ReadyFuture>(response);
        };
        REQUIRE(loop.Run("去办", callbacks).has_value());
        CHECK(tool->call_count == 0);
        const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
        CHECK(tool_result.is_error);
    }
}

TEST_CASE("异步审批:async 回调返回空 future = 悬空收口,不炸") {
    FakeBackend backend;
    backend.scripts = {ToolUseScript("toolu_n", "dangerous_tool"), TextOnlyScript("好")};
    tools::ToolRegistry registry;
    auto* tool = new FakeTool("dangerous_tool", tools::Tool::Result{"ok", false}, true);
    registry.Register(std::unique_ptr<FakeTool>(tool));
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    callbacks.on_tool_confirm_async = [](const agent::ApprovalRequest&) { return nullptr; };
    REQUIRE(loop.Run("去办", callbacks).has_value());
    CHECK(tool->call_count == 0);
    const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content.find("悬空收口") != std::string::npos);
}

TEST_CASE("同步回落:不设 async 时旧 on_tool_confirm 路径一字不变") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_sync", "dangerous_tool"),
        TextOnlyScript("好"),
    };
    tools::ToolRegistry registry;
    auto* tool = new FakeTool("dangerous_tool", tools::Tool::Result{"结果", false}, true);
    registry.Register(std::unique_ptr<FakeTool>(tool));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
    bool sync_asked = false;
    callbacks.on_tool_confirm = [&](const std::string&, const std::string&, const nlohmann::json&) -> bool {
        sync_asked = true;
        return false;
    };

    REQUIRE(loop.Run("去办", callbacks).has_value());
    CHECK(sync_asked);
    CHECK(tool->call_count == 0);
    const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content == "用户拒绝执行该工具");
}

TEST_CASE("审批请求带工具名与入参(hook 改写后的 effective input)") {
    FakeBackend backend;
    backend.scripts = {ToolUseScript("toolu_in", "dangerous_tool"), TextOnlyScript("好")};
    tools::ToolRegistry registry;
    registry.Register(
        std::make_unique<FakeTool>("dangerous_tool", tools::Tool::Result{"ok", false}, true));
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;

    std::string seen_name;
    nlohmann::json seen_input;
    callbacks.on_tool_confirm_async = [&](const agent::ApprovalRequest& request) {
        seen_name = request.tool_name;
        seen_input = request.input;
        agent::ApprovalResponse response;
        response.decision = agent::ApprovalDecision::Accept;
        return std::make_shared<ReadyFuture>(response);
    };
    REQUIRE(loop.Run("去办", callbacks).has_value());
    CHECK(seen_name == "dangerous_tool");
    CHECK(seen_input.is_object());
}
