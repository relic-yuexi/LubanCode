// Workflows 单第 3 批:并行/join 五策略、map/foreach/reduce、稳定汇合顺序。
//
// 用论文四路假工具跑通,不碰真网络也能复现竞态(单子第 3 批原文)。

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "runtime/event_sink.hpp"
#include "workflow/journal.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

namespace {

// 记账 executor:每次执行记 node id 与线程 id,可按 node 名注入延迟/失败。
class TrackingExecutor : public lubancode::workflow::NodeExecutor {
public:
    struct Behavior {
        bool fail = false;
        std::string error_code;
        int delay_ms = 0;
        nlohmann::json output;
        // map 逐项跑同一 body 时的按序延迟:第 index 项睡 base - index*step
        // 毫秒(base 不够减就 0)——下标越靠后回来越快,逼出"后完成者
        // 先落位"的错序竞态。
        int delay_base_ms = 0;
        int delay_step_ms = 0;
    };

    std::map<std::string, Behavior> behaviors;
    std::mutex mutex;
    std::vector<std::pair<std::string, std::thread::id>> calls;
    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};

    lubancode::workflow::NodeExecResult Execute(const lubancode::workflow::NodeExecRequest& request) override {
        const int now = concurrent.fetch_add(1) + 1;
        int expected = max_concurrent.load();
        while (now > expected && !max_concurrent.compare_exchange_weak(expected, now)) {
        }
        lubancode::workflow::NodeExecResult result;
        Behavior behavior;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = behaviors.find(request.node->id);
            if (it != behaviors.end()) behavior = it->second;
            calls.emplace_back(request.node->id, std::this_thread::get_id());
        }
        int delay = behavior.delay_ms;
        if (behavior.delay_base_ms > 0) {
            const std::int64_t index = request.resolved_input.value("index", nlohmann::json(0));
            const std::int64_t by_index =
                behavior.delay_base_ms - index * behavior.delay_step_ms;
            delay = static_cast<int>(std::max<std::int64_t>(0, by_index));
        }
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        if (behavior.fail) {
            result.error_code = behavior.error_code.empty() ? "boom" : behavior.error_code;
            result.error_message = "scripted failure";
        } else {
            result.ok = true;
            result.output = behavior.output.is_null()
                                ? nlohmann::json{{"node", request.node->id}, {"item", request.resolved_input.value("item", nlohmann::json())}}
                                : behavior.output;
        }
        concurrent.fetch_sub(1);
        return result;
    }
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

// ---------------------------------------------------------------------------
// AR-03 合同夹具(Workflow 并行项共写同一节点执行记录):交错全由栅栏钉死,
// 不靠 sleep 赌时序。仓里没有 std::barrier 先例,mutex+condvar 手搓两只。
// ---------------------------------------------------------------------------

// 栅栏:N 方齐到才放行(把"两项同时在执行窗口里"钉成事实)。
class Barrier {
public:
    explicit Barrier(int parties) : parties_(parties) {}

    void Arrive() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (++arrived_ >= parties_) {
            cv_.notify_all();
            return;
        }
        cv_.wait(lock, [&] { return arrived_ >= parties_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int arrived_ = 0;
    const int parties_;
};

// 门闩:一方给信号,另一方等到(把"item0 已推进到第 2 次尝试"钉在
// item1 收尾之前)。
class Latch {
public:
    void Signal() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            signaled_ = true;
        }
        cv_.notify_all();
    }

    void Wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return signaled_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool signaled_ = false;
};

// 交错重试执行器:map 两项按剧本走——
//   item0 a1:与 item1 a1 在栅栏会齐(同时在场)后报 transient 失败;
//   item0 a2:进门即给门闩信号(此刻它的 attempt 已推到 2),随后成功;
//   item1 a1:等门闩(= item0 的第 2 次尝试已开跑)后成功。
// 旧实现里共享槽 record.attempt 此刻已被 item0 写成 2,item1 的
// completed 事件会顶着 -a2 发出去——事件身份串项,合同册当场抓红。
class InterleavedRetryExecutor : public lubancode::workflow::NodeExecutor {
public:
    explicit InterleavedRetryExecutor(int parties) : attempt1_gate_(parties) {}

    lubancode::workflow::NodeExecResult Execute(
        const lubancode::workflow::NodeExecRequest& request) override {
        const int index = request.resolved_input.value("index", 0);
        lubancode::workflow::NodeExecResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            trace_.push_back("i" + std::to_string(index) + ":a" +
                             std::to_string(request.attempt) + ":in");
        }
        if (request.attempt == 1) {
            attempt1_gate_.Arrive();  // 两项同时在执行窗口里(真并发)
            if (index == 0) {
                result.error_code = "transient";  // 默认白名单内的可重试码
                result.error_message = "scripted transient failure";
            } else {
                item0_second_attempt_.Wait();  // 等 item0 的第 2 次尝试开跑
                result.ok = true;
                result.output = nlohmann::json{
                    {"node", request.node->id},
                    {"item", request.resolved_input.value("item", nlohmann::json())}};
            }
        } else {
            // item0 的第 2 次尝试:放行 item1 收尾。
            item0_second_attempt_.Signal();
            result.ok = true;
            result.output = nlohmann::json{
                {"node", request.node->id},
                {"item", request.resolved_input.value("item", nlohmann::json())}};
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            trace_.push_back("i" + std::to_string(index) + ":a" +
                             std::to_string(request.attempt) + ":out");
        }
        return result;
    }

    Barrier attempt1_gate_;
    Latch item0_second_attempt_;
    std::mutex mutex_;
    std::vector<std::string> trace_;
};

// 节点事件录音器:started/retrying/completed 三类,连 attempt 与
// node_run_id 一并收(事件自称的身份)。
struct NodeEventRecorder final : public lubancode::runtime::EventSink {
    struct Rec {
        std::string node_id;
        std::string type;
        int attempt = 0;
        std::string node_run_id;
    };

    void Emit(const lubancode::runtime::ServerEvent& event) override {
        if (!event.payload.is_object()) return;
        const std::string type = event.payload.value("type", std::string());
        if (type != lubancode::workflow::kEventNodeStarted &&
            type != lubancode::workflow::kEventNodeRetrying &&
            type != lubancode::workflow::kEventNodeCompleted) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        records.push_back(Rec{
            event.payload.value("node_id", std::string()),
            type,
            event.payload.value("attempt", 0),
            event.payload.value("node_run_id", std::string()),
        });
    }

    std::mutex mutex;
    std::vector<Rec> records;
};

// node_run_id 形如 <run>-<body>-i<下标>-d<派发号>-a<attempt>:路号与
// attempt 都从 id 里读回来——事件自称的身份必须与账面一一对应。
struct RunIdParts {
    int item = -1;
    int attempt = -1;
};

RunIdParts ParseNodeRunId(const std::string& id) {
    RunIdParts parts;
    const std::size_t a_pos = id.rfind("-a");
    if (a_pos == std::string::npos) return parts;
    parts.attempt = std::stoi(id.substr(a_pos + 2));
    const std::size_t i_pos = id.rfind("-i");
    if (i_pos != std::string::npos && i_pos < a_pos) {
        parts.item = std::stoi(id.substr(i_pos + 2));
    }
    return parts;
}

// 四路并行 + all_settled 汇合的论文检索形(假工具)。
const char* kFourWayYaml = R"YAML(
schema_version: 1
id: paper-flow
version: 1.0.0
name: paper
entry: search_sources
limits:
  max_concurrency: 4
nodes:
  search_sources:
    type: parallel
    branches: [arxiv, dblp, scholar, anysearch]
    join: all_settled
    max_concurrency: 4
  arxiv:
    type: transform
    operation: fetch
    input: { source: arxiv }
  dblp:
    type: transform
    operation: fetch
    input: { source: dblp }
  scholar:
    type: transform
    operation: fetch
    input: { source: scholar }
  anysearch:
    type: transform
    operation: fetch
    input: { source: anysearch }
  fin:
    type: end
edges:
  - { from: search_sources, on: success, to: fin }
result:
  sources: "${nodes.search_sources.output.outputs}"
  unavailable: "${nodes.search_sources.output.unavailable}"
)YAML";

}  // namespace

TEST_SUITE("workflows-parallel") {

TEST_CASE("四路真并行:线程数=分支数,汇合按定义顺序") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(kFourWayYaml);
    auto executor = std::make_shared<TrackingExecutor>();
    executor->behaviors["arxiv"] = {false, "", 30, nlohmann::json{{"count", 31}}};
    executor->behaviors["dblp"] = {false, "", 5, nlohmann::json{{"count", 24}}};
    executor->behaviors["scholar"] = {false, "", 20, nlohmann::json{{"count", 12}}};
    executor->behaviors["anysearch"] = {false, "", 10, nlohmann::json{{"count", 18}}};

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});

    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(executor->calls.size() == 4);
    CHECK(executor->max_concurrent.load() >= 2);  // 真并发,不是排队
    // 汇合按定义顺序(arxiv,dblp,scholar,anysearch),不按完成时间
    //(dblp 5ms 最先回来,但排第二)。
    const nlohmann::json& sources = summary.result["sources"];
    REQUIRE(sources.size() == 4);
    CHECK(sources[0]["branch"] == "arxiv");
    CHECK(sources[1]["branch"] == "dblp");
    CHECK(sources[2]["branch"] == "scholar");
    CHECK(sources[3]["branch"] == "anysearch");
    CHECK(summary.result["unavailable"].size() == 0);
}

TEST_CASE("parallel 汇合边写 joined 也能走到 end 并结算 result") {
    using namespace lubancode::workflow;
    std::string yaml = kFourWayYaml;
    const std::string old_edge = "on: success";
    const std::size_t edge = yaml.find(old_edge);
    REQUIRE(edge != std::string::npos);
    yaml.replace(edge, old_edge.size(), "on: joined");
    const WorkflowDefinition def = ParseOrDie(yaml.c_str());

    auto executor = std::make_shared<TrackingExecutor>();
    for (const char* id : {"arxiv", "dblp", "scholar", "anysearch"}) {
        executor->behaviors[id] = {false, "", 0, nlohmann::json{{"source", id}}};
    }
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    const auto summary = WorkflowRuntime(options).Run(def, RunInputs{});

    REQUIRE(summary.state == RunState::Succeeded);
    REQUIRE(summary.result.contains("sources"));
    CHECK(summary.result["sources"].size() == 4);
}

TEST_CASE("all_settled:一路失败其余照交,缺失账明写") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(kFourWayYaml);
    auto executor = std::make_shared<TrackingExecutor>();
    executor->behaviors["arxiv"] = {false, "", 0, nlohmann::json{{"count", 31}}};
    executor->behaviors["dblp"] = {false, "", 0, nlohmann::json{{"count", 24}}};
    executor->behaviors["scholar"] = {true, "not_configured", 0, nlohmann::json()};
    executor->behaviors["anysearch"] = {false, "", 0, nlohmann::json{{"count", 18}}};

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});

    REQUIRE(summary.state == RunState::Succeeded);  // all_settled:缺一路继续
    CHECK(summary.result["unavailable"].size() == 1);
    CHECK(summary.result["unavailable"][0] == "scholar");
    CHECK(summary.nodes.at("scholar").state == NodeState::Failed);
}

TEST_CASE("join=all:一支失败整场失败") {
    using namespace lubancode::workflow;
    WorkflowDefinition def = ParseOrDie(kFourWayYaml);
    // 换策略:直接改 AST(parser 已填 node_map,两处同步)。
    for (auto& node : def.nodes) {
        if (node.id == "search_sources") node.join = JoinPolicy::All;
    }
    def.node_map.at("search_sources").join = JoinPolicy::All;

    auto executor = std::make_shared<TrackingExecutor>();
    executor->behaviors["arxiv"] = {false, "", 0, nlohmann::json()};
    executor->behaviors["dblp"] = {true, "boom", 0, nlohmann::json()};
    executor->behaviors["scholar"] = {false, "", 0, nlohmann::json()};
    executor->behaviors["anysearch"] = {false, "", 0, nlohmann::json()};

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::Failed);
    CHECK(summary.error_code == "join_failed");
}

TEST_CASE("join=quorum:N 个成功便过关,凑不够立刻失败") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: quorum-flow
version: 1.0.0
name: q
entry: fan
nodes:
  fan:
    type: parallel
    branches: [a, b, c]
    join: quorum
    quorum: 2
  a:
    type: transform
    operation: fetch
  b:
    type: transform
    operation: fetch
  c:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: fan, on: success, to: fin }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);

    SUBCASE("两路成功过 quorum") {
        auto executor = std::make_shared<TrackingExecutor>();
        executor->behaviors["a"] = {false, "", 0, nlohmann::json()};
        executor->behaviors["b"] = {false, "", 0, nlohmann::json()};
        executor->behaviors["c"] = {true, "boom", 0, nlohmann::json()};
        RuntimeOptions options;
        options.executors[NodeKind::Transform] = executor;
        WorkflowRuntime runtime(options);
        CHECK(runtime.Run(def, RunInputs{}).state == RunState::Succeeded);
    }
    SUBCASE("只一路成功,凑不够 2") {
        auto executor = std::make_shared<TrackingExecutor>();
        executor->behaviors["a"] = {false, "", 0, nlohmann::json()};
        executor->behaviors["b"] = {true, "boom", 0, nlohmann::json()};
        executor->behaviors["c"] = {true, "boom", 0, nlohmann::json()};
        RuntimeOptions options;
        options.executors[NodeKind::Transform] = executor;
        WorkflowRuntime runtime(options);
        CHECK(runtime.Run(def, RunInputs{}).state == RunState::Failed);
    }
}

TEST_CASE("join=any:首个成功便过关") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: any-flow
version: 1.0.0
name: a
entry: fan
nodes:
  fan:
    type: parallel
    branches: [a, b]
    join: any
  a:
    type: transform
    operation: fetch
  b:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: fan, on: success, to: fin }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto executor = std::make_shared<TrackingExecutor>();
    executor->behaviors["a"] = {true, "boom", 0, nlohmann::json()};
    executor->behaviors["b"] = {false, "", 0, nlohmann::json()};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);
    CHECK(runtime.Run(def, RunInputs{}).state == RunState::Succeeded);
}

TEST_CASE("并发帽:全局 4、节点 2,实际并发不越 2") {
    using namespace lubancode::workflow;
    WorkflowDefinition def = ParseOrDie(kFourWayYaml);
    // 节点帽 2 压过全局 4。
    for (auto& node : def.nodes) {
        if (node.id == "search_sources") node.max_concurrency = 2;
    }
    def.node_map.at("search_sources").max_concurrency = 2;

    auto executor = std::make_shared<TrackingExecutor>();
    for (const char* b : {"arxiv", "dblp", "scholar", "anysearch"}) {
        executor->behaviors[b] = {false, "", 40, nlohmann::json()};
    }
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::Succeeded);
    CHECK(executor->max_concurrent.load() <= 2);  // 帽子生效
    CHECK(executor->calls.size() == 4);
}

TEST_CASE("map:数组拆项并发跑,结果按 items 顺序") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: map-flow
version: 1.0.0
name: m
entry: setup
nodes:
  setup:
    type: transform
    operation: make_list
  enrich:
    type: map
    items: "${nodes.setup.output.papers}"
    body: read_one
    max_concurrency: 4
  read_one:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: setup, on: success, to: enrich }
  - { from: enrich, on: success, to: fin }
result:
  enriched: "${nodes.enrich.output.items}"
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);

    auto executor = std::make_shared<TrackingExecutor>();
    // make_list:产 6 篇论文;read_one 按下标延迟——下标越靠后回来越快,
    // 完成顺序与 items 顺序整个颠倒,逼出"落位错"的竞态(macOS CI 实锤:
    // 并发 worker 共写 store 键再读回,后完成者覆写,先完成者取到别人的)。
    executor->behaviors["setup"] = {false, "", 0, nlohmann::json{{"papers", nlohmann::json::array({"p0", "p1", "p2", "p3", "p4", "p5"})}}};
    executor->behaviors["read_one"] = {false, "", 0, nlohmann::json(), /*delay_base_ms=*/60,
                                       /*delay_step_ms=*/10};

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});

    REQUIRE(summary.state == RunState::Succeeded);
    const nlohmann::json& enriched = summary.result["enriched"];
    // 尺寸守死:一项不多一项不少(槽位预分配,失败项填 null)。
    REQUIRE(enriched.size() == 6);
    // items 顺序:每项非空(item/node 字段都在)且对回原数组下标。
    // 只查"item 对"会漏掉"槽位空了但恰好别的下标也对"的错位。
    for (int i = 0; i < 6; ++i) {
        REQUIRE(enriched[i].is_object());
        CHECK_FALSE(enriched[i].empty());
        CHECK(enriched[i]["item"] == std::string("p") + std::to_string(i));
        CHECK(enriched[i]["node"] == "read_one");
    }
    // 全量再钉一道:六项一个不多不少,齐齐按序。
    std::string joined;
    for (int i = 0; i < 6; ++i) {
        joined += enriched[i]["item"].get<std::string>();
    }
    CHECK(joined == "p0p1p2p3p4p5");
}

TEST_CASE("map:并发各路的 node_run_id 带路号,事件账不串线") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: map-run-id
version: 1.0.0
name: m
entry: setup
nodes:
  setup:
    type: transform
    operation: make_list
  enrich:
    type: map
    items: "${nodes.setup.output.papers}"
    body: read_one
    max_concurrency: 4
  read_one:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: setup, on: success, to: enrich }
  - { from: enrich, on: success, to: fin }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);

    auto executor = std::make_shared<TrackingExecutor>();
    executor->behaviors["setup"] = {false, "", 0,
                                    nlohmann::json{{"papers", nlohmann::json::array({"p0", "p1", "p2"})}}};
    executor->behaviors["read_one"] = {false, "", 0, nlohmann::json()};

    struct RunIdRecorder final : public lubancode::runtime::EventSink {
        std::mutex mutex;
        std::vector<std::string> started_run_ids;
        void Emit(const lubancode::runtime::ServerEvent& event) override {
            if (event.payload.is_object() &&
                event.payload.value("type", std::string()) ==
                    lubancode::workflow::kEventNodeStarted) {
                std::lock_guard<std::mutex> lock(mutex);
                started_run_ids.push_back(event.payload.value("node_run_id", std::string()));
            }
        }
    } recorder;

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    options.event_sink = &recorder;
    const auto summary = WorkflowRuntime(options).Run(def, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);

    // body 三路并发,每路的 node_run_id 各带 -i<下标> 路号——面板与诊断
    // 账靠它分清谁是谁;不带路号时三路同 id,事件互踩(0.26.79 实翻)。
    // 编排账单(workflow 会话归属统一)起 node_run_id 另带 -d<派发序号>
    //(loop 重入防撞名),下标路号仍在,断言放宽到路号段。
    std::vector<std::string> body_ids;
    for (const std::string& id : recorder.started_run_ids) {
        if (id.find("-read_one-") != std::string::npos) body_ids.push_back(id);
    }
    std::sort(body_ids.begin(), body_ids.end());
    REQUIRE(body_ids.size() == 3);
    CHECK(body_ids[0].find("-read_one-i0-d") != std::string::npos);
    CHECK(body_ids[1].find("-read_one-i1-d") != std::string::npos);
    CHECK(body_ids[2].find("-read_one-i2-d") != std::string::npos);
    // 三路互不相同:并发同跑不串线的基本盘。
    CHECK(body_ids[0] != body_ids[1]);
    CHECK(body_ids[1] != body_ids[2]);
}

TEST_CASE("map 两项栅栏交错重试:事件 attempt/node_run_id 一一对应(AR-03)") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: map-interleave
version: 1.0.0
name: mi
entry: enrich
limits:
  max_concurrency: 2
nodes:
  enrich:
    type: map
    items: "${inputs.papers}"
    body: read_one
    max_concurrency: 2
  read_one:
    type: transform
    operation: fetch
    retry: { attempts: 2, backoff: fixed, initial: 0s }
  fin:
    type: end
edges:
  - { from: enrich, on: success, to: fin }
result:
  enriched: "${nodes.enrich.output.items}"
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto executor = std::make_shared<InterleavedRetryExecutor>(2);
    NodeEventRecorder recorder;

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    options.event_sink = &recorder;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(
        def, RunInputs(nlohmann::json{{"papers", nlohmann::json::array({"p0", "p1"})}}));

    REQUIRE(summary.state == RunState::Succeeded);
    // map 输出保序(既有合同):两项都成功,顺序对回原数组。
    const nlohmann::json& enriched = summary.result["enriched"];
    REQUIRE(enriched.size() == 2);
    CHECK(enriched[0]["item"] == "p0");
    CHECK(enriched[1]["item"] == "p1");

    // 栅栏确实把两项同时钉在第 1 次尝试的执行窗口里:两枚 a1:in 先于
    // 任何 :out——同时在场,不是先后路过。
    REQUIRE(executor->trace_.size() == 6);
    CHECK(executor->trace_[0].find(":a1:in") != std::string::npos);
    CHECK(executor->trace_[1].find(":a1:in") != std::string::npos);
    CHECK(executor->trace_[0] != executor->trace_[1]);

    // 事件账:i0 全程 a1→retry→a2;i1 只跑过 a1。每枚事件的 attempt 与它
    // 自己 node_run_id 的 -a 段一致,且等于该 (item,type) 应有的值。
    // 旧实现共享槽的 attempt 被 i0 推到 2,i1 的 completed 顶着 -a2 发出
    // 去——身份串项,这里当场抓红。
    std::map<std::pair<int, std::string>, std::vector<int>> seen;
    std::map<int, std::map<std::string, std::string>> id_by_item_and_type;
    for (const auto& rec : recorder.records) {
        if (rec.node_id != "read_one") continue;
        const RunIdParts parts = ParseNodeRunId(rec.node_run_id);
        REQUIRE(parts.item >= 0);
        REQUIRE(parts.attempt >= 1);
        CHECK(rec.attempt == parts.attempt);  // payload 与 id 自称一致
        seen[{parts.item, rec.type}].push_back(rec.attempt);
        id_by_item_and_type[parts.item][rec.type] = rec.node_run_id;
    }
    const auto sorted = [](std::vector<int> v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    CHECK(sorted(seen[{0, kEventNodeStarted}]) == std::vector<int>{1, 2});
    CHECK(sorted(seen[{0, kEventNodeRetrying}]) == std::vector<int>{1});
    CHECK(sorted(seen[{0, kEventNodeCompleted}]) == std::vector<int>{2});
    CHECK(sorted(seen[{1, kEventNodeStarted}]) == std::vector<int>{1});
    CHECK(sorted(seen[{1, kEventNodeCompleted}]) == std::vector<int>{1});
    // i1 的 started 与 completed 必须同一个 node_run_id(同一次执行同一
    // 个身份);串项时 completed 会换脸成 -a2。
    CHECK(id_by_item_and_type[1][kEventNodeStarted] ==
          id_by_item_and_type[1][kEventNodeCompleted]);
    // 节点汇总投影只认终态:两项都成功,槽里必是 Succeeded。
    CHECK(summary.nodes.at("read_one").state == NodeState::Succeeded);
}

TEST_CASE("map 展开越 max_nodes 拒跑") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: map-big
version: 1.0.0
name: mb
entry: setup
limits:
  max_nodes: 4
nodes:
  setup:
    type: transform
    operation: make_list
  enrich:
    type: map
    items: "${nodes.setup.output.papers}"
    body: read_one
  read_one:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: setup, on: success, to: enrich }
  - { from: enrich, on: success, to: fin }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto executor = std::make_shared<TrackingExecutor>();
    nlohmann::json papers = nlohmann::json::array();
    for (int i = 0; i < 10; ++i) papers.push_back("p" + std::to_string(i));
    executor->behaviors["setup"] = {false, "", 0, nlohmann::json{{"papers", papers}}};
    executor->behaviors["read_one"] = {false, "", 0, nlohmann::json()};

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::Failed);
    CHECK(summary.error_code == "map_failed");
    CHECK(summary.nodes.at("enrich").error_code == "map_too_large");
}

TEST_CASE("foreach:顺次迭代,一项失败整场停") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: foreach-flow
version: 1.0.0
name: fe
entry: setup
nodes:
  setup:
    type: transform
    operation: make_list
  walk:
    type: foreach
    items: "${nodes.setup.output.items}"
    body: step
  step:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: setup, on: success, to: walk }
  - { from: walk, on: success, to: fin }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto executor = std::make_shared<TrackingExecutor>();
    executor->behaviors["setup"] = {false, "", 0, nlohmann::json{{"items", nlohmann::json::array({1, 2, 3})}}};
    executor->behaviors["step"] = {false, "", 0, nlohmann::json()};

    SUBCASE("全部成功") {
        RuntimeOptions options;
        options.executors[NodeKind::Transform] = executor;
        WorkflowRuntime runtime(options);
        const auto summary = runtime.Run(def, RunInputs{});
        CHECK(summary.state == RunState::Succeeded);
        // setup 1 次 + step 3 次 = 4 次。
        int step_calls = 0;
        for (const auto& [node, thread] : executor->calls) {
            if (node == "step") ++step_calls;
        }
        CHECK(step_calls == 3);
    }
}

TEST_CASE("reduce:按稳定次序累加") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: reduce-flow
version: 1.0.0
name: r
entry: setup
nodes:
  setup:
    type: transform
    operation: make_list
  total:
    type: reduce
    items: "${nodes.setup.output.counts}"
    body: add
    initial: "${inputs.start}"
  add:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: setup, on: success, to: total }
  - { from: total, on: success, to: fin }
result:
  total: "${nodes.total.output}"
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto executor = std::make_shared<TrackingExecutor>();
    executor->behaviors["setup"] = {false, "", 0, nlohmann::json{{"counts", nlohmann::json::array({1, 2, 3, 4})}}};
    // add 节点:吃 acc+item,吐和。TrackingExecutor 的 output 是脚本,这里
    // 用 transform 注册表做真累加。
    auto transform = std::make_shared<TransformExecutor>();
    transform->Register("make_list", [](const nlohmann::json&) {
        return nlohmann::json{{"counts", nlohmann::json::array({1, 2, 3, 4})}};
    });
    transform->Register("fetch", [](const nlohmann::json& in) {
        if (in.contains("acc") && in.contains("item")) {
            return nlohmann::json(in["acc"].get<int>() + in["item"].get<int>());
        }
        return in;
    });

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = transform;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs(nlohmann::json{{"start", 100}}));
    CHECK(summary.state == RunState::Succeeded);
    CHECK(summary.result["total"] == 100 + 1 + 2 + 3 + 4);
}

TEST_CASE("switch:条件选路(结构化值,禁 eval)") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: switch-flow
version: 1.0.0
name: s
entry: route
inputs:
  type: object
  properties:
    mode: { type: string }
nodes:
  route:
    type: switch
    conditions:
      - { op: equals, path: "${inputs.mode}", value: fast, to: quick }
      - { op: equals, path: "${inputs.mode}", value: deep, to: slow }
    default_to: quick
  quick:
    type: transform
    operation: fetch
  slow:
    type: transform
    operation: fetch
  fin:
    type: end
edges:
  - { from: quick, on: success, to: fin }
  - { from: slow, on: success, to: fin }
result:
  mode: "${inputs.mode}"
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto transform = std::make_shared<TransformExecutor>();
    transform->Register("fetch", [](const nlohmann::json& in) { return in; });

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = transform;
    WorkflowRuntime runtime(options);
    // deep 路线:quick 不该跑。
    const auto deep = runtime.Run(def, RunInputs(nlohmann::json{{"mode", std::string("deep")}}));
    REQUIRE(deep.state == RunState::Succeeded);
    CHECK(deep.result["mode"] == "deep");
    CHECK(deep.nodes.count("slow") == 1);
    CHECK(deep.nodes.count("quick") == 0);
}

TEST_CASE("并行取消:cancel_token 传到分支") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(kFourWayYaml);
    auto executor = std::make_shared<TrackingExecutor>();
    for (const char* b : {"arxiv", "dblp", "scholar", "anysearch"}) {
        executor->behaviors[b] = {false, "", 2000, nlohmann::json()};  // 慢活
    }
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);

    std::atomic<bool> cancel{false};
    std::thread stopper([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        cancel.store(true);
    });
    const auto summary = runtime.Run(def, RunInputs{}, &cancel);
    stopper.join();
    CHECK(summary.state == RunState::Cancelled);
}

}  // TEST_SUITE(workflows-parallel)
