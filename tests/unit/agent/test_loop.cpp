// AgentLoop 的核心行为,用 FakeBackend 按脚本吐 StreamEvent,不碰真网络:
// 一轮 text 直接结束;tool_use 一轮 -> 工具执行 -> 第二次请求历史里带
// tool_result -> end_turn;用户拒绝确认 -> tool_result 是 is_error;
// 超过步数上限报错。

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <variant>
#include <vector>

#include "agent/agent.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "agent/loop.hpp"
#include "tools/session_utils.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "tools/tool_search.hpp"  // ToolSearchTool/ToolInvokeTool:P1 代理对的两枚壳

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端:每调一次 send_stream,按调用次序取下一组脚本吐出去,
// 顺带把收到的 Request 记下来,方便断言历史带没带上 tool_result。
//
// cancel_after_event_index:取消测试专用。发完脚本里下标为这个值的事件就
// 假装"流被 ESC 掐断"，直接返回 Kind::Cancelled、后续脚本事件不再喂——不
// 依赖调用方传进来的 cancel 指针真的被置位,单纯模拟"两个具体后端在
// WriteCallback 里发现取消标志就中断传输"这件事在 AgentLoop 这一层看到
// 的效果,好单独测 AgentLoop 收到 Cancelled 之后怎么处理历史。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;
    std::optional<std::size_t> cancel_after_event_index;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        const auto& script = scripts[idx];
        for (std::size_t i = 0; i < script.size(); ++i) {
            on_event(script[i]);
            if (cancel_after_event_index.has_value() && i == *cancel_after_event_index) {
                return std::unexpected(api::Error{api::ErrorKind::Cancelled, "FakeBackend: 模拟取消", 0});
            }
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

// 执行的时候顺手把外面传进来的 cancel 标志置位,模拟"ESC 恰好在这个工具跑
// 的时候按下"——工具本身照常跑完、给出正常结果,取消是紧接着才被
// AgentLoop 发现的。
class CancellingTool : public tools::Tool {
public:
    explicit CancellingTool(std::atomic<bool>& flag) : flag_(flag) {}

    std::string name() const override { return "cancelling_tool"; }
    std::string description() const override { return "fake tool that flips the cancel flag"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }

    tools::Tool::Result execute(const nlohmann::json&) override {
        flag_.store(true);
        return {"跑完了", false};
    }

private:
    std::atomic<bool>& flag_;
};

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 同一条 assistant 消息里带 N 枚 tool_use 块(并行工具调用):一次模型
// 请求回来,块下标 0..N-1 各收一个 tool_use。计数语义测试用它钉"一个
// step 可含多枚工具调用"。
// 显示观察改吃事件流后的录音 sink(批二余款:Callbacks 显示回调退役,
// 测试从 ServerEvent 流里取正文/思考/工具起止/usage)。
class EventRecorder final : public runtime::EventSink {
public:
    void Emit(const runtime::ServerEvent& event) override {
        switch (event.kind) {
            case runtime::ServerEventKind::ItemDelta:
                if (event.item_kind == runtime::ItemKind::Text) {
                    text += event.text;
                } else if (event.item_kind == runtime::ItemKind::Thinking) {
                    thinking += event.text;
                }
                break;
            case runtime::ServerEventKind::ItemStarted:
                if (event.item_kind == runtime::ItemKind::Tool) {
                    started_tools.push_back(event.payload.value("tool_name", std::string()));
                }
                break;
            case runtime::ServerEventKind::UsageUpdated: {
                api::UsageReport report;
                report.usage.input_tokens = event.payload.value("input_tokens", std::int64_t{0});
                report.usage.output_tokens = event.payload.value("output_tokens", std::int64_t{0});
                report.step_index = event.payload.value("step_index", 0);
                report.provider_response_id = event.payload.value("provider_response_id", std::string());
                report.model = event.payload.value("model", std::string());
                reports.push_back(report);
                break;
            }
            default:
                break;
        }
    }
    std::string text;
    std::string thinking;
    std::vector<std::string> started_tools;
    std::vector<api::UsageReport> reports;
};

// 一轮录音装配:本地适配器 + 录音 sink,wiring.events 直连。
struct RecordedTurn {
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter;
    EventRecorder recorder;
    RecordedTurn() : adapter("test", ids) {
        adapter.Attach([this](const runtime::ServerEvent& event) { recorder.Emit(event); });
        adapter.Start();
    }
};

std::vector<api::StreamEvent> MultiToolUseScript(const std::vector<std::string>& tool_ids,
                                                  const std::string& tool_name) {
    std::vector<api::StreamEvent> events;
    events.push_back(api::MessageStart{"msg", "model"});
    for (std::size_t i = 0; i < tool_ids.size(); ++i) {
        events.push_back(api::ToolUseStart{static_cast<int>(i), tool_ids[i], tool_name});
        events.push_back(api::ToolUseInputDelta{static_cast<int>(i), "{}"});
        events.push_back(api::ContentBlockDone{static_cast<int>(i)});
    }
    events.push_back(api::MessageDone{"tool_use", api::Usage{}});
    return events;
}

}  // namespace

TEST_CASE("一轮纯文本直接结束:一次请求,历史里 user+assistant 两条") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("你好呀")};
    tools::ToolRegistry registry;

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    RecordedTurn turn;
    agent::TurnWiring callbacks;
    callbacks.events = &turn.adapter;

    const auto result = loop.Run("你好", callbacks);

    REQUIRE(result.has_value());
    CHECK(turn.recorder.text == "你好呀");
    CHECK(backend.captured_requests.size() == 1);
    // 计数语义(命名规范阶段 A):一条用户输入 = 一个 turn;纯文本直接
    // 收口,turn 内只有一个 step(一次模型请求),零次工具调用。
    CHECK(result->steps_used == 1);
    REQUIRE(loop.history().size() == 2);
    CHECK(loop.history()[0].role == api::Role::User);
    CHECK(loop.history()[1].role == api::Role::Assistant);
}

TEST_CASE("输出上限 unset:请求里不带 max_tokens;声明了才带(规格根因一)") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("好")};
    tools::ToolRegistry registry;
    agent::TurnWiring callbacks;

    // 默认构造(兼容门不传值)= unset:api::Request::max_tokens 是 nullopt,
    // 不再有一枚写死的 4096——chat/responses 端整个不发字段,交服务端默认。
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    REQUIRE(loop.Run("问", callbacks).has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK_FALSE(backend.captured_requests[0].max_tokens.has_value());
    CHECK(loop.runtime_profile().max_output_tokens == std::nullopt);

    // profile 声明了 8192:请求带上声明值,main 与子代理同一份。
    FakeBackend backend2;
    backend2.scripts = {TextOnlyScript("好")};
    agent::AgentRuntimeProfile profile;
    profile.max_output_tokens = 8192;
    profile.max_output_tokens_source = agent::OutputBudgetSource::ConfigFile;
    agent::Agent loop2(backend2, registry, [&] { agent::AgentProfile out; out.runtime = profile; out.request.model = "test-model"; out.system_prompt = "system prompt"; return out; }());
    REQUIRE(loop2.Run("问", callbacks).has_value());
    REQUIRE(backend2.captured_requests.size() == 1);
    REQUIRE(backend2.captured_requests[0].max_tokens.has_value());
    CHECK(*backend2.captured_requests[0].max_tokens == 8192);
    CHECK(loop2.runtime_profile().max_output_tokens == 8192);
}

TEST_CASE("finish_reason=length 且正文为空:续跑一次后仍空,结构化账交出去") {
    // 本地兼容端实测现场:reasoning 吃光输出预算,finish_reason=length,
    // 一个正文字都没有。规格根因四:先按 profile.length_continuations
    // (默认 1)自动续跑一轮——注入宿主标记(非用户输入),不重发原 prompt;
    // 续跑仍空才算耗尽,RunOutcome 交结构化账(上限/续数/usage/思考检查点)。
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ThinkingDelta{"想了很久很久"},
            api::ContentBlockDone{0},
            api::MessageDone{"max_tokens", api::Usage{0, 64, 0, 0, 64}},
        },
        {
            api::MessageStart{"msg", "model"},
            api::ThinkingDelta{"还在想"},
            api::ContentBlockDone{0},
            api::MessageDone{"max_tokens", api::Usage{0, 64, 0, 0, 64}},
        },
    };
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    const auto result = loop.Run("说点什么", callbacks);
    REQUIRE(result.has_value());
    CHECK(result->cancelled == false);
    CHECK(result->length_empty_output == true);
    CHECK(result->stop_reason == "max_tokens");
    CHECK(result->output_budget.exhausted == true);
    CHECK(result->output_budget.continuations_used == 1);
    CHECK(result->output_budget.continuation_limit == 1);
    CHECK(result->output_budget.usage_reported == true);   // usage 非零,报告过
    CHECK(result->output_budget.thinking_bytes > 0);       // 思考检查点有账
    CHECK_FALSE(result->output_budget.thinking_tail.empty());
    // 续跑那一步真的发了第二次请求,且历史里留着宿主标记(非用户输入)。
    REQUIRE(backend.captured_requests.size() == 2);
    REQUIRE(loop.history().size() == 4);  // user / assistant(thinking) / 标记 / assistant(thinking)
    bool saw_marker = false;
    for (const auto& block : loop.history()[2].content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block);
            text != nullptr && text->text.find("[系统标记,非用户输入]") != std::string::npos) {
            saw_marker = true;
        }
    }
    CHECK(saw_marker);
    // 不原样重发 prompt:第二次请求的末条 user 是标记,不是原始问题。
    const std::string second_tail = [&]() {
        std::string out;
        for (const auto& block : backend.captured_requests[1].messages.back().content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                out += text->text;
            }
        }
        return out;
    }();
    CHECK(second_tail.find("说点什么") == std::string::npos);

    // 对照:length 但有正文,不算"空输出",也不续跑。
    FakeBackend backend2;
    backend2.scripts = {{
        api::MessageStart{"msg", "model"},
        api::TextDelta{"说到一半被掐"},
        api::ContentBlockDone{0},
        api::MessageDone{"max_tokens", api::Usage{0, 64, 0, 0}},
    }};
    agent::Agent loop2(backend2, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    const auto result2 = loop2.Run("说点什么", callbacks);
    REQUIRE(result2.has_value());
    CHECK(result2->length_empty_output == false);
    CHECK(result2->output_budget.exhausted == false);
    CHECK(result2->output_budget.continuations_used == 0);
    CHECK(backend2.captured_requests.size() == 1);

    // 对照:end_turn 且正文为空(模型啥也没说但没撞预算),也不算。
    FakeBackend backend3;
    backend3.scripts = {{
        api::MessageStart{"msg", "model"},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{10, 2, 0, 0}},
    }};
    agent::Agent loop3(backend3, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    const auto result3 = loop3.Run("说点什么", callbacks);
    REQUIRE(result3.has_value());
    CHECK(result3->length_empty_output == false);
}

TEST_CASE("length 续跑救得回来:第一轮思考撞墙,标记后续轮交出正文") {
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ThinkingDelta{"先想"},
            api::ContentBlockDone{0},
            api::MessageDone{"max_tokens", api::Usage{}},  // usage 全零:未报告
        },
        TextOnlyScript("结论是 42"),
    };
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    const auto result = loop.Run("问个问题", callbacks);
    REQUIRE(result.has_value());
    CHECK(result->stop_reason == "end_turn");
    CHECK(result->length_empty_output == false);
    // 续跑救回来了:不算耗尽,但账上写清用了几次续跑、usage 未报告。
    CHECK(result->output_budget.exhausted == false);
    CHECK(result->output_budget.continuations_used == 1);
    CHECK(result->output_budget.usage_reported == false);
    CHECK(result->output_budget.thinking_bytes == std::size_t{6});  // "先想"(UTF-8 6 字节)
}

TEST_CASE("length 续跑次数为 0:撞墙即收场,不烧第二次") {
    FakeBackend backend;
    backend.scripts = {{
        api::MessageStart{"msg", "model"},
        api::ThinkingDelta{"想"},
        api::ContentBlockDone{0},
        api::MessageDone{"max_tokens", api::Usage{0, 8, 0, 0, 0}},
    }};
    tools::ToolRegistry registry;
    agent::AgentRuntimeProfile profile;
    profile.length_continuations = 0;  // 显式关掉续跑
    agent::Agent loop(backend, registry, [&] { agent::AgentProfile out; out.runtime = profile; out.request.model = "test-model"; out.system_prompt = "system prompt"; return out; }());
    agent::TurnWiring callbacks;
    const auto result = loop.Run("问", callbacks);
    REQUIRE(result.has_value());
    CHECK(result->length_empty_output == true);
    CHECK(result->output_budget.exhausted == true);
    CHECK(result->output_budget.continuations_used == 0);
    CHECK(backend.captured_requests.size() == 1);
}

TEST_CASE("max_tokens 撞墙但块里有完整 tool_use:信块不信帧,照常执行续轮") {
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ThinkingDelta{"先查一下"},
            api::ToolUseStart{0, "toolu_1", "fake_tool"},
            api::ToolUseInputDelta{0, "{}"},
            api::ContentBlockDone{0},
            api::MessageDone{"max_tokens", api::Usage{0, 100, 0, 0, 0}},  // 帧说 length,块是完整的
        },
        TextOnlyScript("查完了"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"工具结果", false}, false));
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    const auto result = loop.Run("查", callbacks);
    REQUIRE(result.has_value());
    CHECK(result->stop_reason == "end_turn");
    CHECK(result->length_empty_output == false);
    CHECK(result->output_budget.exhausted == false);   // 工具照跑、续轮交了正文,不算耗尽
    REQUIRE(backend.captured_requests.size() == 2);
}

TEST_CASE("tool_use 一轮 -> 执行工具 -> 第二次请求历史带 tool_result -> end_turn") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("toolu_1", "fake_tool"),
        TextOnlyScript("好了"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"工具结果", false}, false));

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    RecordedTurn turn;
    agent::TurnWiring callbacks;
    callbacks.events = &turn.adapter;

    const auto result = loop.Run("帮我用一下工具", callbacks);

    REQUIRE(result.has_value());
    CHECK(backend.captured_requests.size() == 2);
    // 计数语义:工具回填后再请求收正文,turn 内走了两步(step)。
    CHECK(result->steps_used == 2);
    REQUIRE(turn.recorder.started_tools.size() == 1);
    CHECK(turn.recorder.started_tools[0] == "fake_tool");

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

TEST_CASE("计数语义:一次 assistant 并行叫三件工具,仍是一步;工具回填后再收口才是第二步") {
    FakeBackend backend;
    backend.scripts = {
        MultiToolUseScript({"toolu_1", "toolu_2", "toolu_3"}, "fake_tool"),
        TextOnlyScript("三件都办完了"),
    };
    tools::ToolRegistry registry;
    auto* fake_tool_ptr = new FakeTool("fake_tool", tools::Tool::Result{"工具结果", false}, false);
    registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    agent::TurnWiring callbacks;
    const auto result = loop.Run("把三件事都办了", callbacks);

    // 一个 turn、两步:第一步(一次模型请求)带回了三枚工具调用,第二步
    // (回填后再请求)收正文。工具多开三枚不增步数,只增 tool_call_count。
    REQUIRE(result.has_value());
    CHECK(result->steps_used == 2);
    CHECK(backend.captured_requests.size() == 2);
    CHECK(fake_tool_ptr->call_count == 3);

    // 三枚 tool_use 挂在同一条 assistant 消息上(不是三步)。
    REQUIRE(loop.history().size() == 4);
    REQUIRE(loop.history()[1].content.size() == 3);
    // 回填的三枚 tool_result 也攒成同一条 user 消息。
    REQUIRE(loop.history()[2].content.size() == 3);
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    bool confirm_asked = false;
    agent::TurnWiring callbacks;
    callbacks.on_tool_confirm = [&](const std::string&, const std::string&, const nlohmann::json&) -> bool {
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

TEST_CASE("拒绝文案回调:设了 on_tool_denial_text 用回调的,没设用缺省'用户拒绝'") {
    // 后台子代理的拒绝是"没人可问、未预放行",不是用户拒绝(后台代理权限
    // 拒绝无告知单,2026-08-17)——文案必须能由回调层如实给,不然子代理
    // 照缺省文案汇报,最终报告写成"均被用户拒绝"。
    {
        FakeBackend backend;
        backend.scripts = {
            ToolUseScript("toolu_deny", "dangerous_tool"),
            TextOnlyScript("收到,如实汇报受阻"),
        };
        tools::ToolRegistry registry;
        registry.Register(
            std::make_unique<FakeTool>("dangerous_tool", tools::Tool::Result{"不该被看到的结果", false}, true));

        agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
        agent::TurnWiring callbacks;
        callbacks.on_tool_confirm = [](const std::string&, const std::string&, const nlohmann::json&) -> bool {
            return false;
        };
        callbacks.on_tool_denial_text = [](const std::string&, const std::string& name) {
            return "后台任务无法弹权限确认," + name + " 未预先放行,已被拒绝。";
        };
        REQUIRE(loop.Run("去写个文件", callbacks).has_value());

        REQUIRE(loop.history().size() == 4);
        const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
        CHECK(tool_result.is_error);
        CHECK(tool_result.content.find("后台任务无法弹权限确认") != std::string::npos);
        CHECK(tool_result.content.find("未预先放行") != std::string::npos);
        CHECK(tool_result.content.find("用户拒绝") == std::string::npos);
    }
    {
        // 不设回调:缺省文案不变(前台/普通会话的拒绝就是用户拒绝)。
        FakeBackend backend;
        backend.scripts = {
            ToolUseScript("toolu_deny2", "dangerous_tool"),
            TextOnlyScript("好的,不执行了"),
        };
        tools::ToolRegistry registry;
        registry.Register(
            std::make_unique<FakeTool>("dangerous_tool", tools::Tool::Result{"不该被看到的结果", false}, true));

        agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
        agent::TurnWiring callbacks;
        callbacks.on_tool_confirm = [](const std::string&, const std::string&, const nlohmann::json&) -> bool {
            return false;
        };
        REQUIRE(loop.Run("去写个文件", callbacks).has_value());

        const auto& tool_result = std::get<api::ToolResultBlock>(loop.history()[2].content[0]);
        CHECK(tool_result.content == "用户拒绝执行该工具");
    }
}

TEST_CASE("超过步数上限:预算耗尽不是错误,hit_step_limit 带 steps/stop_reason 交回") {
    FakeBackend backend;
    // 永远回 tool_use,模型一直要工具,逼近步数上限。
    for (int i = 0; i < 5; ++i) {
        backend.scripts.push_back(ToolUseScript("toolu_loop", "fake_tool"));
    }
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .runtime{.max_output_tokens = 4096, .max_steps_per_turn = 3}, .system_prompt = "system prompt"});

    agent::TurnWiring callbacks;
    const auto result = loop.Run("死循环吧", callbacks);

    // 0.30.x:步数耗尽从"报错"改为"预算耗尽"——value 分支交回,history 里
    // 留着到限为止的全部来回,调用方(子代理)按 budget_exhausted 收账、
    // 带走部分结果(规格"现场四")。
    REQUIRE(result.has_value());
    CHECK_FALSE(result->cancelled);
    CHECK(result->hit_step_limit);
    CHECK(result->steps_used == 3);
    CHECK(result->stop_reason == "tool_use");  // 最后一次应答的原始 stop reason
    CHECK(backend.captured_requests.size() == 3);  // 正好用满步数预算次请求
}

TEST_CASE("max_steps_per_turn=0(无上限):来回步数不受硬顶限制,跑到 end_turn 才停") {
    FakeBackend backend;
    // 10 轮工具调用,早就超过老默认值(25/100)里"该被拦下"的量级,但
    // 预算为 0 时压根没有硬顶,该一路跑到最后一句 end_turn 才停。
    for (int i = 0; i < 10; ++i) {
        backend.scripts.push_back(ToolUseScript("toolu_" + std::to_string(i), "fake_tool"));
    }
    backend.scripts.push_back(TextOnlyScript("终于收尾了"));
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .runtime{.max_output_tokens = 4096, .max_steps_per_turn = 0}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    const auto result = loop.Run("跑很多轮", callbacks);

    REQUIRE(result.has_value());
    CHECK(backend.captured_requests.size() == 11);  // 10 次工具轮 + 1 次收尾,没被硬顶掐断
}

TEST_CASE("默认步数预算(不传参数)= 无上限:多步工具调用不报错,system 里也不会出现步数将尽提醒") {
    FakeBackend backend;
    for (int i = 0; i < 5; ++i) {
        backend.scripts.push_back(ToolUseScript("toolu_d" + std::to_string(i), "fake_tool"));
    }
    backend.scripts.push_back(TextOnlyScript("收尾"));
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});  // 不传步数预算,用默认值
    agent::TurnWiring callbacks;
    const auto result = loop.Run("跑几轮", callbacks);

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 6);
    for (const auto& req : backend.captured_requests) {
        CHECK(req.system.find("步数将尽") == std::string::npos);
    }
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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    RecordedTurn turn;
    agent::TurnWiring callbacks;
    callbacks.events = &turn.adapter;

    const auto result = loop.Run("帮我用一下工具", callbacks);

    REQUIRE(result.has_value());
    const std::vector<api::UsageReport>& reports = turn.recorder.reports;
    REQUIRE(reports.size() == 2);
    // 逐笔带身份:步号、请求 id、模型——逐步流水账有键可落。
    CHECK(reports[0].step_index == 0);
    CHECK(reports[0].provider_response_id == "msg");
    CHECK(reports[0].model == "model");
    CHECK(reports[1].step_index == 1);
    CHECK(reports[1].provider_response_id == "msg2");
    CHECK(reports[0].usage.input_tokens == 100);
    CHECK(reports[0].usage.output_tokens == 20);
    CHECK(reports[1].usage.input_tokens == 50);
    CHECK(reports[1].usage.output_tokens == 30);

    std::int64_t total_input = 0;
    std::int64_t total_output = 0;
    for (const auto& report : reports) {
        total_input += report.usage.input_tokens;
        total_output += report.usage.output_tokens;
    }
    CHECK(total_input == 150);
    CHECK(total_output == 50);
}

TEST_CASE("on_usage: 没设这个回调,不影响其余行为(可选回调,空着也不崩)") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("没事")};
    tools::ToolRegistry registry;

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;  // on_usage 没设

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

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;

    const auto result = loop.Run("用个不存在的工具", callbacks);

    REQUIRE(result.has_value());
    const auto& tool_result_msg = loop.history()[2];
    const auto& tool_result = std::get<api::ToolResultBlock>(tool_result_msg.content[0]);
    CHECK(tool_result.is_error);
    CHECK(tool_result.content.find("未知工具") != std::string::npos);
}

// ---------------------------------------------------------------------------
// M10:ESC 打断
// ---------------------------------------------------------------------------

TEST_CASE("ESC 打断:流中途取消,半截文本入历史带打断标注,Run() 正常返回(不是错误)") {
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::TextDelta{"半截话"},
        },
    };
    backend.cancel_after_event_index = 1;  // TextDelta 之后就假装被取消,ContentBlockDone/MessageDone 永远不会来
    tools::ToolRegistry registry;

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    std::atomic<bool> cancel_flag{false};
    agent::TurnWiring callbacks;
    const auto result = loop.Run("问点啥", callbacks, &cancel_flag);

    REQUIRE(result.has_value());  // Cancelled 不当错误报
    CHECK(result->cancelled);

    REQUIRE(loop.history().size() == 2);
    const auto& assistant_msg = loop.history()[1];
    CHECK(assistant_msg.role == api::Role::Assistant);
    REQUIRE(assistant_msg.content.size() == 2);  // 半截文本 + 打断标注,两个独立文本块
    REQUIRE(std::holds_alternative<api::TextBlock>(assistant_msg.content[0]));
    CHECK(std::get<api::TextBlock>(assistant_msg.content[0]).text == "半截话");
    REQUIRE(std::holds_alternative<api::TextBlock>(assistant_msg.content[1]));
    CHECK(std::get<api::TextBlock>(assistant_msg.content[1]).text.find("打断") != std::string::npos);
}

TEST_CASE("ESC 打断:什么都还没流出来就取消,assistant 消息只有打断标注,不崩") {
    FakeBackend backend;
    backend.scripts = {
        {api::MessageStart{"msg", "model"}},
    };
    backend.cancel_after_event_index = 0;
    tools::ToolRegistry registry;

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    std::atomic<bool> cancel_flag{false};
    agent::TurnWiring callbacks;
    const auto result = loop.Run("问点啥", callbacks, &cancel_flag);

    REQUIRE(result.has_value());
    CHECK(result->cancelled);
    REQUIRE(loop.history().size() == 2);
    REQUIRE(loop.history()[1].content.size() == 1);
    CHECK(std::get<api::TextBlock>(loop.history()[1].content[0]).text.find("打断") != std::string::npos);
}

TEST_CASE("ESC 打断:工具执行后才发现取消,正在跑的工具结果照常成对入历史,后续工具补合成结果") {
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ToolUseStart{0, "toolu_a", "cancelling_tool"},
            api::ToolUseInputDelta{0, "{}"},
            api::ContentBlockDone{0},
            api::ToolUseStart{1, "toolu_b", "fake_tool"},
            api::ToolUseInputDelta{1, "{}"},
            api::ContentBlockDone{1},
            api::MessageDone{"tool_use", api::Usage{}},
        },
    };
    tools::ToolRegistry registry;
    std::atomic<bool> cancel_flag{false};
    registry.Register(std::make_unique<CancellingTool>(cancel_flag));
    auto* fake_tool_ptr = new FakeTool("fake_tool", tools::Tool::Result{"不该跑到", false}, false);
    registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    const auto result = loop.Run("跑两个工具", callbacks, &cancel_flag);

    REQUIRE(result.has_value());
    CHECK(result->cancelled);
    CHECK(backend.captured_requests.size() == 1);  // 打断后直接停,没有第二次请求
    CHECK(fake_tool_ptr->call_count == 0);          // 排在后面的工具真的没执行

    // 历史:user、assistant(2 个 tool_use)、user(2 个 tool_result)——配对完整
    REQUIRE(loop.history().size() == 3);
    const auto& tool_result_msg = loop.history()[2];
    REQUIRE(tool_result_msg.content.size() == 2);

    const auto& r0 = std::get<api::ToolResultBlock>(tool_result_msg.content[0]);
    CHECK(r0.tool_use_id == "toolu_a");
    CHECK(r0.content == "跑完了");
    CHECK_FALSE(r0.is_error);  // 真跑完的那个,结果照常,不因为紧接着被取消就变成错误

    const auto& r1 = std::get<api::ToolResultBlock>(tool_result_msg.content[1]);
    CHECK(r1.tool_use_id == "toolu_b");
    CHECK(r1.is_error);
    CHECK(r1.content.find("打断") != std::string::npos);
}

TEST_CASE("没有取消:cancel 指针传了但没置位,行为跟不传一模一样") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("正常聊天")};
    tools::ToolRegistry registry;

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    std::atomic<bool> cancel_flag{false};
    agent::TurnWiring callbacks;
    const auto result = loop.Run("你好", callbacks, &cancel_flag);

    REQUIRE(result.has_value());
    CHECK_FALSE(result->cancelled);
    REQUIRE(loop.history().size() == 2);
}

TEST_CASE("防御:stop_reason 不是 tool_use(帧丢了/说成 end_turn)但消息里有 tool_use 块,照样执行工具并成对喂回") {
    FakeBackend backend;
    backend.scripts = {
        {
            // 模型明明发了 tool_use 块,stop_reason 却是 end_turn(或者流坏了
            // 压根没有 MessageDone,stop_reason 是空串)——不能当 end_turn 收场,
            // 不然历史里留一条没有 tool_result 配对的 tool_use,下一轮 400。
            api::MessageStart{"msg", "model"},
            api::ToolUseStart{0, "toolu_x", "fake_tool"},
            api::ToolUseInputDelta{0, "{}"},
            api::ContentBlockDone{0},
            api::MessageDone{"end_turn", api::Usage{}},
        },
        TextOnlyScript("好了"),
    };
    tools::ToolRegistry registry;
    auto* fake_tool_ptr = new FakeTool("fake_tool", tools::Tool::Result{"工具结果", false}, false);
    registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    const auto result = loop.Run("用工具", callbacks);

    REQUIRE(result.has_value());
    CHECK(fake_tool_ptr->call_count == 1);          // 工具真的跑了
    CHECK(backend.captured_requests.size() == 2);   // 结果喂回去又请求了一轮

    // 历史:user、assistant(tool_use)、user(tool_result)、assistant(text),配对完整。
    REQUIRE(loop.history().size() == 4);
    REQUIRE(std::holds_alternative<api::ToolResultBlock>(loop.history()[2].content[0]));
    CHECK(std::get<api::ToolResultBlock>(loop.history()[2].content[0]).tool_use_id == "toolu_x");
}

TEST_CASE("上下文硬上限:裁剪与截断后仍超限(单条用户输入就超大),报错不发请求") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("不该走到这里")};
    tools::ToolRegistry registry;

    // max_context_chars 压到 200,用户输入 10 倍于此——裁不动也截不动
    // (截断只动 tool_result),该明确报错,不能把超大请求发出去。
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .runtime{.max_output_tokens = 4096, .max_steps_per_turn = 25, .max_context_chars = 200}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    const auto result = loop.Run(std::string(2000, 'x'), callbacks);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("上下文") != std::string::npos);
    CHECK(backend.captured_requests.empty());  // 一次请求都没发出去
}

TEST_CASE("token 窗口预检:短词长串加输出预留越窗,本机拦下且不发请求") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("不该走到这里")};
    tools::ToolRegistry registry;
    agent::Agent loop(
        backend, registry,
        agent::AgentProfile{.request{.model = "test-model"},
                            .runtime{.max_output_tokens = 8192,
                                     .max_steps_per_turn = 25,
                                     .max_context_chars = 200000,
                                     .context_window_tokens = 32768},
                            .system_prompt = "sys"});

    std::string input;
    input.reserve(60000);
    for (int i = 0; i < 30000; ++i) input += "a ";
    int pressure_calls = 0;
    agent::AgentWiring wiring;
    wiring.on_context_pressure = [&pressure_calls](const agent::ContextPressure&) { ++pressure_calls; };
    loop.SetWiring(std::move(wiring));
    const auto result = loop.Run(input, agent::TurnWiring{});

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("上下文预检未通过") != std::string::npos);
    CHECK(result.error().find("当前消息本身已装不下") != std::string::npos);
    CHECK(backend.captured_requests.empty());
    CHECK(pressure_calls == 0);  // 不可压的新消息不白跑 compact
}

TEST_CASE("token 窗口预检:短词输入在线内不过度拦截") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("收到")};
    tools::ToolRegistry registry;
    agent::Agent loop(
        backend, registry,
        agent::AgentProfile{.request{.model = "test-model"},
                            .runtime{.max_output_tokens = 8192,
                                     .max_steps_per_turn = 25,
                                     .max_context_chars = 200000,
                                     .context_window_tokens = 32768},
                            .system_prompt = "sys"});
    std::string input;
    for (int i = 0; i < 20000; ++i) input += "a ";

    const auto result = loop.Run(input, agent::TurnWiring{});
    REQUIRE(result.has_value());
    CHECK(backend.captured_requests.size() == 1);
}

TEST_CASE("token 窗口预检:大尺寸截图按像素折账,不再按 base64 字节误拦") {
    // 用户炸单的形状:3072x1918 整窗截图(base64 约 2.6MB 量级)。老字节
    // 口径 100000 字符 base64 就折 25000 token,加输出预留 8192 越窗拦下;
    // 像素口径只记 7845,该发就发。
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("收到")};
    tools::ToolRegistry registry;
    agent::Agent loop(
        backend, registry,
        agent::AgentProfile{.request{.model = "test-model"},
                            .runtime{.max_output_tokens = 8192,
                                     .max_steps_per_turn = 25,
                                     .max_context_chars = 200000,
                                     .context_window_tokens = 32768},
                            .system_prompt = "sys"});
    api::Message image_message;
    image_message.role = api::Role::User;
    image_message.content.push_back(
        api::ImageBlock{"image/png", std::string(100000, 'A'), "full-window.png", 3072, 1918});

    const auto result = loop.Run(std::move(image_message), agent::TurnWiring{});
    REQUIRE(result.has_value());
    CHECK(backend.captured_requests.size() == 1);
}

TEST_CASE("token 窗口预检:CJK、代码长串与 base64 图片都计入固定输入") {
    const auto rejected_without_request = [](const char* shape, api::Message message) {
        const std::string shape_name(shape);
        CAPTURE(shape_name);
        FakeBackend backend;
        backend.scripts = {TextOnlyScript("不该发送")};
        tools::ToolRegistry registry;
        agent::Agent loop(
            backend, registry,
            agent::AgentProfile{.request{.model = "test-model"},
                                .runtime{.max_output_tokens = 8192,
                                         .max_steps_per_turn = 25,
                                         .max_context_chars = 300000,
                                         .context_window_tokens = 32768},
                                .system_prompt = "sys"});
        const auto result = loop.Run(std::move(message), agent::TurnWiring{});
        CHECK_FALSE(result.has_value());
        CHECK(backend.captured_requests.empty());
    };

    api::Message cjk;
    cjk.role = api::Role::User;
    std::string cjk_text;
    for (int i = 0; i < 17000; ++i) cjk_text += "汉";
    cjk.content.push_back(api::TextBlock{std::move(cjk_text)});
    rejected_without_request("CJK", std::move(cjk));

    api::Message code;
    code.role = api::Role::User;
    std::string code_text;
    for (int i = 0; i < 30000; ++i) code_text += "x();";
    code.content.push_back(api::TextBlock{std::move(code_text)});
    rejected_without_request("code", std::move(code));

    api::Message image;
    image.role = api::Role::User;
    image.content.push_back(api::ImageBlock{"image/png", std::string(120000, 'A'), "large.png"});
    rejected_without_request("base64", std::move(image));
}

// ---------------------------------------------------------------------------
// 预检应急预留(派工单 §四):常规输出预留装不下、当前消息自己装得下时,
// 收窄本请求输出上限放行并注入一次收尾交代——同任务续跑,不叫用户开新
// 会话;应急也装不下才稳定报错,错误文案带"现场不丢"的交接说明。
// ---------------------------------------------------------------------------

namespace {

// 按次序返回不同结果的假工具:第一次 16000 token、第二次 14500 token 的
// "a a a ..."——构造"历史累积将窗挤满、当前消息自己仍装得下"的形状。
class GrowingResultTool : public tools::Tool {
public:
    std::vector<std::string> results;
    int call_count = 0;

    std::string name() const override { return "growing_tool"; }
    std::string description() const override { return "fake tool for preflight tests"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }

    tools::Tool::Result execute(const nlohmann::json&) override {
        const std::size_t idx = static_cast<std::size_t>(call_count);
        ++call_count;
        std::string text;
        const std::size_t words = idx < results.size() ? results[idx].size() / 2 : 1;
        for (std::size_t i = 0; i < words; ++i) {
            text += "a ";
        }
        return {text, false};
    }
};

// "a a a" 短词串按空白折 token:词数就是估算 token 数。
std::string WordyText(std::size_t words) {
    std::string text;
    text.reserve(words * 2);
    for (std::size_t i = 0; i < words; ++i) {
        text += "a ";
    }
    return text;
}

}  // namespace

TEST_CASE("预检应急预留: 常规预留装不下时收窄放行,注入一次收尾交代(派工单 §四)") {
    // 缩小版事故形状:窗口 32768,输出上限声明 16384(半扇窗),工具结果
    // 16000 token——第二份请求 16000 + 16384 + 512 越窗,应急预留 2048
    // (32768/16)装得下:请求照发,max_tokens 收窄,尾消息带收尾交代。
    FakeBackend backend;
    backend.scripts = {ToolUseScript("t1", "growing_tool"), TextOnlyScript("短交接:已查完,结论如上")};
    tools::ToolRegistry registry;
    auto tool = std::make_unique<GrowingResultTool>();
    tool->results = {WordyText(16000)};
    registry.Register(std::move(tool));

    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"},
                                          .runtime{.max_output_tokens = 16384,
                                                   .max_steps_per_turn = 25,
                                                   .max_context_chars = 400000,
                                                   .context_window_tokens = 32768},
                                          .system_prompt = "sys"});
    int preflight_events = 0;
    bool saw_clamped = false;
    agent::AgentWiring wiring;
    wiring.on_context_pressure = [&](const agent::ContextPressure& pressure) {
        if (pressure.phase == agent::ContextPressure::Phase::PreflightExceeded) {
            ++preflight_events;
            // 三项账齐:estimated_input + reserved_output + protocol_margin。
            saw_clamped = pressure.reserve_clamped && pressure.reserved_output_tokens == 2048 &&
                          pressure.protocol_headroom_tokens > 0 && pressure.estimated_input_tokens > 0;
        }
    };
    loop.SetWiring(std::move(wiring));

    class PressureRecorder final : public agent::LoopBoundaryRecorder {
    public:
        std::vector<agent::ContextPressure> pressure;
        int prepared_count = 0;
        void OnContextPressure(const agent::ContextPressure& value) override { pressure.push_back(value); }
        std::string OnRequestPrepared(const api::Request&, const agent::RequestPreparedContext&) override {
            return "req-" + std::to_string(++prepared_count);
        }
        void OnRequestSent(const std::string&) override {}
        void OnUsageRecorded(const std::string&, const api::Usage&, bool, const std::string&, int, bool,
                             bool) override {}
        bool OnOutputCompleted(const std::string&, const api::Message&, const std::string&,
                               const std::string&) override {
            return true;
        }
        void OnOutputFailed(const std::string&, const std::string&) override {}
        void OnOutputCancelled(const std::string&) override {}
    } recorder;
    agent::TurnWiring turn_wiring;
    turn_wiring.boundary_recorder = &recorder;

    const auto result = loop.Run("查一查", std::move(turn_wiring));
    REQUIRE(result.has_value());  // 没死,收窄续跑
    REQUIRE(backend.captured_requests.size() == 2);
    REQUIRE(backend.captured_requests[1].max_tokens.has_value());
    CHECK(*backend.captured_requests[1].max_tokens == 2048);
    // 收尾交代进了第二份请求的尾消息(也随 durable history 留住)。
    const auto& last_message = backend.captured_requests[1].messages.back();
    bool has_nudge = false;
    for (const auto& block : last_message.content) {
        if (const auto* text = std::get_if<api::TextBlock>(&block); text != nullptr &&
            text->text.find("上下文将尽") != std::string::npos) {
            has_nudge = true;
        }
    }
    CHECK(has_nudge);
    CHECK(preflight_events >= 1);
    CHECK(saw_clamped);
    REQUIRE(recorder.pressure.size() == 1);
    CHECK(recorder.pressure.front().phase == agent::ContextPressure::Phase::PreflightExceeded);
    CHECK(recorder.pressure.front().reserved_output_tokens == 2048);
    CHECK(recorder.pressure.front().reserve_clamped);
}

TEST_CASE("预检应急预留: 应急也装不下时稳定报错,文案带现场保留说明") {
    // 两轮工具把历史攒到 ~30800 token(窗 32768):第一份 15800 常规预留
    // 刚好放行,第二份 15000 之后应急 2048 也装不下;但当前消息(第二份
    // 工具结果 15000)自己装得下——报"自动压缩后仍装不下"那一支,且带
    // "现场不丢"的续派指引。
    FakeBackend backend;
    backend.scripts = {ToolUseScript("t1", "growing_tool"), ToolUseScript("t2", "growing_tool"),
                       TextOnlyScript("不该走到这里")};
    tools::ToolRegistry registry;
    auto tool = std::make_unique<GrowingResultTool>();
    tool->results = {WordyText(15800), WordyText(15000)};
    registry.Register(std::move(tool));

    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"},
                                          .runtime{.max_output_tokens = 16384,
                                                   .max_steps_per_turn = 25,
                                                   .max_context_chars = 400000,
                                                   .context_window_tokens = 32768},
                                          .system_prompt = "sys"});
    class RejectPressureRecorder final : public agent::LoopBoundaryRecorder {
    public:
        std::vector<agent::ContextPressure> pressure;
        int prepared_count = 0;
        void OnContextPressure(const agent::ContextPressure& value) override { pressure.push_back(value); }
        std::string OnRequestPrepared(const api::Request&, const agent::RequestPreparedContext&) override {
            return "req-" + std::to_string(++prepared_count);
        }
        void OnRequestSent(const std::string&) override {}
        void OnUsageRecorded(const std::string&, const api::Usage&, bool, const std::string&, int, bool,
                             bool) override {}
        bool OnOutputCompleted(const std::string&, const api::Message&, const std::string&,
                               const std::string&) override {
            return true;
        }
        void OnOutputFailed(const std::string&, const std::string&) override {}
        void OnOutputCancelled(const std::string&) override {}
    } recorder;
    agent::TurnWiring turn_wiring;
    turn_wiring.boundary_recorder = &recorder;
    const auto result = loop.Run("查一查", std::move(turn_wiring));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("上下文预检未通过") != std::string::npos);
    CHECK(result.error().find("自动压缩后仍装不下") != std::string::npos);
    CHECK(result.error().find("现场不丢") != std::string::npos);
    CHECK(backend.captured_requests.size() == 2);  // 第三份请求没发出去
    REQUIRE(recorder.pressure.size() == 1);
    CHECK_FALSE(recorder.pressure.front().reserve_clamped);
    CHECK(recorder.prepared_count == 2);  // 拒绝分支本身没有 model.request.prepared
}

// ---------------------------------------------------------------------------
// 步数将尽提醒:ShouldNudgeStepLimit 是纯函数,直接测触发时机;再用一个真跑
// AgentLoop 的用例确认提醒文本真的附到了发出去的末条消息尾部。
// ---------------------------------------------------------------------------

TEST_CASE("ShouldNudgeStepLimit: 剩 3 步那一步触发一次,其余各步不再重复") {
    // 预算 100 步:step_index=96 时剩余 4 步,不提醒;step_index=97 时剩余 3 步,提醒
    // (唯一一次);turn=98、99 不再重复(规格"现场四":收口提示只注入一次)。
    CHECK_FALSE(agent::ShouldNudgeStepLimit(96, 100));
    CHECK(agent::ShouldNudgeStepLimit(97, 100));
    CHECK_FALSE(agent::ShouldNudgeStepLimit(98, 100));
    CHECK_FALSE(agent::ShouldNudgeStepLimit(99, 100));  // 最后一步(step_index=预算-1)
}

TEST_CASE("ShouldNudgeStepLimit: 预算本来就小于阈值时第一步触发一次") {
    CHECK(agent::ShouldNudgeStepLimit(0, 3));
    CHECK(agent::ShouldNudgeStepLimit(0, 1));
    CHECK_FALSE(agent::ShouldNudgeStepLimit(1, 3));  // 剩 2 步,但第一步已提醒过
    CHECK_FALSE(agent::ShouldNudgeStepLimit(1, 5));  // 剩 4 步,还不该
    CHECK_FALSE(agent::ShouldNudgeStepLimit(0, 5));  // 剩 5 步,更不该
}

TEST_CASE("ShouldNudgeStepLimit: 预算 <= 0(无上限)永不触发,不管 step_index 是多少") {
    CHECK_FALSE(agent::ShouldNudgeStepLimit(0, 0));
    CHECK_FALSE(agent::ShouldNudgeStepLimit(1000, 0));
    CHECK_FALSE(agent::ShouldNudgeStepLimit(0, -1));
    CHECK_FALSE(agent::ShouldNudgeStepLimit(5, -5));
}

TEST_CASE("步数将尽提醒:剩 3 步那一步随尾部消息带一次固定收口提示;system 不动") {
    FakeBackend backend;
    // 预算 4 步:step_index=0(剩 4 步)不带;step_index=1(剩 3 步)带——唯一一次;
    // step_index=2(剩 2 步)不再重复。
    backend.scripts = {
        ToolUseScript("toolu_1", "fake_tool"),  // step 0,剩余 4 步,不该带提醒
        ToolUseScript("toolu_2", "fake_tool"),  // step 1,剩余 3 步,该带提醒
        TextOnlyScript("收尾了"),                // step 2,剩余 2 步,不再重复
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .runtime{.max_output_tokens = 4096, .max_steps_per_turn = 4}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    const auto result = loop.Run("跑吧", callbacks);

    REQUIRE(result.has_value());
    REQUIRE(backend.captured_requests.size() == 3);
    // 提醒不进 system:三份请求的 system 都是原样的 "system prompt"。
    for (const auto& req : backend.captured_requests) {
        CHECK(req.system == "system prompt");
    }
    // 提醒在 step1 的尾部消息(刚攒完的 tool result)里,只此一次:step0
    // 的尾部消息只有一枚 user 文本块,step1 的多出提醒文本块。
    REQUIRE(backend.captured_requests[0].messages.size() == 1);
    CHECK(backend.captured_requests[0].messages.back().content.size() == 1);
    REQUIRE(backend.captured_requests[1].messages.size() == 3);
    REQUIRE(backend.captured_requests[1].messages.back().content.size() == 2);
    const auto* nudge = std::get_if<api::TextBlock>(&backend.captured_requests[1].messages.back().content[1]);
    REQUIRE(nudge != nullptr);
    CHECK(nudge->text.find("收尾区") != std::string::npos);
    CHECK(nudge->text.find("检查点") != std::string::npos);  // 收口:写检查点
    // 随 history 留住:step2 的请求里,同一条消息(下标 2)原样带着提醒,
    // 不改不撤;step2 自己的尾部 tool result 不再重复提醒。
    REQUIRE(backend.captured_requests[2].messages.size() == 5);
    REQUIRE(backend.captured_requests[2].messages[2].content.size() == 2);
    CHECK(backend.captured_requests[2].messages.back().content.size() == 1);
}

TEST_CASE("本轮动态上下文:随本轮 user 消息尾部进请求视图,发过即钉住") {
    FakeBackend backend;
    backend.scripts = {ToolUseScript("tool-1", "fake"), TextOnlyScript("done")};
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake", tools::Tool::Result{"ok", false}, false));
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "stable system"});
    loop.SetTurnContext("project memory context");

    REQUIRE(loop.Run("go", agent::TurnWiring{}).has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    // system 不带动态上下文——那是会话稳定材料的地盘。
    CHECK(backend.captured_requests[0].system == "stable system");
    CHECK(backend.captured_requests[1].system == "stable system");
    // 上下文在首轮 user 消息的尾部块里,两份请求原样重放。
    REQUIRE(backend.captured_requests[0].messages.size() == 1);
    REQUIRE(backend.captured_requests[0].messages[0].content.size() == 2);
    const auto* context = std::get_if<api::TextBlock>(&backend.captured_requests[0].messages[0].content[1]);
    REQUIRE(context != nullptr);
    CHECK(context->text == "project memory context");
    REQUIRE(backend.captured_requests[1].messages.size() == 3);
    REQUIRE(backend.captured_requests[1].messages[0].content.size() == 2);

    // 持久历史只写真输入。session 与 export 都从 History() 取数,不得把
    // 临时召回包带出本轮请求。
    REQUIRE_FALSE(loop.History().empty());
    REQUIRE(loop.History()[0].content.size() == 1);
    const auto* durable_user = std::get_if<api::TextBlock>(&loop.History()[0].content[0]);
    REQUIRE(durable_user != nullptr);
    CHECK(durable_user->text == "go");
    const std::string session_line = "(P0-6 序列化已删)";
    CHECK(session_line.find("project memory context") == std::string::npos);
    const std::string exported = tools::ExportSessionMarkdown({}, loop.History(), "test");
    CHECK(exported.find("project memory context") == std::string::npos);
}

TEST_CASE("图片用户消息入历史，下一轮请求仍带着") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("看到了"), TextOnlyScript("还在")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;

    api::Message image_message;
    image_message.role = api::Role::User;
    image_message.content.push_back(api::ImageBlock{"image/png", "aW1hZ2U=", "error.png", 100, 50});
    REQUIRE(loop.Run(std::move(image_message), callbacks).has_value());
    REQUIRE(loop.Run("再说一句", callbacks).has_value());

    REQUIRE(backend.captured_requests.size() == 2);
    REQUIRE(backend.captured_requests[1].messages.size() >= 3);
    const auto* image = std::get_if<api::ImageBlock>(&backend.captured_requests[1].messages[0].content[0]);
    REQUIRE(image != nullptr);
    CHECK(image->filename == "error.png");
    CHECK(image->data == "aW1hZ2U=");
}


// ---------------------------------------------------------------------------
// 模型输出图片(ccmoon 真机巡检单 P0):ImageOutput -> on_model_image 落盘口
// -> 引用块入历史;未接线/落盘失败明败;重复终帧去重;打断不落半张图。
// ---------------------------------------------------------------------------

namespace {

// 落盘口的假实现:不碰磁盘,还一只造好的引用块。
agent::ModelImageLanding FakeLanding(const api::ImageOutput& image) {
    agent::ModelImageLanding landing;
    landing.block.id = image.id;
    landing.block.filename = "img-" + image.id + ".png";
    landing.block.path = "images/img-" + image.id + ".png";
    landing.block.mime_type = "image/png";
    landing.block.width = 16;
    landing.block.height = 16;
    landing.block.bytes = 68;
    landing.block.sha256 = std::string(64, 'a');
    landing.display_path = "/tmp/session/images/img-" + image.id + ".png";
    return landing;
}

std::vector<api::StreamEvent> ImageTurnScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::BuiltinToolStart{"ig_1", "image_generation", nlohmann::json::object()},
        api::ImageOutput{"ig_1", "aVBBTk8="},
        api::TextDelta{text},
        api::ContentBlockDone{1},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

}  // namespace

TEST_CASE("图片+文字:落盘口还引用,历史带 ModelImageBlock,续聊不重放 base64") {
    FakeBackend backend;
    backend.scripts = {ImageTurnScript("图好了"), TextOnlyScript("好")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    RecordedTurn turn;
    agent::TurnWiring callbacks;
    callbacks.events = &turn.adapter;
    callbacks.on_model_image = [](const api::ImageOutput& image) { return FakeLanding(image); };

    REQUIRE(loop.Run("画一张", callbacks).has_value());
    REQUIRE(loop.history().size() == 2);
    const api::Message& assistant = loop.history()[1];
    REQUIRE(assistant.role == api::Role::Assistant);
    REQUIRE(assistant.content.size() == 2);  // 文本块 + 图片引用块
    const auto* text = std::get_if<api::TextBlock>(&assistant.content[0]);
    REQUIRE(text != nullptr);
    CHECK(text->text == "图好了");
    const auto* image = std::get_if<api::ModelImageBlock>(&assistant.content[1]);
    REQUIRE(image != nullptr);
    CHECK(image->id == "ig_1");
    CHECK(image->path == "images/img-ig_1.png");

    // 续聊:第二份请求的历史里图片仍是引用块(base64 不入请求);引用块
    // 翻短文本标记的活在各家 wire builder(见 test_responses_request),这
    // 层只管"正文一个字节都不带走"。
    REQUIRE(loop.Run("再来一句", callbacks).has_value());
    REQUIRE(backend.captured_requests.size() == 2);
    bool saw_reference = false;
    for (const auto& message : backend.captured_requests[1].messages) {
        for (const auto& block : message.content) {
            if (const auto* ref = std::get_if<api::ModelImageBlock>(&block); ref != nullptr) {
                saw_reference = true;
                CHECK(ref->path == "images/img-ig_1.png");
                CHECK(ref->sha256.find("aVBBTk8=") == std::string::npos);
            }
            if (const auto* user_image = std::get_if<api::ImageBlock>(&block); user_image != nullptr) {
                CHECK(user_image->data.find("aVBBTk8=") == std::string::npos);
            }
        }
    }
    CHECK(saw_reference);
}

TEST_CASE("未接线落盘口:图片来了明败,不吞图冒充成功") {
    FakeBackend backend;
    backend.scripts = {ImageTurnScript("图好了")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;  // 不设 on_model_image

    const auto result = loop.Run("画一张", callbacks);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("未接线图片落盘") != std::string::npos);
    CHECK(result.error().find("未保存") != std::string::npos);
    // 失败的回合不入半截图:历史只有 user 那条。
    REQUIRE(loop.history().size() == 1);
}

TEST_CASE("落盘失败:回合明败,错误带落盘原因") {
    FakeBackend backend;
    backend.scripts = {ImageTurnScript("图好了")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    callbacks.on_model_image = [](const api::ImageOutput&) -> std::expected<agent::ModelImageLanding, std::string> {
        return std::unexpected(std::string("图片解码后超过上限"));
    };
    const auto result = loop.Run("画一张", callbacks);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("图片未保存") != std::string::npos);
    CHECK(result.error().find("图片解码后超过上限") != std::string::npos);
}

TEST_CASE("重复终帧:同一 id 的 ImageOutput 到两遍,只落一回、只入一次历史") {
    FakeBackend backend;
    backend.scripts = {{
        api::MessageStart{"msg", "model"},
        api::BuiltinToolStart{"ig_dup", "image_generation", nlohmann::json::object()},
        api::ImageOutput{"ig_dup", "QUFBQQ=="},
        api::TextDelta{"一张"},
        api::ContentBlockDone{1},
        api::ImageOutput{"ig_dup", "QUFBQQ=="},
        api::MessageDone{"end_turn", api::Usage{}},
    }};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    int landings = 0;
    callbacks.on_model_image = [&landings](const api::ImageOutput& image) {
        ++landings;
        return FakeLanding(image);
    };
    REQUIRE(loop.Run("画一张", callbacks).has_value());
    CHECK(landings == 1);
    int image_blocks = 0;
    for (const auto& block : loop.history()[1].content) {
        if (std::holds_alternative<api::ModelImageBlock>(block)) {
            ++image_blocks;
        }
    }
    CHECK(image_blocks == 1);
}

TEST_CASE("生成中断:ESC 掐在正文之前,没到 ImageOutput 就没有图,历史干净收场") {
    FakeBackend backend;
    // 脚本:开卡(BuiltinToolStart)后立刻掐断——result 还没到。
    backend.scripts = {{
        api::MessageStart{"msg", "model"},
        api::BuiltinToolStart{"ig_cut", "image_generation", nlohmann::json::object()},
    }};
    backend.cancel_after_event_index = 1;
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry, agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});
    agent::TurnWiring callbacks;
    bool landed = false;
    callbacks.on_model_image = [&landed](const api::ImageOutput& image) {
        landed = true;
        return FakeLanding(image);
    };
    const auto result = loop.Run("画一张", callbacks);
    REQUIRE(result.has_value());
    CHECK(result->cancelled);
    CHECK_FALSE(landed);
    bool has_image = false;
    for (const auto& block : loop.history()[1].content) {
        has_image = has_image || std::holds_alternative<api::ModelImageBlock>(block);
    }
    CHECK_FALSE(has_image);
}

// ---------------------------------------------------------------------------
// 动态工具 PromptCache 守恒单 P1(通用 ProxyReference):AgentLoop 的代理
// 调用规范化(单子 §6.1/§6.2/§12.1/§12.4)——tool_invoke 解引用后只对真实
// 目标走一次 RunOneTool;Hook/确认/执行资格/取消全落在真实目标;发现
// 不等于授权,直接按名调用延迟工具照旧被拦;伪拼 ref、越权策略、坏参数
// 各有稳定拒绝码。
// ---------------------------------------------------------------------------

namespace {

// proxy 测试的计数延迟工具:记下每次执行的入参(断言"只执行一次、参数
// 不串"全靠它)。
class CountingDeferredTool : public tools::Tool {
public:
    CountingDeferredTool(std::string name, nlohmann::json schema, bool confirm = false)
        : name_(std::move(name)), schema_(std::move(schema)), confirm_(confirm) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "counting deferred tool for proxy tests"; }
    nlohmann::json input_schema() const override { return schema_; }
    bool deferred() const override { return true; }
    bool needs_confirm() const override { return confirm_; }

    tools::Tool::Result execute(const nlohmann::json& input) override {
        ++call_count;
        last_input = input;
        return {"执行了 " + name_, false};
    }

    int call_count = 0;
    nlohmann::json last_input;

private:
    std::string name_;
    nlohmann::json schema_;
    bool confirm_;
};

std::vector<api::StreamEvent> ProxyToolCallScript(const std::string& id, const std::string& name,
                                                  const std::string& input_json) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, id, name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// proxy 模式的生产同款皮:resolver + exposure 过滤(延迟工具恒不进顶层
// tools,loaded 集合不参与)。
agent::AgentProfile ProxyProfile(std::shared_ptr<tools::DeferredToolResolver> resolver) {
    agent::AgentProfile profile{.request{.model = "test-model"}, .system_prompt = "system prompt"};
    profile.tool_ref_resolver = std::move(resolver);
    profile.tool_filter = [](const tools::Tool& tool) { return !tool.deferred(); };
    profile.tool_filter_denial =
        "延迟工具不能按名直调:请先用 tool_search 检索拿到 tool_ref,再以 tool_invoke 调用。";
    return profile;
}

// 从历史里抠出 tool_search 结果 JSON 里的第一枚 tool_ref(脚本没法预知
// 运行时铸出来的号,跑完第一轮再取)。
std::string FirstToolRefFromHistory(const agent::Agent& loop) {
    for (const auto& message : loop.history()) {
        for (const auto& block : message.content) {
            const auto* result = std::get_if<api::ToolResultBlock>(&block);
            if (result == nullptr) {
                continue;
            }
            const nlohmann::json parsed =
                nlohmann::json::parse(result->content, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_object() && parsed.contains("matches") && parsed["matches"].is_array() &&
                !parsed["matches"].empty() && parsed["matches"][0].contains("tool_ref")) {
                return parsed["matches"][0]["tool_ref"].get<std::string>();
            }
        }
    }
    return std::string();
}

struct TraceCollector {
    std::vector<agent::ToolTraceEvent> events;
    void Wire(agent::TurnWiring& callbacks) {
        callbacks.on_tool_trace = [this](const agent::ToolTraceEvent& event) { events.push_back(event); };
    }
    const agent::ToolTraceEvent* Find(agent::ToolTraceEventKind kind, const std::string& tool_name) const {
        for (const auto& event : events) {
            if (event.kind == kind && event.tool_name == tool_name) {
                return &event;
            }
        }
        return nullptr;
    }
};

// 按 wire id 全历史找 tool_result:轮末最后一条是收尾正文,别赌消息位次。
const api::ToolResultBlock* FindToolResult(const agent::Agent& loop, const std::string& tool_use_id) {
    for (const auto& message : loop.history()) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
                result != nullptr && result->tool_use_id == tool_use_id) {
                return result;
            }
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("P1 proxy: 解引用后真实工具只执行一次,trace 记 transport/resolved 两层") {
    FakeBackend backend;
    backend.scripts = {
        ProxyToolCallScript("call_search", "tool_search", R"({"query":"counting"})"),
        TextOnlyScript("搜到了"),
        ProxyToolCallScript("call_invoke", "tool_invoke", R"(PLACEHOLDER)"),
        TextOnlyScript("完成"),
    };
    backend.scripts[2][2] = api::ToolUseInputDelta{0, R"({"tool_ref":"REF","arguments":{"q":"cache"}})"};

    tools::ToolRegistry registry;
    auto target = std::make_unique<CountingDeferredTool>(
        "mcp__db__query",
        nlohmann::json{{"type", "object"},
                       {"properties", {{"q", {{"type", "string"}}}}},
                       {"required", nlohmann::json::array({"q"})}});
    CountingDeferredTool* target_ptr = target.get();
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::Agent loop(backend, registry, ProxyProfile(resolver));
    agent::TurnWiring callbacks;
    TraceCollector trace;
    trace.Wire(callbacks);

    // 第一轮:tool_search 发现目标,拿到 ref。
    REQUIRE(loop.Run("帮我查", callbacks).has_value());
    const std::string tool_ref = FirstToolRefFromHistory(loop);
    REQUIRE_FALSE(tool_ref.empty());
    CHECK(target_ptr->call_count == 0);  // 搜索只读,不执行目标

    // 第二轮:tool_invoke 走 ref。脚本里的占位 ref 换成真号(模型侧无从
    // 预知宿主铸的号,测试也一样——先发现后调用)。
    backend.scripts[2][2] = api::ToolUseInputDelta{
        0, R"({"tool_ref":")" + tool_ref + R"(","arguments":{"q":"cache"}})"};
    REQUIRE(loop.Run("调用它", callbacks).has_value());

    // 真实目标只执行一次,入参是解包后的 arguments(不是 {tool_ref,...} 壳)。
    REQUIRE(target_ptr->call_count == 1);
    CHECK(target_ptr->last_input == nlohmann::json{{"q", "cache"}});

    // tool_result 与 wire 上那枚 tool_invoke call id 配对(不是目标名)。
    // 末条消息是收尾正文,tool_result 在倒数第二条——按 id 全历史找,别赌位次。
    bool paired = false;
    for (const auto& message : loop.history()) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block);
                result != nullptr && result->tool_use_id == "call_invoke") {
                paired = true;
                CHECK_FALSE(result->is_error);
                CHECK(result->content.find("执行了 mcp__db__query") != std::string::npos);
            }
        }
    }
    CHECK(paired);

    // trace 两层事实(§6.2):scheduled 是 wire 事实(tool_invoke),执行
    // 事件的一等名是真实目标,details 里 transport/resolved/tool_ref 齐。
    const auto* scheduled = trace.Find(agent::ToolTraceEventKind::Scheduled, "tool_invoke");
    REQUIRE(scheduled != nullptr);
    CHECK(scheduled->details.value("transport_tool", std::string()) == "tool_invoke");
    const auto* started = trace.Find(agent::ToolTraceEventKind::ExecutionStarted, "mcp__db__query");
    REQUIRE(started != nullptr);
    CHECK(started->details.value("transport_tool", std::string()) == "tool_invoke");
    CHECK(started->details.value("resolved_tool", std::string()) == "mcp__db__query");
    CHECK(started->details.value("tool_ref", std::string()) == tool_ref);
    CHECK_FALSE(started->details.value("schema_digest", std::string()).empty());
    const auto* finished = trace.Find(agent::ToolTraceEventKind::ExecutionFinished, "mcp__db__query");
    REQUIRE(finished != nullptr);
    CHECK(finished->details.value("transport_tool", std::string()) == "tool_invoke");

    // 顶层 tools 恒为 core + tool_search + tool_invoke:目标工具从没进过
    // 任何一份请求的 tools 数组(§5.1)。
    REQUIRE(backend.captured_requests.size() == 4);
    for (const auto& request : backend.captured_requests) {
        bool has_search = false;
        bool has_invoke = false;
        bool has_target = false;
        for (const auto& def : request.tools) {
            has_search = has_search || def.name == "tool_search";
            has_invoke = has_invoke || def.name == "tool_invoke";
            has_target = has_target || def.name == "mcp__db__query";
        }
        CHECK(has_search);
        CHECK(has_invoke);
        CHECK_FALSE(has_target);
    }
}

TEST_CASE("P1 proxy: 直接按名调用延迟工具被拦——发现不等于授权") {
    FakeBackend backend;
    backend.scripts = {
        ProxyToolCallScript("call_direct", "mcp__db__query", R"({"q":"直调"})"),
        TextOnlyScript("收下拒绝"),
    };
    tools::ToolRegistry registry;
    auto target = std::make_unique<CountingDeferredTool>(
        "mcp__db__query", nlohmann::json{{"type", "object"}, {"properties", {{"q", {{"type", "string"}}}}}});
    CountingDeferredTool* target_ptr = target.get();
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::Agent loop(backend, registry, ProxyProfile(resolver));
    agent::TurnWiring callbacks;
    TraceCollector trace;
    trace.Wire(callbacks);

    REQUIRE(loop.Run("直接调", callbacks).has_value());
    CHECK(target_ptr->call_count == 0);  // 没执行

    const api::ToolResultBlock* result = FindToolResult(loop, "call_direct");
    REQUIRE(result != nullptr);
    CHECK(result->is_error);
    CHECK(result->content.find("tool_search") != std::string::npos);
    CHECK(result->content.find("tool_invoke") != std::string::npos);
    // 拒绝也留终态栅栏,不冒充没发生过。
    const auto* finished = trace.Find(agent::ToolTraceEventKind::ExecutionFinished, "mcp__db__query");
    REQUIRE(finished != nullptr);
    CHECK(finished->error_code == agent::kErrRegistryNotMounted);
}

TEST_CASE("P1 proxy: 伪拼/跨账 ref 报 unknown_tool_ref,不执行") {
    FakeBackend backend;
    backend.scripts = {
        ProxyToolCallScript("call_fake", "tool_invoke", R"({"tool_ref":"dt_deadbeef_1","arguments":{}})"),
        TextOnlyScript("收下拒绝"),
    };
    tools::ToolRegistry registry;
    auto target = std::make_unique<CountingDeferredTool>(
        "mcp__db__query", nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}});
    CountingDeferredTool* target_ptr = target.get();
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::Agent loop(backend, registry, ProxyProfile(resolver));
    agent::TurnWiring callbacks;
    TraceCollector trace;
    trace.Wire(callbacks);

    REQUIRE(loop.Run("拿假引用调", callbacks).has_value());
    CHECK(target_ptr->call_count == 0);
    const api::ToolResultBlock* result = FindToolResult(loop, "call_fake");
    REQUIRE(result != nullptr);                  // 仍用 wire id 配对(§十)
    CHECK(result->is_error);
    CHECK(result->content.find("重新 tool_search") != std::string::npos);
    const auto* finished = trace.Find(agent::ToolTraceEventKind::ExecutionFinished, "tool_invoke");
    REQUIRE(finished != nullptr);
    CHECK(finished->error_code == tools::kErrToolRefUnknown);
}

TEST_CASE("P1 proxy: 入参不合真实 schema 报 invalid_target_arguments,不执行") {
    FakeBackend backend;
    backend.scripts = {
        ProxyToolCallScript("call_search", "tool_search", R"({"query":"counting"})"),
        TextOnlyScript("x"),
        ProxyToolCallScript("call_bad_args", "tool_invoke", R"(PLACEHOLDER)"),
        TextOnlyScript("y"),
    };
    tools::ToolRegistry registry;
    auto target = std::make_unique<CountingDeferredTool>(
        "mcp__db__query",
        nlohmann::json{{"type", "object"},
                       {"properties", {{"q", {{"type", "string"}}}}},
                       {"required", nlohmann::json::array({"q"})}});
    CountingDeferredTool* target_ptr = target.get();
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::Agent loop(backend, registry, ProxyProfile(resolver));
    agent::TurnWiring callbacks;
    TraceCollector trace;
    trace.Wire(callbacks);

    REQUIRE(loop.Run("先搜", callbacks).has_value());
    const std::string tool_ref = FirstToolRefFromHistory(loop);
    REQUIRE_FALSE(tool_ref.empty());
    // 少了必填的 q:宽 schema(tool_invoke 顶层)拦不住,真 schema 拦得住。
    backend.scripts[2][2] = api::ToolUseInputDelta{
        0, R"({"tool_ref":")" + tool_ref + R"(","arguments":{"别的东西":1}})"};
    REQUIRE(loop.Run("坏参数调", callbacks).has_value());

    CHECK(target_ptr->call_count == 0);
    const auto* finished = trace.Find(agent::ToolTraceEventKind::ExecutionFinished, "mcp__db__query");
    REQUIRE(finished != nullptr);
    CHECK(finished->error_code == tools::kErrToolRefInvalidArguments);
    CHECK(finished->details.value("transport_tool", std::string()) == "tool_invoke");
}

TEST_CASE("P1 proxy: 执行策略拒绝报 tool_not_allowed,不得重试") {
    FakeBackend backend;
    backend.scripts = {
        ProxyToolCallScript("call_search", "tool_search", R"({"query":"counting"})"),
        TextOnlyScript("x"),
        ProxyToolCallScript("call_denied", "tool_invoke", R"(PLACEHOLDER)"),
        TextOnlyScript("y"),
    };
    tools::ToolRegistry registry;
    auto target = std::make_unique<CountingDeferredTool>(
        "mcp__db__query", nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}});
    CountingDeferredTool* target_ptr = target.get();
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::AgentProfile profile = ProxyProfile(resolver);
    // 执行资格拒绝(单子 §十 tool_not_allowed):deny 一切目标(模拟角色
    // 闸/deny policy)。ref 有效也放不了行。
    profile.tool_execution_policy = [](const tools::Tool& tool) { return tool.name() != "mcp__db__query"; };
    profile.tool_execution_denial =
        std::string(tools::kErrToolRefNotAllowed) + "|当前角色不许调用该工具,不得重试同一调用。";
    agent::Agent loop(backend, registry, std::move(profile));
    agent::TurnWiring callbacks;
    TraceCollector trace;
    trace.Wire(callbacks);

    REQUIRE(loop.Run("先搜", callbacks).has_value());
    const std::string tool_ref = FirstToolRefFromHistory(loop);
    REQUIRE_FALSE(tool_ref.empty());
    backend.scripts[2][2] = api::ToolUseInputDelta{
        0, R"({"tool_ref":")" + tool_ref + R"(","arguments":{}})"};
    REQUIRE(loop.Run("越权调", callbacks).has_value());

    CHECK(target_ptr->call_count == 0);
    const auto* finished = trace.Find(agent::ToolTraceEventKind::ExecutionFinished, "mcp__db__query");
    REQUIRE(finished != nullptr);
    CHECK(finished->error_code == tools::kErrToolRefNotAllowed);
    const api::ToolResultBlock* result = FindToolResult(loop, "call_denied");
    REQUIRE(result != nullptr);
    CHECK(result->is_error);
    CHECK(result->content.find("不得重试") != std::string::npos);
}

TEST_CASE("P1 proxy: 钩子与确认看真实目标——真名一次、真参数、真确认") {
    FakeBackend backend;
    backend.scripts = {
        ProxyToolCallScript("call_search", "tool_search", R"({"query":"counting"})"),
        TextOnlyScript("x"),
        ProxyToolCallScript("call_gate", "tool_invoke", R"(PLACEHOLDER)"),
        TextOnlyScript("y"),
    };
    tools::ToolRegistry registry;
    auto target = std::make_unique<CountingDeferredTool>(
        "mcp__db__query",
        nlohmann::json{{"type", "object"},
                       {"properties", {{"q", {{"type", "string"}}}}},
                       {"required", nlohmann::json::array({"q"})}},
        /*confirm=*/true);
    CountingDeferredTool* target_ptr = target.get();
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::Agent loop(backend, registry, ProxyProfile(resolver));
    agent::TurnWiring callbacks;
    std::vector<std::string> hook_names;
    std::vector<nlohmann::json> hook_inputs;
    callbacks.on_pre_tool_use_hook = [&](const std::string& /*id*/, const std::string& name,
                                         const nlohmann::json& input) {
        hook_names.push_back(name);
        hook_inputs.push_back(input);
        return runtime::ToolHookDecision{};  // 不表态
    };
    std::vector<std::string> confirm_names;
    std::vector<nlohmann::json> confirm_inputs;
    callbacks.on_tool_confirm = [&](const std::string& /*id*/, const std::string& name,
                                    const nlohmann::json& input) {
        confirm_names.push_back(name);
        confirm_inputs.push_back(input);
        return true;
    };

    REQUIRE(loop.Run("先搜", callbacks).has_value());
    const std::string tool_ref = FirstToolRefFromHistory(loop);
    REQUIRE_FALSE(tool_ref.empty());
    backend.scripts[2][2] = api::ToolUseInputDelta{
        0, R"({"tool_ref":")" + tool_ref + R"(","arguments":{"q":"hook 看"}})"};
    REQUIRE(loop.Run("确认路调用", callbacks).has_value());

    // 钩子每枚调用都过一遍(先 tool_search、后真实目标);确认只问
    // needs_confirm 的那一枚——真实目标一次、真名真参,不是 tool_invoke
    // 那层壳,外层壳的 {tool_ref,...} 不许漏进确认文案。
    REQUIRE(hook_names.size() == 2);
    CHECK(hook_names[0] == "tool_search");
    CHECK(hook_names[1] == "mcp__db__query");
    CHECK(hook_inputs[1] == nlohmann::json{{"q", "hook 看"}});
    REQUIRE(confirm_names.size() == 1);
    CHECK(confirm_names[0] == "mcp__db__query");
    CHECK(confirm_inputs[0] == nlohmann::json{{"q", "hook 看"}});
    CHECK(target_ptr->call_count == 1);
}

TEST_CASE("P1 proxy: PreToolUse 改写入参后用真实 schema 复验,改坏即拦") {
    FakeBackend backend;
    backend.scripts = {
        ProxyToolCallScript("call_search", "tool_search", R"({"query":"counting"})"),
        TextOnlyScript("x"),
        ProxyToolCallScript("call_rewrite", "tool_invoke", R"(PLACEHOLDER)"),
        TextOnlyScript("y"),
    };
    tools::ToolRegistry registry;
    auto target = std::make_unique<CountingDeferredTool>(
        "mcp__db__query",
        nlohmann::json{{"type", "object"},
                       {"properties", {{"q", {{"type", "string"}}}}},
                       {"required", nlohmann::json::array({"q"})}});
    CountingDeferredTool* target_ptr = target.get();
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::Agent loop(backend, registry, ProxyProfile(resolver));
    agent::TurnWiring callbacks;
    TraceCollector trace;
    trace.Wire(callbacks);
    callbacks.on_pre_tool_use_hook = [](const std::string&, const std::string& name,
                                        const nlohmann::json&) {
        runtime::ToolHookDecision decision;
        // 只改写真实目标的入参——tool_search 那枚照常跑(改了它,发现这步
        // 就先失败了)。
        if (name == "mcp__db__query") {
            decision.decision = runtime::ToolHookDecision::Decision::Allow;
            // 改写成丢掉必填 q 的形状:真 schema 复检必须拦(§5.5 链末段)。
            decision.updated_input = nlohmann::json{{"not_q", 1}};
        }
        return decision;
    };

    REQUIRE(loop.Run("先搜", callbacks).has_value());
    const std::string tool_ref = FirstToolRefFromHistory(loop);
    REQUIRE_FALSE(tool_ref.empty());
    backend.scripts[2][2] = api::ToolUseInputDelta{
        0, R"({"tool_ref":")" + tool_ref + R"(","arguments":{"q":"本来合法"}})"};
    REQUIRE(loop.Run("改坏参", callbacks).has_value());

    CHECK(target_ptr->call_count == 0);  // 改坏的形状不许跑
    const auto* finished = trace.Find(agent::ToolTraceEventKind::ExecutionFinished, "mcp__db__query");
    REQUIRE(finished != nullptr);
    CHECK(finished->error_code == agent::kErrHookUpdatedInputInvalid);
}

TEST_CASE("P1 proxy: 同批两枚 tool_invoke 各自解引用,参数不串") {
    FakeBackend backend;
    tools::ToolRegistry registry;
    auto target_a = std::make_unique<CountingDeferredTool>(
        "mcp__alpha__do", nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}});
    auto target_b = std::make_unique<CountingDeferredTool>(
        "mcp__beta__do", nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}});
    CountingDeferredTool* a_ptr = target_a.get();
    CountingDeferredTool* b_ptr = target_b.get();
    registry.Register(std::move(target_a));
    registry.Register(std::move(target_b));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::Agent loop(backend, registry, ProxyProfile(resolver));
    agent::TurnWiring callbacks;

    // 第一轮:搜 alpha 拿 ref_a。FakeBackend 的脚本按下标全局累进,后两轮
    // 只能追加、不能整份换(换了就下标越界)。
    backend.scripts.push_back(ProxyToolCallScript("call_search_a", "tool_search", R"({"query":"alpha"})"));
    backend.scripts.push_back(TextOnlyScript("x"));
    REQUIRE(loop.Run("搜 alpha", callbacks).has_value());
    const std::string ref_a = FirstToolRefFromHistory(loop);
    REQUIRE_FALSE(ref_a.empty());

    // 第二轮:搜 beta 拿 ref_b(历史按序收两枚 ref)。
    backend.scripts.push_back(ProxyToolCallScript("call_search_b", "tool_search", R"({"query":"beta"})"));
    backend.scripts.push_back(TextOnlyScript("y"));
    REQUIRE(loop.Run("搜 beta", callbacks).has_value());
    std::vector<std::string> refs;
    for (const auto& message : loop.history()) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block); result != nullptr) {
                const nlohmann::json parsed =
                    nlohmann::json::parse(result->content, nullptr, /*allow_exceptions=*/false);
                if (parsed.is_object() && parsed.contains("matches") && !parsed["matches"].empty() &&
                    parsed["matches"][0].contains("tool_ref")) {
                    refs.push_back(parsed["matches"][0]["tool_ref"].get<std::string>());
                }
            }
        }
    }
    REQUIRE(refs.size() == 2);
    const std::string ref_b = refs[1];
    CHECK(ref_a != ref_b);

    // 第三轮:同一条 assistant 消息里两枚 tool_invoke(并行工具调用)——
    // 各自解引用、各拿各的参数,不串。
    backend.scripts.push_back(std::vector<api::StreamEvent>{
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, "invoke_a", "tool_invoke"},
        api::ToolUseInputDelta{0, R"({"tool_ref":")" + ref_a + R"(","arguments":{"who":"a"}})"},
        api::ContentBlockDone{0},
        api::ToolUseStart{1, "invoke_b", "tool_invoke"},
        api::ToolUseInputDelta{1, R"({"tool_ref":")" + ref_b + R"(","arguments":{"who":"b"}})"},
        api::ContentBlockDone{1},
        api::MessageDone{"tool_use", api::Usage{}},
    });
    backend.scripts.push_back(TextOnlyScript("z"));
    REQUIRE(loop.Run("双调", callbacks).has_value());

    REQUIRE(a_ptr->call_count == 1);
    CHECK(a_ptr->last_input == nlohmann::json{{"who", "a"}});
    REQUIRE(b_ptr->call_count == 1);
    CHECK(b_ptr->last_input == nlohmann::json{{"who", "b"}});
}

// 单子 §9.3/§12.3:compact 换史不许动 DiscoveryLedger——账跟 resolver
// 走,不跟 history 走。compact 后旧 ref 若仍有效,可照常调用;若 schema
// 变了才报 stale,不能因摘要写着旧参数便放行。实现上是"不作为"(Agent::
// ReplaceHistory 只换史、不碰账),不作为恰恰最怕没人看着:这里钉一册,
// 将来谁在换史路上顺手清账,测试当场翻红。
TEST_CASE("P1 proxy: compact 换史不丢 DiscoveryLedger,旧 ref 照常可调") {
    FakeBackend backend;
    tools::ToolRegistry registry;
    auto target = std::make_unique<CountingDeferredTool>(
        "mcp__db__query",
        nlohmann::json{{"type", "object"},
                       {"properties", {{"q", {{"type", "string"}}}}},
                       {"required", nlohmann::json::array({"q"})}});
    CountingDeferredTool* target_ptr = target.get();
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::Agent loop(backend, registry, ProxyProfile(resolver));
    agent::TurnWiring callbacks;

    // 第一轮:发现目标,拿 ref。
    backend.scripts.push_back(ProxyToolCallScript("call_search", "tool_search", R"({"query":"counting"})"));
    backend.scripts.push_back(TextOnlyScript("搜到了"));
    REQUIRE(loop.Run("帮我查", callbacks).has_value());
    const std::string tool_ref = FirstToolRefFromHistory(loop);
    REQUIRE_FALSE(tool_ref.empty());
    REQUIRE(resolver->ledger().Size() == 1);

    // compact:历史换成摘要版(archive + 新起点的 user 问话)。真实 /compact
    // 的新历史由 BuildCompactedHistory 算,这里只要"整份换掉"这个动作本身。
    const int epoch_before = loop.cache_epoch();
    std::vector<api::Message> compacted;
    api::Message archive;
    archive.role = api::Role::User;
    archive.content.push_back(api::TextBlock{"[compact 摘要] 此前发现过 mcp__db__query。"});
    compacted.push_back(std::move(archive));
    loop.ReplaceHistory(std::move(compacted));

    // 换史开新 epoch(compact 本就是有意的 epoch break),但账不动:resolver
    // 里那枚 ref 还在、还能解——模型 compact 后凭旧 ref 继续调用,照常走
    // 正门执行,不必重新 tool_search(单子 §9.3 第 3 条)。
    CHECK(loop.cache_epoch() != epoch_before);
    REQUIRE(resolver->ledger().Size() == 1);
    const auto resolved = resolver->Resolve(registry, [&] {
        api::ToolUseBlock call;
        call.id = "recheck";
        call.name = "tool_invoke";
        call.input = nlohmann::json{{"tool_ref", tool_ref}, {"arguments", nlohmann::json{{"q", "x"}}}};
        return call;
    }());
    REQUIRE(resolved.has_value());
    CHECK(resolved->target_name == "mcp__db__query");

    // compact 后的调用照常走通:同一枚 ref 再执行一次真目标。
    backend.scripts.push_back(std::vector<api::StreamEvent>{
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, "call_after_compact", "tool_invoke"},
        api::ToolUseInputDelta{0, R"({"tool_ref":")" + tool_ref + R"(","arguments":{"q":"after"}})"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    });
    backend.scripts.push_back(TextOnlyScript("compact 后照调"));
    REQUIRE(loop.Run("再调一次", callbacks).has_value());
    REQUIRE(target_ptr->call_count == 1);
    CHECK(target_ptr->last_input == nlohmann::json{{"q", "after"}});
    const api::ToolResultBlock* after = FindToolResult(loop, "call_after_compact");
    REQUIRE(after != nullptr);
    CHECK_FALSE(after->is_error);
}

// ---------------------------------------------------------------------------
// 动态工具 PromptCache 守恒单 P2(条件工具也守恒·§8.2/§12.1):
// goal_checkpoint/loop_control 一类条件工具的定义常驻 tools 数组(暴露
// 策略会话内恒定,保 tools hash),"这一轮可不可用"由 AgentProfile.
// tool_turn_gate 在 RunOneTool 调用当口现判——直名调用与经 tool_invoke
// 解引用的调用同一道闸,拒绝给 turn.tool_not_active 稳定码、终态
// TurnGateDenied。轮次状态段([turn capabilities])只是通报,不是安全
// 边界:模型硬叫了,执行门照样硬拦。
// ---------------------------------------------------------------------------

namespace {

// P2 册的条件工具替身:非延迟(常驻顶层 tools,goal/loop 窄工具的真实
// 形状),记调用次数。
class CountingConditionalTool : public tools::Tool {
public:
    explicit CountingConditionalTool(std::string name) : name_(std::move(name)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "conditional tool for turn gate tests"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }

    tools::Tool::Result execute(const nlohmann::json&) override {
        ++calls;
        return {name_ + " 执行了", false};
    }

    int calls = 0;

private:
    std::string name_;
};

// P2 的生产同款皮:暴露恒 true(条件工具定义常驻),turn 闸现判。
agent::AgentProfile TurnGateProfile(bool* goal_active) {
    agent::AgentProfile profile{.request{.model = "test-model"}, .system_prompt = "system prompt"};
    profile.tool_filter = [](const tools::Tool&) { return true; };
    profile.tool_turn_gate = [goal_active](const tools::Tool& tool) {
        if (tool.name() == "goal_checkpoint") {
            return *goal_active;
        }
        return true;
    };
    profile.tool_turn_gate_denial = std::string(agent::kErrTurnToolNotActive) +
                                    "|goal_checkpoint 只在 goal 执行轮可用;当前轮不是——等相应轮次或换路径,"
                                    "不要重试同一调用。";
    return profile;
}

}  // namespace

TEST_CASE("P2 turn 闸: goal 未激活时直名调用被硬拦,稳定码 turn.tool_not_active") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("call_oop", "goal_checkpoint"),
        TextOnlyScript("收下拒绝"),
    };
    tools::ToolRegistry registry;
    auto goal_tool = std::make_unique<CountingConditionalTool>("goal_checkpoint");
    CountingConditionalTool* goal_ptr = goal_tool.get();
    registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"正文", false}, false));
    registry.Register(std::move(goal_tool));

    bool goal_active = false;  // 普通轮:goal 泵没置位
    agent::Agent loop(backend, registry, TurnGateProfile(&goal_active));
    agent::TurnWiring callbacks;
    TraceCollector trace;
    trace.Wire(callbacks);

    REQUIRE(loop.Run("普通轮硬叫窄工具", callbacks).has_value());
    CHECK(goal_ptr->calls == 0);  // 没执行

    // 拒绝:is_error、人话带指路,终态栅栏的稳定码与 outcome 都钉死。
    const api::ToolResultBlock* result = FindToolResult(loop, "call_oop");
    REQUIRE(result != nullptr);
    CHECK(result->is_error);
    CHECK(result->content.find("等相应轮次") != std::string::npos);
    const auto* finished = trace.Find(agent::ToolTraceEventKind::ExecutionFinished, "goal_checkpoint");
    REQUIRE(finished != nullptr);
    CHECK(finished->error_code == agent::kErrTurnToolNotActive);
    CHECK(finished->outcome == agent::ToolOutcome::TurnGateDenied);
    // turn 闸拦下的调用确定没越过执行边界(恢复语义)。
    CHECK(agent::OutcomeNeverStarted(finished->outcome));
    CHECK(agent::ToString(finished->outcome) == "turn_gate_denied");
    agent::ToolOutcome parsed = agent::ToolOutcome::ToolError;
    CHECK(agent::ParseToolOutcome("turn_gate_denied", parsed));
    CHECK(parsed == agent::ToolOutcome::TurnGateDenied);
}

TEST_CASE("P2 turn 闸: goal 轮里同一次调用放行——闸认真实状态,不是常闭") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("call_ok", "goal_checkpoint"),
        TextOnlyScript("写完检查点"),
    };
    tools::ToolRegistry registry;
    auto goal_tool = std::make_unique<CountingConditionalTool>("goal_checkpoint");
    CountingConditionalTool* goal_ptr = goal_tool.get();
    registry.Register(std::move(goal_tool));

    bool goal_active = true;  // goal 泵已置位:goal 执行轮
    agent::Agent loop(backend, registry, TurnGateProfile(&goal_active));
    agent::TurnWiring callbacks;
    TraceCollector trace;
    trace.Wire(callbacks);

    REQUIRE(loop.Run("goal 执行轮", callbacks).has_value());
    CHECK(goal_ptr->calls == 1);
    const api::ToolResultBlock* result = FindToolResult(loop, "call_ok");
    REQUIRE(result != nullptr);
    CHECK_FALSE(result->is_error);
    const auto* finished = trace.Find(agent::ToolTraceEventKind::ExecutionFinished, "goal_checkpoint");
    REQUIRE(finished != nullptr);
    CHECK(finished->outcome == agent::ToolOutcome::Succeeded);
}

TEST_CASE("P2 turn 闸: proxy 解引用来的调用同一道闸,不在轮次照样拦") {
    FakeBackend backend;
    backend.scripts = {
        ProxyToolCallScript("call_search", "tool_search", R"({"query":"counting"})"),
        TextOnlyScript("x"),
        ProxyToolCallScript("call_invoke", "tool_invoke", R"(PLACEHOLDER)"),
        TextOnlyScript("y"),
    };
    tools::ToolRegistry registry;
    // 延迟的条件工具:定义不进顶层(暴露策略对延迟工具照旧拦),经
    // tool_invoke 解引用后仍要过 turn 闸——两条路不许一松一紧。
    auto target = std::make_unique<CountingDeferredTool>(
        "mcp__goal__checkpoint", nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}});
    CountingDeferredTool* target_ptr = target.get();
    registry.Register(std::move(target));
    auto resolver = std::make_shared<tools::DeferredToolResolver>("main");
    registry.Register(std::make_unique<tools::ToolSearchTool>(registry, resolver));
    registry.Register(std::make_unique<tools::ToolInvokeTool>());

    agent::AgentProfile profile = ProxyProfile(resolver);
    bool goal_active = false;
    profile.tool_turn_gate = [&goal_active](const tools::Tool& tool) {
        if (tool.name() == "mcp__goal__checkpoint") {
            return goal_active;
        }
        return true;
    };
    profile.tool_turn_gate_denial = std::string(agent::kErrTurnToolNotActive) +
                                    "|该工具只在 goal 执行轮可用;当前轮不是——等相应轮次或换路径,不要重试同一调用。";
    agent::Agent loop(backend, registry, std::move(profile));
    agent::TurnWiring callbacks;
    TraceCollector trace;
    trace.Wire(callbacks);

    REQUIRE(loop.Run("先搜", callbacks).has_value());
    const std::string tool_ref = FirstToolRefFromHistory(loop);
    REQUIRE_FALSE(tool_ref.empty());
    backend.scripts[2][2] = api::ToolUseInputDelta{0, R"({"tool_ref":")" + tool_ref + R"(","arguments":{}})"};
    REQUIRE(loop.Run("越轮调", callbacks).has_value());

    CHECK(target_ptr->call_count == 0);  // 执行门拦下,目标没跑
    const auto* finished = trace.Find(agent::ToolTraceEventKind::ExecutionFinished, "mcp__goal__checkpoint");
    REQUIRE(finished != nullptr);
    CHECK(finished->error_code == agent::kErrTurnToolNotActive);
    CHECK(finished->outcome == agent::ToolOutcome::TurnGateDenied);
    // 代理壳那层的事实仍留账(transport/resolved 两层,§6.2)。
    CHECK(finished->details.value("transport_tool", std::string()) == "tool_invoke");
    const api::ToolResultBlock* result = FindToolResult(loop, "call_invoke");
    REQUIRE(result != nullptr);
    CHECK(result->is_error);
    CHECK(result->content.find("等相应轮次") != std::string::npos);
}
