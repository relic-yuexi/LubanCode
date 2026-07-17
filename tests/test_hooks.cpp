// M9:hooks 系统测试。分两块:
//   1) config::ParseHooksConfig —— 纯函数,喂各种 JSON 形状进去,只查解析
//      结果/错误信息,不碰任何 IO。
//   2) tools::RunPreToolHooks / RunPostToolHooks —— 真跑 `cmd /c exit N`
//      这样的真实命令,验证拦截语义(pre_tool 非零退出拦截、post_tool 不拦
//      截、matcher 不命中就不跑)。这部分只在 Windows 下跑(RunProcess 眼下
//      只实现了 Windows)。
//   3) 一次 FakeBackend 驱动的完整 AgentLoop 往返,验证模型真的能在
//      tool_result 里看到"被 pre_tool 钩子拦截"的说明文字,继续对话。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "tools/hooks.hpp"
#include "tools/tool.hpp"

#include "api/backend.hpp"
#include "api/types.hpp"
#include "agent/loop.hpp"
#include "tools/registry.hpp"

using namespace lubancode;

// ---------------------------------------------------------------------------
// 1) ParseHooksConfig:纯函数,不碰 IO。
// ---------------------------------------------------------------------------

TEST_CASE("ParseHooksConfig: 完整的四段 hooks,matcher 缺省当 *") {
    const auto json = nlohmann::json::parse(R"({
        "pre_tool":  [{"matcher": "run_command", "command": "python check.py"}],
        "post_tool": [{"matcher": "*", "command": "echo post"}],
        "session_start": [{"command": "echo start"}],
        "session_end":   [{"command": "echo end"}]
    })");

    const auto result = config::ParseHooksConfig(json, "test.json");
    REQUIRE(result.has_value());
    REQUIRE(result->pre_tool.size() == 1);
    CHECK(result->pre_tool[0].matcher == "run_command");
    CHECK(result->pre_tool[0].command == "python check.py");
    REQUIRE(result->post_tool.size() == 1);
    CHECK(result->post_tool[0].matcher == "*");
    REQUIRE(result->session_start.size() == 1);
    CHECK(result->session_start[0].command == "echo start");
    REQUIRE(result->session_end.size() == 1);
    CHECK(result->session_end[0].command == "echo end");
    CHECK_FALSE(result->Empty());
}

TEST_CASE("ParseHooksConfig: 空 object,四个数组都是空的,Empty() 为真") {
    const auto json = nlohmann::json::object();
    const auto result = config::ParseHooksConfig(json, "test.json");
    REQUIRE(result.has_value());
    CHECK(result->pre_tool.empty());
    CHECK(result->post_tool.empty());
    CHECK(result->session_start.empty());
    CHECK(result->session_end.empty());
    CHECK(result->Empty());
}

TEST_CASE("ParseHooksConfig: pre_tool 缺 matcher 字段,缺省当 *") {
    const auto json = nlohmann::json::parse(R"({
        "pre_tool": [{"command": "echo no-matcher"}]
    })");
    const auto result = config::ParseHooksConfig(json, "test.json");
    REQUIRE(result.has_value());
    REQUIRE(result->pre_tool.size() == 1);
    CHECK(result->pre_tool[0].matcher == "*");
}

TEST_CASE("ParseHooksConfig: 缺 command 字段报错") {
    const auto json = nlohmann::json::parse(R"({
        "pre_tool": [{"matcher": "write_file"}]
    })");
    const auto result = config::ParseHooksConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("command") != std::string::npos);
}

TEST_CASE("ParseHooksConfig: 顶层不是 object 报错") {
    const auto json = nlohmann::json::parse(R"(["oops"])");
    const auto result = config::ParseHooksConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseHooksConfig: pre_tool 不是数组报错") {
    const auto json = nlohmann::json::parse(R"({"pre_tool": "not-an-array"})");
    const auto result = config::ParseHooksConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("pre_tool") != std::string::npos);
}

TEST_CASE("ParseHooksConfig: 数组元素不是 object 报错") {
    const auto json = nlohmann::json::parse(R"({"post_tool": ["oops"]})");
    const auto result = config::ParseHooksConfig(json, "test.json");
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ParseHooksConfig: session_start 写了 matcher 也不报错、不使用") {
    const auto json = nlohmann::json::parse(R"({
        "session_start": [{"matcher": "写了也没用", "command": "echo hi"}]
    })");
    const auto result = config::ParseHooksConfig(json, "test.json");
    REQUIRE(result.has_value());
    REQUIRE(result->session_start.size() == 1);
    CHECK(result->session_start[0].matcher.empty());
    CHECK(result->session_start[0].command == "echo hi");
}

// ---------------------------------------------------------------------------
// 2) RunPreToolHooks / RunPostToolHooks:真跑命令,只在 Windows 下测
//    (RunProcess 眼下只实现了 Windows,见 tools/process_exec.hpp)。
// ---------------------------------------------------------------------------

#ifdef _WIN32

TEST_CASE("RunPreToolHooks: 命中的钩子退出码非零,拦截,block_message 带退出码") {
    config::HooksConfig hooks;
    hooks.pre_tool.push_back(config::HookEntry{"write_file", "cmd /c exit 1"});

    const auto outcome = tools::RunPreToolHooks(hooks, "write_file", nlohmann::json::object());
    CHECK(outcome.intercepted);
    CHECK(outcome.block_message.find("pre_tool") != std::string::npos);
    CHECK(outcome.block_message.find("1") != std::string::npos);
}

TEST_CASE("RunPreToolHooks: matcher 不命中,压根不跑,不拦截") {
    config::HooksConfig hooks;
    hooks.pre_tool.push_back(config::HookEntry{"run_command", "cmd /c exit 1"});

    const auto outcome = tools::RunPreToolHooks(hooks, "write_file", nlohmann::json::object());
    CHECK_FALSE(outcome.intercepted);
}

TEST_CASE("RunPreToolHooks: 退出码为 0,放行") {
    config::HooksConfig hooks;
    hooks.pre_tool.push_back(config::HookEntry{"*", "cmd /c exit 0"});

    const auto outcome = tools::RunPreToolHooks(hooks, "write_file", nlohmann::json::object());
    CHECK_FALSE(outcome.intercepted);
}

TEST_CASE("RunPostToolHooks: 退出码非零只警告,不影响调用方(没有返回值可拦)") {
    config::HooksConfig hooks;
    hooks.post_tool.push_back(config::HookEntry{"*", "cmd /c exit 1"});
    // 这里只要不崩、能跑完就算过 —— post_tool 天生没有拦截能力,函数返回
    // void,没有别的可断言的地方,真正的"不拦截"由下面 FakeBackend 往返
    // 测试里再验证一次(post_tool 命中但 write_file 依然真的写了文件)。
    tools::RunPostToolHooks(hooks, "write_file", nlohmann::json::object(), tools::Tool::Result{"ok", false});
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// 3) FakeBackend 驱动的完整 AgentLoop 往返:验证模型真的能在 tool_result 里
//    看到"被 pre_tool 钩子拦截"的说明文字。跟 test_agent_tool.cpp 同一套
//    FakeBackend/FakeTool 写法。
// ---------------------------------------------------------------------------

namespace {

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

class FakeWriteTool : public tools::Tool {
public:
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "fake write_file"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }

    tools::Tool::Result execute(const nlohmann::json&) override {
        ++call_count;
        return {"写好了", false};
    }

    int call_count = 0;
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

TEST_CASE("AgentLoop 往返: on_pre_tool_hook 拦截后,模型在下一轮 tool_result 里看到拦截说明") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_1", "write_file"),
        TextOnlyScript("看到了,被拦截了,我不写了"),
    };
    tools::ToolRegistry registry;
    auto* write_tool = new FakeWriteTool();
    registry.Register(std::unique_ptr<FakeWriteTool>(write_tool));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    agent::Callbacks callbacks;
    callbacks.on_pre_tool_hook = [](const std::string& name, const nlohmann::json&) -> std::optional<std::string> {
        if (name == "write_file") {
            return "被 pre_tool 钩子拦截(退出码 1): 检测到危险操作";
        }
        return std::nullopt;
    };

    const auto result = loop.Run("帮我写个文件", callbacks);
    REQUIRE(result.has_value());
    CHECK(write_tool->call_count == 0);  // 真的没有执行

    // 第二次请求(工具结果那一轮)里,tool_result 的内容要带上拦截说明。
    REQUIRE(backend.captured_requests.size() == 2);
    const auto& second_request = backend.captured_requests[1];
    REQUIRE_FALSE(second_request.messages.empty());
    const auto& last_message = second_request.messages.back();
    REQUIRE_FALSE(last_message.content.empty());
    REQUIRE(std::holds_alternative<api::ToolResultBlock>(last_message.content[0]));
    const auto& tool_result = std::get<api::ToolResultBlock>(last_message.content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content.find("被 pre_tool 钩子拦截") != std::string::npos);
}

TEST_CASE("AgentLoop 往返: on_pre_tool_hook 放行(nullopt)时工具正常执行") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_2", "write_file"),
        TextOnlyScript("写好了"),
    };
    tools::ToolRegistry registry;
    auto* write_tool = new FakeWriteTool();
    registry.Register(std::unique_ptr<FakeWriteTool>(write_tool));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    agent::Callbacks callbacks;
    bool hook_called = false;
    callbacks.on_pre_tool_hook = [&](const std::string&, const nlohmann::json&) -> std::optional<std::string> {
        hook_called = true;
        return std::nullopt;
    };
    bool post_hook_called = false;
    callbacks.on_post_tool_hook = [&](const std::string&, const nlohmann::json&, const tools::Tool::Result&) {
        post_hook_called = true;
    };

    const auto result = loop.Run("帮我写个文件", callbacks);
    REQUIRE(result.has_value());
    CHECK(hook_called);
    CHECK(post_hook_called);
    CHECK(write_tool->call_count == 1);  // 真的执行了
}
