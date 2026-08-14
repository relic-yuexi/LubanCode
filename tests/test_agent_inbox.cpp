// 0.28.x 面板一单的定向介入与台账测试:SendTaskMessage/TaskRecord inbox、
// CancelTask/CancelAllTasks/ClearFinishedTask、TaskSummaries 轻量全量、收场
// 报告。全部用可阻塞的 fake subagent(不碰真网络);"两只互不串台"那只
// 用例是规格点名的验收:给 #2 的消息只出现在 #2 的下一请求里。

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

#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(与 test_agent_tool.cpp 同款手艺,那边在另一个
// 编译单元,这里自带一份小的)。
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

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id, const std::string& tool_name) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, tool_name},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 可阻塞的"两段脚本"后端:第一请求先挂住等闸(模拟子代理正在跑),放闸后
// 吐一次 tool_use(工具执行完)——第二请求原样放行吐文本结论。收到的请求
// 全记着,供"介入消息只出现在下一边界之后"的断言用。
struct GateBackendState {
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    bool release = false;
    std::vector<api::Request> captured;
};

class GateBackend : public api::Backend {
public:
    explicit GateBackend(std::shared_ptr<GateBackendState> state) : state_(std::move(state)) {}

    std::expected<void, api::Error> send_stream(
        const api::Request& request, const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        {
            std::unique_lock<std::mutex> lock(state_->mutex);
            state_->captured.push_back(request);
            state_->started = true;
            state_->cv.notify_all();
            const bool released = state_->cv.wait_for(lock, std::chrono::seconds(5), [&]() {
                return state_->release || (cancel != nullptr && cancel->load(std::memory_order_acquire));
            });
            if (!released) {
                return std::unexpected(api::Error{api::ErrorKind::Api, "gate timeout", 0});
            }
        }
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
        }
        std::size_t call_index;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            call_index = state_->captured.size() - 1;
        }
        const std::vector<api::StreamEvent> events =
            call_index == 0 ? ToolUseScript("t1", "probe") : TextOnlyScript("结论已就绪");
        for (const auto& event : events) {
            on_event(event);
        }
        return {};
    }

private:
    std::shared_ptr<GateBackendState> state_;
};

class FakeTool : public tools::Tool {
public:
    std::string name() const override { return "probe"; }
    std::string description() const override { return "fake tool for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"probe ok", false}; }
};

std::string DumpMessageTexts(const std::vector<api::Message>& messages) {
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

void WaitStarted(const std::shared_ptr<GateBackendState>& state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait_for(lock, std::chrono::seconds(2), [&]() { return state->started; });
}

void OpenGate(const std::shared_ptr<GateBackendState>& state) {
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

}  // namespace

TEST_CASE("定向介入:消息排进指定任务的 inbox,按序在工具收尾后的下一请求注入") {
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>());
    ScriptBackend foreground_backend;
    auto gate = std::make_shared<GateBackendState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([gate]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<GateBackend>(gate);
        return detached;
    });

    CHECK(agent_tool.execute(nlohmann::json{{"prompt", "后台摸排"}, {"run_in_background", true}}).content.find(
              "#1") != std::string::npos);
    WaitStarted(gate);

    // 连投两条:都排队、顺序保留。
    CHECK(agent_tool.SendTaskMessage(1, "只读,不要修改") == tools::TaskMessageStatus::Queued);
    CHECK(agent_tool.SendTaskMessage(1, "把证据列全") == tools::TaskMessageStatus::Queued);
    const auto pending = agent_tool.PendingTaskMessages(1);
    REQUIRE(pending.size() == 2);
    CHECK(pending[0] == "只读,不要修改");
    CHECK(pending[1] == "把证据列全");

    OpenGate(gate);
    WaitIdle(agent_tool);
    REQUIRE_FALSE(agent_tool.HasRunningTasks());

    // 第二请求(工具收尾后的下一边界)里:工具结果在前,两条介入按原顺序
    // 追加在后面;第一请求(投递之前)里没有这些话。
    std::vector<api::Request> captured;
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        captured = gate->captured;
    }
    REQUIRE(captured.size() == 2);
    const std::string first_dump = DumpMessageTexts(captured[0].messages);
    const std::string second_dump = DumpMessageTexts(captured[1].messages);
    CHECK(first_dump.find("只读,不要修改") == std::string::npos);
    CHECK(second_dump.find("probe ok") != std::string::npos);  // 工具结果没被截掉
    CHECK(second_dump.find("[主会话用户介入]") != std::string::npos);
    const auto first_pos = second_dump.find("只读,不要修改");
    const auto second_pos = second_dump.find("把证据列全");
    REQUIRE(first_pos != std::string::npos);
    REQUIRE(second_pos != std::string::npos);
    CHECK(first_pos < second_pos);
    CHECK(second_dump.find("probe ok") < first_pos);  // 注入发生在工具结果之后
    CHECK(agent_tool.PendingTaskMessages(1).empty());  // 全送达
}

TEST_CASE("定向介入:终态明确拒收,不改投 main;任务号不存在也是拒收") {
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>());
    ScriptBackend foreground_backend;
    auto gate = std::make_shared<GateBackendState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([gate]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<GateBackend>(gate);
        return detached;
    });
    agent_tool.execute(nlohmann::json{{"prompt", "后台摸排"}, {"run_in_background", true}});
    WaitStarted(gate);
    OpenGate(gate);
    WaitIdle(agent_tool);

    CHECK(agent_tool.SendTaskMessage(1, "还来得及吗") == tools::TaskMessageStatus::Finished);
    CHECK(agent_tool.SendTaskMessage(999, "没有这只") == tools::TaskMessageStatus::NotFound);
    CHECK(agent_tool.SendTaskMessage(1, "") == tools::TaskMessageStatus::NotFound);
    // 前台 backend 一枚请求都没收到:拒收的话没有绕道 main。
    CHECK(foreground_backend.captured_requests.empty());
}

TEST_CASE("正式取消接口:CancelTask 只停那只;ClearFinishedTask 运行中不给清、终态清台账") {
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>());
    ScriptBackend foreground_backend;
    auto gate_a = std::make_shared<GateBackendState>();
    auto gate_b = std::make_shared<GateBackendState>();
    int factory_calls = 0;
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([&]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<GateBackend>(factory_calls++ == 0 ? gate_a : gate_b);
        return detached;
    });
    agent_tool.execute(nlohmann::json{{"prompt", "任务一"}, {"run_in_background", true}});
    agent_tool.execute(nlohmann::json{{"prompt", "任务二"}, {"run_in_background", true}});
    WaitStarted(gate_a);
    WaitStarted(gate_b);

    CHECK_FALSE(agent_tool.ClearFinishedTask(1));  // 运行中不给清
    CHECK(agent_tool.CancelTask(1));               // 只停一号
    OpenGate(gate_a);
    OpenGate(gate_b);
    WaitIdle(agent_tool);

    auto summaries = agent_tool.TaskSummaries();
    REQUIRE(summaries.size() == 2);
    tools::AgentTaskState state_one = tools::AgentTaskState::Done;
    for (const auto& summary : summaries) {
        if (summary.id == 1) {
            state_one = summary.state;
        }
    }
    CHECK(state_one == tools::AgentTaskState::Cancelled);

    CHECK(agent_tool.CancelAllTasks() == 0);  // 都终态了,没有可停的
    CHECK(agent_tool.ClearFinishedTask(1));   // 终态可清
    CHECK_FALSE(agent_tool.ClearFinishedTask(1));
    CHECK(agent_tool.TaskSummaries().size() == 1);
}

TEST_CASE("两只可阻塞 fake subagent 定向收信:给 #2 的话只进 #2,不串 #1、不串 main") {
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>());
    ScriptBackend foreground_backend;
    auto gate_a = std::make_shared<GateBackendState>();
    auto gate_b = std::make_shared<GateBackendState>();
    int factory_calls = 0;
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([&]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<GateBackend>(factory_calls++ == 0 ? gate_a : gate_b);
        return detached;
    });
    agent_tool.execute(nlohmann::json{{"prompt", "一号任务"}, {"run_in_background", true}});
    agent_tool.execute(nlohmann::json{{"prompt", "二号任务"}, {"run_in_background", true}});
    WaitStarted(gate_a);
    WaitStarted(gate_b);

    CHECK(agent_tool.SendTaskMessage(2, "给二号的话") == tools::TaskMessageStatus::Queued);
    OpenGate(gate_a);
    OpenGate(gate_b);
    WaitIdle(agent_tool);

    std::vector<api::Request> a_requests;
    std::vector<api::Request> b_requests;
    {
        std::lock_guard<std::mutex> lock(gate_a->mutex);
        a_requests = gate_a->captured;
    }
    {
        std::lock_guard<std::mutex> lock(gate_b->mutex);
        b_requests = gate_b->captured;
    }
    REQUIRE(a_requests.size() == 2);
    REQUIRE(b_requests.size() == 2);
    CHECK(DumpMessageTexts(a_requests[0].messages).find("给二号的话") == std::string::npos);
    CHECK(DumpMessageTexts(a_requests[1].messages).find("给二号的话") == std::string::npos);
    CHECK(DumpMessageTexts(b_requests[1].messages).find("给二号的话") != std::string::npos);
    CHECK(foreground_backend.captured_requests.empty());  // main 的 backend 一枚请求都没有
}

TEST_CASE("收场报告与未送达标注:打断后结果带标注;报告列原文且只报一次") {
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>());
    ScriptBackend foreground_backend;
    auto gate = std::make_shared<GateBackendState>();
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    agent_tool.SetDetachedBackendFactory([gate]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<GateBackend>(gate);
        return detached;
    });
    agent_tool.execute(nlohmann::json{{"prompt", "后台摸排"}, {"run_in_background", true}});
    WaitStarted(gate);
    CHECK(agent_tool.SendTaskMessage(1, "来不及的话") == tools::TaskMessageStatus::Queued);

    // 取消打断收尾:排着的话没有下一个边界可等,结果文本带"未送达"标注。
    CHECK(agent_tool.CancelTask(1));
    OpenGate(gate);
    WaitIdle(agent_tool);
    const auto snapshot = agent_tool.TaskDetail(1);
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->result.find("介入消息未送达") != std::string::npos);

    // 收场报告(会话退出/清场那路):第二只任务上另排一条,报告把两只任务
    // 的未送达消息按任务各列一行、报过即清;随后取消收尾,不让线程挂着。
    agent_tool.execute(nlohmann::json{{"prompt", "再来一只"}, {"run_in_background", true}});
    WaitStarted(gate);
    CHECK(agent_tool.SendTaskMessage(2, "也来不及") == tools::TaskMessageStatus::Queued);
    const auto report = agent_tool.TakeUndeliveredInboxReport();
    REQUIRE(report.size() == 2);  // 第一只的"来不及的话"也还在台账里没送达
    bool saw_first = false;
    bool saw_second = false;
    for (const auto& line : report) {
        saw_first = saw_first || line.find("来不及的话") != std::string::npos;
        saw_second = saw_second || line.find("也来不及") != std::string::npos;
    }
    CHECK(saw_first);
    CHECK(saw_second);
    CHECK(agent_tool.TakeUndeliveredInboxReport().empty());
    CHECK(agent_tool.CancelTask(2));
    OpenGate(gate);
    WaitIdle(agent_tool);
}

TEST_CASE("TaskSummaries 轻量全量:完成与运行中一并列出,pending 计数在列") {
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>());
    ScriptBackend foreground_backend;
    auto gate = std::make_shared<GateBackendState>();
    int factory_calls = 0;
    tools::AgentTool agent_tool(foreground_backend, sub_registry, "/work/dir");
    // 前 2 只快完事;确认收尾之后再挂 6 只闸上任务(8 路上限是既有规矩,
    // 得等前面的真进终态才轮得到后面的)。
    agent_tool.SetDetachedBackendFactory([&]() {
        tools::DetachedAgentBackend detached;
        if (factory_calls++ < 2) {
            auto script_backend = std::make_unique<ScriptBackend>();
            script_backend->scripts = {TextOnlyScript("快完事"), TextOnlyScript("快完事"),
                                       TextOnlyScript("快完事")};
            detached.backend = std::move(script_backend);
        } else {
            detached.backend = std::make_unique<GateBackend>(gate);
        }
        return detached;
    });
    for (int i = 0; i < 2; ++i) {
        REQUIRE_FALSE(agent_tool.execute(nlohmann::json{{"prompt", "快任务" + std::to_string(i)},
                                                        {"run_in_background", true}})
                          .is_error);
    }
    for (int i = 0; i < 500; ++i) {
        std::size_t finished = 0;
        for (const auto& summary : agent_tool.TaskSummaries()) {
            if (summary.state != tools::AgentTaskState::Running) {
                ++finished;
            }
        }
        if (finished >= 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i = 0; i < 8; ++i) {
        REQUIRE_FALSE(agent_tool.execute(nlohmann::json{{"prompt", "慢任务" + std::to_string(i)},
                                                        {"run_in_background", true}})
                          .is_error);
    }
    // 8 路运行中跑满:下一只进不来(上限按"运行中"计数,规矩不动)。
    CHECK(agent_tool.execute(nlohmann::json{{"prompt", "第十只"}, {"run_in_background", true}}).is_error);

    // 轻量列表不该因为"2 只已完成"就把运行中的挤出去:10 只全在
    // (旧 TaskSnapshots(8) 会把 8 只运行中之外的全挤掉,这里 2 只完成的
    // 也一个不少)。
    const auto summaries = agent_tool.TaskSummaries();
    CHECK(summaries.size() == 10);
    std::size_t running = 0;
    std::size_t finished = 0;
    for (const auto& summary : summaries) {
        if (summary.state == tools::AgentTaskState::Running) {
            ++running;
        } else {
            ++finished;
        }
        CHECK(summary.pending_message_count == 0);
    }
    CHECK(finished == 2);
    CHECK(running == 8);
    // 顺手给一只运行中的排条消息:pending 计数立刻在列(面板尾巴那行)。
    CHECK(agent_tool.SendTaskMessage(3, "看一眼就好") == tools::TaskMessageStatus::Queued);
    bool saw_pending = false;
    for (const auto& summary : agent_tool.TaskSummaries()) {
        if (summary.id == 3 && summary.pending_message_count == 1) {
            saw_pending = true;
        }
    }
    CHECK(saw_pending);
    OpenGate(gate);
    WaitIdle(agent_tool);
}
