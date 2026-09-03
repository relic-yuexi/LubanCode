// tools::AgentTool:内置 "agent" 工具,用 FakeBackend 按脚本吐 StreamEvent
// (不碰真网络),验证:一轮纯文本直接返回;子代理调工具(用假工具)后
// 返回;超步数预算按 budget_exhausted 收账(带部分结果与步数账);最后一步
// 无文本结论/接口报错/输出预算耗尽按结构化 TaskOutcome 分型;确认回调转发
// (父拒绝 -> 子内工具收到拒绝);usage 累计到父回调;同级派工与深度治理
// (子表可挂 AgentDispatchTool 转发壳,递归靠显式深度上限防,不靠拿掉工具)。

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
#include "tools/todo_tool.hpp"
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
    FakeTool(std::string name, tools::Tool::Result result, bool needs_confirm_flag,
             tools::ApprovalClass approval_class = tools::ApprovalClass::None)
        : name_(std::move(name)), result_(std::move(result)), needs_confirm_flag_(needs_confirm_flag),
          approval_class_(approval_class) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return needs_confirm_flag_; }
    tools::ApprovalClass approval_class() const override { return approval_class_; }

    tools::Tool::Result execute(const nlohmann::json& input) override {
        ++call_count;
        inputs.push_back(input);
        return result_;
    }

    int call_count = 0;
    std::vector<nlohmann::json> inputs;

private:
    std::string name_;
    tools::Tool::Result result_;
    bool needs_confirm_flag_;
    tools::ApprovalClass approval_class_;
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

// 带实参的工具调用脚本(放行账测试用:run_command 的 command 字段要真传,
// 前缀匹配才有输入可查)。
std::vector<api::StreamEvent> ToolUseScriptInput(const std::string& tool_id, const std::string& tool_name,
                                                 const std::string& input_json) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
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

// ---------------------------------------------------------------------------
// 输出预算耗尽(规格根因四,vLLM 0.27.1 + qwen3.8-27b 现场的收场侧):
// thinking 吃满输出上限、续跑用完仍无正文 → budget_exhausted /
// output_budget_exhausted,不再笼统 no_final_text;失败页带实际上限、
// 已续次数、usage 是否报告、思考检查点与四条去路。usage 未报告时台账
// 不画 0,标 usage_reported=false。
// ---------------------------------------------------------------------------

TEST_CASE("agent 工具:输出预算耗尽——budget_exhausted 分型带结构化账") {
    FakeBackend backend;
    // 两轮都是 reasoning-only + finish_reason=length(默认续跑 1 次)。
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ThinkingDelta{"先想棋盘布局"},
            api::ContentBlockDone{0},
            api::MessageDone{"max_tokens", api::Usage{0, 0, 0, 0, 0}},  // usage 全零:未报告
        },
        {
            api::MessageStart{"msg", "model"},
            api::ThinkingDelta{"还在想"},
            api::ContentBlockDone{0},
            api::MessageDone{"max_tokens", api::Usage{0, 0, 0, 0, 0}},
        },
    };
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    // 子代理运行策略:声明了上限才在失败页报数;步数不限。
    agent::AgentRuntimeProfile profile;
    profile.max_output_tokens = 4096;
    profile.max_output_tokens_source = agent::OutputBudgetSource::ConfigFile;
    agent_tool.SetRuntimeProfile(profile);

    const tools::Tool::Result result =
        agent_tool.execute(nlohmann::json{{"title", "写象棋网页"}, {"prompt", "写一个中国象棋网页游戏"}});

    CHECK(result.is_error);
    CHECK(result.content.find("budget_exhausted") != std::string::npos);
    CHECK(result.content.find("输出预算耗尽") != std::string::npos);
    // 四条去路(规格根因四):失败页必须给全。
    CHECK(result.content.find("继续") != std::string::npos);
    CHECK(result.content.find("max_output_tokens") != std::string::npos);
    CHECK(result.content.find("/think") != std::string::npos);
    CHECK(result.content.find("拆小") != std::string::npos);
    CHECK(result.content.find("4096") != std::string::npos);  // 实际上限写进失败页

    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    const auto& outcome = snapshots[0].outcome;
    CHECK(snapshots[0].state == tools::AgentTaskState::BudgetExhausted);
    CHECK(outcome.status == tools::TaskOutcomeStatus::BudgetExhausted);
    CHECK(outcome.reason == tools::TaskOutcomeReason::OutputBudgetExhausted);
    CHECK(outcome.output_limit_tokens == 4096);
    CHECK(outcome.length_continuations_used == 1);
    CHECK(outcome.usage_reported == false);  // 两轮 usage 全零:未报告,不冒充 0
    CHECK(outcome.stop_reason == "max_tokens");
    CHECK(outcome.thinking_checkpoint.find("思考") != std::string::npos);
    // 台账同款三态:usage_reported=false(面板据此写"tokens 未报告")。
    CHECK(snapshots[0].usage_reported == false);
    CHECK(snapshots[0].steps_used == 2);
}

TEST_CASE("agent 工具:思考撞墙后续跑救回来——正常完成,usage 报告位翻真") {
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ThinkingDelta{"想"},
            api::ContentBlockDone{0},
            api::MessageDone{"max_tokens", api::Usage{0, 4096, 0, 0, 0}},
        },
        TextOnlyScript("结论:改用分步实现"),
    };
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    const tools::Tool::Result result =
        agent_tool.execute(nlohmann::json{{"title", "小结"}, {"prompt", "给个结论"}});

    CHECK_FALSE(result.is_error);
    CHECK(result.content.find("结论:改用分步实现") != std::string::npos);
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].outcome.status == tools::TaskOutcomeStatus::Completed);
    CHECK(snapshots[0].outcome.reason == tools::TaskOutcomeReason::None);
    // 第一轮 usage 带了 4096 输出:报告位为真(哪怕第二轮全零也算报告过)。
    CHECK(snapshots[0].usage_reported == true);
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

// ---------------------------------------------------------------------------
// 缺参明说 + 连败保险(缺 title 无限重试拖死主循环单):必填参数缺失时
// 错误文案写明哪个字段、示例什么样,模型一遍就能补上;同一回合内同因
// 参数错连败 3 次明拒收场;新回合(SetHooks 重灌)计数清零,不跨回合记仇。
// ---------------------------------------------------------------------------

TEST_CASE("agent 工具:缺 title / 缺 prompt 的错误文案写明字段名与示例") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    const auto missing_title = agent_tool.execute(nlohmann::json{{"prompt", "查调用链"}});
    CHECK(missing_title.is_error);
    CHECK(missing_title.content.find("title") != std::string::npos);        // 字段名
    CHECK(missing_title.content.find("必填") != std::string::npos);
    CHECK(missing_title.content.find("示例") != std::string::npos);         // 有示例
    CHECK(missing_title.content.find("检索构建配置") != std::string::npos);  // 示例内容一眼能抄

    const auto missing_prompt = agent_tool.execute(nlohmann::json{{"title", "查调用链"}});
    CHECK(missing_prompt.is_error);
    CHECK(missing_prompt.content.find("prompt") != std::string::npos);
    CHECK(missing_prompt.content.find("必填") != std::string::npos);
    CHECK(missing_prompt.content.find("示例") != std::string::npos);

    CHECK(backend.captured_requests.empty());  // 拒在门外,一次请求都没发
}

TEST_CASE("agent 工具:连败保险——同因参数错第 3 次起明拒,改对后照常受理") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    // 前两次缺 title:引导文案(字段 + 示例),不带连败字样。
    for (int i = 0; i < 2; ++i) {
        const auto result = agent_tool.execute(nlohmann::json{{"prompt", "查调用链"}});
        CHECK(result.is_error);
        CHECK(result.content.find("示例") != std::string::npos);
        CHECK(result.content.find("连败") == std::string::npos);
    }
    // 第三次同样的错:明拒收场——写清重试不会成功与两条出路,但拒绝里仍
    // 带参数要求(字段 + 示例),不是死胡同。
    const auto refused = agent_tool.execute(nlohmann::json{{"prompt", "查调用链"}});
    CHECK(refused.is_error);
    CHECK(refused.content.find("连败保险") != std::string::npos);
    CHECK(refused.content.find("重试也不会成功") != std::string::npos);
    CHECK(refused.content.find("title") != std::string::npos);
    CHECK(refused.content.find("示例") != std::string::npos);
    CHECK(backend.captured_requests.empty());  // 三次全拒在门外,模型没被放进去

    // 连败后把 title 补上:照常受理(过检即清账),不祸害后续调用。
    backend.scripts = {TextOnlyScript("结论")};
    const auto ok = agent_tool.execute(nlohmann::json{{"title", "查调用链"}, {"prompt", "查"}});
    CHECK_FALSE(ok.is_error);
    REQUIRE(backend.captured_requests.size() == 1);

    // 清账后再犯:重新从引导起(第一次不是直接明拒)。
    const auto fresh = agent_tool.execute(nlohmann::json{{"prompt", "查调用链"}});
    CHECK(fresh.is_error);
    CHECK(fresh.content.find("连败") == std::string::npos);

    // 不同原因各自起算:缺 title 连败两次后换成坏 title,拿的仍是引导。
    const auto bad_title = agent_tool.execute(nlohmann::json{{"title", "两行\n标题"}, {"prompt", "查"}});
    CHECK(bad_title.is_error);
    CHECK(bad_title.content.find("连败") == std::string::npos);
    CHECK(bad_title.content.find("示例") != std::string::npos);
}

TEST_CASE("agent 工具:连败不跨回合记仇——SetHooks 重灌(新回合)后计数清零") {
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    // 回合一内连败两次(未到明拒线)。
    (void)agent_tool.execute(nlohmann::json{{"prompt", "查"}});
    (void)agent_tool.execute(nlohmann::json{{"prompt", "查"}});

    // 新回合:宿主每轮 RunTurn 都重灌 Hooks,连败账随之清零。
    tools::AgentTool::Hooks fresh_hooks;
    fresh_hooks.on_sub_tool_start = [](const std::string&, const std::string&, const nlohmann::json&) {};
    agent_tool.SetHooks(std::move(fresh_hooks));

    // 同样的错误第三次发生:拿的是引导文案(计数从头起),不是明拒。
    const auto after_new_turn = agent_tool.execute(nlohmann::json{{"prompt", "查"}});
    CHECK(after_new_turn.is_error);
    CHECK(after_new_turn.content.find("示例") != std::string::npos);
    CHECK(after_new_turn.content.find("连败") == std::string::npos);
    CHECK(backend.captured_requests.empty());
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
    hooks.on_sub_tool_start = [&](const std::string&, const std::string& name, const nlohmann::json&) {
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

TEST_CASE("agent 工具:入参新名 max_steps_per_turn 解析层仍生效,但两个键都不出 schema") {
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
    // P1-0(turn 预算单 §5.3):max_steps_per_turn 保持旧义并告警——结果
    // 尾部带回一行弃用提示,数值行为一字不动。
    CHECK(result.content.find("[弃用提示]") != std::string::npos);
    CHECK(result.content.find("max_steps_per_turn") != std::string::npos);
    CHECK(result.content.find("runtime.max_turns") != std::string::npos);
    CHECK(backend.captured_requests.size() == 7);  // 6 次工具步 + 1 次收尾,新名生效

    // 步数预算两个键都不出 schema:限步走配置,不给模型旋钮——敞着它模型就
    // 见字段填数,把配置里"不限步"的默认给悄悄夺了。解析层照旧收(上面那趟
    // 7 步就是从入参进来的),手写 JSON、老脚本不受影响。
    const nlohmann::json schema = agent_tool.input_schema();
    const nlohmann::json& props = schema.at("properties");
    CHECK_FALSE(props.contains("max_steps_per_turn"));
    CHECK_FALSE(props.contains("max_turns"));
    // 别的参数还在,别把整张表误删了。
    CHECK(props.contains("title"));
    CHECK(props.contains("prompt"));
    CHECK(props.contains("agent_type"));
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
    // P1-0(turn 预算单 §5.3):旧别名 max_turns 语义照旧(无上限透传),
    // 结果尾部多一行弃用提示——手写脚本作者看得见,数值行为一字不动。
    CHECK(result.content.find("跑完了") == 0);
    CHECK(result.content.find("[弃用提示]") != std::string::npos);
    CHECK(result.content.find("max_turns") != std::string::npos);
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
    hooks.on_tool_confirm = [&](const std::string&, const std::string&, const nlohmann::json&) -> bool {
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

TEST_CASE("同级派工:子表可挂 agent 转发壳,递归靠显式深度上限防,不靠拿掉工具") {
    FakeBackend backend;

    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"ok", false}, false));

    tools::ToolRegistry main_registry;
    main_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"ok", false}, false));
    auto agent_tool = std::make_unique<tools::AgentTool>(backend, sub_registry, "/work/dir");
    main_registry.Register(std::make_unique<tools::AgentDispatchTool>(*agent_tool));
    // AgentDispatchTool 只是转发壳:name/schema/描述与目标一致,execute 直通。
    CHECK(main_registry.Find("agent") != nullptr);
    CHECK(main_registry.Find("agent")->name() == "agent");
    CHECK(main_registry.Find("agent")->input_schema() == agent_tool->input_schema());
    // 旧测试直建的子表没有转发壳(调用方没挂),行为照旧——递归治理在
    // AgentTool 的深度账上,不在"表里有没有这枚工具"上。
    CHECK(sub_registry.Find("agent") == nullptr);
}

// ---------------------------------------------------------------------------
// 后台能力三处一致(派工单 §二):schema 按当前入口生成、派工前 preflight
// 稳定拒绝(带错误码/入口/可用模式/改法)、无后端时 backend 零调用且
// worktree 零创建;配了工厂则枚举恢复、后台照常可派。
// ---------------------------------------------------------------------------

TEST_CASE("后台能力: 无后台后端——schema 摘掉 background,preflight 稳定拒绝,前台出路可走") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("前台结论")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    // schema:枚举里没有 background,说明点名不可用与改法。
    const nlohmann::json schema = agent_tool.input_schema();
    const nlohmann::json& enum_values = schema.at("properties").at("execution_mode").at("enum");
    bool lists_background = false;
    for (const auto& value : enum_values) {
        lists_background = lists_background || value.get<std::string>() == "background";
    }
    CHECK_FALSE(lists_background);
    const std::string mode_description = schema.at("properties").at("execution_mode").at("description");
    CHECK(mode_description.find("background_unavailable") != std::string::npos);

    // preflight:稳定错误码 + 当前入口 + 可用模式 + 改法;注册前拒绝。
    const auto rejected = agent_tool.execute(
        nlohmann::json{{"title", "后台被拒"}, {"prompt", "查"}, {"execution_mode", "background"}});
    CHECK(rejected.is_error);
    CHECK(rejected.content.find("[background_unavailable]") != std::string::npos);
    CHECK(rejected.content.find("主入口") != std::string::npos);
    CHECK(rejected.content.find("foreground") != std::string::npos);
    CHECK(agent_tool.TaskSummaries().empty());  // 任务压根没注册
    CHECK(backend.captured_requests.empty());   // backend 零调用

    // 前台降级出路真的可走。
    const auto fg = agent_tool.execute(
        nlohmann::json{{"title", "前台照跑"}, {"prompt", "查"}, {"execution_mode", "foreground"}});
    CHECK_FALSE(fg.is_error);
    CHECK(fg.content == "前台结论");
}

TEST_CASE("后台能力: 配了工厂——schema 列出 background,后台派工返回任务号") {
    auto detached_backend = std::make_unique<FakeBackend>();
    detached_backend->scripts = {TextOnlyScript("后台结论")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(*detached_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory(
        [&detached_backend]() {
            tools::DetachedAgentBackend detached;
            detached.backend = std::move(detached_backend);
            return detached;
        });

    // schema:枚举恢复三值(与无后端的入口区分开)。
    const nlohmann::json schema = agent_tool.input_schema();
    const nlohmann::json& enum_values = schema.at("properties").at("execution_mode").at("enum");
    bool lists_background = false;
    for (const auto& value : enum_values) {
        lists_background = lists_background || value.get<std::string>() == "background";
    }
    CHECK(lists_background);

    // 后台派工照常起任务,回执带任务号。
    const auto accepted = agent_tool.execute(
        nlohmann::json{{"title", "后台照跑"}, {"prompt", "查"}, {"execution_mode", "background"}});
    CHECK_FALSE(accepted.is_error);
    CHECK(accepted.content.find("已启动") != std::string::npos);
    REQUIRE(agent_tool.TaskSummaries().size() == 1);
    CHECK_FALSE(agent_tool.TaskSummaries()[0].foreground);
}

TEST_CASE("深度治理:嵌套派工超过上限明报,不发请求") {
    // 造一只"被问到就再派一只子代理"的假 agent 目标,验证深度账在
    // AgentTool 内部滚动——不真跑模型,只看拒绝文案与请求数。
    FakeBackend backend;
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    agent_tool.SetDispatchGovernance(/*max_active=*/8, /*max_depth=*/1);

    // 直接 execute = 第 1 层,允许(会正常起任务);
    // 在第 1 层任务里再经转发壳派工 = 第 2 层,拒绝。
    // 用 RunTask 的公开路径难在单测里嵌套,这里钉治理入口的语义:
    // 深度上限 1 时,处在第 1 层里再派会被拒。用一个在前台任务执行期间
    // 再次调用 execute 的假工具模拟嵌套。
    class NestingTool : public tools::Tool {
    public:
        NestingTool(tools::AgentDispatchTool& dispatch) : dispatch_(dispatch) {}
        std::string name() const override { return "nesting_probe"; }
        std::string description() const override { return "再派一只"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        bool needs_confirm() const override { return false; }
        tools::Tool::Result execute(const nlohmann::json&) override {
            called = true;
            last_result = dispatch_.execute(nlohmann::json{{"title", "孙任务"}, {"prompt", "查"}});
            return {"嵌套结果见旁账", false};
        }
        bool called = false;
        tools::Tool::Result last_result{"", false};

    private:
        tools::AgentDispatchTool& dispatch_;
    };

    tools::AgentDispatchTool dispatch(agent_tool);
    auto nesting = std::make_unique<NestingTool>(dispatch);
    NestingTool& nesting_ref = *nesting;
    sub_registry.Register(std::move(nesting));
    // 脚本:第一轮让模型调用 nesting_probe(它会在第 1 层内再派第 2 层)。
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ToolUseStart{0, "toolu_n", "nesting_probe"},
            api::ToolUseInputDelta{0, "{}"},
            api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{}},
        },
        TextOnlyScript("外层收口"),
    };
    const tools::Tool::Result result =
        agent_tool.execute(nlohmann::json{{"title", "外层"}, {"prompt", "开工"}});
    // 外层任务本身照常完成;嵌套那次被深度上限拒绝——明报,不发请求。
    CHECK_FALSE(result.is_error);
    CHECK(nesting_ref.called);
    CHECK(nesting_ref.last_result.is_error);
    CHECK(nesting_ref.last_result.content.find("深度上限") != std::string::npos);
    CHECK(nesting_ref.last_result.content.find("subagent.max_depth") != std::string::npos);
    // 只有外层的两次请求:嵌套派工在被拒时没碰模型。
    CHECK(backend.captured_requests.size() == 2);
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

TEST_CASE("Explore 撞只读墙:错误写明'角色限制',不写'子代理无权限'") {
    // 模型幻觉调了白名单外的写工具:Explore 的过滤谓词不放行,错误文案
    // 得说清限制来自只读角色(规格"同级矩阵"第 3 条),下一轮交检查点。
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ToolUseStart{0, "toolu_w", "write_file"},
            api::ToolUseInputDelta{0, "{}"},
            api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{}},
        },
        TextOnlyScript("检查点:只读调查完成,写入建议附后"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>("read_file", tools::Tool::Result{"read", false}, false));
    sub_registry.Register(std::make_unique<FakeTool>("write_file", tools::Tool::Result{"write", false}, false));

    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    const auto result = agent_tool.execute(
        nlohmann::json{{"title", "只读查"}, {"prompt", "查"}, {"agent_type", "Explore"}, {"run_in_background", false}});

    CHECK_FALSE(result.is_error);
    REQUIRE(backend.captured_requests.size() == 2);
    // 第二次请求带回的 tool_result 里是"角色限制"的说法。
    const auto& second = backend.captured_requests[1];
    std::string tool_result_text;
    for (const auto& message : second.messages) {
        for (const auto& block : message.content) {
            if (const auto* tool_result = std::get_if<api::ToolResultBlock>(&block);
                tool_result != nullptr && tool_result->tool_use_id == "toolu_w") {
                tool_result_text = tool_result->content;
            }
        }
    }
    CHECK(tool_result_text.find("角色限制") != std::string::npos);
    CHECK(tool_result_text.find("Explore") != std::string::npos);
    CHECK(tool_result_text.find("子代理无权限") == std::string::npos);
}

TEST_CASE("子代理记忆召回:派工当刻冻结快照,注入本轮 user 消息尾部") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("结论")};
    tools::ToolRegistry sub_registry;
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    // 召回 provider:吃任务 prompt 与子 run id,吐一段上下文(会话层闭包
    // ProjectMemory,这里用假实现钉注入语义;child_run_id 没轨迹账时空)。
    agent_tool.SetTurnContextProvider([](const std::string& task_prompt, const std::string& child_run_id) {
        (void)child_run_id;
        return "[项目记忆] 与\"" + task_prompt + "\"相关的召回段";
    });

    const tools::Tool::Result result =
        agent_tool.execute(nlohmann::json{{"title", "带记忆"}, {"prompt", "查构建系统"}});
    CHECK_FALSE(result.is_error);
    REQUIRE(backend.captured_requests.size() == 1);
    // 召回段随本轮 user 消息进请求视图(SetTurnContext 语义),模型第一
    // 份请求就看得见。
    std::string first_user_text;
    for (const auto& message : backend.captured_requests[0].messages) {
        if (message.role != api::Role::User) {
            continue;
        }
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                first_user_text += text->text;
            }
        }
        break;
    }
    CHECK(first_user_text.find("查构建系统") != std::string::npos);
    CHECK(first_user_text.find("[项目记忆]") != std::string::npos);
}

TEST_CASE("子代理私有 todo:每只任务的 todo_write 独占一块板,不写主表状态") {
    FakeBackend backend;
    backend.scripts = {
        {
            api::MessageStart{"msg", "model"},
            api::ToolUseStart{0, "toolu_t", "todo_write"},
            api::ToolUseInputDelta{0, R"({"items":[{"content":"查","status":"pending"}]})"},
            api::ContentBlockDone{0},
            api::MessageDone{"tool_use", api::Usage{}},
        },
        TextOnlyScript("办完了"),
    };
    tools::ToolRegistry sub_registry;
    const auto shared_board = std::make_shared<tools::TodoListState>();
    sub_registry.Register(std::make_unique<tools::TodoWriteTool>(shared_board));
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    const tools::Tool::Result result =
        agent_tool.execute(nlohmann::json{{"title", "带 todo"}, {"prompt", "办"}});
    CHECK_FALSE(result.is_error);
    // 主表/会话级那块板一个字没动:子代理的 todo 是任务私有的,任务退出
    // 即散(规格"不让子代理共享并并发修改 main 的同一份 todo")。
    CHECK(shared_board->items.empty());
}

TEST_CASE("后台子代理立即交回任务号,独立跑完,结果只投递一次") {
    FakeBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    auto state = std::make_shared<BlockingBackendState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([state]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<BlockingBackend>(state);
        detached.request_profile.model = "detached-model";
        detached.request_profile.reasoning_effort = "high";
        detached.request_profile.reasoning.supports_effort = true;
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
        CHECK(state->captured_requests[0].reasoning.supports_effort);
    }

    const std::string first_notice = agent_tool.DrainCompletionNotices();
    CHECK(first_notice.find("后台摸排完毕") != std::string::npos);
    CHECK(agent_tool.DrainCompletionNotices().empty());
}

// 后台代理权限拒绝无告知单(2026-08-17):后台子代理没人可问,needs_confirm
// 的工具一律当场拒。要验的是三件事:通知账里当场落一条(带任务号/工具名
// 与 /permissions 出路,取走即清,不攒到最终报告);任务事件账里给子代理的
// 拒绝文案如实写"后台无法弹确认、未预放行",全文不出现"用户拒绝";需确
// 认工具从头到尾没真执行。
TEST_CASE("后台子代理的 needs_confirm 工具被拒:当场入通知账,拒绝文案如实") {
    FakeBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    auto* gated_ptr = new FakeTool("write_confirm", tools::Tool::Result{"写了", false}, /*needs_confirm=*/true);
    sub_registry.Register(std::unique_ptr<FakeTool>(gated_ptr));

    auto backend = std::make_unique<FakeBackend>();
    backend->scripts = {
        ToolUseScript("toolu_deny", "write_confirm"),
        TextOnlyScript("受阻:写操作未放行,如实汇报"),
    };
    FakeBackend* backend_ptr = backend.get();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([&backend]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::move(backend);
        return detached;
    });

    const auto launch = agent_tool.execute(
        nlohmann::json{{"title", "写份材料"}, {"prompt", "去写个文件"}, {"run_in_background", true}});
    REQUIRE_FALSE(launch.is_error);
    (void)backend_ptr;

    for (int i = 0; i < 300 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    // 通知账:恰好一条,带任务号、工具名与 /permissions 出路;取走即清。
    const std::vector<std::string> notices = agent_tool.TakePermissionDenialNotices();
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].find("#1") != std::string::npos);
    CHECK(notices[0].find("write_confirm") != std::string::npos);
    CHECK(notices[0].find("未放行") != std::string::npos);
    CHECK(notices[0].find("/permissions") != std::string::npos);
    CHECK(agent_tool.TakePermissionDenialNotices().empty());

    // 事件账:给子代理的拒绝文案如实说原因与出路,不冒充"用户拒绝"。
    const auto events = agent_tool.TaskEvents(1);
    bool saw_denial = false;
    for (const auto& event : events) {
        if (event.kind == tools::AgentTaskEventKind::ToolResult && event.tool_name == "write_confirm") {
            saw_denial = true;
            CHECK(event.is_error);
            CHECK(event.result.find("后台任务无法弹出权限确认") != std::string::npos);
            CHECK(event.result.find("未预先放行") != std::string::npos);
            CHECK(event.result.find("并非用户拒绝") != std::string::npos);
            CHECK(event.result.find("用户拒绝执行该工具") == std::string::npos);
        }
    }
    CHECK(saw_denial);
    // 需确认工具没真执行。
    CHECK(gated_ptr->call_count == 0);
}

// 后台权限合同钉死(后台代理管控三连 bug 单,Bug A 验收原文):needs_confirm
// =false 的只读工具(read_file/search 一族)在后台子代理里零拒绝、零拒绝
// 通知——判定路只拦 needs_confirm=true 且未预放行的。真机实录里"read_file
// 全被拒"的观感来自监督提醒顶着权限拒绝标题连刷(渲染张冠李戴,Bug A 主
// 修),判定路本身放行;这条护栏防将来有人把 DontAsk 档扩成无差别拒绝。
TEST_CASE("后台子代理权限合同:needs_confirm=false 工具零拒绝零通知") {
    FakeBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    // needs_confirm=false 的只读工具(与 read_file/search 同档),外加一只
    // 账外的需确认工具作对照——同一任务里,只读直跑,需确认照拒。
    auto* read_ptr = new FakeTool("read_file", tools::Tool::Result{"读到了", false}, /*needs_confirm=*/false);
    auto* search_ptr = new FakeTool("search", tools::Tool::Result{"搜到了", false}, /*needs_confirm=*/false);
    auto* write_ptr = new FakeTool("write_file", tools::Tool::Result{"写了", false}, /*needs_confirm=*/true);
    sub_registry.Register(std::unique_ptr<FakeTool>(read_ptr));
    sub_registry.Register(std::unique_ptr<FakeTool>(search_ptr));
    sub_registry.Register(std::unique_ptr<FakeTool>(write_ptr));

    auto backend = std::make_unique<FakeBackend>();
    backend->scripts = {
        ToolUseScript("toolu_read", "read_file"),
        ToolUseScript("toolu_search", "search"),
        ToolUseScript("toolu_write", "write_file"),
        TextOnlyScript("收工:读搜成了,写被拒"),
    };
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([&backend]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::move(backend);
        return detached;
    });

    REQUIRE_FALSE(agent_tool
                      .execute(nlohmann::json{{"title", "读搜写"}, {"prompt", "读两个搜一个写一个"},
                                              {"run_in_background", true}})
                      .is_error);
    for (int i = 0; i < 300 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    // 只读工具真执行了——零拒绝。
    CHECK(read_ptr->call_count == 1);
    CHECK(search_ptr->call_count == 1);
    // 需确认工具照拒(没执行)。
    CHECK(write_ptr->call_count == 0);
    // 拒绝通知只落在 needs_confirm 那笔上,只读的零通知。
    const std::vector<std::string> notices = agent_tool.TakePermissionDenialNotices();
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].find("write_file") != std::string::npos);
    CHECK(notices[0].find("read_file") == std::string::npos);
    CHECK(notices[0].find("search") == std::string::npos);
}

// 修"后台审批不查放行账"(2026-08):主会话的"总是允许"账(settings.local
// 的 allow_tools + 会话内按 a 落的集合)以快照传入后台任务,审批回调先查
// 账再问钩子。钉三态:账上工具免问放行、账外照拒、账是派出时刻的定格。
TEST_CASE("后台子代理放行账:账上工具免问放行,账外照旧拒") {
    FakeBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    auto* allowed_ptr = new FakeTool("write_confirm", tools::Tool::Result{"写了", false}, /*needs_confirm=*/true);
    auto* gated_ptr = new FakeTool("secret_tool", tools::Tool::Result{"动了", false}, /*needs_confirm=*/true);
    sub_registry.Register(std::unique_ptr<FakeTool>(allowed_ptr));
    sub_registry.Register(std::unique_ptr<FakeTool>(gated_ptr));

    auto backend = std::make_unique<FakeBackend>();
    backend->scripts = {
        ToolUseScript("toolu_a", "write_confirm"),
        ToolUseScript("toolu_b", "secret_tool"),
        TextOnlyScript("收工:一件放行一件被拒"),
    };
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([&backend]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::move(backend);
        return detached;
    });
    std::set<std::string> ledger_names = {"write_confirm"};
    agent_tool.SetBackgroundPermissionSource([&ledger_names]() {
        tools::BackgroundPermissionLedger ledger;
        ledger.always_allowed = ledger_names;
        return ledger;
    });

    REQUIRE_FALSE(agent_tool
                      .execute(nlohmann::json{{"title", "两件写活"}, {"prompt", "写两个文件"}, {"run_in_background", true}})
                      .is_error);
    for (int i = 0; i < 300 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    // 账上的真执行了,账外的没执行。
    CHECK(allowed_ptr->call_count == 1);
    CHECK(gated_ptr->call_count == 0);
    // 只有账外那笔落拒绝通知。
    const std::vector<std::string> notices = agent_tool.TakePermissionDenialNotices();
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].find("secret_tool") != std::string::npos);
    CHECK(notices[0].find("未放行") != std::string::npos);
}

TEST_CASE("后台子代理放行账:run_command 前缀——allow 命中放行,deny 压过 allow") {
    FakeBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    auto* run_ptr = new FakeTool("run_command", tools::Tool::Result{"跑完了", false}, /*needs_confirm=*/true,
                                 tools::ApprovalClass::Command);
    sub_registry.Register(std::unique_ptr<FakeTool>(run_ptr));

    auto backend = std::make_unique<FakeBackend>();
    backend->scripts = {
        ToolUseScriptInput("toolu_ok", "run_command", R"({"command":"git status"})"),
        ToolUseScriptInput("toolu_deny", "run_command", R"({"command":"git push origin"})"),
        TextOnlyScript("收工:一条放行一条被黑名单拒了"),
    };
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([&backend]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::move(backend);
        return detached;
    });
    // run_command 整名在账上(顶格放行),但 git push 命中 deny 前缀——deny
    // 压过 allow,与主路 EvaluatePermission 的"策略黑名单最高"同序。
    agent_tool.SetBackgroundPermissionSource([]() {
        tools::BackgroundPermissionLedger ledger;
        ledger.always_allowed = {"run_command"};
        ledger.allow_commands = {"git"};
        ledger.deny_commands = {"git push"};
        return ledger;
    });

    REQUIRE_FALSE(agent_tool
                      .execute(nlohmann::json{{"title", "跑两条命令"}, {"prompt", "跑一下"}, {"run_in_background", true}})
                      .is_error);
    for (int i = 0; i < 300 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    // git status 真执行;git push 被 deny 前缀压下,没轮到 allow。
    CHECK(run_ptr->call_count == 1);
    REQUIRE(run_ptr->inputs.size() == 1);
    CHECK(run_ptr->inputs[0].value("command", std::string()) == "git status");

    const std::vector<std::string> notices = agent_tool.TakePermissionDenialNotices();
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].find("deny 命令前缀") != std::string::npos);

    // 给模型的拒绝文案把"预放行也放不开"说清,不诱导子代理去讨 /permissions。
    const auto events = agent_tool.TaskEvents(1);
    bool saw_deny_text = false;
    for (const auto& event : events) {
        if (event.kind == tools::AgentTaskEventKind::ToolResult && event.is_error) {
            saw_deny_text = true;
            CHECK(event.result.find("deny 命令前缀黑名单") != std::string::npos);
            CHECK(event.result.find("deny 压过预放行") != std::string::npos);
        }
    }
    CHECK(saw_deny_text);
}

TEST_CASE("后台子代理放行账是派出时刻的定格:任务跑着时主会话落新账,任务不认") {
    FakeBackend foreground_backend;
    tools::ToolRegistry sub_registry;
    auto* gated_ptr = new FakeTool("write_confirm", tools::Tool::Result{"写了", false}, /*needs_confirm=*/true);
    sub_registry.Register(std::unique_ptr<FakeTool>(gated_ptr));

    // 快照在 LaunchBackground 里拷贝、execute() 返回前定格——所以"launch
    // 返回后再改源账"必然晚于定格,时序天然成立,不用挡线程。
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    auto scripted = std::make_unique<FakeBackend>();
    scripted->scripts = {
        ToolUseScript("toolu_late", "write_confirm"),
        TextOnlyScript("受阻如实汇报"),
    };
    agent_tool.SetDetachedBackendFactory([&scripted]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::move(scripted);
        return detached;
    });
    std::set<std::string> ledger_names;  // 派出时:空账
    agent_tool.SetBackgroundPermissionSource([&ledger_names]() {
        tools::BackgroundPermissionLedger ledger;
        ledger.always_allowed = ledger_names;
        return ledger;
    });

    REQUIRE_FALSE(agent_tool
                      .execute(nlohmann::json{{"title", "定格验证"}, {"prompt", "写个文件"}, {"run_in_background", true}})
                      .is_error);
    // execute 已返回 = 快照已定格。此刻主会话按 a 落了新账——任务不认。
    ledger_names.insert("write_confirm");
    for (int i = 0; i < 300 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    CHECK(gated_ptr->call_count == 0);
    const std::vector<std::string> notices = agent_tool.TakePermissionDenialNotices();
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].find("write_confirm") != std::string::npos);
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

// ---------------------------------------------------------------------------
// 病十裁决(骨架拆解批三):四段开关进皮。
// 子代理默认与主代理同段(mcp/web/lsp/platforms 全补);真要少的皮显式关。
// 皮上写不出的差别,就是还没想清的差别——这条测试把裁决钉死。
// ---------------------------------------------------------------------------
TEST_CASE("agent 病十:四段开关随皮走——默认全补,显式关就真没有") {
    SUBCASE("默认(不 SetAgentProfile):mcp/web/lsp 三段注入") {
        FakeBackend backend;
        backend.scripts = {TextOnlyScript("结论")};
        tools::ToolRegistry sub_registry;
        tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
        CHECK_FALSE(agent_tool.execute(nlohmann::json{{"title", "四段"}, {"prompt", "查"}}).is_error);
        const std::string& system = backend.captured_requests[0].system;
        // 三段的功能模块正文(锚点取各模块首行标题)。
        CHECK(system.find("外接工具(MCP)") != std::string::npos);
        CHECK(system.find("联网查证") != std::string::npos);
        CHECK(system.find("语义查询(LSP)") != std::string::npos);
    }

    SUBCASE("皮上显式给段:wire 注平台段,关掉的段一个字不占") {
        FakeBackend backend;
        backend.scripts = {TextOnlyScript("结论")};
        tools::ToolRegistry sub_registry;
        tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
        agent::AgentProfile profile;
        profile.prompt_sections.mcp = false;
        profile.prompt_sections.web = false;
        profile.prompt_sections.lsp = false;
        profile.prompt_sections.wire = "anthropic-messages";
        agent_tool.SetAgentProfile(std::move(profile));
        CHECK_FALSE(agent_tool.execute(nlohmann::json{{"title", "四段"}, {"prompt", "查"}}).is_error);
        const std::string& system = backend.captured_requests[0].system;
        CHECK(system.find("外接工具(MCP)") == std::string::npos);
        CHECK(system.find("联网查证") == std::string::npos);
        CHECK(system.find("语义查询(LSP)") == std::string::npos);
        // 平台段按 wire 注了 Anthropic 的协议说明。
        CHECK(system.find("协议说明(Anthropic Messages)") != std::string::npos);
    }
}
