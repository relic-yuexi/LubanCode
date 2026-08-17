// AgentLoop 的核心行为,用 FakeBackend 按脚本吐 StreamEvent,不碰真网络:
// 一轮 text 直接结束;tool_use 一轮 -> 工具执行 -> 第二次请求历史里带
// tool_result -> end_turn;用户拒绝确认 -> tool_result 是 is_error;
// 超过步数上限报错。

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <variant>
#include <vector>

#include "agent/loop.hpp"
#include "agent/session_store.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    std::string accumulated_text;
    agent::Callbacks callbacks;
    callbacks.on_text_delta = [&](const std::string& text) { accumulated_text += text; };

    const auto result = loop.Run("你好", callbacks);

    REQUIRE(result.has_value());
    CHECK(accumulated_text == "你好呀");
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
    agent::Callbacks callbacks;

    // 默认构造(兼容门不传值)= unset:api::Request::max_tokens 是 nullopt,
    // 不再有一枚写死的 4096——chat/responses 端整个不发字段,交服务端默认。
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    REQUIRE(loop.Run("问", callbacks).has_value());
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK_FALSE(backend.captured_requests[0].max_tokens.has_value());
    CHECK(loop.runtime_profile().max_output_tokens == std::nullopt);

    // profile 声明了 8192:请求带上声明值,main 与子代理同一份。
    FakeBackend backend2;
    backend2.scripts = {TextOnlyScript("好")};
    agent::AgentRuntimeProfile profile;
    profile.model = "test-model";
    profile.max_output_tokens = 8192;
    profile.max_output_tokens_source = agent::OutputBudgetSource::ConfigFile;
    agent::AgentLoop loop2(backend2, registry, profile, "system prompt");
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
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
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
    agent::AgentLoop loop2(backend2, registry, "test-model", "system prompt");
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
    agent::AgentLoop loop3(backend3, registry, "test-model", "system prompt");
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
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
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
    profile.model = "test-model";
    profile.length_continuations = 0;  // 显式关掉续跑
    agent::AgentLoop loop(backend, registry, profile, "system prompt");
    agent::Callbacks callbacks;
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
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    std::vector<std::string> started_tools;
    agent::Callbacks callbacks;
    callbacks.on_tool_start = [&](const std::string& name, const nlohmann::json&) { started_tools.push_back(name); };

    const auto result = loop.Run("帮我用一下工具", callbacks);

    REQUIRE(result.has_value());
    CHECK(backend.captured_requests.size() == 2);
    // 计数语义:工具回填后再请求收正文,turn 内走了两步(step)。
    CHECK(result->steps_used == 2);
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

TEST_CASE("计数语义:一次 assistant 并行叫三件工具,仍是一步;工具回填后再收口才是第二步") {
    FakeBackend backend;
    backend.scripts = {
        MultiToolUseScript({"toolu_1", "toolu_2", "toolu_3"}, "fake_tool"),
        TextOnlyScript("三件都办完了"),
    };
    tools::ToolRegistry registry;
    auto* fake_tool_ptr = new FakeTool("fake_tool", tools::Tool::Result{"工具结果", false}, false);
    registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    agent::Callbacks callbacks;
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

TEST_CASE("超过步数上限:预算耗尽不是错误,hit_step_limit 带 steps/stop_reason 交回") {
    FakeBackend backend;
    // 永远回 tool_use,模型一直要工具,逼近步数上限。
    for (int i = 0; i < 5; ++i) {
        backend.scripts.push_back(ToolUseScript("toolu_loop", "fake_tool"));
    }
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt", 4096, /*max_steps_per_turn=*/3);

    agent::Callbacks callbacks;
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt", 4096, /*max_steps_per_turn=*/0);
    agent::Callbacks callbacks;
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");  // 不传步数预算,用默认值
    agent::Callbacks callbacks;
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    std::vector<api::UsageReport> reports;
    agent::Callbacks callbacks;
    callbacks.on_usage = [&](const api::UsageReport& report) { reports.push_back(report); };

    const auto result = loop.Run("帮我用一下工具", callbacks);

    REQUIRE(result.has_value());
    REQUIRE(reports.size() == 2);
    // 逐笔带身份:步号、请求 id、模型——逐步流水账有键可落。
    CHECK(reports[0].step_index == 0);
    CHECK(reports[0].request_id == "msg");
    CHECK(reports[0].model == "model");
    CHECK(reports[1].step_index == 1);
    CHECK(reports[1].request_id == "msg2");
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");

    std::atomic<bool> cancel_flag{false};
    agent::Callbacks callbacks;
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    std::atomic<bool> cancel_flag{false};
    agent::Callbacks callbacks;
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    std::atomic<bool> cancel_flag{false};
    agent::Callbacks callbacks;
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;
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
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt", 4096, 25,
                          /*max_context_chars=*/200);
    agent::Callbacks callbacks;
    const auto result = loop.Run(std::string(2000, 'x'), callbacks);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("上下文") != std::string::npos);
    CHECK(backend.captured_requests.empty());  // 一次请求都没发出去
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

    agent::AgentLoop loop(backend, registry, "test-model", "system prompt", 4096, /*max_steps_per_turn=*/4);
    agent::Callbacks callbacks;
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
    agent::AgentLoop loop(backend, registry, "test-model", "stable system");
    loop.SetTurnContext("project memory context");

    REQUIRE(loop.Run("go", agent::Callbacks{}).has_value());
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
    const std::string session_line = agent::SerializeSessionMessage(loop.History()[0], "ts");
    CHECK(session_line.find("project memory context") == std::string::npos);
    const std::string exported = agent::ExportSessionMarkdown(agent::SessionMeta{}, loop.History(), "test");
    CHECK(exported.find("project memory context") == std::string::npos);
}

TEST_CASE("图片用户消息入历史，下一轮请求仍带着") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("看到了"), TextOnlyScript("还在")};
    tools::ToolRegistry registry;
    agent::AgentLoop loop(backend, registry, "test-model", "system prompt");
    agent::Callbacks callbacks;

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
