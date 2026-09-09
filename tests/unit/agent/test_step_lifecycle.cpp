// 四层生命周期单 P1:Step 稳定身份与账目。
//
// 钉三件事:
//   1. StepUsageRecord 扩展字段(step_id/turn_id/attempts/api_duration_ms/
//      stop_reason)随 usage 流水落账,unknown usage(reported=false)不写
//      零、不清身份,汇总口径(从记录求和)不被新字段扰动;
//   2. step_id 在 Agent 域单调:同一只 Agent 连跑两次 Run(续跑/接力的形
//      状),号不重不裂;Run 局部 step_index 照旧各自从 0 起(展示坐标);
//   3. turn_id 钉进 wiring 后随每枚 Step 带出——多次 Run 缝同一只 Turn 时
//      全程同一枚号。

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/turn_event_adapter.hpp"
#include "runtime/turn_runtime.hpp"
#include "tools/registry.hpp"

using namespace lubancode;

namespace {

// 按脚本吐事件的假后端(与 test_loop.cpp 同款,只留本册要用的最小面)。
class FakeBackend : public api::Backend {
public:
    std::vector<std::vector<api::StreamEvent>> scripts;
    std::size_t calls = 0;

    std::expected<void, api::Error> send_stream(
        const api::Request& /*request*/,
        const std::function<void(const api::StreamEvent&)>& on_event,
        const std::atomic<bool>* /*cancel*/ = nullptr) override {
        if (calls >= scripts.size()) {
            return std::unexpected(api::Error{api::ErrorKind::Api, "FakeBackend: 脚本用完了", 0});
        }
        for (const auto& event : scripts[calls]) {
            on_event(event);
        }
        ++calls;
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

// 固定返回一个结果的假工具(与 test_loop.cpp 同款):不需要确认,跑完
// 就回结果,让第二轮 Run 能凑出"一次 Run 两只 Step"的形状。
class FakeTool : public tools::Tool {
public:
    FakeTool(std::string name, tools::Tool::Result result)
        : name_(std::move(name)), result_(std::move(result)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake tool for step lifecycle test"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return false; }

    tools::Tool::Result execute(const nlohmann::json&) override { return result_; }

private:
    std::string name_;
    tools::Tool::Result result_;
};

// 录音 sink:只取 UsageUpdated,把 Step 身份账原样抄下来。
class UsageRecorder final : public runtime::EventSink {
public:
    void Emit(const runtime::ServerEvent& event) override {
        if (event.kind != runtime::ServerEventKind::UsageUpdated) {
            return;
        }
        StepLine line;
        line.step_index = event.payload.value("step_index", 0);
        line.step_id = event.payload.value("step_id", std::string());
        line.turn_id = event.payload.value("turn_id", std::string());
        line.attempts = event.payload.value("attempts", 0);
        line.api_duration_ms = event.payload.value("api_duration_ms", std::int64_t{0});
        line.stop_reason = event.payload.value("stop_reason", std::string());
        lines.push_back(line);
    }

    struct StepLine {
        int step_index = 0;
        std::string step_id;
        std::string turn_id;
        int attempts = 0;
        std::int64_t api_duration_ms = 0;
        std::string stop_reason;
    };
    std::vector<StepLine> lines;
};

struct RecordedTurn {
    runtime::IdAuthority ids;
    runtime::TurnEventAdapter adapter;
    UsageRecorder recorder;
    RecordedTurn() : adapter("test", ids) {
        adapter.Attach([this](const runtime::ServerEvent& event) { recorder.Emit(event); });
        adapter.Start();
    }
};

}  // namespace

TEST_CASE("StepUsageRecord:扩展字段随流水落账,unknown usage 不写零不清身份") {
    runtime::TurnUsageStats stats;

    api::UsageReport first;
    first.step_index = 0;
    first.step_id = "step-1";
    first.turn_id = "turn-1";
    first.attempts = 2;
    first.api_duration_ms = 1234;
    first.stop_reason = "end_turn";
    first.usage.input_tokens = 100;
    first.usage.output_tokens = 50;
    first.reported_by_provider = true;
    stats.Add(first);

    // unknown usage:provider 没回 usage 帧——数字保持零,显式位 false,
    // 身份账(attempts/耗时)照记,不许拿零冒充实测。
    api::UsageReport second;
    second.step_index = 1;
    second.step_id = "step-2";
    second.turn_id = "turn-1";
    second.attempts = 1;
    second.api_duration_ms = 7;
    second.stop_reason = "end_turn";
    second.reported_by_provider = false;
    stats.Add(second);

    REQUIRE(stats.steps.size() == 2);
    CHECK(stats.steps[0].step_id == "step-1");
    CHECK(stats.steps[0].turn_id == "turn-1");
    CHECK(stats.steps[0].attempts == 2);
    CHECK(stats.steps[0].api_duration_ms == 1234);
    CHECK(stats.steps[0].stop_reason == "end_turn");
    CHECK(stats.steps[0].reported);
    CHECK(stats.steps[1].step_id == "step-2");
    CHECK(stats.steps[1].reported == false);
    CHECK(stats.steps[1].input_tokens == 0);  // unknown 不写零:没有实测就留零
                                               // 并靠 reported=false 说话,不猜
    // 汇总口径不被新字段扰动:仍从记录求和。
    CHECK(stats.request_count() == 2);
    CHECK(stats.input_tokens() == 100);
    CHECK(stats.output_tokens() == 50);
    CHECK(stats.any_reported());
}

TEST_CASE("StepUsageRecord:旧调用方不填新字段,缺省空/0,不造号") {
    runtime::TurnUsageStats stats;
    api::UsageReport legacy;
    legacy.step_index = 0;
    legacy.usage.input_tokens = 10;
    stats.Add(legacy);
    REQUIRE(stats.steps.size() == 1);
    CHECK(stats.steps[0].step_id.empty());
    CHECK(stats.steps[0].turn_id.empty());
    CHECK(stats.steps[0].attempts == 0);
    CHECK(stats.steps[0].api_duration_ms == 0);
    CHECK(stats.steps[0].stop_reason.empty());
}

TEST_CASE("step_id 跨 Run 单调:两次 Run 三只 Step,号不重不裂;turn_id 同一枚") {
    FakeBackend backend;
    backend.scripts = {
        TextOnlyScript("第一轮回答"),
        ToolUseScript("t-1", "fake_tool"),
        TextOnlyScript("第二轮第二拍"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("fake_tool", tools::Tool::Result{"跑完了", false}));
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    RecordedTurn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.turn_id = "turn-42";

    REQUIRE(loop.Run("第一问", wiring).has_value());
    REQUIRE(loop.Run("第二问", wiring).has_value());

    // 三只 Step:两次 Run 的展示坐标各自由 0 起(step_index 0 / 0 / 1——
    // 这正是"Run 局部序号重号是既有病"的形状),身份认 step_id:
    // step-1/2/3 恰好各一枚。
    REQUIRE(turn.recorder.lines.size() == 3);
    CHECK(turn.recorder.lines[0].step_index == 0);
    CHECK(turn.recorder.lines[1].step_index == 0);
    CHECK(turn.recorder.lines[2].step_index == 1);
    CHECK(turn.recorder.lines[0].step_id == "step-1");
    CHECK(turn.recorder.lines[1].step_id == "step-2");
    CHECK(turn.recorder.lines[2].step_id == "step-3");
    // 多次 Run 缝同一只 Turn:turn_id 全程同一枚。
    for (const auto& line : turn.recorder.lines) {
        CHECK(line.turn_id == "turn-42");
        CHECK(line.attempts == 1);  // 一次过(脚本不重试)
        CHECK(line.stop_reason == "end_turn");
        CHECK(line.api_duration_ms >= 0);
    }
}

TEST_CASE("wiring 不钉 turn_id:StepUsageRecord 侧如实留空,不现造号") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("好")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    RecordedTurn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;  // turn_id 缺省空

    REQUIRE(loop.Run("问", wiring).has_value());
    REQUIRE(turn.recorder.lines.size() == 1);
    CHECK(turn.recorder.lines[0].turn_id.empty());
    CHECK(turn.recorder.lines[0].step_id == "step-1");  // step 号与 turn 号分家
}

TEST_CASE("PreStep 否决:请求不出门,Run 按收场交账(不是错误,不跳过继续)") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("不该被看到")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    RecordedTurn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.on_pre_step_hook = [](const std::string& step_id, const std::string& turn_id, int step_index) {
        runtime::PromptGate gate;
        gate.blocked = true;
        gate.block_reason = "审计要求停";
        // 挂点收到了三件身份账(P1 的 Step 身份 + Run 内坐标)。
        CHECK(step_id == "step-1");
        CHECK(turn_id == "turn-7");
        CHECK(step_index == 0);
        return gate;
    };
    wiring.turn_id = "turn-7";

    const auto result = loop.Run("问", wiring);
    REQUIRE(result.has_value());              // 否决不是错误
    CHECK(result->steps_used == 0);           // 请求没发出,不算 Step
    CHECK(backend.calls == 0);                // 模型零请求
    CHECK(turn.recorder.lines.empty());       // 没有 usage 流水
}

TEST_CASE("PostStep 观察:响应落账后收到完整 UsageReport(含 API 耗时分账料)") {
    FakeBackend backend;
    backend.scripts = {TextOnlyScript("答")};
    tools::ToolRegistry registry;
    agent::Agent loop(backend, registry,
                      agent::AgentProfile{.request{.model = "test-model"}, .system_prompt = "system prompt"});

    RecordedTurn turn;
    agent::TurnWiring wiring;
    wiring.events = &turn.adapter;
    wiring.turn_id = "turn-9";
    std::vector<api::UsageReport> post_step_reports;
    wiring.on_post_step_hook = [&post_step_reports](const api::UsageReport& report) {
        post_step_reports.push_back(report);
    };

    REQUIRE(loop.Run("问", wiring).has_value());
    REQUIRE(post_step_reports.size() == 1);
    CHECK(post_step_reports[0].step_id == "step-1");
    CHECK(post_step_reports[0].turn_id == "turn-9");
    CHECK(post_step_reports[0].attempts == 1);
    CHECK(post_step_reports[0].api_duration_ms >= 0);
    CHECK(post_step_reports[0].stop_reason == "end_turn");
}
