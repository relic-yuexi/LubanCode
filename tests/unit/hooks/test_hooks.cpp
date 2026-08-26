// M9:hooks 系统测试。分两块:
//   1) config::ParseHooksConfig —— 纯函数,喂各种 JSON 形状进去,只查解析
//      结果/错误信息,不碰任何 IO。
//   2) tools::RunPreToolHooks / RunPostToolHooks —— 真跑 `cmd /c exit N`
//      (POSIX 下是 `exit N`)这样的真实命令,验证拦截语义(pre_tool 非零
//      退出拦截、post_tool 不拦截、matcher 不命中就不跑)。两平台都跑,
//      底下是 platform::RunShellCommand。
//   3) 一次 FakeBackend 驱动的完整 AgentLoop 往返,验证模型真的能在
//      tool_result 里看到"被 pre_tool 钩子拦截"的说明文字,继续对话。

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "tools/hooks.hpp"
#include "tools/tool.hpp"

#include "api/backend.hpp"
#include "api/types.hpp"
#include "agent/agent.hpp"
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
// 2) RunPreToolHooks / RunPostToolHooks:真跑命令(平台默认 shell:Windows
//    cmd.exe / POSIX /bin/sh,见 platform/process.hpp 的 RunShellCommand)。
//    钩子命令按平台给——exit N 两边 shell 都认,前缀不同而已。
// ---------------------------------------------------------------------------

namespace {
#ifdef _WIN32
constexpr const char* kHookExit1 = "cmd /c exit 1";
constexpr const char* kHookExit0 = "cmd /c exit 0";
#else
constexpr const char* kHookExit1 = "exit 1";
constexpr const char* kHookExit0 = "exit 0";
#endif
}  // namespace

TEST_CASE("RunPreToolHooks: 命中的钩子退出码非零,拦截,block_message 带退出码") {
    config::HooksConfig hooks;
    hooks.pre_tool.push_back(config::HookEntry{"write_file", kHookExit1});

    const auto outcome = tools::RunPreToolHooks(hooks, "write_file", nlohmann::json::object());
    CHECK(outcome.intercepted);
    CHECK(outcome.block_message.find("pre_tool") != std::string::npos);
    CHECK(outcome.block_message.find("1") != std::string::npos);
}

TEST_CASE("RunPreToolHooks: matcher 不命中,压根不跑,不拦截") {
    config::HooksConfig hooks;
    hooks.pre_tool.push_back(config::HookEntry{"run_command", kHookExit1});

    const auto outcome = tools::RunPreToolHooks(hooks, "write_file", nlohmann::json::object());
    CHECK_FALSE(outcome.intercepted);
}

TEST_CASE("RunPreToolHooks: 退出码为 0,放行") {
    config::HooksConfig hooks;
    hooks.pre_tool.push_back(config::HookEntry{"*", kHookExit0});

    const auto outcome = tools::RunPreToolHooks(hooks, "write_file", nlohmann::json::object());
    CHECK_FALSE(outcome.intercepted);
}

TEST_CASE("RunPostToolHooks: 退出码非零只警告,不影响调用方(没有返回值可拦)") {
    config::HooksConfig hooks;
    hooks.post_tool.push_back(config::HookEntry{"*", kHookExit1});
    // 这里只要不崩、能跑完就算过 —— post_tool 天生没有拦截能力,函数返回
    // void,没有别的可断言的地方,真正的"不拦截"由下面 FakeBackend 往返
    // 测试里再验证一次(post_tool 命中但 write_file 依然真的写了文件)。
    tools::RunPostToolHooks(hooks, "write_file", nlohmann::json::object(), tools::Tool::Result{"ok", false});
}

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

class FakeWriteTool : public tools::Tool {
public:
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "fake write_file"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }

    tools::Tool::Result execute(const nlohmann::json& input) override {
        ++call_count;
        last_input = input;
        return {"写好了", false};
    }

    int call_count = 0;
    nlohmann::json last_input = nlohmann::json::object();
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    agent::Callbacks callbacks;
    callbacks.on_pre_tool_hook = [](const std::string&, const std::string& name,
                                     const nlohmann::json&) -> std::optional<std::string> {
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    agent::Callbacks callbacks;
    bool hook_called = false;
    callbacks.on_pre_tool_hook = [&](const std::string&, const std::string&,
                                     const nlohmann::json&) -> std::optional<std::string> {
        hook_called = true;
        return std::nullopt;
    };
    bool post_hook_called = false;
    callbacks.on_post_tool_hook = [&](const std::string&, const std::string&, const nlohmann::json&,
                                      const tools::Tool::Result&) {
        post_hook_called = true;
    };

    const auto result = loop.Run("帮我写个文件", callbacks);
    REQUIRE(result.has_value());
    CHECK(hook_called);
    CHECK(post_hook_called);
    CHECK(write_tool->call_count == 1);  // 真的执行了
}

// ---------------------------------------------------------------------------
// 4) hooks 框架第三步:PreToolUse 完整表态 / 工具状态机相位 / updatedInput
//    重过 schema / PostToolUse 反馈追加。
// ---------------------------------------------------------------------------

TEST_CASE("RunOneTool 状态机: 相位序列 requested->checking_hook->running->done;deny 停在 blocked") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_10", "write_file"),
        TextOnlyScript("被拦了,不写了"),
    };
    tools::ToolRegistry registry;
    auto* write_tool = new FakeWriteTool();
    registry.Register(std::unique_ptr<FakeWriteTool>(write_tool));
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    std::vector<runtime::ToolPhase> phases;
    agent::Callbacks callbacks;
    callbacks.on_tool_phase = [&phases](const std::string&, const std::string&,
                                        runtime::ToolPhase phase) { phases.push_back(phase); };
    callbacks.on_pre_tool_use_hook = [](const std::string& /*tool_use_id*/, const std::string&,
                                        const nlohmann::json&) -> runtime::ToolHookDecision {
        runtime::ToolHookDecision decision;
        decision.decision = runtime::ToolHookDecision::Decision::Deny;
        decision.reason = "危险操作";
        decision.additional_context = {"换个目录再试"};
        return decision;
    };

    const auto result = loop.Run("写个文件", callbacks);
    REQUIRE(result.has_value());
    CHECK(write_tool->call_count == 0);  // 没执行
    REQUIRE(phases.size() == 2);
    CHECK(phases[0] == runtime::ToolPhase::CheckingHook);
    CHECK(phases[1] == runtime::ToolPhase::Blocked);  // 停在 blocked,不冒充跑过

    REQUIRE(backend.captured_requests.size() == 2);
    const auto& tool_result =
        std::get<api::ToolResultBlock>(backend.captured_requests[1].messages.back().content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content.find("危险操作") != std::string::npos);
    CHECK(tool_result.content.find("换个目录再试") != std::string::npos);  // additional_context 给模型看
}

TEST_CASE("RunOneTool: updatedInput 合法时按改写后的入参执行") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_11", "write_file"),
        TextOnlyScript("照改后的写"),
    };
    tools::ToolRegistry registry;
    auto* write_tool = new FakeWriteTool();
    registry.Register(std::unique_ptr<FakeWriteTool>(write_tool));
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    agent::Callbacks callbacks;
    callbacks.on_pre_tool_use_hook = [](const std::string& /*tool_use_id*/, const std::string&,
                                        const nlohmann::json& input) -> runtime::ToolHookDecision {
        runtime::ToolHookDecision decision;
        decision.decision = runtime::ToolHookDecision::Decision::Allow;
        nlohmann::json updated = input.is_null() ? nlohmann::json::object() : input;
        updated["path"] = "rewritten.txt";  // FakeWriteTool 的 schema 没约束,放行
        decision.updated_input = updated;
        return decision;
    };

    const auto result = loop.Run("写个文件", callbacks);
    REQUIRE(result.has_value());
    CHECK(write_tool->call_count == 1);
    CHECK(write_tool->last_input.value("path", std::string()) == "rewritten.txt");
}

namespace {

// schema 认真要求的假工具:required path(string)。
class StrictSchemaTool : public tools::Tool {
public:
    std::string name() const override { return "write_file"; }
    std::string description() const override { return "strict"; }
    nlohmann::json input_schema() const override {
        return nlohmann::json::parse(R"({
            "type": "object",
            "required": ["path"],
            "properties": {"path": {"type": "string"}, "content": {"type": "string"}}
        })");
    }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json& input) override {
        ++call_count;
        last_input = input;
        return {"写好了", false};
    }
    int call_count = 0;
    nlohmann::json last_input;
};

}  // namespace

TEST_CASE("RunOneTool: updatedInput 不过工具 schema = 打回并拦截,不按原参跑") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_12", "write_file"),
        TextOnlyScript("改写被拒"),
    };
    tools::ToolRegistry registry;
    auto* strict_tool = new StrictSchemaTool();
    registry.Register(std::unique_ptr<StrictSchemaTool>(strict_tool));
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    agent::Callbacks callbacks;
    callbacks.on_pre_tool_use_hook = [](const std::string& /*tool_use_id*/, const std::string&,
                                        const nlohmann::json&) -> runtime::ToolHookDecision {
        runtime::ToolHookDecision decision;
        decision.decision = runtime::ToolHookDecision::Decision::Allow;
        decision.updated_input = nlohmann::json::object({{"path", 12345}});  // 类型错:string 槽塞数字
        return decision;
    };

    const auto result = loop.Run("写个文件", callbacks);
    REQUIRE(result.has_value());
    CHECK(strict_tool->call_count == 0);  // 拦下,不悄悄按原参数执行

    const auto& tool_result =
        std::get<api::ToolResultBlock>(backend.captured_requests[1].messages.back().content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content.find("schema") != std::string::npos);
}

TEST_CASE("RunOneTool: PostToolUse 反馈追加进模型所见 tool_result") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_13", "write_file"),
        TextOnlyScript("收到反馈"),
    };
    tools::ToolRegistry registry;
    auto* write_tool = new FakeWriteTool();
    registry.Register(std::unique_ptr<FakeWriteTool>(write_tool));
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    agent::Callbacks callbacks;
    bool post_hook_called = false;
    callbacks.on_post_tool_hook = [&](const std::string&, const std::string&, const nlohmann::json&,
                                      const tools::Tool::Result&) {
        post_hook_called = true;  // 旧回调照旧在
    };
    callbacks.on_post_tool_use_hook = [](const std::string&, const std::string&, const nlohmann::json&,
                                         const tools::Tool::Result&) -> std::vector<std::string> {
        return {"格式检查通过", "行尾有多余空格"};
    };

    const auto result = loop.Run("写个文件", callbacks);
    REQUIRE(result.has_value());
    CHECK(write_tool->call_count == 1);
    CHECK(post_hook_called);

    const auto& tool_result =
        std::get<api::ToolResultBlock>(backend.captured_requests[1].messages.back().content[0]);
    CHECK_FALSE(tool_result.is_error);
    CHECK(tool_result.content.find("写好了") != std::string::npos);
    CHECK(tool_result.content.find("post-tool-use hook 追加] 格式检查通过") != std::string::npos);
    CHECK(tool_result.content.find("post-tool-use hook 追加] 行尾有多余空格") != std::string::npos);
}

TEST_CASE("RunOneTool: ask 决策时仍走确认回调(钩子不越过确认,确认层定夺)") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_14", "write_file"),
        TextOnlyScript("好"),
    };
    // needs_confirm=true 的假工具:ask 之后确认回调必须被问到。
    class ConfirmTool : public FakeWriteTool {
    public:
        bool needs_confirm() const override { return true; }
    };
    tools::ToolRegistry registry;
    auto* confirm_tool = new ConfirmTool();
    registry.Register(std::unique_ptr<ConfirmTool>(confirm_tool));
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    bool confirm_asked = false;
    agent::Callbacks callbacks;
    callbacks.on_pre_tool_use_hook =
        [](const std::string&, const std::string&, const nlohmann::json&) -> runtime::ToolHookDecision {
        runtime::ToolHookDecision decision;
        decision.decision = runtime::ToolHookDecision::Decision::Ask;
        decision.reason = "这命令得问问";
        return decision;
    };
    callbacks.on_tool_confirm = [&confirm_asked](const std::string&, const std::string&, const nlohmann::json&) {
        confirm_asked = true;
        return true;  // 用户(测试替身)允许
    };

    const auto result = loop.Run("写个文件", callbacks);
    REQUIRE(result.has_value());
    CHECK(confirm_asked);  // ask 决策传到了确认层,不是钩子直接放行
    CHECK(confirm_tool->call_count == 1);
}
