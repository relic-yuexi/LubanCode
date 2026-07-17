// AgentLoop 的核心行为,用 FakeBackend 按脚本吐 StreamEvent,不碰真网络:
// 一轮 text 直接结束;tool_use 一轮 -> 工具执行 -> 第二次请求历史里带
// tool_result -> end_turn;用户拒绝确认 -> tool_result 是 is_error;
// 超过轮数上限报错。

#include <doctest/doctest.h>

#include <cstdint>
#include <variant>
#include <vector>

#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端:每调一次 send_stream,按调用次序取下一组脚本吐出去,
// 顺带把收到的 Request 记下来,方便断言历史带没带上 tool_result。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event) override {
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

// 固定返回一个结果的假工具,记下被调用了几次、needs_confirm 能配置。
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

}  // namespace

TEST_CASE("一轮纯文本直接结束:一次请求,历史里 user+assistant 两条") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("你好呀")};
    tools::ToolRegistry registry;

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    std::string accumulated_text;
    agent::Callbacks callbacks;
    callbacks.on_text_delta = [&](const std::string& text) { accumulated_text += text; };

    const auto result = loop.Run("你好", callbacks);

    REQUIRE(result.has_value());
    CHECK(accumulated_text == "你好呀");
    CHECK(backend.captured_requests.size() == 1);
    REQUIRE(loop.history().size() == 2);
    CHECK(loop.history()[0].role == api::Role::User);
    CHECK(loop.history()[1].role == api::Role::Assistant);
}

TEST_CASE("tool_use 一轮 -> 执行工具 -> 第二次请求历史带 tool_result -> end_turn") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_1", "fake_tool"),
        TextOnlyScript("好了"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"工具结果", false}, false));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    std::vector<std::string> started_tools;
    agent::Callbacks callbacks;
    callbacks.on_tool_start = [&](const std::string& name, const nlohmann::json&) { started_tools.push_back(name); };

    const auto result = loop.Run("帮我用一下工具", callbacks);

    REQUIRE(result.has_value());
    CHECK(backend.captured_requests.size() == 2);
    REQUIRE(started_tools.size() == 1);
    CHECK(started_tools[0] == "fake_tool");

    // 历史:user、assistant(tool_use)、user(tool_result)、assistant(text)
    REQUIRE(loop.history().size() == 4);
    REQUIRE(loop.history()[2].content.size() == 1);
    REQUIRE(std::holds_alternative<api::ToolResultBlock>(loop.history()[2].content[0]));
    const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
    CHECK(tool_result.tool_use_id == "toolu_1");
    CHECK(tool_result.content == "工具结果");
    CHECK_FALSE(tool_result.is_error);

    // 第二次发出去的请求,messages 里也要能看到这条 tool_result。
    REQUIRE(backend.captured_requests[1].messages.size() == 3);
    const auto& second_request_tool_result_msg = backend.captured_requests[1].messages[2];
    REQUIRE(second_request_tool_result_msg.content.size() == 1);
    CHECK(std::holds_alternative<api::ToolResultBlock>(second_request_tool_result_msg.content[0]));
}

TEST_CASE("用户拒绝确认:工具不执行,tool_result 是 is_error") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_2", "dangerous_tool"),
        TextOnlyScript("好的,不执行了"),
    };
    tools::ToolRegistry registry;
    auto* fake_tool_ptr = new FakeTool("dangerous_tool", tools::Tool::Result{"不该被看到的结果", false}, true);
    registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    bool confirm_asked = false;
    agent::Callbacks callbacks;
    callbacks.on_tool_confirm = [&](const std::string&, const nlohmann::json&) -> bool {
        confirm_asked = true;
        return false;  // 拒绝
    };

    const auto result = loop.Run("帮我删点东西", callbacks);

    REQUIRE(result.has_value());
    CHECK(confirm_asked);
    CHECK(fake_tool_ptr->call_count == 0);  // 拒绝了就不该真的跑

    REQUIRE(loop.history().size() == 4);
    const auto& tool_result_msg = loop.history()[2];
    REQUIRE(tool_result_msg.content.size() == 1);
    const auto& tool_result = std::get<api::ToolResultBlock>(tool_result_msg.content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content.find("拒绝") != std::string::npos);
}

TEST_CASE("超过最大轮数报错") {
    FakeBackend backend;
    // 永远回 tool_use,模型一直要工具,逼近轮数上限。
    for (int i = 0; i < 5; ++i) {
        backend.scripts.push_back(ToolUseScript("toolu_loop", "fake_tool"));
    }
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt", 4096, /*max_turns=*/3);

    agent::Callbacks callbacks;
    const auto result = loop.Run("死循环吧", callbacks);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("轮数") != std::string::npos);
    CHECK(backend.captured_requests.size() == 3);  // 正好用满 max_turns 次请求
}

TEST_CASE("on_usage: 一次 Run() 内多次请求(工具调用来回),每次 MessageDone 都触发一次回调,可累计") {
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ToolUseStart{0, "toolu_usage", "fake_tool"},
            api::ToolUseInputDelta{0, "{}"},
            api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{100, 20}},
        },
        {
            api::MessageStart{"msg2", "model"},
            api::TextDelta{"好了"},
            api::ContentBlockDone{0},
            api::MessageDone{"end_turn", api::Usage{50, 30}},
        },
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"工具结果", false}, false));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    std::vector<api::Usage> usages;
    agent::Callbacks callbacks;
    callbacks.on_usage = [&](const api::Usage& usage) { usages.push_back(usage); };

    const auto result = loop.Run("帮我用一下工具", callbacks);

    REQUIRE(result.has_value());
    REQUIRE(usages.size() == 2);
    CHECK(usages[0].input_tokens == 100);
    CHECK(usages[0].output_tokens == 20);
    CHECK(usages[1].input_tokens == 50);
    CHECK(usages[1].output_tokens == 30);

    std::int64_t total_input = 0;
    std::int64_t total_output = 0;
    for (const auto& u : usages) {
        total_input += u.input_tokens;
        total_output += u.output_tokens;
    }
    CHECK(total_input == 150);
    CHECK(total_output == 50);
}

TEST_CASE("on_usage: 没设这个回调,不影响其余行为(可选回调,空着也不崩)") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("没事")};
    tools::ToolRegistry registry;

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;  // on_usage 没设

    const auto result = loop.Run("你好", callbacks);
    REQUIRE(result.has_value());
}

TEST_CASE("未知工具名:不崩,tool_result 里说明是未知工具") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_3", "no_such_tool"),
        TextOnlyScript("好的"),
    };
    tools::ToolRegistry registry;  // 空注册表,什么工具都没有

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;

    const auto result = loop.Run("用个不存在的工具", callbacks);

    REQUIRE(result.has_value());
    const auto& tool_result_msg = loop.history()[2];
    const auto& tool_result = std::get<api::ToolResultBlock>(tool_result_msg.content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content.find("未知工具") != std::string::npos);
}
