// tools::AgentTool:内置 "agent" 工具,用 FakeBackend 按脚本吐 StreamEvent
// (不碰真网络),验证:一轮纯文本直接返回;子代理调工具(用假工具)后
// 返回;超 max_turns 报 is_error;确认回调转发(父拒绝 -> 子内工具收到
// 拒绝);usage 累计到父回调;注册表排除(主表见 agent,子表不见,防递归)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    // 根因一回归用:每次 send_stream 收到的 cancel 指针原样记下来,断言它
    // 就是 main.cpp 那份 cancel_flag 的地址,证明 AgentTool -> sub_loop.Run()
    // 这一路确实把打断信号透传进去了,不是每次都收 nullptr。
    std::vector<const std::atomic<bool>*> captured_cancel_ptrs;

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        captured_requests.push_back(request);
        captured_cancel_ptrs.push_back(cancel);
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

struct BlockingBackendState {
    std::mutex mutex;
    std::condition_variable ready;
    bool started = false;
    bool release = false;
    std::vector<api::Request> captured_requests;
};

// 后台回归用。请求进来后先挂住，直到测试放闸；若 AgentTool 错把它跑在
// 前台，execute() 就会跟着等，耗时断言会当场抓住。
class BlockingBackend : public api::Backend {
public:
    explicit BlockingBackend(std::shared_ptr<BlockingBackendState> state) : state_(std::move(state)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->captured_requests.push_back(request);
            state_->started = true;
            state_->ready.notify_all();
            state_->ready.wait_for(lock, std::chrono::seconds(2), [&]() {
                return state_->release || (cancel != nullptr && cancel->load(std::memory_order_acquire));
            });
        }
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
        }
        const std::vector<api::StreamEvent> events = {
            api::MessageStart{"msg", "model"},
            api::TextDelta{"后台摸排完毕"},
            api::ContentBlockDone{0},
            api::MessageDone{"end_turn", api::Usage{120, 30, 0, 0}},
        };
        for (const auto& event : events) {
            on_event(event);
        }
        return {};
    }

private:
    std::shared_ptr<BlockingBackendState> state_;
};

// "等到 cancel 才放行"的后端:统一台账后前台任务的打断信号是合并指针
// (RunTask 栈上的局部变量),测试侧不能在它析构后再解引用——所以在
// send_stream 返回前就地记下"cancel 是否被置位",事后只读这个布尔。
struct WaitCancelState {
    std::mutex mutex;
    std::condition_variable ready;
    bool started = false;
    bool saw_cancel = false;
};

class WaitCancelBackend : public api::Backend {
public:
    explicit WaitCancelBackend(std::shared_ptr<WaitCancelState> state) : state_(std::move(state)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)request;
        (void)on_event;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->started = true;
            state_->ready.notify_all();
        }
        if (cancel == nullptr) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "no cancel pointer", 0});
        }
        while (!cancel->load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->saw_cancel = true;  // 合并指针还活着,就地记账
        }
        return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
    }

private:
    std::shared_ptr<WaitCancelState> state_;
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
    input["title"] = "测试任务";
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
    {
        const tools::Tool::Result result =
            agent_tool.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "查一查"}, {"agent_type", 7}});
        CHECK(result.is_error);
    }
    {
        const tools::Tool::Result result =
            agent_tool.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "查一查"}, {"run_in_background", "yes"}});
        CHECK(result.is_error);
    }
    CHECK(backend.captured_requests.empty());
}

// 真正短 title(规格一):缺失/空白/多行/超 40 显示列一律拒绝,提示补标题
// 重试;绝不替调用方截成另一句话,更不拿 prompt 片段冒充。
TEST_CASE("agent 工具:title 必填——缺失、空白、多行、超宽一律拒绝,不发请求") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    // 缺失。
    CHECK(agent_tool.execute(nlohmann::json{{"prompt", "查调用链"}}).is_error);
    // 只有空白(trim 后空)。
    CHECK(agent_tool.execute(nlohmann::json{{"title", "   \t "}, {"prompt", "查调用链"}}).is_error);
    // 多行。
    CHECK(agent_tool.execute(nlohmann::json{{"title", "两行\n标题"}, {"prompt", "查调用链"}}).is_error);
    CHECK(agent_tool.execute(nlohmann::json{{"title", "带回车\r标题"}, {"prompt", "查调用链"}}).is_error);
    // 超 40 显示列(21 个三列宽汉字 = 63 列)。
    std::string wide;
    for (int i = 0; i < 21; ++i) {
        wide += "汉";
    }
    const auto too_wide = agent_tool.execute(nlohmann::json{{"title", wide}, {"prompt", "查调用链"}});
    CHECK(too_wide.is_error);
    CHECK(too_wide.content.find("40") != std::string::npos);
    // 恰好 40 显示列(40 个 ASCII)——不拒绝。
    backend.scripts = {TextOnlyScript("结论")};
    const auto exact = agent_tool.execute(
        nlohmann::json{{"title", std::string(40, 't')}, {"prompt", "查调用链"}});
    CHECK_FALSE(exact.is_error);
    CHECK(backend.captured_requests.size() == 1);  // 只有最后这次真发了请求
    // 拒绝信息提示补标题(不是 prompt 报错)。
    const auto missing = agent_tool.execute(nlohmann::json{{"prompt", "查调用链"}});
    CHECK(missing.content.find("title") != std::string::npos);
}

TEST_CASE("agent 工具:title 与 prompt 各司其职——快照只存原样 title,prompt 不上显示名") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("结论")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    const std::string long_prompt = "你在一个 C++ 项目的隔离 git worktree 里实施项目记忆系统升级,这段说明很长很长";
    const auto result = agent_tool.execute(
        nlohmann::json{{"title", "项目记忆升级一期"}, {"prompt", long_prompt}});
    CHECK_FALSE(result.is_error);
    // 前台任务也进统一台账:title/prompt 原样入账,foreground 标记为真。
    const auto fg = agent_tool.TaskSummaries();
    REQUIRE(fg.size() == 1);
    CHECK(fg[0].title == "项目记忆升级一期");
    CHECK(fg[0].prompt == long_prompt);
    CHECK(fg[0].foreground);

    auto detached_backend = std::make_unique<FakeBackend>();
    detached_backend->scripts = {TextOnlyScript("后台结论")};
    tools::AgentTool bg_tool(*detached_backend, sub_registry, "/work/dir");
    bg_tool.SetDetachedBackendFactory(
        [&detached_backend]() {
            tools::DetachedAgentBackend detached;
            detached.backend = std::move(detached_backend);
            return detached;
        });
    CHECK(bg_tool
              .execute(nlohmann::json{{"title", "检索阈值回归"}, {"prompt", long_prompt},
                                      {"run_in_background", true}})
              .content.find("#") != std::string::npos);
    const auto summaries = bg_tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].title == "检索阈值回归");
    CHECK(summaries[0].prompt == long_prompt);
    CHECK_FALSE(summaries[0].foreground);
    const auto detail = bg_tool.TaskDetail(summaries[0].id);
    REQUIRE(detail.has_value());
    CHECK(detail->title == "检索阈值回归");
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
    input["title"] = "测试任务";
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
    input["title"] = "测试任务";
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
    input2["title"] = "测试任务二";
    input2["prompt"] = "死循环吧";
    input2["max_turns"] = 2;
    const tools::Tool::Result result2 = agent_tool2.execute(input2);
    CHECK(result2.is_error);
    CHECK(backend2.captured_requests.size() == 2);
}

TEST_CASE("agent 工具:入参 max_turns=0 透传给子代理,子代理循环按无上限跑,不会被截断") {
    FakeBackend backend;
    for (int i = 0; i < 8; ++i) {
        backend.scripts.push_back(ToolUseScript("toolu_many", "fake_tool"));
    }
    backend.scripts.push_back(TextOnlyScript("跑完了"));
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    // 构造时的默认值是个有限数(3),证明"无上限"确实是入参 max_turns=0
    // 生效,不是构造默认值恰好够用。
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir", /*model=*/"", /*default_max_turns=*/3);

    nlohmann::json input;
    input["title"] = "测试任务";
    input["prompt"] = "跑很多轮";
    input["max_turns"] = 0;
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(result.content == "跑完了");
    CHECK(backend.captured_requests.size() == 9);  // 8 次工具轮 + 1 次收尾,没被截断
}

TEST_CASE("agent 工具:入参 max_turns 是负数直接报错,不透传给子代理") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    nlohmann::json input;
    input["title"] = "测试任务";
    input["prompt"] = "随便";
    input["max_turns"] = -1;
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("负数") != std::string::npos);
    CHECK(backend.captured_requests.empty());  // 校验没过,压根没发出请求
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
    input["title"] = "测试任务";
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
    input["title"] = "测试任务";
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
    input["title"] = "测试任务";
    input["prompt"] = "随便跑跑";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    // on_tool_confirm 没设,AgentLoop 默认视为允许(见 loop.cpp RunOneTool)。
    CHECK(fake_tool_ptr->call_count == 1);
}

TEST_CASE("agent 工具:hooks.cancel 原样透传给 sub_loop.Run()(根因一回归:ESC/Ctrl+C 打断信号不再在子代理这一层断线)") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("子代理跑完了")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    // 没设 hooks.cancel(默认 nullptr)时,子代理里的请求应该收到 nullptr——
    // 跟从前(没有这个字段时)行为完全一样,不该凭空冒出一个非空指针。
    {
        nlohmann::json input;
        input["title"] = "测试任务";
        input["prompt"] = "第一次不设 cancel";
        const tools::Tool::Result result = agent_tool.execute(input);
        CHECK_FALSE(result.is_error);
        REQUIRE(backend.captured_cancel_ptrs.size() == 1);
        // 统一台账后前台任务也有自己的 TaskRecord:cancel 指针是任务自己的
        // 停止信号(面板 x 接这根),不再是 nullptr。
        CHECK(backend.captured_cancel_ptrs[0] != nullptr);
    }

    // 设了 hooks.cancel 之后,父轮 ESC 的停止信号必须能进子代理的请求。
    // 统一台账后前台任务的 cancel 是"父轮信号 + 面板 x 信号"合并出的一根
    // 指针(20ms 粒度合并线程),不再与 hooks.cancel 同址,但父轮置位后它
    // 必须跟着置位——用一只等到 cancel 才放行的后端钉死这条功能链。
    std::atomic<bool> cancel_flag{false};
    tools::AgentTool::Hooks hooks;
    hooks.cancel = &cancel_flag;
    agent_tool.SetHooks(hooks);

    auto wait_state = std::make_shared<WaitCancelState>();
    WaitCancelBackend wait_backend(wait_state);
    tools::ToolRegistry wait_registry;
    tools::AgentTool wait_tool(wait_backend, wait_registry, "/work/dir");
    tools::AgentTool::Hooks wait_hooks;
    wait_hooks.cancel = &cancel_flag;
    wait_tool.SetHooks(wait_hooks);
    std::thread runner([&] {
        (void)wait_tool.execute(nlohmann::json{{"title", "等打断"}, {"prompt", "跑吧"}});
    });
    std::unique_lock<std::mutex> state_lock(wait_state->mutex);
    REQUIRE(wait_state->ready.wait_for(state_lock, std::chrono::seconds(2), [&] { return wait_state->started; }));
    state_lock.unlock();
    cancel_flag.store(true);
    runner.join();
    // 父轮 ESC 真传进了子代理请求(合并指针在请求返回前被置位)。
    CHECK(wait_state->saw_cancel);
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

TEST_CASE("Explore 子代理只把只读白名单工具交给模型") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("只读摸排完成")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"read", false}, false));
    sub_registry.Register(std::make_unique<FakeTool>("search", tools::Tool::Result{"search", false}, false));
    sub_registry.Register(std::make_unique<FakeTool>("skill", tools::Tool::Result{"skill", false}, false));
    sub_registry.Register(std::make_unique<FakeTool>("write_file", tools::Tool::Result{"write", false}, true));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    const auto result = agent_tool.execute(
        nlohmann::json{{"title", "测试任务"}, {"prompt", "查清调用链"}, {"agent_type", "Explore"}, {"run_in_background", false}});

    CHECK_FALSE(result.is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    CHECK(backend.captured_requests[0].system.find("Explore") != std::string::npos);
    REQUIRE(backend.captured_requests[0].tools.size() == 2);
    CHECK(backend.captured_requests[0].tools[0].name == "read_file");
    CHECK(backend.captured_requests[0].tools[1].name == "search");
}

TEST_CASE("后台子代理立即交回任务号,独立跑完,结果只投递一次") {
    FakeBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    auto state = std::make_shared<BlockingBackendState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([state]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<BlockingBackend>(state);
        detached.model = "detached-model";
        detached.reasoning_effort = "high";
        detached.request_extra_body["temperature"] = 0;
        return detached;
    });

    const auto begin = std::chrono::steady_clock::now();
    const auto launch = agent_tool.execute(
        nlohmann::json{{"title", "测试任务"}, {"prompt", "在后台查清楚"}, {"agent_type", "Explore"}, {"run_in_background", true}});
    const auto launch_time = std::chrono::steady_clock::now() - begin;

    CHECK_FALSE(launch.is_error);
    CHECK(launch.content.find("#1") != std::string::npos);
    CHECK(launch_time < std::chrono::milliseconds(500));
    REQUIRE(agent_tool.TaskSnapshots().size() == 1);
    CHECK(agent_tool.TaskSnapshots()[0].state == tools::AgentTaskState::Running);
    // 跑着的时候没有"未投递的完成结果"——主循环不该被唤醒。
    CHECK_FALSE(agent_tool.HasUndeliveredCompletions());

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->ready.notify_all();

    for (int i = 0; i < 100 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(agent_tool.HasRunningTasks());
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].state == tools::AgentTaskState::Done);
    CHECK(snapshots[0].result == "后台摸排完毕");
    CHECK(snapshots[0].input_tokens == 120);
    CHECK(snapshots[0].output_tokens == 30);

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        REQUIRE(state->captured_requests.size() == 1);
        CHECK(state->captured_requests[0].model == "detached-model");
        CHECK(state->captured_requests[0].reasoning_effort == "high");
        CHECK(state->captured_requests[0].extra_body["temperature"] == 0);
    }

    const std::string first_notice = agent_tool.DrainCompletionNotices();
    CHECK(first_notice.find("后台摸排完毕") != std::string::npos);
    CHECK(agent_tool.DrainCompletionNotices().empty());
}

// 回流回归(2026-08-14 死机会话):后台子代理在会话空闲时跑完,主循环正
// 阻塞在等键输入上。面板轮询看得见"完成"(TaskRevision 变了),但结果
// 只在下一个 RunTurn 开头才被 Drain 走——没有"有货待投递"的信号,主循环
// 就永远不会醒,会话冻死在代理面板。HasUndeliveredCompletions 就是那条
// 缺掉的信号:进终态而未投递时为真,投递一次后翻回假。
TEST_CASE("agent 工具:空闲时跑完的后台子代理,HasUndeliveredCompletions 翻真直到被投递") {
    tools::ToolRegistry sub_registry;
    auto state = std::make_shared<BlockingBackendState>();
    FakeBackend foreground_backend;  // 前台路径不触发,挂着只为构造
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([state]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<BlockingBackend>(state);
        return detached;
    });

    CHECK_FALSE(agent_tool.HasUndeliveredCompletions());  // 一个任务都没有:别唤醒
    CHECK(agent_tool.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "后台摸排"}, {"run_in_background", true}}).content.find(
              "#1") != std::string::npos);
    CHECK_FALSE(agent_tool.HasUndeliveredCompletions());  // 跑着:也别唤醒

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->ready.notify_all();
    for (int i = 0; i < 100 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    // 任务已进终态、结果还没投递——这正是主循环该被唤醒的窗口。
    REQUIRE(agent_tool.HasUndeliveredCompletions());
    const std::string notice = agent_tool.DrainCompletionNotices();
    CHECK(notice.find("后台摸排完毕") != std::string::npos);
    CHECK_FALSE(agent_tool.HasUndeliveredCompletions());  // 投递过就翻回假,不会反复唤醒
}

// ---------------------------------------------------------------------------
// 统一台账(规格二):前台任务也进 TaskRecord,面板/详情/统计全认同一本账。
// ---------------------------------------------------------------------------

TEST_CASE("统一台账:前台任务当场入账 Running,跑完 Done,统计落准且不走后台回流") {
    auto state = std::make_shared<BlockingBackendState>();
    BlockingBackend backend(state);
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    std::thread runner([&] {
        const tools::Tool::Result r =
            agent_tool.execute(nlohmann::json{{"title", "前台摸排"}, {"prompt", "慢慢查"}});
        CHECK_FALSE(r.is_error);
        CHECK(r.content == "后台摸排完毕");
    });
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        REQUIRE(state->ready.wait_for(lock, std::chrono::seconds(2), [&] { return state->started; }));
    }
    // 阻塞着的当口,台账里就是一条 Running 的前台任务——面板看得到。
    const auto mid = agent_tool.TaskSummaries();
    REQUIRE(mid.size() == 1);
    CHECK(mid[0].state == tools::AgentTaskState::Running);
    CHECK(mid[0].foreground);
    CHECK(mid[0].title == "前台摸排");

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->ready.notify_all();
    runner.join();

    const auto done = agent_tool.TaskSummaries();
    REQUIRE(done.size() == 1);
    CHECK(done[0].state == tools::AgentTaskState::Done);
    CHECK(done[0].foreground);
    CHECK(done[0].input_tokens == 120);
    CHECK(done[0].output_tokens == 30);
    // 前台结论直接交回父级工具调用,不进后台完成回流。
    CHECK_FALSE(agent_tool.HasUndeliveredCompletions());
    CHECK(agent_tool.DrainCompletionNotices().empty());
}

TEST_CASE("统一台账:面板 x 停掉前台任务——父级收到取消结果,台账变 Cancelled") {
    auto state = std::make_shared<BlockingBackendState>();
    BlockingBackend backend(state);
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    std::thread runner([&] {
        const tools::Tool::Result r = agent_tool.execute(nlohmann::json{{"title", "要被停的"}, {"prompt", "查"}});
        // 请求被取消:AgentLoop 按打断收场,半截话带打断标注交回父级
        // (is_error=false,内容是打断说明,不是崩溃)。
        CHECK_FALSE(r.is_error);
        CHECK(r.content.find("打断") != std::string::npos);
    });
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        REQUIRE(state->ready.wait_for(lock, std::chrono::seconds(2), [&] { return state->started; }));
    }
    const auto mid = agent_tool.TaskSummaries();
    REQUIRE(mid.size() == 1);
    CHECK(agent_tool.CancelTask(mid[0].id));
    runner.join();

    const auto after = agent_tool.TaskSummaries();
    REQUIRE(after.size() == 1);
    CHECK(after[0].state == tools::AgentTaskState::Cancelled);
    CHECK_FALSE(agent_tool.HasRunningTasks());
}

TEST_CASE("统一台账:一轮里先后多只前台代理,各自留终态,新一只不清空上一只") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("结论甲"), TextOnlyScript("结论乙")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    CHECK_FALSE(
        agent_tool.execute(nlohmann::json{{"title", "甲任务"}, {"prompt", "查甲"}, {"run_in_background", false}})
            .is_error);
    CHECK_FALSE(
        agent_tool.execute(nlohmann::json{{"title", "乙任务"}, {"prompt", "查乙"}, {"run_in_background", false}})
            .is_error);

    const auto summaries = agent_tool.TaskSummaries();
    REQUIRE(summaries.size() == 2);
    CHECK(summaries[0].title == "甲任务");
    CHECK(summaries[1].title == "乙任务");
    CHECK(summaries[0].state == tools::AgentTaskState::Done);
    CHECK(summaries[1].state == tools::AgentTaskState::Done);
    CHECK(summaries[0].foreground);
    CHECK(summaries[1].foreground);
}
