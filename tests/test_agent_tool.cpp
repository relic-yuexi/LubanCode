// tools::AgentTool:内置 "agent" 工具,用 FakeBackend 按脚本吐 StreamEvent
// (不碰真网络),验证:一轮纯文本直接返回;子代理调工具(用假工具)后
// 返回;超步数预算按 budget_exhausted 收账(带部分结果与步数账);最后一步
// 无文本结论/接口报错按结构化 TaskOutcome 分型;确认回调转发(父拒绝 ->
// 子内工具收到拒绝);usage 累计到父回调;注册表排除(主表见 agent,子表
// 不见,防递归)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/backend.hpp"
#include "api/types.hpp"
#include "hooks/dispatcher.hpp"
#include "hooks/hash.hpp"
#include "hooks/loader.hpp"
#include "hooks/trust.hpp"
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

// 同一条 assistant 消息里带 N 枚 tool_use(并行工具调用):计数语义测试
// 用它钉"一个 step 可含多枚工具调用,只发一次模型请求"。
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

// 计数语义(命名规范阶段 A):turn/step/tool_call 三本账分开。子代理台账
// 里的 steps_used 记的是模型请求数(step);同一 assistant 消息里并行叫
// 几件工具,只算一步,tool_calls 流水另记。
TEST_CASE("agent 工具:计数账——一步并行三件工具 steps_used 仍记一步,工具流水记三笔") {
    FakeBackend backend;
    backend.scripts = {
        MultiToolUseScript({"toolu_1", "toolu_2", "toolu_3"}, "fake_tool"),
        TextOnlyScript("三件都办完了"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"工具结果", false}, false));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    nlohmann::json input;
    input["title"] = "测试任务";
    input["prompt"] = "把三件事都办了";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    // 两次模型请求:第一步带回三枚工具调用,第二步收正文。
    REQUIRE(backend.captured_requests.size() == 2);
    // 台账:步数两笔,工具流水三笔。
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].steps_used == 2);
    CHECK(snapshots[0].tool_calls.size() == 3);
    CHECK(snapshots[0].outcome.steps_used == 2);
    CHECK(snapshots[0].outcome.status == tools::TaskOutcomeStatus::Completed);
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

TEST_CASE("agent 工具:超过步数预算返回 budget_exhausted,带部分结果与步数账") {
    FakeBackend backend;
    for (int i = 0; i < 5; ++i) {
        backend.scripts.push_back(ToolUseScript("toolu_loop", "fake_tool"));
    }
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir", /*model=*/"", /*default_max_steps_per_turn=*/3);

    nlohmann::json input;
    input["title"] = "测试任务";
    input["prompt"] = "死循环吧";
    const tools::Tool::Result result = agent_tool.execute(input);

    // 0.30.x:预算耗尽不再是笼统 failed——结果以 [budget_exhausted] 打头,
    // 带步数账(3/3)、stop_reason 与检查点(最后取得的工具结果),几十步
    // 探索不许一笔勾销(规格"现场三/四")。
    CHECK(result.is_error);
    CHECK(result.content.find("[budget_exhausted]") == 0);
    CHECK(result.content.find("3/3 步") != std::string::npos);
    CHECK(result.content.find("stop_reason: tool_use") != std::string::npos);
    CHECK(result.content.find("fake_tool") != std::string::npos);  // 检查点带最后工具结果
    CHECK(backend.captured_requests.size() == 3);  // 正好用满步数预算次请求
    // 台账:终态 BudgetExhausted,步数/预算/结构化 outcome 都在快照里。
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].state == tools::AgentTaskState::BudgetExhausted);
    CHECK(snapshots[0].steps_used == 3);
    CHECK(snapshots[0].step_limit == 3);
    CHECK(snapshots[0].outcome.status == tools::TaskOutcomeStatus::BudgetExhausted);
    CHECK(snapshots[0].outcome.reason == tools::TaskOutcomeReason::StepLimitExhausted);
    CHECK(snapshots[0].outcome.stop_reason == "tool_use");
    CHECK(snapshots[0].outcome.steps_used == 3);

    // 入参给的步数预算能覆盖构造时的默认值。
    FakeBackend backend2;
    for (int i = 0; i < 5; ++i) {
        backend2.scripts.push_back(ToolUseScript("toolu_loop", "fake_tool"));
    }
    tools::ToolRegistry sub_registry2;
    sub_registry2.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));
    tools::AgentTool agent_tool2(backend2, sub_registry2, "/work/dir");  // 默认 0(不限轮,见构造器注释)

    nlohmann::json input2;
    input2["title"] = "测试任务二";
    input2["prompt"] = "死循环吧";
    input2["max_turns"] = 2;
    const tools::Tool::Result result2 = agent_tool2.execute(input2);
    CHECK(result2.is_error);
    CHECK(result2.content.find("[budget_exhausted]") == 0);
    CHECK(backend2.captured_requests.size() == 2);
}

TEST_CASE("agent 工具:最后一轮无文本结论时保留 stop reason 与最后工具状态,不交白卷") {
    FakeBackend backend;
    // 第一轮调工具,第二轮 end_turn 但一个字都没有(空 TextDelta):老版只报
    // "没有给出文本结论",stop reason 与最后工具状态全丢;新版按结构化
    // failed/no_final_text 收账,检查点带最后工具结果(规格"现场三")。
    backend.scripts = {ToolUseScript("toolu_a", "fake_tool"), TextOnlyScript("")};
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"查到三个入口", false}, false));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    nlohmann::json input;
    input["title"] = "空结论任务";
    input["prompt"] = "查完别说话";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("[failed]") == 0);
    CHECK(result.content.find("没有文本结论") != std::string::npos);
    CHECK(result.content.find("stop_reason: end_turn") != std::string::npos);
    CHECK(result.content.find("查到三个入口") != std::string::npos);  // 部分结果带回
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].state == tools::AgentTaskState::Failed);
    CHECK(snapshots[0].outcome.reason == tools::TaskOutcomeReason::NoFinalText);
    CHECK(snapshots[0].outcome.stop_reason == "end_turn");
    CHECK(snapshots[0].outcome.last_tool.find("fake_tool") != std::string::npos);
    CHECK(snapshots[0].outcome.partial_result.find("查到三个入口") != std::string::npos);
}

TEST_CASE("agent 工具:接口报错按 failed/api_error 分型,不再混进笼统失败") {
    FakeBackend backend;
    backend.scripts = {};  // 第一次请求就"脚本用完了" -> 请求失败

    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    nlohmann::json input;
    input["title"] = "接口错任务";
    input["prompt"] = "还没开始就结束";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK(result.is_error);
    CHECK(result.content.find("[failed]") != std::string::npos);
    CHECK(result.content.find("请求失败") != std::string::npos);
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].state == tools::AgentTaskState::Failed);
    CHECK(snapshots[0].outcome.reason == tools::TaskOutcomeReason::ApiError);
}

TEST_CASE("agent 工具:入参新名 max_steps_per_turn 生效,schema 只出新名、旧名 max_turns 仍收") {
    FakeBackend backend;
    for (int i = 0; i < 6; ++i) {
        backend.scripts.push_back(ToolUseScript("toolu_step", "fake_tool"));
    }
    backend.scripts.push_back(TextOnlyScript("收工"));
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"ok", false}, false));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir", /*model=*/"", /*default_max_steps_per_turn=*/3);

    // 新名入参:预算 7 步(6 次工具步 + 1 次收尾),压过构造默认 3——
    // 默认 3 的话第三次工具步后就被硬闸掐断,能过这里全靠新名生效。
    nlohmann::json input;
    input["title"] = "测试任务";
    input["prompt"] = "跑六步";
    input["max_steps_per_turn"] = 7;
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    CHECK(backend.captured_requests.size() == 7);  // 6 次工具步 + 1 次收尾,新名生效

    // schema 只出新名,旧名不在(兼容只在解析层)。
    const nlohmann::json schema = agent_tool.input_schema();
    const std::string dumped = schema.dump();
    CHECK(dumped.find("max_steps_per_turn") != std::string::npos);
    CHECK(dumped.find("\"max_turns\"") == std::string::npos);
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
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir", /*model=*/"", /*default_max_steps_per_turn=*/3);

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

    std::vector<api::UsageReport> reports;
    tools::AgentTool::Hooks hooks;
    hooks.on_usage = [&](const api::UsageReport& report) { reports.push_back(report); };
    agent_tool.SetHooks(hooks);

    nlohmann::json input;
    input["title"] = "测试任务";
    input["prompt"] = "帮我用一下工具";
    const tools::Tool::Result result = agent_tool.execute(input);

    CHECK_FALSE(result.is_error);
    REQUIRE(reports.size() == 2);  // 子代理内部两次独立请求,各触发一次
    CHECK(reports[0].usage.input_tokens == 100);
    CHECK(reports[0].usage.output_tokens == 20);
    CHECK(reports[1].usage.input_tokens == 50);
    CHECK(reports[1].usage.output_tokens == 30);
    // 子代理的逐步身份也跟着转上来(步号在子代理自己的 Run() 里数)。
    CHECK(reports[0].step_index == 0);
    CHECK(reports[1].step_index == 1);
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

// ---------------------------------------------------------------------------
// execution_mode(auto/foreground/background,默认 auto):首版 auto 只随
// 会话缺省走(交互=后台,管道/单发=前台),不做自动猜测;旧 run_in_background
// 仍认,显式(非 auto)的 execution_mode 优先。
// ---------------------------------------------------------------------------

TEST_CASE("execution_mode:显式 foreground/background 生效,auto 随会话缺省,旧参兼容") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("前台结论")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    // 缺省 auto + 会话缺省前台(管道/单发):阻塞跑前台。
    CHECK_FALSE(agent_tool
                    .execute(nlohmann::json{{"title", "模式一"}, {"prompt", "查"}, {"execution_mode", "auto"}})
                    .is_error);
    REQUIRE(agent_tool.TaskSummaries().size() == 1);
    CHECK(agent_tool.TaskSummaries()[0].foreground);

    // 会话缺省翻成后台(交互)后,auto 走后台:没有 detached 工厂会明确报错。
    agent_tool.SetBackgroundByDefault(true);
    const auto no_factory =
        agent_tool.execute(nlohmann::json{{"title", "模式二"}, {"prompt", "查"}, {"execution_mode", "auto"}});
    CHECK(no_factory.is_error);
    CHECK(no_factory.content.find("run_in_background") != std::string::npos);  // 提示写明两套参数

    // 显式 foreground 压过会话缺省后台:照旧阻塞等结论。
    backend.scripts.push_back(TextOnlyScript("显式前台结论"));
    const auto fg = agent_tool.execute(
        nlohmann::json{{"title", "模式三"}, {"prompt", "查"}, {"execution_mode", "foreground"}});
    CHECK_FALSE(fg.is_error);
    CHECK(fg.content == "显式前台结论");

    // 旧参兼容:false = foreground。
    backend.scripts.push_back(TextOnlyScript("旧参前台结论"));
    const auto legacy_fg =
        agent_tool.execute(nlohmann::json{{"title", "模式四"}, {"prompt", "查"}, {"run_in_background", false}});
    CHECK_FALSE(legacy_fg.is_error);
    CHECK(legacy_fg.content == "旧参前台结论");

    // 旧参 true = background:同样吃"没配后台后端"的明确报错。
    const auto legacy_bg =
        agent_tool.execute(nlohmann::json{{"title", "模式五"}, {"prompt", "查"}, {"run_in_background", true}});
    CHECK(legacy_bg.is_error);

    // 显式 background 压过一切(这里没工厂,仍报错,但报的是后台未配置)。
    const auto bg = agent_tool.execute(
        nlohmann::json{{"title", "模式六"}, {"prompt", "查"}, {"execution_mode", "background"}});
    CHECK(bg.is_error);

    // 不认得的模式值拒绝,不发请求。
    const auto before = backend.captured_requests.size();
    const auto bad =
        agent_tool.execute(nlohmann::json{{"title", "模式七"}, {"prompt", "查"}, {"execution_mode", "somewhere"}});
    CHECK(bad.is_error);
    CHECK(bad.content.find("execution_mode") != std::string::npos);
    CHECK(backend.captured_requests.size() == before);
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

// 退场链钉死(查看态回流单第一桩):done+delivered 的坞行退场靠面板数据源
// 按 TaskRevision 缓存拉新快照——DrainCompletionNotices 置 delivered 时若不
// Touch 修订号,退场永远到不了屏上,行赖在坞里直到别的任务碰巧碰一下账。
// 用户实测"看着 #2,#1 完成后坞里那行不退场"正是这一环断的。
TEST_CASE("agent 工具:投递置位即 TouchTasks,面板修订号跟上,退场不再迟滞") {
    tools::ToolRegistry sub_registry;
    auto state = std::make_shared<BlockingBackendState>();
    FakeBackend foreground_backend;  // 前台路径不触发,挂着只为构造
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([state]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<BlockingBackend>(state);
        return detached;
    });

    CHECK(agent_tool.execute(nlohmann::json{{"title", "测试任务"}, {"prompt", "后台摸排"}, {"run_in_background", true}})
              .content.find("#1") != std::string::npos);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->ready.notify_all();
    for (int i = 0; i < 100 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    // 未投递任务号口子(toast 报"谁完成了"用):peek 不置位。
    REQUIRE(agent_tool.UndeliveredCompletionTaskIds() == std::vector<int>{1});

    const std::uint64_t revision_before = agent_tool.TaskRevision();
    (void)agent_tool.DrainCompletionNotices();
    // delivered 一翻,修订号必须跟上——面板(BuildAgentPanelEntries 的缓存)
    // 只认修订号,不 Touch 就看不见退场。
    CHECK(agent_tool.TaskRevision() != revision_before);
    CHECK(agent_tool.UndeliveredCompletionTaskIds().empty());

    // 再 Drain 无货:不动账,修订号原地不动(不该无端触发面板重拉)。
    const std::uint64_t revision_after = agent_tool.TaskRevision();
    CHECK(agent_tool.DrainCompletionNotices().empty());
    CHECK(agent_tool.TaskRevision() == revision_after);
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

// ---------------------------------------------------------------------------
// 子代理消息账(规格"现场三"):每只任务独立的、按时间追加的事件流。
// 事件类型与 main 对齐,查看态复用 main renderer——这里钉账本身:时序、
// 中间文字不丢、工具配对、介入落点、终局事件。
// ---------------------------------------------------------------------------

// 一条 assistant 消息里"先说话再调工具"的脚本:正文块(block 0)在前,
// tool_use 块(block 1)在后——钉"助手中间文字不能因后续工具调用而丢"。
std::vector<api::StreamEvent> TextThenToolScript(const std::string& text, const std::string& tool_id,
                                                 const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::ToolUseStart{1, tool_id, tool_name},
        api::ToolUseInputDelta{1, "{}"},
        api::ContentBlockDone{1},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

TEST_CASE("消息账:按次序 user -> 正文 -> 工具 -> 结果 -> 正文 -> 完成,顺序不乱") {
    FakeBackend backend;
    backend.scripts = {
        TextThenToolScript("先看一眼目录", "toolu_1", "fake_tool"),
        TextOnlyScript("结论:共三处入口"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"工具结果全文", false}, false));
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    REQUIRE_FALSE(agent_tool.execute(nlohmann::json{{"title", "查入口"}, {"prompt", "数一数入口"}}).is_error);
    const auto summaries = agent_tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    const auto events = agent_tool.TaskEvents(summaries[0].id);
    REQUIRE(events.size() == 6);
    CHECK(events[0].kind == tools::AgentTaskEventKind::UserMessage);
    CHECK(events[0].text == "数一数入口");
    CHECK(events[1].kind == tools::AgentTaskEventKind::AssistantText);
    CHECK(events[1].text == "先看一眼目录");  // 中间文字不因后续工具调用丢掉
    CHECK(events[2].kind == tools::AgentTaskEventKind::ToolStart);
    CHECK(events[2].tool_name == "fake_tool");
    CHECK(events[3].kind == tools::AgentTaskEventKind::ToolResult);
    CHECK(events[3].tool_name == "fake_tool");
    CHECK(events[3].result == "工具结果全文");
    CHECK_FALSE(events[3].is_error);
    CHECK(events[4].kind == tools::AgentTaskEventKind::AssistantText);
    CHECK(events[4].text == "结论:共三处入口");
    CHECK(events[5].kind == tools::AgentTaskEventKind::Completion);
    CHECK(events[5].text == "结论:共三处入口");  // 完成事件带最终结论全文
}

TEST_CASE("消息账:思考入账、工具出错带错误标记;失败任务的终局是 Failure") {
    FakeBackend backend;
    // 第一轮:思考 + 出错的工具;第二轮:没有文本结论 -> NoFinalText 失败。
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ThinkingDelta{"想一想从哪下手"},
            api::ContentBlockDone{0},
            api::ToolUseStart{1, "toolu_1", "fake_tool"},
            api::ToolUseInputDelta{1, "{}"},
            api::ContentBlockDone{1},
            api::MessageDone{"tool_use", api::Usage{}},
        },
        {
            api::MessageStart{"msg", "model"},
            api::MessageDone{"end_turn", api::Usage{}},
        },
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"炸了", true}, false));
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    agent_tool.execute(nlohmann::json{{"title", "试错"}, {"prompt", "试一下"}});
    const auto summaries = agent_tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    const auto events = agent_tool.TaskEvents(summaries[0].id);
    REQUIRE(events.size() == 5);
    CHECK(events[0].kind == tools::AgentTaskEventKind::UserMessage);
    CHECK(events[1].kind == tools::AgentTaskEventKind::AssistantReasoning);
    CHECK(events[1].text == "想一想从哪下手");
    CHECK(events[2].kind == tools::AgentTaskEventKind::ToolStart);
    CHECK(events[3].kind == tools::AgentTaskEventKind::ToolResult);
    CHECK(events[3].is_error);  // 工具错误在账
    CHECK(events[4].kind == tools::AgentTaskEventKind::Failure);  // 失败终局入账
}

TEST_CASE("消息账:终态任务事件账保留,投递与否不清账(台账与退场分离)") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("完事")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    REQUIRE_FALSE(agent_tool.execute(nlohmann::json{{"title", "小任务"}, {"prompt", "去"}}).is_error);
    const auto summaries = agent_tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    REQUIRE(summaries[0].state == tools::AgentTaskState::Done);
    const auto events = agent_tool.TaskEvents(summaries[0].id);
    REQUIRE(events.size() == 3);  // user + 正文 + completion
    CHECK(events[0].kind == tools::AgentTaskEventKind::UserMessage);
    CHECK(events[1].kind == tools::AgentTaskEventKind::AssistantText);
    CHECK(events[2].kind == tools::AgentTaskEventKind::Completion);
}

// 每轮都挂住的后端:任务线程在轮次里等放闸,测试侧从容 SendTaskMessage
// (无竞态——封账交接只会发生在轮次收口后),放一轮、跑一轮。
class StallAllBackend : public api::Backend {
public:
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        std::size_t started = 0;
        bool release = false;
    };
    explicit StallAllBackend(std::shared_ptr<State> state) : state_(std::move(state)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)request;
        std::size_t index;
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            index = state_->started;
            ++state_->started;
            state_->cv.notify_all();
            state_->cv.wait_for(lock, std::chrono::seconds(5), [&]() {
                return state_->release || (cancel != nullptr && cancel->load(std::memory_order_acquire));
            });
            state_->release = false;  // 一次放行只放一轮
        }
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
        }
        const std::vector<api::StreamEvent> events =
            index == 0 ? ToolUseScript("toolu_1", "fake_tool") : TextOnlyScript("收尾:按补充意见办");
        for (const auto& event : events) {
            on_event(event);
        }
        return {};
    }

private:
    std::shared_ptr<State> state_;
};

TEST_CASE("消息账:中途介入记 steering_message,落在收到它的那个位置") {
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"probe ok", false}, false));
    const auto state = std::make_shared<StallAllBackend::State>();
    FakeBackend foreground_placeholder;  // 构造用,后台走 detached factory
    tools::AgentTool agent_tool(foreground_placeholder, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([state]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<StallAllBackend>(state);
        return detached;
    });

    CHECK(agent_tool
              .execute(nlohmann::json{{"title", "摸排"}, {"prompt", "查一查"}, {"run_in_background", true}})
              .content.find("#1") != std::string::npos);
    {  // 等第一轮挂住。
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait_for(lock, std::chrono::seconds(2), [&]() { return state->started >= 1; });
    }
    CHECK(agent_tool.SendTaskMessage(1, "只读,不要修改") == tools::TaskMessageStatus::Queued);
    CHECK(agent_tool.SendTaskMessage(1, "把证据列全") == tools::TaskMessageStatus::Queued);
    {  // 放第一轮:工具收口后封账交接取走两条,续跑第二轮。
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->cv.notify_all();
    {  // 等第二轮挂住,再放行收尾。
        std::unique_lock<std::mutex> lock(state->mutex);
        state->cv.wait_for(lock, std::chrono::seconds(2), [&]() { return state->started >= 2; });
        state->release = true;
    }
    state->cv.notify_all();
    for (int i = 0; i < 300 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    const auto summaries = agent_tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    const auto events = agent_tool.TaskEvents(1);
    REQUIRE(events.size() == 7);
    CHECK(events[0].kind == tools::AgentTaskEventKind::UserMessage);
    CHECK(events[1].kind == tools::AgentTaskEventKind::ToolStart);
    CHECK(events[2].kind == tools::AgentTaskEventKind::ToolResult);
    // 两条介入按收到次序记在工具之后、收尾正文之前——"main 何时补了话"
    // 在账上看得见落点(规格 transcript 单测第 3 条)。
    CHECK(events[3].kind == tools::AgentTaskEventKind::SteeringMessage);
    CHECK(events[3].text == "只读,不要修改");
    CHECK(events[4].kind == tools::AgentTaskEventKind::SteeringMessage);
    CHECK(events[4].text == "把证据列全");
    CHECK(events[5].kind == tools::AgentTaskEventKind::AssistantText);
    CHECK(events[5].text == "收尾:按补充意见办");
    CHECK(events[6].kind == tools::AgentTaskEventKind::Completion);
}

// ---------------------------------------------------------------------------
// 后台子代理 hooks:只读快照执行 + 记录投递,主会话安全点归并落账。
// 后台线程不碰 dispatcher 账本;PreToolUse deny 在后台真拦;没有快照时
// 行为与从前一致(不触发)。
// ---------------------------------------------------------------------------

namespace {

// 写一只临时钩子脚本(与 test_hooks_dispatcher.cpp 同款套路),返回可直接
// 当 handler.command 用的整条命令。
std::string WriteAgentHookScript(const std::string& name, const std::string& body) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-agent-bg-hooks";
    std::filesystem::create_directories(dir);
#ifdef _WIN32
    const std::filesystem::path file = dir / (name + ".cmd");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "@echo off\r\n" << body << "\r\n";
    const std::u8string u8 = file.u8string();
    return "cmd /d /s /c \"\"" + std::string(reinterpret_cast<const char*>(u8.data()), u8.size()) + "\"\"";
#else
    const std::filesystem::path file = dir / (name + ".sh");
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "#!/bin/sh\n" << body << "\n";
    return "sh \"" + file.string() + "\"";
#endif
}

// 一枚用户级钩子定义(命令原样给定)。
hooks::HookDefinition MakeBackgroundHookDef(hooks::HookEvent event, const std::string& command) {
    hooks::HookDefinition def;
    def.event = event;
    def.source_kind = hooks::HookSourceKind::User;
    def.source_path = "test://user-config";
    def.source_label = "user test://user-config";
    def.matcher = "*";
    def.handler.command = command;
    def.handler.timeout_ms = 15000;
    def.handler.failure_policy = "warn";
    def.definition_hash = hooks::ComputeDefinitionHash(def.handler);
    def.definition_hash_short = hooks::DefinitionHashShort(def.definition_hash);
    def.trusted = true;
    return def;
}

// 等后台任务进终态(最多 20 秒),返回 false = 超时。
bool WaitTaskFinished(tools::AgentTool& tool, int spins = 2000) {
    for (int i = 0; i < spins && tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !tool.HasRunningTasks();
}

}  // namespace

TEST_CASE("agent 后台 hooks:SubagentStart/工具事件/SubagentStop 真跑,记录经安全点归并") {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "lubancode-agent-bg-hooks";
    std::filesystem::create_directories(dir);
    const std::string marker =
        (dir / "bg-hook-marker.txt").string();
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(reinterpret_cast<const char8_t*>(marker.c_str())), ec);
#ifdef _WIN32
    const std::string marker_command = "cmd /d /s /c \"echo fired>>\"" + marker + "\"\"";
#else
    const std::string marker_command = "sh -c 'echo fired >> " + marker + "'";
#endif

    // 四只钩子:SubagentStart、PreToolUse、PostToolUse、SubagentStop——都只
    // 写标记文件,证明进程真起、记录真投。
    hooks::HookDispatcher dispatcher;
    {
        hooks::LoadedHooks loaded;
        loaded.definitions = {MakeBackgroundHookDef(hooks::HookEvent::SubagentStart, marker_command),
                              MakeBackgroundHookDef(hooks::HookEvent::PreToolUse, marker_command),
                              MakeBackgroundHookDef(hooks::HookEvent::PostToolUse, marker_command),
                              MakeBackgroundHookDef(hooks::HookEvent::SubagentStop, marker_command)};
        hooks::HookTrustStore trust = hooks::HookTrustStore::Load(std::nullopt).first;
        hooks::HookContext ctx;
        ctx.session_id = "test-session";
        ctx.cwd = "/test";
        dispatcher.Configure(std::move(loaded), std::move(trust), std::move(ctx));
    }

    auto detached_backend = std::make_unique<FakeBackend>();
    detached_backend->scripts = {ToolUseScript("toolu_1", "fake_tool"), TextOnlyScript("后台结论")};
    tools::ToolRegistry sub_registry;
    auto* fake_tool_ptr = new FakeTool("fake_tool", tools::Tool::Result{"工具结果", false}, false);
    sub_registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));
    tools::AgentTool bg_tool(*detached_backend, sub_registry, "/work/dir");
    bg_tool.SetDetachedBackendFactory([&detached_backend]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::move(detached_backend);
        return detached;
    });
    tools::AgentTool::Hooks hooks;
    hooks.hook_dispatcher = &dispatcher;
    bg_tool.SetHooks(std::move(hooks));

    CHECK(bg_tool
              .execute(nlohmann::json{{"title", "后台钩子回归"}, {"prompt", "查一查"}, {"run_in_background", true}})
              .content.find("#") != std::string::npos);
    REQUIRE(WaitTaskFinished(bg_tool));

    // 标记文件被钩子写过——进程真起过。
    CHECK(std::filesystem::exists(std::filesystem::path(reinterpret_cast<const char8_t*>(marker.c_str()))));

    // 归并前账本是空的(后台线程只投递,不碰账本);主会话安全点收编后,
    // 四种事件各有一条可查流水。
    CHECK(dispatcher.RecentRecords(20).empty());
    const auto adoption = dispatcher.AdoptExternalRecords();
    std::set<std::string> seen_events;
    for (const auto& record : adoption.records) {
        seen_events.insert(record.event_name);
        CHECK(record.outcome == "ok");
    }
    CHECK(seen_events.count("SubagentStart") == 1);
    CHECK(seen_events.count("PreToolUse") == 1);
    CHECK(seen_events.count("PostToolUse") == 1);
    CHECK(seen_events.count("SubagentStop") == 1);
    CHECK(dispatcher.RecentRecords(20).size() == 4);

    // 工具真执行了(钩子没拦,后台结论照常带回)。
    CHECK(fake_tool_ptr->call_count == 1);
}

TEST_CASE("agent 后台 hooks:PreToolUse deny 在后台真拦,工具不执行") {
    // deny 版 PreToolUse:exit 2,stderr 给理由。
#ifdef _WIN32
    const std::string command = WriteAgentHookScript("bg-deny", "echo bg-policy-deny >&2\nexit 2");
#else
    const std::string command = WriteAgentHookScript("bg-deny", "echo bg-policy-deny >&2\nexit 2");
#endif

    hooks::HookDispatcher dispatcher;
    {
        hooks::LoadedHooks loaded;
        loaded.definitions = {MakeBackgroundHookDef(hooks::HookEvent::PreToolUse, command)};
        hooks::HookTrustStore trust = hooks::HookTrustStore::Load(std::nullopt).first;
        hooks::HookContext ctx;
        ctx.session_id = "test-session";
        ctx.cwd = "/test";
        dispatcher.Configure(std::move(loaded), std::move(trust), std::move(ctx));
    }

    auto detached_backend = std::make_unique<FakeBackend>();
    detached_backend->scripts = {ToolUseScript("toolu_1", "fake_tool"), TextOnlyScript("工具被拦,结论改为报告")};
    tools::ToolRegistry sub_registry;
    auto* fake_tool_ptr = new FakeTool("fake_tool", tools::Tool::Result{"不该执行到", false}, false);
    sub_registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));
    tools::AgentTool bg_tool(*detached_backend, sub_registry, "/work/dir");
    bg_tool.SetDetachedBackendFactory([&detached_backend]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::move(detached_backend);
        return detached;
    });
    tools::AgentTool::Hooks hooks;
    hooks.hook_dispatcher = &dispatcher;
    bg_tool.SetHooks(std::move(hooks));

    CHECK_FALSE(
        bg_tool.execute(nlohmann::json{{"title", "后台 deny 回归"}, {"prompt", "查一查"}, {"run_in_background", true}})
            .is_error);
    REQUIRE(WaitTaskFinished(bg_tool));

    // 工具被拦:一次都没执行,任务台账里那笔工具调用是 error,理由说清。
    CHECK(fake_tool_ptr->call_count == 0);
    const auto summaries = bg_tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    const auto detail = bg_tool.TaskDetail(summaries[0].id);
    REQUIRE(detail.has_value());
    REQUIRE(detail->tool_calls.size() == 1);
    CHECK(detail->tool_calls[0].is_error);
    CHECK(detail->tool_calls[0].result.find("被 PreToolUse 钩子拦截") != std::string::npos);

    // 钩子的拦截记录在案(经安全点归并)。
    const auto adoption = dispatcher.AdoptExternalRecords();
    bool saw_blocked = false;
    for (const auto& record : adoption.records) {
        if (record.event_name == "PreToolUse" && record.outcome == "blocked") {
            saw_blocked = true;
        }
    }
    CHECK(saw_blocked);
}

TEST_CASE("agent 后台 hooks:没配 hooks 时后台路径不触发,行为与从前一致") {
    auto detached_backend = std::make_unique<FakeBackend>();
    detached_backend->scripts = {ToolUseScript("toolu_1", "fake_tool"), TextOnlyScript("无钩子结论")};
    tools::ToolRegistry sub_registry;
    auto* fake_tool_ptr = new FakeTool("fake_tool", tools::Tool::Result{"工具结果", false}, false);
    sub_registry.Register(std::unique_ptr<FakeTool>(fake_tool_ptr));
    tools::AgentTool bg_tool(*detached_backend, sub_registry, "/work/dir");
    bg_tool.SetDetachedBackendFactory([&detached_backend]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::move(detached_backend);
        return detached;
    });
    // 不 SetHooks(hook_dispatcher 为空)——后台照常跑完,无任何 hooks 参与。

    CHECK_FALSE(bg_tool
                    .execute(nlohmann::json{{"title", "无钩子后台"}, {"prompt", "查"}, {"run_in_background", true}})
                    .is_error);
    REQUIRE(WaitTaskFinished(bg_tool));
    CHECK(fake_tool_ptr->call_count == 1);
}
