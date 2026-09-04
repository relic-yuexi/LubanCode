// 任务级 Model Turn 预算(turn 预算单)的引擎层单测:AgentLoop 的准入三口
// (§13.3)、TurnHarness 的续投/Stop 钩子共账(§13.4)、AgentTool 派发路的
// 任务级收口。册子顶上另钉三枚"续投重领预算"缺口回归(§13.1/P0-0)——
// 它们钉的是 legacy per-run step 路的兼容窗现状:每开一次 Agent::Run()
// 计数从零起算,配置写的 12 可以跑成 36。P0-1/P0-2 的任务级 turn 账正是
// 治这个的;legacy 路在兼容窗内保持旧义不动(P1-0 兼容批再迁)。
//
// ---------------------------------------------------------------------------
// 旧 step 限制的迁移清单(P0-0 盘点,消费方逐处钉账;搬动前先对这份册)
// ---------------------------------------------------------------------------
// 写口(值从哪里来):
//   src/config/config.cpp        max_steps_per_turn / subagent.max_steps_
//                                per_turn(旧名 max_turns 双读)+ 本批新增
//                                subagent.default_max_turns(任务总 turn)
//   src/app/tool_runtime.cpp     子代理默认步数 -> AgentTool 构造参数;
//                                本批新增 SetDefaultMaxTurns
//   src/tools/agent_tool.cpp     ExecuteDispatch 入参双读(max_steps_per_
//                                turn / 旧 max_turns,均不出模型 schema)
//                                -> SubagentBudget.max_steps_per_turn ->
//                                快照 step_limit 与 profile.runtime
//   src/agent/agent_definition.cpp runtime.max_steps_per_turn(legacy)
//                                + 本批新增 runtime.max_turns
//   src/agent/agent_profile_resolver.cpp 步数三级合并(入参 > YAML > 默认)
//                                + 本批新增任务 turn 三级与收窄规则
//   src/workflow/parser.cpp / definition.cpp 节点 step_limit(legacy)
//                                + 本批新增 turn_limit(同现明拒)
//   src/app/wirings/workflow_wiring.cpp 节点 step_limit -> overrides
// 读口(账被谁消费):
//   src/agent/loop.cpp           for 循环条件(单 Run 硬闸)+ 将尽提醒
//   src/agent/turn_harness.cpp   DriveReport.steps_used 事后展示(不是硬闸)
//   src/tools/agent_tool.cpp     RunTask 收场分型 StepLimitExhausted +
//                                台账 steps_used 记账(on_round_settled)
//   src/tools/task_ledger.cpp    快照/摘要/outcome 的 step_limit/steps_used
//   src/app/agent_panel_presenter.cpp Dock 行与详情的步数展示
//   src/app/commands/agent_commands.cpp / doctor_commands.cpp / settings_
//   commands.cpp / goal_commands.cpp 命令侧读写
//   src/app/cli_app.cpp / src/app/turn_runner.cpp 主会话装配
//   src/app_server/schema.cpp / server.cpp 对外 schema 与状态投影
//   src/workflow/host_executors.cpp 节点 step_limit -> 落皮 + 预算收口
//   src/cli/i18n.cpp 步数文案
// 本批落的新账(不再依赖上面那批):TurnBudgetAccount/ModelTurnBudgetGate
// (engine)+ TaskLedger 的 turn_account(唯一真账)+ 快照/摘要/outcome 的
// turns_* 投影。展示与文案的全量换血归 P1-1。
#include <doctest/doctest.h>

#include <atomic>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.hpp"
#include "agent/loop.hpp"
#include "agent/turn_budget.hpp"
#include "agent/turn_harness.hpp"
#include "api/backend.hpp"
#include "api/types.hpp"
#include "tools/agent_tool.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"

using namespace lubancode;

namespace {

// 轨迹边界的记录仪替身(P1-1,§11.1):只记 sent 两口的调用序列,验证
// loop 在 permit 提交后走带 turn 账的口、主会话(没装门)走旧口。
class FakeBoundaryRecorder : public agent::LoopBoundaryRecorder {
public:
    struct Sent {
        std::string request_id;
        int task_turn_index = 0;   // 0 = 走的旧口(没有 turn 账)
        int turn_limit = 0;
        int input_round_index = 0;
    };
    std::vector<Sent> sent;
    int prepared_count = 0;

    std::string OnRequestPrepared(const api::Request&, const agent::RequestPreparedContext&) override {
        ++prepared_count;
        return "req-" + std::to_string(prepared_count);
    }
    void OnRequestSent(const std::string& request_id) override { sent.push_back({request_id, 0, 0, 0}); }
    void OnRequestSentWithTurn(const std::string& request_id, int task_turn_index, int turn_limit,
                               int input_round_index) override {
        sent.push_back({request_id, task_turn_index, turn_limit, input_round_index});
    }
    void OnUsageRecorded(const std::string&, const api::Usage&, bool, const std::string&, int, bool,
                         bool) override {}
    bool OnOutputCompleted(const std::string&, const api::Message&, const std::string&,
                           const std::string&) override {
        return true;
    }
    void OnOutputFailed(const std::string&, const std::string&) override {}
    void OnOutputCancelled(const std::string&) override {}
};

// 按脚本吐事件的假后端(与 test_turn_harness.cpp 同款):每调一次
// send_stream 取下一组脚本,顺带记请求,方便对账。
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

std::vector<api::StreamEvent> TextScript(const std::string& text) {
    return {
        api::MessageStart{"msg", "model"},
        api::TextDelta{text},
        api::ContentBlockDone{0},
        api::MessageDone{"end_turn", api::Usage{}},
    };
}

std::vector<api::StreamEvent> ToolUseScript(const std::string& tool_id) {
    return {
        api::MessageStart{"msg", "model"},
        api::ToolUseStart{0, tool_id, "fake_tool"},
        api::ToolUseInputDelta{0, "{}"},
        api::ContentBlockDone{0},
        api::MessageDone{"tool_use", api::Usage{}},
    };
}

// 一条 assistant 里并行 N 枚工具调用(§13.3"并行三工具只算一 turn"的靶子)。
std::vector<api::StreamEvent> ParallelToolUseScript(const std::vector<std::string>& tool_ids) {
    std::vector<api::StreamEvent> events;
    events.push_back(api::MessageStart{"msg", "model"});
    for (std::size_t i = 0; i < tool_ids.size(); ++i) {
        events.push_back(api::ToolUseStart{static_cast<int>(i), tool_ids[i], "fake_tool"});
        events.push_back(api::ToolUseInputDelta{static_cast<int>(i), "{}"});
        events.push_back(api::ContentBlockDone{static_cast<int>(i)});
    }
    events.push_back(api::MessageDone{"tool_use", api::Usage{}});
    return events;
}

api::Message UserText(const std::string& text) {
    api::Message message;
    message.role = api::Role::User;
    message.content.push_back(api::TextBlock{text});
    return message;
}

int CountNudgeBlocks(const agent::Agent& loop) {
    int count = 0;
    for (const auto& message : loop.history()) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<api::TextBlock>(&block);
                text != nullptr && text->text.find("已进入轮数上限前的收尾区") != std::string::npos) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// P0-0:冻结现状缺口(§13.1)——legacy per-run step 路的兼容窗现状。
// 没装任务级 turn 门时,预算只管单次 Run;这是钉死现状的回归,不是榜样。
// ---------------------------------------------------------------------------

TEST_CASE("缺口回归:legacy max_steps_per_turn=2,续投轮可再跑 2——预算按 Run 重置(兼容窗现状)") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1"),
        TextScript("第一轮结论。"),   // Run A:2 步,正常收口
        ToolUseScript("t2"),
        TextScript("第二轮结论。"),   // Run B:又是 2 步
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    profile.runtime.max_steps_per_turn = 2;  // legacy:每个 input round 2 步
    agent::Agent loop(backend, registry, std::move(profile));

    int continuations = 0;
    agent::DriveOptions options;
    options.continuation = [&continuations]() -> std::optional<agent::ContinuationBatch> {
        if (continuations >= 1) {
            return std::nullopt;
        }
        ++continuations;
        agent::ContinuationBatch batch;
        batch.input = "mailbox 增量";
        return batch;
    };
    const agent::DriveReport report = agent::DriveTurn(loop, agent::TurnWiring{}, UserText("干活"), options);

    // 现状:配置写 2,任务跑 4。任务级 turn 账(P0-1/P0-2)装门后才有总闸;
    // legacy 路保持旧义,P1-0 兼容批再迁。
    CHECK(report.ok);
    CHECK(report.steps_used == 4);
    CHECK(backend.captured_requests.size() == 4);
    // 展示累计(4)与硬闸作用域(2)打架——单子 §2.2 的病灶,如实钉死。
    CHECK(report.steps_used > 2);
}

TEST_CASE("缺口回归:初始 Run 正常收口后,Stop 钩子续跑重新拿整份 step 上限(兼容窗现状)") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1"),
        TextScript("首轮结论。"),   // 初始 Run:2 步
        ToolUseScript("t2"),
        TextScript("续跑结论。"),   // Stop 续跑轮:又是 2 步
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    profile.runtime.max_steps_per_turn = 2;
    agent::Agent loop(backend, registry, std::move(profile));

    agent::DriveReport report = agent::DriveTurn(loop, agent::TurnWiring{}, UserText("干活"), agent::DriveOptions{});
    REQUIRE(report.ok);
    int emits = 0;
    agent::StopOptions stop_options;
    stop_options.emit = [&emits](bool stop_hook_active, const std::string&) {
        ++emits;
        hooks::HookEventResult merged;
        merged.blocked = !stop_hook_active;  // 头一回拉闸,续过一次就放行
        merged.block_reason = "再看一眼";
        return merged;
    };
    agent::RunStopContinuation(loop, agent::TurnWiring{}, stop_options, report);
    CHECK(emits == 2);
    // 现状:钩子续跑不吃初始轮的账,2+2=4——Stop hook 是预算豁免口(单子
    // §2.3 的病灶)。任务级 turn 门装上后共账,见下面的新账用例。
    CHECK(report.steps_used == 4);
    CHECK(backend.captured_requests.size() == 4);
}

// ---------------------------------------------------------------------------
// §13.3:AgentLoop 的准入三口。
// ---------------------------------------------------------------------------

TEST_CASE("turn 账:一条 assistant 并行三工具只算一 turn,工具结果后的下一请求再耗一枚") {
    FakeBackend backend;
    backend.scripts = {
        ParallelToolUseScript({"t1", "t2", "t3"}),
        TextScript("收工。"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    agent::TurnBudgetAccount account(2);
    agent::ModelTurnBudgetGate gate = agent::MakeLocalTurnBudgetGate(&account);
    agent::TurnWiring wiring;
    wiring.turn_budget = &gate;

    const auto outcome = loop.Run(UserText("并行干活"), wiring);
    REQUIRE(outcome.has_value());
    CHECK_FALSE(outcome->hit_turn_limit);

    const agent::ModelTurnBudgetSnapshot snapshot = account.SnapshotLock();
    CHECK(snapshot.attempted == 2);   // 两枚逻辑请求
    CHECK(snapshot.completed == 2);
    CHECK(snapshot.reserved == 0);    // 正常收场归零
    CHECK(backend.captured_requests.size() == 2);
    // 三枚工具各自入账(三条 tool_result),但不多扣 turn。
    int tool_results = 0;
    for (const auto& message : loop.history()) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolResultBlock>(block)) {
                ++tool_results;
            }
        }
    }
    CHECK(tool_results == 3);
}

TEST_CASE("turn 账:最后一枚返回 ToolUse,工具照常执行;其后不得再发模型请求") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1"),
        TextScript("永远到不了这里。"),
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    agent::TurnBudgetAccount account(1);  // 只有一枚
    agent::ModelTurnBudgetGate gate = agent::MakeLocalTurnBudgetGate(&account);
    agent::TurnWiring wiring;
    wiring.turn_budget = &gate;

    const auto outcome = loop.Run(UserText("干一步"), wiring);
    REQUIRE(outcome.has_value());
    CHECK(outcome->hit_turn_limit);  // 工具跑完后下一枚被拒

    // 第 2 枚逻辑请求确实没发:backend 只见过 1 次。
    CHECK(backend.captured_requests.size() == 1);
    const agent::ModelTurnBudgetSnapshot snapshot = account.SnapshotLock();
    CHECK(snapshot.attempted == 1);
    CHECK(snapshot.completed == 1);
    CHECK(snapshot.reserved == 0);
    // 工具结果与部分结论不丢:tool_result 已入 history。
    bool has_tool_result = false;
    for (const auto& message : loop.history()) {
        for (const auto& block : message.content) {
            if (std::holds_alternative<api::ToolResultBlock>(block)) {
                has_tool_result = true;
            }
        }
    }
    CHECK(has_tool_result);
}

TEST_CASE("turn 账:请求发出前 denied,backend 捕获请求数不增加") {
    FakeBackend backend;
    backend.scripts = {TextScript("到不了。")};
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    agent::TurnBudgetAccount account(1);
    agent::ModelTurnBudgetGate gate = agent::MakeLocalTurnBudgetGate(&account);
    agent::TurnWiring wiring;
    wiring.turn_budget = &gate;

    // 头一枚先耗掉(文本一步)。
    REQUIRE(loop.Run(UserText("先用掉唯一额度"), wiring).has_value());
    const std::size_t requests_after_first = backend.captured_requests.size();
    REQUIRE(requests_after_first == 1);

    // 第二次 Run:准入被拒,零 backend 调用,history 原样。
    const auto denied = loop.Run(UserText("还想再跑"), wiring);
    REQUIRE(denied.has_value());
    CHECK(denied->hit_turn_limit);
    CHECK(backend.captured_requests.size() == 1);  // 没涨
}

TEST_CASE("turn 账:接口报错 attempted=1、completed=0——失败请求可能花钱,不当没发生") {
    FakeBackend backend;
    backend.scripts = {};  // 第一次请求即失败
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    agent::TurnBudgetAccount account(3);
    agent::ModelTurnBudgetGate gate = agent::MakeLocalTurnBudgetGate(&account);
    agent::TurnWiring wiring;
    wiring.turn_budget = &gate;

    const auto outcome = loop.Run(UserText("注定失败"), wiring);
    CHECK_FALSE(outcome.has_value());
    const agent::ModelTurnBudgetSnapshot snapshot = account.SnapshotLock();
    CHECK(snapshot.attempted == 1);
    CHECK(snapshot.completed == 0);
    CHECK(snapshot.reserved == 0);
}

TEST_CASE("turn 账:完整正文 attempted=1、completed=1") {
    FakeBackend backend;
    backend.scripts = {TextScript("结论。")};
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    agent::TurnBudgetAccount account(3);
    agent::ModelTurnBudgetGate gate = agent::MakeLocalTurnBudgetGate(&account);
    agent::TurnWiring wiring;
    wiring.turn_budget = &gate;

    REQUIRE(loop.Run(UserText("问一句"), wiring).has_value());
    const agent::ModelTurnBudgetSnapshot snapshot = account.SnapshotLock();
    CHECK(snapshot.attempted == 1);
    CHECK(snapshot.completed == 1);
    CHECK(snapshot.reserved == 0);
}

TEST_CASE("turn 账:将尽提示全任务只注入一次,跨 Run 不重发") {
    FakeBackend backend;
    backend.scripts = {
        TextScript("第一轮。"),
        TextScript("第二轮。"),
    };
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    agent::TurnBudgetAccount account(3);  // 剩 3 == min(3,3):第一步就提醒
    agent::ModelTurnBudgetGate gate = agent::MakeLocalTurnBudgetGate(&account);
    agent::TurnWiring wiring;
    wiring.turn_budget = &gate;

    REQUIRE(loop.Run(UserText("第一问"), wiring).has_value());
    CHECK(CountNudgeBlocks(loop) == 1);
    REQUIRE(loop.Run(UserText("第二问"), wiring).has_value());
    CHECK(CountNudgeBlocks(loop) == 1);  // 不重发:剩余 turn 不烧在念叨上
    REQUIRE(account.SnapshotLock().attempted == 2);
}

// ---------------------------------------------------------------------------
// §13.4:TurnHarness 续投与 Stop 钩子共账。
// ---------------------------------------------------------------------------

namespace {

// 装好门与轮间闸的装配(共账用例共用):账户、门、接了门的 TurnWiring、
// 带轮间闸的 DriveOptions 一套。
struct GatedDrive {
    agent::TurnBudgetAccount account;
    agent::ModelTurnBudgetGate gate;
    agent::TurnWiring wiring;
    agent::DriveOptions options;
    explicit GatedDrive(int limit) : account(limit), gate(agent::MakeLocalTurnBudgetGate(&account)) {
        wiring.turn_budget = &gate;
        options.turn_budget_exhausted = [this]() { return account.SnapshotLock().Exhausted(); };
        options.turn_budget_snapshot = [this]() { return account.SnapshotLock(); };
    }
};

}  // namespace

TEST_CASE("共账:初始 Run 与 mailbox 续投共用 max_turns=3,累计第三次后封门") {
    FakeBackend backend;
    backend.scripts = {
        TextScript("一。"),
        TextScript("二。"),
        TextScript("三。"),
        TextScript("四——到不了。"),
    };
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    GatedDrive gated(3);
    int continuations = 0;
    gated.options.continuation = [&continuations]() -> std::optional<agent::ContinuationBatch> {
        ++continuations;
        agent::ContinuationBatch batch;
        batch.input = "mailbox 增量 " + std::to_string(continuations);
        return batch;
    };
    const agent::DriveReport report =
        agent::DriveTurn(loop, gated.wiring, UserText("干活"), gated.options);

    CHECK(report.ok);
    CHECK(report.hit_turn_limit);
    CHECK(backend.captured_requests.size() == 3);  // 第 4 枚逻辑请求未发
    CHECK(report.steps_used == 3);
    REQUIRE(report.turn_budget.has_value());
    CHECK(report.turn_budget->attempted == 3);
    CHECK(report.turn_budget->limit == 3);
    CHECK(gated.account.SnapshotLock().attempted == 3);  // 与真账逐笔一致
    CHECK(gated.account.SnapshotLock().reserved == 0);
    // 第三轮收口后额度已尽:第三次领到的批次不带去开新一轮(送不出,
    // 退回未送),请求停在 3 枚。
    CHECK(continuations == 3);
}

TEST_CASE("共账:额度在轮边界耗尽时,领到的批次退回未送——不确认送达") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1"),
        TextScript("结论。"),  // Run:恰好 2 步用满额度
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    GatedDrive gated(2);
    int restores = 0;
    gated.options.continuation = [&restores]() -> std::optional<agent::ContinuationBatch> {
        agent::ContinuationBatch batch;
        batch.input = "child completion 批次";
        batch.restore = [&restores]() { ++restores; };
        return batch;
    };
    const agent::DriveReport report =
        agent::DriveTurn(loop, gated.wiring, UserText("干活"), gated.options);

    CHECK(report.ok);
    CHECK(report.hit_turn_limit);
    CHECK(restores == 1);  // 批次退回未送(§7.3"取走了不等于送到了")
    CHECK(backend.captured_requests.size() == 2);
}

TEST_CASE("共账:末枚额度上交出结论、又无续投活儿,按自然完成收口——不误报耗尽") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1"),
        TextScript("结论:恰好用满。"),  // Run:2 步用满,末步交出正文结论
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    GatedDrive gated(2);
    gated.options.continuation = []() -> std::optional<agent::ContinuationBatch> {
        return std::nullopt;  // 封账:mailbox 空、无活孩子
    };
    const agent::DriveReport report =
        agent::DriveTurn(loop, gated.wiring, UserText("干活"), gated.options);

    CHECK(report.ok);
    CHECK_FALSE(report.hit_turn_limit);  // 自然完成,不是预算耗尽
    CHECK(gated.account.SnapshotLock().attempted == 2);
    CHECK(gated.account.SnapshotLock().completed == 2);
}

TEST_CASE("共账:领了批的那轮撞门,inflight 批次退回未送(取走了不等于送到了)") {
    FakeBackend backend;
    backend.scripts = {
        TextScript("一。"),          // 初始 Run:1 步
        ToolUseScript("t1"),         // 续投 Run:第 2 步(最后一枚)返回工具
        TextScript("到不了。"),      // 第 3 枚被拒
    };
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>());
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    GatedDrive gated(2);
    int restores = 0;
    gated.options.continuation = [&restores]() -> std::optional<agent::ContinuationBatch> {
        agent::ContinuationBatch batch;
        batch.input = "父消息批次";
        batch.restore = [&restores]() { ++restores; };
        return batch;
    };
    const agent::DriveReport report =
        agent::DriveTurn(loop, gated.wiring, UserText("干活"), gated.options);

    CHECK(report.hit_turn_limit);
    CHECK(restores == 1);  // 批次退回未送
    CHECK(backend.captured_requests.size() == 2);
    CHECK(gated.account.SnapshotLock().attempted == 2);
}

TEST_CASE("共账:Stop 钩子续跑吃剩余额度不重置;remaining=0 时 backend 零调用") {
    FakeBackend backend;
    backend.scripts = {
        TextScript("结论。"),  // 唯一一枚额度
    };
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    GatedDrive gated(1);
    agent::DriveReport report = agent::DriveTurn(loop, gated.wiring, UserText("干活"), gated.options);
    REQUIRE(report.ok);
    // 没有续投源、也没挂起活儿:末枚额度上交出结论,按自然完成收口——
    // 不误报耗尽(边界判定先问有没有活儿,再问额度)。
    CHECK_FALSE(report.hit_turn_limit);
    REQUIRE(backend.captured_requests.size() == 1);

    // Stop 钩子拉闸要求续跑:额度已尽——钩子可留阻断理由,但宿主拿不到
    // 新 permit,一发请求都不多打(§7.5);报告按 TurnLimit 如实翻账。
    int emits = 0;
    std::string block_reason_seen;
    agent::StopOptions stop_options;
    stop_options.emit = [&emits, &block_reason_seen](bool stop_hook_active, const std::string&) {
        ++emits;
        hooks::HookEventResult merged;
        merged.blocked = !stop_hook_active;
        merged.block_reason = "还有遗漏";
        block_reason_seen = merged.block_reason;
        return merged;
    };
    stop_options.final_text = []() { return std::string("结论。"); };
    agent::RunStopContinuation(loop, gated.wiring, stop_options, report);
    CHECK(emits >= 1);                       // 钩子被调了,理由留了痕
    CHECK(block_reason_seen == "还有遗漏");
    CHECK(report.hit_turn_limit);            // 续跑被门拦:翻成 turn limit
    CHECK(backend.captured_requests.size() == 1);  // backend 零调用
    CHECK(gated.account.SnapshotLock().attempted == 1);
}

TEST_CASE("共账:Stop 钩子在额度尚余时可以续跑一轮,消耗同一本账") {
    FakeBackend backend;
    backend.scripts = {
        TextScript("首轮。"),
        TextScript("续跑轮。"),
    };
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    GatedDrive gated(2);  // 首轮 1 枚,续跑轮再 1 枚,恰用满
    agent::TurnWiring wiring;
    wiring.turn_budget = &gated.gate;
    agent::DriveReport report = agent::DriveTurn(loop, wiring, UserText("干活"), gated.options);
    REQUIRE(report.ok);
    REQUIRE(backend.captured_requests.size() == 1);

    agent::StopOptions stop_options;
    stop_options.emit = [](bool stop_hook_active, const std::string&) {
        hooks::HookEventResult merged;
        merged.blocked = !stop_hook_active;
        merged.block_reason = "再收口一轮";
        return merged;
    };
    stop_options.final_text = []() { return std::string("首轮。"); };
    agent::RunStopContinuation(loop, wiring, stop_options, report);
    CHECK(backend.captured_requests.size() == 2);  // 续跑轮真发了 1 枚
    CHECK(gated.account.SnapshotLock().attempted == 2);
    CHECK(gated.account.SnapshotLock().completed == 2);
    CHECK(gated.account.SnapshotLock().reserved == 0);
}

TEST_CASE("分型:turn_budget_exhausted -> BudgetExhausted/TurnLimit,先于 legacy 步数闸") {
    agent::TurnEndgame end;
    end.turn_budget_exhausted = true;
    end.hit_step_limit = true;
    const agent::TurnVerdict verdict = agent::ClassifyTurnEnd(end);
    CHECK(verdict.status == agent::TurnVerdict::Status::BudgetExhausted);
    CHECK(verdict.reason == agent::TurnVerdict::Reason::TurnLimit);

    agent::TurnEndgame only_step;
    only_step.hit_step_limit = true;
    CHECK(agent::ClassifyTurnEnd(only_step).reason == agent::TurnVerdict::Reason::StepLimit);
}

// ---------------------------------------------------------------------------
// AgentTool 派发路:任务级预算从注册冻到收口,台账与 outcome 投影。
// ---------------------------------------------------------------------------

TEST_CASE("AgentTool:任务总 turn 2 封门——第 3 枚逻辑请求不发,收口带部分结果与 turn 账") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1"),
        ToolUseScript("t2"),
        TextScript("到不了。"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>());
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");
    agent_tool.SetDefaultMaxTurns(2);

    const tools::Tool::Result result = agent_tool.execute({
        {"title", "复核迁移"},
        {"prompt", "两步调查后交结论。"},
    });
    CHECK(result.is_error);
    CHECK(result.content.find("turn 预算已用满") != std::string::npos);
    CHECK(result.content.find("2/2") != std::string::npos);

    // backend 只见过 2 次请求;工具结果都入了账。
    CHECK(backend.captured_requests.size() == 2);
    // 台账:任务 turn 账与 outcome 的分型。
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    const tools::AgentTaskSnapshot& snapshot = snapshots.front();
    CHECK(snapshot.turn_limit == 2);
    CHECK(snapshot.turns_attempted == 2);
    CHECK(snapshot.turns_completed == 2);
    CHECK(snapshot.turns_reserved == 0);
    CHECK(snapshot.outcome.reason == tools::TaskOutcomeReason::TurnLimitExhausted);
    CHECK(snapshot.outcome.status == tools::TaskOutcomeStatus::BudgetExhausted);
    CHECK(snapshot.outcome.turns_attempted == 2);
    CHECK(snapshot.outcome.turns_completed == 2);
    // 两枚工具调用都落了账(部分结果不丢)。
    REQUIRE(snapshot.tool_calls.size() == 2);
    CHECK(snapshot.tool_calls[0].done);
    CHECK(snapshot.tool_calls[1].done);
}

TEST_CASE("AgentTool:不设任务总 turn(SetDefaultMaxTurns 缺省 0)时行为与从前一字不差") {
    FakeBackend backend;
    backend.scripts = {
        ToolUseScript("t1"),
        ToolUseScript("t2"),
        TextScript("结论:三步跑完。"),
    };
    tools::ToolRegistry sub_registry;
    sub_registry.Register(std::make_unique<FakeTool>());
    tools::AgentTool agent_tool(backend, sub_registry, "/work/dir");

    const tools::Tool::Result result = agent_tool.execute({
        {"title", "不限额任务"},
        {"prompt", "三步调查。"},
    });
    CHECK_FALSE(result.is_error);
    CHECK(backend.captured_requests.size() == 3);
    const auto snapshots = agent_tool.TaskSnapshots();
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots.front().turn_limit == 0);  // 0 = 不设任务总帽,不拦
    CHECK(snapshots.front().turns_attempted == 3);
}

// ---------------------------------------------------------------------------
// P1-1(§11.1):轨迹 sent 边界的任务 turn 账。装了门的会话走
// OnRequestSentWithTurn(index 从 commit 来,limit/input round 随行);没装门
// 的会话走旧口 OnRequestSent,一字不差。
// ---------------------------------------------------------------------------

TEST_CASE("轨迹边界:装了 turn 门的请求走带账 sent 口,index 逐枚对上 commit") {
    FakeBackend backend;
    backend.scripts = {
        TextScript("一。"),
        TextScript("二。"),
    };
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    agent::TurnBudgetAccount account(2);
    agent::ModelTurnBudgetGate gate = agent::MakeLocalTurnBudgetGate(&account);
    agent::TurnWiring wiring;
    wiring.turn_budget = &gate;
    FakeBoundaryRecorder recorder;
    wiring.boundary_recorder = &recorder;

    REQUIRE(loop.Run(UserText("第一步"), wiring).has_value());
    REQUIRE(loop.Run(UserText("第二步,恰好用满"), wiring).has_value());
    REQUIRE(loop.Run(UserText("封门后的第三步"), wiring)->hit_turn_limit);

    REQUIRE(recorder.sent.size() == 2);  // 第三枚被拒,连 prepared 都没有
    CHECK(recorder.sent[0].task_turn_index == 1);
    CHECK(recorder.sent[0].turn_limit == 2);
    CHECK(recorder.sent[0].input_round_index == 0);
    CHECK(recorder.sent[1].task_turn_index == 2);
    CHECK(recorder.sent[1].turn_limit == 2);
}

TEST_CASE("轨迹边界:没装门的会话走旧口 OnRequestSent,一字不差") {
    FakeBackend backend;
    backend.scripts = {TextScript("结论。")};
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    agent::TurnWiring wiring;
    FakeBoundaryRecorder recorder;
    wiring.boundary_recorder = &recorder;

    REQUIRE(loop.Run(UserText("问一句"), wiring).has_value());
    REQUIRE(recorder.sent.size() == 1);
    CHECK(recorder.sent[0].task_turn_index == 0);  // 旧口:没有 turn 账
}

TEST_CASE("轨迹边界:DriveTurn 的续投轮把 input_round_index 递进") {
    FakeBackend backend;
    backend.scripts = {
        TextScript("一。"),
        TextScript("二。"),
    };
    tools::ToolRegistry registry;
    agent::AgentProfile profile;
    profile.request.model = "test-model";
    profile.system_prompt = "system";
    agent::Agent loop(backend, registry, std::move(profile));

    agent::TurnBudgetAccount account(0);  // 不限:只为给 sent 带账
    agent::ModelTurnBudgetGate gate = agent::MakeLocalTurnBudgetGate(&account);
    agent::TurnWiring wiring;
    wiring.turn_budget = &gate;
    FakeBoundaryRecorder recorder;
    wiring.boundary_recorder = &recorder;

    int continuations = 0;
    agent::DriveOptions options;
    options.continuation = [&continuations]() -> std::optional<agent::ContinuationBatch> {
        if (continuations >= 1) {
            return std::nullopt;
        }
        ++continuations;
        agent::ContinuationBatch batch;
        batch.input = "mailbox 增量";
        return batch;
    };
    const agent::DriveReport report = agent::DriveTurn(loop, wiring, UserText("干活"), options);
    CHECK(report.ok);

    // 初始轮 input_round_index=0,续投轮=1——两枚 sent 各带各的坐标。
    REQUIRE(recorder.sent.size() == 2);
    CHECK(recorder.sent[0].input_round_index == 0);
    CHECK(recorder.sent[1].input_round_index == 1);
}
