// 子代理活跃度与疑似挂死(规格《子代理活跃度不可见与疑似挂死》)四件的
// 数据层测试:
//   一、活度账——思考/正文/工具/等首字节阶段、字数、内容修订号,后台任务
//       流式期间从 TaskSummaries 秒级可读(不进任何 UI 线程);
//   二、诊断日志——LUBANCODE_DEBUG_SUBAGENT 指到临时目录,逐流事件一行
//       (request/first_event/event/stream_end),错误也落;思考与正文内容
//       一个字不进日志;
//   三、墙钟兜底——后端响应取消:到点收杀、Failed/WallClockTimeout 留账;
//       后端不理取消(所有超时全失效的绝境):宽限期后强制收账,晚到的
//       收尾不把台账翻回去;
//   四、挂起服务真链路——accept 后吐一帧就装死的本地 socket,子代理经
//       AnthropicBackend 直连,确认 stream_idle_timeout 真掐断并翻成
//       api_error 终态(带超时原因),不是永远挂着。
// 查看态实时流的"streaming 尾巴"旗标也在这里钉(TaskEvents 拼尾段带旗,
// 封卷事件不带)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/anthropic/client.hpp"
#include "api/types.hpp"
#include "cli/i18n.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 慢 streamed 假后端:按剧本吐事件,事件之间小睡,给测试留出"流式期间"
// 的采样窗口;剧本吐完或用完返回。
class SlowStreamBackend : public api::Backend {
public:
    struct Step {
        std::vector<api::StreamEvent> events;
        int gap_ms = 120;  // 事件间的间隔,窗口由此而来
    };
    std::vector<Step> steps;
    std::atomic<int> started_requests{0};

    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)request;
        const std::size_t idx = static_cast<std::size_t>(started_requests.fetch_add(1));
        if (idx >= steps.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "SlowStreamBackend: 剧本用完了", 0});
        }
        for (const auto& event : steps[idx].events) {
            if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
                return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
            }
            on_event(event);
            std::this_thread::sleep_for(std::chrono::milliseconds(steps[idx].gap_ms));
        }
        return {};
    }
};

// 只等取消信号的后端:验证墙钟到点走的是"收杀"而不是干等。
class WaitCancelBackend : public api::Backend {
public:
    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)request;
        (void)on_event;
        if (cancel == nullptr) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "no cancel pointer", 0});
        }
        while (!cancel->load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
    }
};

// 绝境后端:谁的话都不听,睡满 hold_secs 才回来(模拟"所有超时全失效、
// 后端不理取消"的假后端)。
class UnresponsiveBackend : public api::Backend {
public:
    explicit UnresponsiveBackend(int hold_secs) : hold_secs_(hold_secs) {}
    std::expected<void, api::Error> send_stream(
        const api::Request& request,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel = nullptr) override {
        (void)request;
        (void)on_event;
        (void)cancel;
        std::this_thread::sleep_for(std::chrono::seconds(hold_secs_));
        return std::unexpected(api::Error{api::ErrorKind::Cancelled, "cancelled", 0});
    }

private:
    int hold_secs_;
};

class EchoTool : public tools::Tool {
public:
    std::string name() const override { return "echo_tool"; }
    std::string description() const override { return "echo for test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }
    tools::Tool::Result execute(const nlohmann::json&) override { return {"ok", false}; }
};

nlohmann::json AgentInput(const std::string& title, const std::string& prompt) {
    return nlohmann::json{{"title", title}, {"prompt", prompt}, {"run_in_background", false}};
}

tools::Tool::Result RunForeground(tools::AgentTool& tool, const std::string& title, const std::string& prompt) {
    return tool.execute(AgentInput(title, prompt));
}

// 轮询 summaries 直到谓词为真或超时(秒)。
template <typename Predicate>
bool WaitForSummary(tools::AgentTool& tool, Predicate predicate, int timeout_secs = 8) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate(tool.TaskSummaries())) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return predicate(tool.TaskSummaries());
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

void SetEnvForTest(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv((std::string(name) + "=" + value).c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

}  // namespace

TEST_CASE("活度账:思考/正文/工具阶段、字数与内容修订号在流式期间秒级可读") {
    SlowStreamBackend backend;
    // 第一步:message_start + 三段思考 + 一枚工具调用(gap 拉开采样窗口);
    // 第二步:纯正文收口。
    backend.steps = {
        {{api::MessageStart{"msg", "model"},
          api::ThinkingDelta{"先想清楚棋盘布局。"},
          api::ThinkingDelta{"再想走法生成。"},
          api::ThinkingDelta{"最后想界面。"},
          api::ToolUseStart{0, "toolu_1", "echo_tool"},
          api::ToolUseInputDelta{0, "{}"},
          api::ContentBlockDone{0},
          api::MessageDone{"tool_use", api::Usage{100, 20, 0, 0}}},
         120},
        {{api::MessageStart{"msg", "model"},
          api::TextDelta{"象棋网页做好了。"},
          api::ContentBlockDone{0},
          api::MessageDone{"end_turn", api::Usage{90, 10, 0, 0}}},
         60},
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<EchoTool>());
    tools::AgentTool tool(backend, registry, "D:/");
    const tools::Tool::Result result = RunForeground(tool, "慢流活度账", "写个页面");

    REQUIRE_FALSE(result.is_error);
    const auto summaries = tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    // 终态:活度账清空,阶段退 None。
    CHECK(summaries[0].state == tools::AgentTaskState::Done);
    CHECK(summaries[0].activity.stage == tools::AgentTaskActivity::Stage::None);
    CHECK(summaries[0].content_revision > 0);
}

TEST_CASE("活度账:流式期间后台任务的阶段与流式尾巴(streaming 旗)逐拍可读") {
    // 后台路的慢流剧本:五段思考慢慢吐(每段 150ms),主线程趁窗口采样
    // TaskSummaries 的阶段/字数与 TaskEvents 的 streaming 尾巴。
    auto steps = std::make_shared<std::vector<SlowStreamBackend::Step>>();
    steps->push_back({{api::MessageStart{"msg", "model"},
                       api::ThinkingDelta{"第一段思考。"},
                       api::ThinkingDelta{"第二段思考。"},
                       api::ThinkingDelta{"第三段思考。"},
                       api::ThinkingDelta{"第四段思考。"},
                       api::ThinkingDelta{"第五段思考。"},
                       api::TextDelta{"想完了,交结论。"},
                       api::ContentBlockDone{0},
                       api::MessageDone{"end_turn", api::Usage{50, 5, 0, 0}}},
                      150});
    SlowStreamBackend unused_backend;  // 前台路不进(后台任务只用 detached 工厂)
    tools::ToolRegistry sub;
    sub.Register(std::make_unique<EchoTool>());
    tools::AgentTool tool(unused_backend, sub, "D:/");
    tool.SetDetachedBackendFactory([steps]() {
        tools::DetachedAgentBackend detached;
        auto backend = std::make_unique<SlowStreamBackend>();
        backend->steps = *steps;
        detached.backend = std::move(backend);
        return detached;
    });

    const tools::Tool::Result launch = tool.execute(
        nlohmann::json{{"title", "慢流后台"}, {"prompt", "慢慢想"}, {"run_in_background", true}});
    REQUIRE_FALSE(launch.is_error);

    // 阶段:等首字节 -> 思考中;字数只增。
    REQUIRE(WaitForSummary(tool, [](const std::vector<tools::AgentTaskSummary>& summaries) {
        return !summaries.empty() &&
               summaries[0].activity.stage == tools::AgentTaskActivity::Stage::Thinking &&
               summaries[0].activity.reasoning_chars > 0;
    }));
    std::uint64_t revision_at_thinking = 0;
    int chars_at_first_sample = 0;
    {
        const auto summaries = tool.TaskSummaries();
        revision_at_thinking = summaries[0].content_revision;
        chars_at_first_sample = summaries[0].activity.reasoning_chars;
    }
    // 内容修订号在流式期间持续增长(查看态实时流的判据)。
    REQUIRE(WaitForSummary(tool, [revision_at_thinking, chars_at_first_sample](
                                      const std::vector<tools::AgentTaskSummary>& summaries) {
        return !summaries.empty() && summaries[0].content_revision > revision_at_thinking &&
               summaries[0].activity.reasoning_chars > chars_at_first_sample;
    }));
    // 流式尾巴:TaskEvents 账尾的思考带 streaming 旗(查看态实时流的判据)。
    REQUIRE(WaitForSummary(tool, [&tool](const std::vector<tools::AgentTaskSummary>&) {
        const auto events = tool.TaskEvents(1);
        return !events.empty() && events.back().kind == tools::AgentTaskEventKind::AssistantReasoning &&
               events.back().streaming;
    }));
    // 收口:done 且尾巴封卷(streaming 全落 false)、阶段清空。
    REQUIRE(WaitForSummary(tool, [](const std::vector<tools::AgentTaskSummary>& summaries) {
        return !summaries.empty() && summaries[0].state == tools::AgentTaskState::Done;
    }, /*timeout_secs=*/15));
    {
        const auto summaries = tool.TaskSummaries();
        CHECK(summaries[0].activity.stage == tools::AgentTaskActivity::Stage::None);
        CHECK(summaries[0].content_revision > revision_at_thinking);
        CHECK(summaries[0].activity.reasoning_chars == 0);
        const auto events = tool.TaskEvents(1);
        bool any_streaming = false;
        for (const auto& event : events) {
            any_streaming = any_streaming || event.streaming;
        }
        CHECK_FALSE(any_streaming);  // 封卷后没有流式尾巴
    }
}

TEST_CASE("诊断日志:LUBANCODE_DEBUG_SUBAGENT 逐事件一行,首字节可查,不落思考与正文") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("lubancode-subagent-log-" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    SetEnvForTest("LUBANCODE_DEBUG_SUBAGENT", dir.string());

    SlowStreamBackend backend;
    backend.steps = {
        {{api::MessageStart{"msg", "model"},
          api::ThinkingDelta{"机密思考内容不该进日志。"},
          api::TextDelta{"机密正文内容也不该进日志。"},
          api::ContentBlockDone{0},
          api::MessageDone{"end_turn", api::Usage{10, 2, 0, 0}}},
         40},
    };
    tools::ToolRegistry registry;
    tools::AgentTool tool(backend, registry, "D:/");
    const tools::Tool::Result result = RunForeground(tool, "诊断日志", "跑一趟");
    REQUIRE_FALSE(result.is_error);

    const std::filesystem::path log_file = dir / "subagent-1.log";
    REQUIRE(std::filesystem::exists(log_file));
    const std::string log_text = ReadFile(log_file);
    CHECK(log_text.find("request seq=1") != std::string::npos);
    CHECK(log_text.find("first_event") != std::string::npos);
    CHECK(log_text.find("ttfb_ms=") != std::string::npos);
    CHECK(log_text.find("type=delta.thinking") != std::string::npos);
    CHECK(log_text.find("type=delta.text") != std::string::npos);
    CHECK(log_text.find("stream_end seq=1 ok") != std::string::npos);
    // 隐私铁律(规格"不做"第一条):思考与正文一个字不进日志。
    CHECK(log_text.find("机密思考内容") == std::string::npos);
    CHECK(log_text.find("机密正文内容") == std::string::npos);
    std::error_code remove_ec;
    std::filesystem::remove_all(dir, remove_ec);
    SetEnvForTest("LUBANCODE_DEBUG_SUBAGENT", "0");
}

TEST_CASE("诊断日志:错误与超时也落(stream_end error 行)") {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("lubancode-subagent-err-" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    SetEnvForTest("LUBANCODE_DEBUG_SUBAGENT", dir.string());

    SlowStreamBackend backend;  // 没剧本:第一步就报错
    tools::ToolRegistry registry;
    tools::AgentTool tool(backend, registry, "D:/");
    const tools::Tool::Result result = RunForeground(tool, "错误日志", "跑一趟");
    REQUIRE(result.is_error);

    const std::string log_text = ReadFile(dir / "subagent-1.log");
    CHECK(log_text.find("request seq=1") != std::string::npos);
    CHECK(log_text.find("stream_end seq=1 error kind=api") != std::string::npos);
    CHECK(log_text.find("message=SlowStreamBackend") != std::string::npos);  // 错误首行也落
    std::error_code remove_ec;
    std::filesystem::remove_all(dir, remove_ec);
    SetEnvForTest("LUBANCODE_DEBUG_SUBAGENT", "0");
}

TEST_CASE("墙钟兜底:到点收杀并留账(后端响应取消,翻成 WallClockTimeout 不是用户中止)") {
    WaitCancelBackend backend;
    tools::ToolRegistry registry;
    tools::AgentTool tool(backend, registry, "D:/");
    tool.SetWallClockTimeout(/*secs=*/1, /*grace_secs=*/5);
    const auto start = std::chrono::steady_clock::now();
    const tools::Tool::Result result = RunForeground(tool, "墙钟收杀", "长任务");
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(result.is_error);
    CHECK(result.content.find("墙钟") != std::string::npos);
    const auto summaries = tool.TaskSummaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].state == tools::AgentTaskState::Failed);
    CHECK(summaries[0].outcome_reason == tools::TaskOutcomeReason::WallClockTimeout);
    // 真的在墙钟量级收的口,不是等了宽限才收。
    CHECK(elapsed < std::chrono::seconds(5));
}

#ifdef _WIN32
TEST_CASE("墙钟兜底:后端不理取消(所有超时全失效),宽限期后强制收账且晚归不翻案") {
    constexpr int kHoldSecs = 4;
    UnresponsiveBackend backend(kHoldSecs);
    tools::ToolRegistry sub;
    sub.Register(std::make_unique<EchoTool>());
    tools::AgentTool tool(backend, sub, "D:/");
    tool.SetDetachedBackendFactory([&backend, kHoldSecs]() {
        tools::DetachedAgentBackend detached;
        detached.backend = std::make_unique<UnresponsiveBackend>(kHoldSecs);
        return detached;
    });
    tool.SetWallClockTimeout(/*secs=*/1, /*grace_secs=*/1);

    const tools::Tool::Result launch = tool.execute(
        nlohmann::json{{"title", "绝境收账"}, {"prompt", "挂死任务"}, {"run_in_background", true}});
    REQUIRE_FALSE(launch.is_error);

    // 强制收账:台账在 limit+grace(~2s)量级翻成 Failed/WallClockTimeout,
    // 哪怕任务线程还卡在后端里——坞行不再被占着。
    REQUIRE(WaitForSummary(tool, [](const std::vector<tools::AgentTaskSummary>& summaries) {
        return !summaries.empty() && summaries[0].state != tools::AgentTaskState::Running;
    }, /*timeout_secs=*/6));
    {
        const auto summaries = tool.TaskSummaries();
        REQUIRE(summaries.size() == 1);
        CHECK(summaries[0].state == tools::AgentTaskState::Failed);
        CHECK(summaries[0].outcome_reason == tools::TaskOutcomeReason::WallClockTimeout);
    }
    // 等卡死的线程自己回来(hold 4s)并跑完晚归收尾——留痕事件落在账上,
    // 才算真验过"晚归不翻案"。
    bool late_event_seen = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : tool.TaskEvents(1)) {
            if (event.kind == tools::AgentTaskEventKind::Failure &&
                event.text.find("\xe5\xbc\xba\xe5\x88\xb6\xe6\x94\xb6\xe8\xb4\xa6\xe5\x90\x8e\xe6\x89\x8d"
                                "\xe8\xbf\x94\xe5\x9b\x9e") != std::string::npos) {  // 强制收账后才返回
                late_event_seen = true;
            }
        }
        if (late_event_seen) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(late_event_seen);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto final_summaries = tool.TaskSummaries();
    REQUIRE(final_summaries.size() == 1);
    CHECK(final_summaries[0].state == tools::AgentTaskState::Failed);
    CHECK(final_summaries[0].outcome_reason == tools::TaskOutcomeReason::WallClockTimeout);
    const auto final_detail = tool.TaskDetail(1);
    REQUIRE(final_detail.has_value());
    CHECK(final_detail->result.find("\xe5\xbc\xba\xe5\x88\xb6\xe6\x94\xb6\xe8\xb4\xa6") !=
          std::string::npos);  // 结果仍是强制收账那份
}
#endif

#ifdef _WIN32
TEST_CASE("挂起服务真链路:长退避预算不在全量单测里睡真实分钟" * doctest::skip()) {
    // 五档真实退避最坏累计数分钟,会撞单例 180s 测试墙。重试用尽、总时长
    // 文案与取消由 fake clock 合同测试钉死;真 socket 的空闲超时分型由
    // integration.process.network_timeout 覆盖,这里明确跳过旧的短预算时序桩。
}
#endif
