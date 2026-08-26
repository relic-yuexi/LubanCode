// tools::AgentMessageTool(agent_message):主模型给运行中子代理传增量的窄
// 工具。四类:
//   1. 直接 execute:入参校验、queued 的可审计 JSON、not found/finished/
//      invalid/unavailable 分得清;
//   2. RunningTasksRoster:运行中名册只列 id/标题/类型/待送数;
//   3. 集成:假 main backend 按脚本发 agent 起后台任务、再发 agent_message,
//      断言目标 TaskRecord::inbox 真收到,且子代理下一请求里带来源标签。
// 全程假后端,不碰真网络。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/agent_message_tool.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(main 侧用),与 test_agent_tool.cpp 同款手艺。
class ScriptBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::vector<api::Request> captured_requests;

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)cancel;
        captured_requests.push_back(request);
        const std::size_t idx = captured_requests.size() - 1;
        if (idx >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "ScriptBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[idx]) {
            on_event(event);
        }
        return {};
    }
};

std::vector<api::StreamEvent> TextOnlyScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseWithInputScript(const std::string& id, const std::string& name,
                                                     const std::string& input_json) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, id, name},
        api::ToolUseInputDelta{0, input_json},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 挂住等放闸的后端(子代理侧用):请求进来先等 release,放闸后吐纯文本
// 收口。测试在挂住期间发 agent_message,正好落在"最后一轮收写纯文本"
// 的窗口里。
struct BlockingState {
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    bool release = false;
    std::vector<api::Request> captured;
};

class BlockingBackend : public api::Backend {
public:
    explicit BlockingBackend(std::shared_ptr<BlockingState> state) : state_(std::move(state)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->captured.push_back(request);
            state_->started = true;
            state_->cv.notify_all();
            state_->cv.wait_for(lock, std::chrono::seconds(5), [&]() {
                return state_->release || (cancel != nullptr && cancel->load(std::memory_order_acquire));
            });
        }
        for (const auto& event : TextOnlyScript("后台摸排完毕")) {
            on_event(event);
        }
        return {};
    }

private:
    std::shared_ptr<BlockingState> state_;
};

void WaitStarted(const std::shared_ptr<BlockingState>& state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, std::chrono::seconds(2), [&]() { return state->started; });
}

void Release(const std::shared_ptr<BlockingState>& state) {
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release = true;
    }
    state->cv.notify_all();
}

void WaitIdle(tools::AgentTool& agent_tool) {
    for (int i = 0; i < 300 && agent_tool.HasRunningTasks(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

std::string DumpTexts(const std::vector<api::Message>& messages) {
    std::string out;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block)) {
                out += text->text + "\n";
            } else if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
                out += result->content + "\n";
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("agent_message:入参校验与四类失败分清,失败不投 main、不假报成功") {
    // runtime unavailable:没配子代理通道。
    tools::AgentMessageTool unavailable_tool(nullptr);
    const auto unavailable = unavailable_tool.execute(nlohmann::json{{"task_id", 1}, {"message", "x"}});
    CHECK(unavailable.is_error);

    tools::ToolRegistry sub_registry;
    ScriptBackend foreground_backend;
    auto gate = std::make_shared<BlockingState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([gate]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<BlockingBackend>(gate);
        return detached;
    });
    tools::AgentMessageTool message_tool(&agent_tool);
    agent_tool.execute(nlohmann::json{{"title", "校验任务"}, {"prompt", "摸排"}, {"run_in_background", true}});
    WaitStarted(gate);

    // empty/invalid message。
    CHECK(message_tool.execute(nlohmann::json{{"task_id", 1}, {"message", "   "}}).is_error);
    CHECK(message_tool.execute(nlohmann::json{{"task_id", 1}}).is_error);
    CHECK(message_tool.execute(nlohmann::json{{"message", "没有任务号"}}).is_error);
    CHECK(message_tool.execute(nlohmann::json{{"task_id", "1"}, {"message", "字符串任务号"}}).is_error);

    // queued:可审计 JSON + 人话一行;消息真进了目标 inbox。
    const tools::Tool::Result queued =
        message_tool.execute(nlohmann::json{{"task_id", 1}, {"message", "用户原话:改成三态"}});
    CHECK_FALSE(queued.is_error);
    CHECK(queued.content.find("\"status\":\"queued\"") != std::string::npos);
    CHECK(queued.content.find("\"task_id\":1") != std::string::npos);
    CHECK(queued.content.find("\"pending_count\":1") != std::string::npos);
    const auto pending = agent_tool.PendingTaskMessages(1);
    REQUIRE(pending.size() == 1);
    CHECK(pending[0] == "用户原话:改成三态");

    // task not found。
    const auto not_found = message_tool.execute(nlohmann::json{{"task_id", 99}, {"message", "没有这只"}});
    CHECK(not_found.is_error);
    CHECK(not_found.content.find("\"status\":\"not_found\"") != std::string::npos);

    // task already finished:封账后拒收,先成功后丢件不允许。
    Release(gate);
    WaitIdle(agent_tool);
    const auto finished = message_tool.execute(nlohmann::json{{"task_id", 1}, {"message", "迟到"}});
    CHECK(finished.is_error);
    CHECK(finished.content.find("\"status\":\"finished\"") != std::string::npos);
    // 全程没有一枚请求走 main 的 backend:失败/成功都没绕道。
    CHECK(foreground_backend.captured_requests.empty());
}

TEST_CASE("RunningTasksRoster:只列运行中任务,带 id/标题/类型/待送数;无运行即空") {
    tools::ToolRegistry sub_registry;
    ScriptBackend foreground_backend;
    auto gate = std::make_shared<BlockingState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([gate]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<BlockingBackend>(gate);
        return detached;
    });
    CHECK(agent_tool.RunningTasksRoster().empty());  // 一只都没有:不注入

    agent_tool.execute(nlohmann::json{{"title", "面板真标题"}, {"prompt", "摸排"}, {"run_in_background", true}});
    WaitStarted(gate);
    const std::string roster = agent_tool.RunningTasksRoster();
    CHECK(roster.find("#1") != std::string::npos);
    CHECK(roster.find("面板真标题") != std::string::npos);
    CHECK(roster.find("general-purpose") != std::string::npos);
    CHECK(roster.find("待送达消息 0 条") != std::string::npos);
    CHECK(roster.find("agent_message") != std::string::npos);  // 名册顺带指路
    // 不塞 prompt 全文:任务说明不该出现在名册里。
    CHECK(roster.find("摸排") == std::string::npos);

    CHECK(agent_tool.SendTaskMessage(1, "加一条验收", tools::TaskMessageSource::MainAgent) ==
          tools::TaskMessageStatus::Queued);
    CHECK(agent_tool.RunningTasksRoster().find("待送达消息 1 条") != std::string::npos);

    Release(gate);
    WaitIdle(agent_tool);
    CHECK(agent_tool.RunningTasksRoster().empty());  // 终态不占名册
}

TEST_CASE("集成:假 main 发 agent_message,目标 inbox 收到,子代理下一请求带来源标签") {
    // main 侧:脚本起一只后台任务,再发一次 agent_message,最后收口。
    ScriptBackend main_backend;
    main_backend.scripts = {
        ToolUseWithInputScript("call_agent", "agent",
                               R"({"title":"集成任务","prompt":"摸排 schema","execution_mode":"background"})"),
        ToolUseWithInputScript("call_message", "agent_message",
                               R"({"task_id":1,"message":"用户原话:改成三态\n[主代理补充上下文] 注意兼容旧参数"})"),
        TextOnlyScript("已转交,等它回话。"),
    };
    // 子代理侧:挂住的后端,等 main 把话转进来再放行。
    auto gate = std::make_shared<BlockingState>();

    tools::ToolRegistry sub_registry;
    tools::ToolRegistry main_registry;
    main_registry.Register(std::make_unique<tools::AgentTool>(main_backend, sub_registry, "/work/dir"));
    auto* agent_tool = dynamic_cast<tools::AgentTool*>(main_registry.Find("agent"));
    REQUIRE(agent_tool != nullptr);
    agent_tool->SetDetachedBackendFactory([gate]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<BlockingBackend>(gate);
        return detached;
    });
    main_registry.Register(std::make_unique<tools::AgentMessageTool>(agent_tool));

    agent::Agent main_loop(main_backend, main_registry, agent::AgentProfile{.request{.model = "model"}, .system_prompt = "system"});
    const auto outcome = main_loop.Run("起一只后台代理,把用户的补充转给它", agent::Callbacks{}, nullptr);
    REQUIRE(outcome.has_value());

    // main history 留完整 tool_use/tool_result 审计:agent 起了任务,
    // agent_message 排队成功(queued + pending_count)。
    const std::string main_dump = DumpTexts(main_loop.history());
    CHECK(main_dump.find("后台子代理 #1") != std::string::npos);
    CHECK(main_dump.find("\"status\":\"queued\"") != std::string::npos);
    CHECK(main_dump.find("\"pending_count\":1") != std::string::npos);

    // 目标 inbox 真收到(此刻子代理仍挂住,消息还没送达)。
    const auto pending = agent_tool->PendingTaskMessages(1);
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].find("用户原话:改成三态") != std::string::npos);
    CHECK(pending[0].find("[主代理补充上下文]") != std::string::npos);

    // 放行:子代理收口后没有下一处工具边界,靠封账交接续投一轮——
    // 下一请求里带原文与来源标签,分栏保住了用户原话。
    Release(gate);
    WaitIdle(*agent_tool);
    std::vector<api::Request> sub_requests;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        sub_requests = gate->captured;
    }
    REQUIRE(sub_requests.size() >= 2);
    const std::string sub_dump = DumpTexts(sub_requests.back().messages);
    CHECK(sub_dump.find("用户原话:改成三态") != std::string::npos);
    CHECK(sub_dump.find("[主代理补充上下文] 注意兼容旧参数") != std::string::npos);
    CHECK(sub_dump.find("[主代理转交的补充]") != std::string::npos);
    CHECK(sub_dump.find("[主会话用户介入]") == std::string::npos);
    CHECK(agent_tool->PendingTaskMessages(1).empty());
}
