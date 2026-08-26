// 批二尾巴(骨架拆解批三):子代理 sub_callbacks 切到事件流。
// 编译边界:tools/ 住 engine 库不许 include runtime——TurnEventAdapter 的
// 翻译在宿主(app)侧做完,经 AgentTool::Hooks::event_callbacks 以
// agent::Callbacks 的中立形状递进去,agent_tool 里并轨。这份测试钉三桩:
//   1. 前台子代理的正文/思考增量、工具起止落进宿主轮的事件流(先于台账,
//      一份事件两路消费);
//   2. 不设 event_callbacks(后台任务/旧调用方)时事件流零事件,行为不变;
//   3. 工具条目不重复开(批二的 on_sub_tool_start 喂法已拆,并轨只此一路)。

#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "app/turn_runner.hpp"
#include "cli/context_tracker.hpp"
#include "cli/theme.hpp"
#include "cli/tool_display.hpp"
#include "cli/transcript.hpp"
#include "hooks/dispatcher.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "runtime/turn_runtime.hpp"
#include "tools/agent_tool.hpp"
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
        const std::atomic<bool>* cancel = nullptr) override {
        (void)cancel;
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
    std::string name() const override { return "fake_tool"; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"工具结果", false}; }
};

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, "fake_tool"},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

struct Collected {
    std::vector<runtime::ServerEvent> events;

    int Count(runtime::ServerEventKind kind) const {
        int count = 0;
        for (const auto& event : events) {
            if (event.kind == kind) {
                ++count;
            }
        }
        return count;
    }
    bool SawToolStarted(const std::string& tool_use_id) const {
        for (const auto& event : events) {
            if (event.kind != runtime::ServerEventKind::ItemStarted || event.item_kind != runtime::ItemKind::Tool) {
                continue;
            }
            const std::string id = event.payload.value("tool_use_id", std::string());
            if (id == tool_use_id) {
                return true;
            }
        }
        return false;
    }
    std::string JoinText() const {
        std::string out;
        for (const auto& event : events) {
            if (event.kind == runtime::ServerEventKind::ItemDelta && event.item_kind == runtime::ItemKind::Text) {
                out += event.text;
            }
        }
        return out;
    }
};

}  // namespace

TEST_CASE("批二尾巴:前台子代理的增量与工具起止先落事件流,再进台账") {
    FakeBackend sub_backend;
    sub_backend.scripts = {
        ToolUseScript("sub_t1"),
        TextScript("子代理的最终结论。"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>());
    FakeBackend main_backend;  // 主回合不跑,只借装配

    tools::ToolRegistry registry;
    registry.Register(std::make_unique<tools::AgentTool>(sub_backend, sub_registry, "/work/dir"));

    Collected collected;
    runtime::TurnEventAdapter adapter("test-thread", runtime::ProcessIdAuthority());
    adapter.Attach([&collected](const runtime::ServerEvent& event) { collected.events.push_back(event); });
    adapter.Start("turn-1");
    lubancode::agent::Callbacks event_callbacks = adapter.MakeCallbacks();

    cli::Theme theme;
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> expanded{false};
    cli::ToolDisplay display(transcript, theme, /*console=*/false, nullptr, &cancel_flag, &expanded);
    cli::StreamBodyTracker body(theme, false);
    cli::ContextTracker tracker(1000);
    runtime::TurnUsageStats stats;
    std::set<std::string> always_allowed;
    hooks::HookDispatcher hooks;

    app::TurnContext ctx;
    ctx.auto_confirm = false;
    ctx.always_allowed_tools = &always_allowed;
    ctx.theme = theme;
    ctx.context_tracker = &tracker;
    ctx.registry = &registry;
    ctx.hook_dispatcher = &hooks;
    app::TurnWiring wiring;
    wiring.usage_stats = &stats;
    wiring.display = &display;
    wiring.body_tracker = &body;
    wiring.cancel_flag = &cancel_flag;
    wiring.event_callbacks = &event_callbacks;
    const agent::Callbacks callbacks = app::BuildCallbacks(ctx, wiring);
    CHECK(callbacks.on_text_delta != nullptr);  // 装配本身没坏

    // 跑一只前台子代理:它的增量/工具事件应经 hooks.event_callbacks 并轨,
    // 出现在宿主轮的事件流里。
    const tools::Tool::Result result =
        registry.Find("agent")->execute(nlohmann::json{{"title", "事件流"}, {"prompt", "干完汇报"}});
    CHECK_FALSE(result.is_error);

    CHECK(collected.SawToolStarted("sub_t1"));                 // 子代理内层工具上了事件流
    CHECK(collected.JoinText().find("子代理的最终结论。") != std::string::npos);  // 正文增量上了
    // 工具条目不重复开:同 id 的 ItemStarted 恰一枚(并轨只此一路,批二的
    // on_sub_tool_start 喂法已拆)。
    int started_count = 0;
    for (const auto& event : collected.events) {
        if (event.kind == runtime::ServerEventKind::ItemStarted && event.item_kind == runtime::ItemKind::Tool) {
            if (event.payload.value("tool_use_id", std::string()) == "sub_t1") {
                ++started_count;
            }
        }
    }
    CHECK(started_count == 1);
}

TEST_CASE("批二尾巴:不设 event_callbacks 时子代理不上事件流(行为与从前一致)") {
    FakeBackend sub_backend;
    sub_backend.scripts = {TextScript("后台照常跑。")};
    tools::ToolRegistry sub_registry;
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<tools::AgentTool>(sub_backend, sub_registry, "/work/dir"));

    Collected collected;
    runtime::TurnEventAdapter adapter("test-thread", runtime::ProcessIdAuthority());
    adapter.Attach([&collected](const runtime::ServerEvent& event) { collected.events.push_back(event); });
    adapter.Start("turn-2");
    lubancode::agent::Callbacks event_callbacks = adapter.MakeCallbacks();
    (void)event_callbacks;  // 不递进 wiring:模拟后台任务/旧调用方

    cli::Theme theme;
    std::vector<cli::TranscriptItem> transcript;
    std::atomic<bool> cancel_flag{false};
    std::atomic<bool> expanded{false};
    cli::ToolDisplay display(transcript, theme, false, nullptr, &cancel_flag, &expanded);
    cli::StreamBodyTracker body(theme, false);
    cli::ContextTracker tracker(1000);
    runtime::TurnUsageStats stats;
    std::set<std::string> always_allowed;
    hooks::HookDispatcher hooks;

    app::TurnContext ctx;
    ctx.auto_confirm = false;
    ctx.always_allowed_tools = &always_allowed;
    ctx.theme = theme;
    ctx.context_tracker = &tracker;
    ctx.registry = &registry;
    ctx.hook_dispatcher = &hooks;
    app::TurnWiring wiring;
    wiring.usage_stats = &stats;
    wiring.display = &display;
    wiring.body_tracker = &body;
    wiring.cancel_flag = &cancel_flag;
    wiring.event_callbacks = nullptr;  // 不上事件流
    const agent::Callbacks callbacks = app::BuildCallbacks(ctx, wiring);
    (void)callbacks;

    const tools::Tool::Result result =
        registry.Find("agent")->execute(nlohmann::json{{"title", "无事件流"}, {"prompt", "干完汇报"}});
    CHECK_FALSE(result.is_error);
    // 事件流一个字节没进:Start 的那枚 TurnStarted 是适配器自己的开卷标记,
    // 除它之外零事件——子代理的增量/工具/usage 都没进来(不设零变化)。
    int beyond_turn_started = 0;
    for (const auto& event : collected.events) {
        if (event.kind != runtime::ServerEventKind::TurnStarted) {
            ++beyond_turn_started;
        }
    }
    CHECK(beyond_turn_started == 0);
}
