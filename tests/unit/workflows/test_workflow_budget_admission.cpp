// AR-04(Workflow 控制节点绕过全局预算入口)合同册。
//
// 病灶(审查单 2026-09-21):map/reduce 逐项 RunNode 不记全局步数;
// parallel 各分支自数 guard 顶替全局预算;控制节点在主循环预算闸前
// continue,绕开 tokens/tool_calls/timeout 准入;成功工具调用不计数,
// 失败的非工具节点反倒冒充一次工具调用。
//
// 合同:所有真实节点 attempt 的准入收进同一本运行预算账——预留步数
// (重试也计)、查总时限、Tool 身份预留工具调用、对账已累计 tokens。
// foreach/map/parallel/reduce/async/重试全走 RunNode 的预算准入口。
// 夹具用 fake executor/fake clock;唯一真等待是 async 时限监察的
// 20ms 轮询(被测机制本身),其余栅栏不靠 sleep 赌时序。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

namespace {

// fake clock:手动拨。
class FakeClock : public lubancode::workflow::JournalClock {
public:
    std::int64_t now_ms_ = 1000000;
    std::int64_t NowMs() const override { return now_ms_; }
    void Advance(std::int64_t ms) { now_ms_ += ms; }
};

// 记账 executor:按脚本走(耗尽重复最后一格),调用数原子计数——
// map 并发项也数得准。
class CountingExecutor : public lubancode::workflow::NodeExecutor {
public:
    struct Step {
        bool ok = true;
        std::string error_code;
        std::int64_t tokens = 0;
        nlohmann::json output = nlohmann::json::object();
    };
    std::vector<Step> script;
    std::atomic<int> calls{0};

    lubancode::workflow::NodeExecResult Execute(
        const lubancode::workflow::NodeExecRequest& request) override {
        ++calls;
        lubancode::workflow::NodeExecResult result;
        const std::size_t index = script.empty() ? 0 : std::min(static_cast<std::size_t>(calls.load()) - 1,
                                                                script.size() - 1);
        const Step& step = script.empty() ? Step{} : script[index];
        result.ok = step.ok;
        result.error_code = step.error_code;
        result.output = step.output;
        result.tokens_used = step.tokens;
        // 时限册用:只拨 map/foreach 逐项注入的 item 输入(容器自己与
        // setup 节点不拨),把"第一项跑完、第二项开跑前"钉在钟上。
        if (clock_for_time_travel != nullptr && request.resolved_input.is_object() &&
            request.resolved_input.contains("item")) {
            clock_for_time_travel->Advance(clock_advance_ms);
        }
        return result;
    }

    // 时限册用:item 输入的执行把 fake clock 拨过线。
    FakeClock* clock_for_time_travel = nullptr;
    std::int64_t clock_advance_ms = 0;
};

// async 时限册的 body:先拨钟过线,再等取消(总时限监察会把 cancel
// 递进来;等的是被测机制自己的动作,不是赌 sleep)。
class StallExecutor : public lubancode::workflow::NodeExecutor {
public:
    lubancode::workflow::NodeExecResult Execute(
        const lubancode::workflow::NodeExecRequest& request) override {
        ++calls;
        if (clock != nullptr) clock->Advance(advance_ms);
        lubancode::workflow::NodeExecResult result;
        if (request.cancel != nullptr) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!request.cancel->load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        result.ok = true;
        result.output = nlohmann::json{{"done", true}};
        return result;
    }

    std::atomic<int> calls{0};
    FakeClock* clock = nullptr;
    std::int64_t advance_ms = 0;
};

lubancode::workflow::WorkflowDefinition ParseOrDie(const char* yaml) {
    auto parsed = lubancode::workflow::ParseWorkflowYaml(yaml);
    if (!parsed.has_value()) {
        for (const auto& issue : parsed.error()) {
            MESSAGE("parse issue: ", issue.location, ": ", issue.message);
        }
    }
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_SUITE("workflows-budget-admission") {

// 病灶钉:foreach 第一项 tokens 越帽,第二项不得再执行。
TEST_CASE("foreach:第一项用量越帽后不再执行第二项") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: foreach-token-cap
version: 1.0.0
entry: setup
limits:
  tokens: 100
nodes:
  setup:
    type: transform
    operation: make_list
  walk:
    type: foreach
    items: "${nodes.setup.output.items}"
    body: work
  work:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: setup, on: success, to: walk }
  - { from: walk, on: success, to: fin }
)YAML");

    auto executor = std::make_shared<CountingExecutor>();
    // 第一格给 setup(零费,产两项数组);往后重复最后一格——每项 work
    // 报 150 tokens(帽 100):第一项落账后越帽。
    executor->script = {
        {true, "", 0, nlohmann::json{{"items", nlohmann::json::array({"a", "b"})}}},
        {true, "", 150, nlohmann::json{{"done", true}}}};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);

    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::BudgetExhausted);
    CHECK(summary.error_code == "budget_exhausted");
    CHECK(summary.tokens_used == 150);  // 第一项的实际用量如实入账
    // 一项不多:第二项在准入口被拦(setup 1 + 第一项 1,共 2 次执行)。
    CHECK(executor->calls.load() == 2);
    // 拒绝事实落进节点账(同一 body 槽,last write wins 是拒绝那笔)。
    CHECK(summary.nodes.at("walk").state == NodeState::Failed);
    CHECK(summary.nodes.at("walk").error_code == "budget_exhausted");
}

// 病灶钉:map 并发逐项 RunNode 从不记全局步数——长数组能在单并发下
// 持续越预算。合同:max_steps=4 罩住"setup + 容器访问 + 2 次项
// attempt",第 3 项起拒绝派发。
TEST_CASE("map:并发只派发已获额度的项(max_steps 罩展开)") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: map-step-cap
version: 1.0.0
entry: setup
limits:
  max_steps: 4
  max_concurrency: 4
nodes:
  setup:
    type: transform
    operation: make_list
  fan:
    type: map
    items: "${nodes.setup.output.items}"
    body: work
    max_concurrency: 4
  work:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: setup, on: success, to: fan }
  - { from: fan, on: success, to: fin }
)YAML");

    auto executor = std::make_shared<CountingExecutor>();
    executor->script = {
        {true, "", 0, nlohmann::json{{"items", nlohmann::json::array({0, 1, 2, 3, 4, 5})}}}};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);

    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::BudgetExhausted);
    CHECK(summary.error_code == "max_steps");
    // 步账:setup(1)+ map 容器访问(1)+ 两项 attempt(2)= 4;第 3 项
    // 在准入口被拦。执行器只见 setup 与两项(setup 1 + 项 2 = 3 次)。
    CHECK(executor->calls.load() == 3);
    CHECK(summary.nodes.at("fan").state == NodeState::Failed);
    CHECK(summary.nodes.at("fan").error_code == "max_steps");
}

// 病灶钉:parallel 旧实现各分支自数 guard,不进全局账。合同:分支链上
// 每个真实节点 attempt 都过全局准入口,guard 删除后一把尺罩到底。
TEST_CASE("parallel:分支只派发已获额度的步数(全局帽,非分支自数)") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: parallel-step-cap
version: 1.0.0
entry: gate
limits:
  max_steps: 4
  max_concurrency: 4
nodes:
  gate:
    type: parallel
    branches: [b0, b1, b2, b3, b4, b5]
    join: all_settled
    max_concurrency: 4
  b0: { type: transform, operation: fetch }
  b1: { type: transform, operation: fetch }
  b2: { type: transform, operation: fetch }
  b3: { type: transform, operation: fetch }
  b4: { type: transform, operation: fetch }
  b5: { type: transform, operation: fetch }
  fin:
    type: end
edges:
  - { from: gate, on: success, to: fin }
)YAML");

    auto executor = std::make_shared<CountingExecutor>();
    executor->script = {{true, "", 0, nlohmann::json{{"n", 1}}}};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);

    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::BudgetExhausted);
    CHECK(summary.error_code == "max_steps");
    // parallel 访问(1)+ 3 条获额度分支(3)= 4;第 4 条分支拒派。
    CHECK(executor->calls.load() == 3);
    CHECK(summary.nodes.at("gate").state == NodeState::Failed);
    CHECK(summary.nodes.at("gate").error_code == "max_steps");
}

// 病灶钉:工具计数 "!result.ok 才 +1"——成功的 Tool 不计数,失败的
// LLM 冒充工具调用。合同:Tool 节点 attempt 预留计数(成功/失败/重试
// 都算);非 Tool 节点不动 tool_calls;帽满(Headroom)即拦。
TEST_CASE("tool_calls:成功 Tool attempt 也计数,失败 LLM 不冒充") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: tool-count
version: 1.0.0
entry: t1
limits:
  tool_calls: 2
nodes:
  t1:
    type: tool
    tool: demo.reader
  t2:
    type: tool
    tool: demo.reader
  boom:
    type: llm
    prompt: prompts/boom.md
  t3:
    type: tool
    tool: demo.reader
  fin:
    type: end
edges:
  - { from: t1, on: success, to: t2 }
  - { from: t2, on: success, to: boom }
  - { from: boom, on: error, to: t3 }
  - { from: t3, on: success, to: fin }
)YAML");

    auto tool = std::make_shared<CountingExecutor>();
    tool->script = {{true, "", 0, nlohmann::json{{"read", true}}}};
    auto llm = std::make_shared<CountingExecutor>();
    llm->script = {{false, "model_error"}};
    RuntimeOptions options;
    options.executors[NodeKind::Tool] = tool;
    options.executors[NodeKind::Llm] = llm;
    WorkflowRuntime runtime(options);

    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::BudgetExhausted);
    CHECK(summary.error_code == "budget_exhausted");
    // 两次成功 Tool attempt 都入账;LLM 失败没冒充;第三次 Tool 在
    // 准入口被拦(帽 2 满),计数不虚增。
    CHECK(summary.tool_calls == 2);
    CHECK(tool->calls.load() == 2);
    CHECK(llm->calls.load() == 1);
    CHECK(summary.nodes.at("t3").error_code == "budget_exhausted");
}

// 病灶钉:reduce 逐项 RunNode 从不记步数。合同:reduce 每项 attempt
// 消耗步数——max_steps=4 罩住"setup + 容器访问 + 2 项",第 3 项拒跑。
TEST_CASE("reduce:每项消耗步数,越帽不再汇下一项") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: reduce-step-cap
version: 1.0.0
entry: setup
limits:
  max_steps: 4
nodes:
  setup:
    type: transform
    operation: make_list
  total:
    type: reduce
    items: "${nodes.setup.output.counts}"
    body: add
  add:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: setup, on: success, to: total }
  - { from: total, on: success, to: fin }
)YAML");

    auto executor = std::make_shared<CountingExecutor>();
    executor->script = {
        {true, "", 0, nlohmann::json{{"counts", nlohmann::json::array({1, 2, 3, 4})}}}};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);

    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::BudgetExhausted);
    CHECK(summary.error_code == "max_steps");
    // 步账:setup(1)+ reduce 容器访问(1)+ 两项 attempt(2)= 4;第
    // 3 项拒跑。执行器只见 setup 与两项。
    CHECK(executor->calls.load() == 3);
    CHECK(summary.nodes.at("total").state == NodeState::Failed);
    CHECK(summary.nodes.at("total").error_code == "max_steps");
}

// 病灶钉:重试不消耗步数(旧账只记节点访问一次)。合同:每个 attempt
// 都过准入口预留一步,重试烧穿预算时收 budget_exhausted。
TEST_CASE("重试:每个 attempt 消耗步数,烧穿 max_steps 即停") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: retry-step-cap
version: 1.0.0
entry: flaky
limits:
  max_steps: 2
nodes:
  flaky:
    type: transform
    operation: echo
    retry: { attempts: 3, backoff: fixed, initial: 0s, when: [rate_limited] }
  fin:
    type: end
edges:
  - { from: flaky, on: success, to: fin }
)YAML");

    auto executor = std::make_shared<CountingExecutor>();
    executor->script = {{false, "rate_limited"}};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);

    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::BudgetExhausted);
    CHECK(summary.error_code == "max_steps");
    // 两个 attempt 各留一步(2),第三次尝试在准入口被拦。
    CHECK(executor->calls.load() == 2);
    CHECK(summary.nodes.at("flaky").error_code == "max_steps");
}

// 病灶钉:控制分派(map/foreach 项)不查总时限。合同:项开跑前同一
// 把 elapsed 尺、同一个码(timeout)、同一句文案——与主图旧闸同码。
TEST_CASE("时限:foreach 项间越线,同码收口 timeout") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: foreach-timeout
version: 1.0.0
entry: setup
limits:
  timeout: 10s
nodes:
  setup:
    type: transform
    operation: make_list
  walk:
    type: foreach
    items: "${nodes.setup.output.items}"
    body: work
  work:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: setup, on: success, to: walk }
  - { from: walk, on: success, to: fin }
)YAML");

    auto clock = std::make_shared<FakeClock>();
    auto executor = std::make_shared<CountingExecutor>();
    executor->script = {
        {true, "", 0, nlohmann::json{{"items", nlohmann::json::array({"a", "b"})}}}};
    executor->clock_for_time_travel = clock.get();
    executor->clock_advance_ms = 11000;  // 第一项执行把钟拨过 10s 线
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    options.clock = clock;
    WorkflowRuntime runtime(options);

    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::BudgetExhausted);
    CHECK(summary.error_code == "timeout");
    CHECK(summary.error_message == "总时限越过 10s");  // 同码同话
    CHECK(executor->calls.load() == 2);                 // setup + 第一项;第二项拒跑
}

// async 等待边界是时限的另一个入口:总时限监察与准入口同一把尺、同一
// 个码、同一句文案(旧实现各写各的)。唯一真等待是监察自身的 20ms
// 轮询——被测机制,不是赌时序的栅栏。
TEST_CASE("时限:async 等待撞线,同码同话收 timeout") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: async-timeout
version: 1.0.0
entry: wait
limits:
  timeout: 10s
nodes:
  wait: { type: async, body: slow }
  slow: { type: transform, operation: stall }
  fin: { type: end }
edges:
  - { from: wait, on: success, to: fin }
)YAML");

    auto clock = std::make_shared<FakeClock>();
    auto stall = std::make_shared<StallExecutor>();
    stall->clock = clock.get();
    stall->advance_ms = 11000;  // body 起跑即把钟拨过线,等监察来收
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = stall;
    options.clock = clock;
    WorkflowRuntime runtime(options);

    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::BudgetExhausted);
    CHECK(summary.error_code == "timeout");
    CHECK(summary.error_message == "总时限越过 10s");
    CHECK(stall->calls.load() == 1);
    CHECK(summary.nodes.at("wait").error_code == "timeout");
}

}  // TEST_SUITE(workflows-budget-admission)
