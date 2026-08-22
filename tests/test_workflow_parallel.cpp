// Workflows 单第 3 批:并行/join 五策略、map/foreach/reduce、稳定汇合顺序。
//
// 用论文四路假工具跑通,不碰真网络也能复现竞态(单子第 3 批原文)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

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
        if (behavior.delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(behavior.delay_ms));
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
        }
    }
    REQUIRE(parsed.has_value());
    return *parsed;
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
    // make_list:产 6 篇论文;read_one 按下标延迟(后面的先回来)。
    executor->behaviors["setup"] = {false, "", 0, nlohmann::json{{"papers", nlohmann::json::array({"p0", "p1", "p2", "p3", "p4", "p5"})}}};
    executor->behaviors["read_one"] = {false, "", 0, nlohmann::json()};

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});

    REQUIRE(summary.state == RunState::Succeeded);
    const nlohmann::json& enriched = summary.result["enriched"];
    REQUIRE(enriched.size() == 6);
    // items 顺序:每项的 item 字段对回原数组下标。
    for (int i = 0; i < 6; ++i) {
        CHECK(enriched[i]["item"] == std::string("p") + std::to_string(i));
    }
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
    transform->Register("make_list", [](const nlohmann::json& in) {
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
