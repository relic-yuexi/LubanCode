// tools::AgentTool:内置 "agent" 工具,用 FakeBackend 按脚本吐 StreamEvent
// (不碰真网络),验证:一轮纯文本直接返回;子代理调工具(用假工具)后
// 返回;超 max_turns 报 is_error;确认回调转发(父拒绝 -> 子内工具收到
// 拒绝);usage 累计到父回调;注册表排除(主表见 agent,子表不见,防递归)。

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端,跟 test_loop.cpp / test_compact.cpp 同一套写法:
// 每调一次 send_stream,按调用次序取下一组脚本吐出去,顺带记下收到的
// Request,方便断言。
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

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text, api::Usage usage = api::Usage{}) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", usage},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name,
                                             api::Usage usage = api::Usage{}) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", usage},
    };
}

}  // namespace

TEST_CASE("agent 工具:子代理一轮文本直接返回,Result.content 就是那段文本") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("子代理的结论:一共 12 个文件")};
    tools::ToolRegistry sub_registry;  // 子代理什么工具都不用

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    nlohmann::json input;
    input["prompt"] = "数一数文件个数";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content == "子代理的结论:一共 12 个文件");
    REQUIRE(backend.captured_requests.size() == 1);
    // 子代理系统提示里带子代理人格,不是主代理默认人格。
    CHECK(backend.captured_requests[0].system.find("子代理") != std::string::npos);
    CHECK(backend.captured_requests[0].system.find("/work/dir") != std::string::npos);
}

TEST_CASE("agent 工具:缺 prompt / 空 prompt 报 is_error,不发请求") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    {
        nlohmann::json input = nlohmann::json::object();
        const tools::Tool::Result result = agent_tool.execute(input);
        CHECK(result.is_error);
    }
    {
        nlohmann::json input;
        input["prompt"] = "";
        const tools::Tool::Result result = agent_tool.execute(input);
        CHECK(result.is_error);
    }
    CHECK(backend.captured_requests.empty());
}

TEST_CASE("agent 工具:子代理调了个工具再返回,on_sub_tool_start 钩子被调用,call_count 增加") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_1", "fake_tool"),
        TextOnlyScript("工具用完了,结论是 ok"),
    };
    tools::ToolRegistry sub_registry;
    auto* fake_tool_ptr = new FakeTool("fake_tool", tools::Tool::Result{"工具结果", false}, false);
    sub_registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    std::vector<std::string> sub_tool_starts;
    tools::AgentTool::Hooks hooks;
    hooks.on_sub_tool_start = [&](const std::string& name, const nlohmann::json&) {
        sub_tool_starts.push_back(name);
    };
    agent_tool.SetHooks(hooks);

    nlohmann::json input;
    input["prompt"] = "帮我用一下工具";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content == "工具用完了,结论是 ok");
    CHECK(fake_tool_ptr->call_count == 1);
    REQUIRE(sub_tool_starts.size() == 1);
    CHECK(sub_tool_starts[0] == "fake_tool");
}

TEST_CASE("agent 工具:超过 max_turns 报 is_error,说明里带上原因") {
    FakeBackend backend;
    for (int i = 0; i < 5; ++i) {
        backend.scripts.push_back(ToolUseScript("toolu_loop", "fake_tool"));
    }
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir", /*model=*/"", /*default_max_turns=*/3);

    nlohmann::json input;
    input["prompt"] = "死循环吧";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("子代理执行失败") != std::string::npos);
    CHECK(backend.captured_requests.size() == 3);  // 正好用满 max_turns 次请求

    // 入参给的 max_turns 能覆盖构造时的默认值。
    FakeBackend backend2;
    for (int i = 0; i < 5; ++i) {
        backend2.scripts.push_back(ToolUseScript("toolu_loop", "fake_tool"));
    }
    tools::ToolRegistry sub_registry2;
    sub_registry2.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));
    tools::AgentTool agent_tool2(backend2, sub_registry2, "/work/dir");  // 默认 15

    nlohmann::json input2;
    input2["prompt"] = "死循环吧";
    input2["max_turns"] = 2;
    const tools::Tool::Result result2 = agent_tool2.execute(input2);
    CHECK(result2.is_error);
    CHECK(backend2.captured_requests.size() == 2);
}

TEST_CASE("agent 工具:确认回调转发——父拒绝,子内工具收到拒绝,不真的执行") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_2", "dangerous_tool"),
        TextOnlyScript("好的,不执行了"),
    };
    tools::ToolRegistry sub_registry;
    auto* fake_tool_ptr = new FakeTool("dangerous_tool", tools::Tool::Result{"不该被看到的结果", false}, true);
    sub_registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    bool confirm_asked = false;
    tools::AgentTool::Hooks hooks;
    hooks.on_tool_confirm = [&](const std::string&, const nlohmann::json&) -> bool {
        confirm_asked = true;
        return false;  // 父级拒绝
    };
    agent_tool.SetHooks(hooks);

    nlohmann::json input;
    input["prompt"] = "帮我删点东西";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);  // 子代理自己没崩,只是那次工具调用被拒绝了
    CHECK(confirm_asked);
    CHECK(fake_tool_ptr->call_count == 0);  // 拒绝了就不该真的跑
}

TEST_CASE("agent 工具:usage 累计到父回调,含请求次数") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_usage", "fake_tool", api::Usage{100, 20, 0, 0}),
        TextOnlyScript("好了", api::Usage{50, 30, 0, 0}),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"工具结果", false}, false));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    std::vector<api::Usage> usages;
    tools::AgentTool::Hooks hooks;
    hooks.on_usage = [&](const api::Usage& usage) { usages.push_back(usage); };
    agent_tool.SetHooks(hooks);

    nlohmann::json input;
    input["prompt"] = "帮我用一下工具";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    REQUIRE(usages.size() == 2);  // 子代理内部两次独立请求,各触发一次
    CHECK(usages[0].input_tokens == 100);
    CHECK(usages[0].output_tokens == 20);
    CHECK(usages[1].input_tokens == 50);
    CHECK(usages[1].output_tokens == 30);
}

TEST_CASE("agent 工具:不设 Hooks 也不崩(默认允许确认、不打印、不转发 usage)") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_3", "fake_tool"),
        TextOnlyScript("跑完了"),
    };
    tools::ToolRegistry sub_registry;
    auto* fake_tool_ptr = new FakeTool("fake_tool", tools::Tool::Result{"ok", false}, true);  // needs_confirm
    sub_registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");  // 没调 SetHooks

    nlohmann::json input;
    input["prompt"] = "随便跑跑";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    // on_tool_confirm 没设,AgentLoop 默认视为允许(见 loop.cpp RunOneTool)。
    CHECK(fake_tool_ptr->call_count == 1);
}

TEST_CASE("注册表排除:子代理工具表不含 agent 自己,主表含 agent,防递归深度硬限 1") {
    FakeBackend backend;

    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"ok", false}, false));

    tools::ToolRegistry main_registry;
    main_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"ok", false}, false));
    main_registry.Register(std::make_unique<tools::AgentTool>(backend, sub_registry, "/work/dir"));

    CHECK(sub_registry.Find("agent") == nullptr);
    CHECK(main_registry.Find("agent") != nullptr);
    CHECK(main_registry.Find("agent")->name() == "agent");
}
