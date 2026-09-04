// workflow 会话归属统一单:WorkflowRuntime × 真 TrajectorySessionLedger 的
// 编排账集成(fail closed 合同由 runtime 落)。四场:
//   1. 顺序图:编排账 + 每节点一份 node 账,trajectory verify 全过。
//   2. parallel + map:并发各路 node stream 互不串线,verify 全过。
//   3. loop + retry:重试新开 node 文件、loop 重入新派发号,verify 全过。
//   4. fail closed:编排账/node 账开不出,run/节点停在明确失败态,verify 过。
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

namespace {

namespace fs = std::filesystem;
using namespace lubancode::workflow;

fs::path FreshDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::optional<lubancode::runtime::TrajectorySessionLedger> OpenLedger(
    const fs::path& root,
    std::function<std::optional<std::string>()> run_fault = {},
    std::function<std::optional<std::string>()> node_fault = {}) {
    lubancode::runtime::TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces";
    options.workspace_root = root / "repo";
    options.lubancode_version = "test";
    options.workflow_start_fault = std::move(run_fault);
    options.workflow_node_start_fault = std::move(node_fault);
    std::error_code ec;
    fs::create_directories(root / "repo", ec);
    auto ledger = lubancode::runtime::TrajectorySessionLedger::Open(std::move(options));
    if (!ledger.has_value()) {
        return std::nullopt;
    }
    return std::move(*ledger);
}

WorkflowDefinition ParseOrDie(const char* yaml) {
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    return std::move(*parsed);
}

// 线程安全的假执行器(parallel worker 并发调):按节点名给脚本,可数调用。
class BehaviorExecutor : public NodeExecutor {
public:
    struct Step {
        bool ok = true;
        std::string error_code;
        nlohmann::json output = nlohmann::json::object();
    };
    // 节点名 -> 依次消耗的脚本;耗尽后重复最后一格。
    std::map<std::string, std::vector<Step>> behaviors;

    NodeExecResult Execute(const NodeExecRequest& request) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string node_id = request.node->id;
        auto it = behaviors.find(node_id);
        if (it == behaviors.end() || it->second.empty()) {
            NodeExecResult result;
            result.ok = true;
            result.output = nlohmann::json{{"node", node_id}};
            saw_trajectory[node_id] = request.trajectory != nullptr;
            return result;
        }
        if (next_index[node_id] >= it->second.size()) {
            next_index[node_id] = it->second.size() - 1;
        }
        const Step& step = it->second[next_index[node_id]++];
        saw_trajectory[node_id] = request.trajectory != nullptr;
        calls[node_id] += 1;
        NodeExecResult result;
        result.ok = step.ok;
        result.error_code = step.error_code;
        result.output = step.output;
        return result;
    }

    std::map<std::string, int> calls;
    std::map<std::string, bool> saw_trajectory;

private:
    std::mutex mutex_;
    std::map<std::string, std::size_t> next_index;
};

std::vector<std::string> KindsOf(const fs::path& stream) {
    std::vector<std::string> kinds;
    const auto lines = lubancode::trajectory::ReadJournalLines(stream);
    if (!lines.has_value()) {
        return kinds;
    }
    for (const std::string& line : *lines) {
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        kinds.push_back(parsed.is_discarded() ? std::string("<bad>")
                                              : parsed.value("kind", std::string()));
    }
    return kinds;
}

fs::path WorkflowRunDir(const lubancode::runtime::TrajectorySessionLedger& ledger,
                        const std::string& run_id) {
    return ledger.session_dir() / "workflows" / fs::path(run_id);
}

int CountJsonlUnder(const fs::path& dir) {
    int count = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return 0;
    }
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            ++count;
        }
    }
    return count;
}

bool EveryEdgeOk(const lubancode::trajectory::SessionVerifyReport& report) {
    for (const auto& edge : report.child_edges) {
        if (!edge.error_code.empty()) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("runtime 集成 1:顺序图——编排账+node 账全落,verify 全过") {
    const char* yaml = R"YAML(
schema_version: 1
id: seq-traj
version: 1.0.0
name: s
entry: x
nodes:
  x:
    type: transform
    operation: echo
    input: { q: "${inputs.topic}" }
  y:
    type: transform
    operation: echo
  fin:
    type: end
edges:
  - { from: x, on: success, to: y }
  - { from: y, on: success, to: fin }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto opened = OpenLedger(FreshDir("lubancode-wf-traj-seq"));
    REQUIRE(opened.has_value());
    auto ledger = std::make_unique<lubancode::runtime::TrajectorySessionLedger>(std::move(*opened));

    auto executor = std::make_shared<BehaviorExecutor>();
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    options.trajectory_ledger = ledger.get();
    const WorkflowRunSummary summary =
        WorkflowRuntime(std::move(options)).Run(def, RunInputs{nlohmann::json{{"topic", "账"}}});

    REQUIRE(summary.state == RunState::Succeeded);
    // 执行器真拿到了 node 账(模型边界挂点由 host executor 接,这里验
    // runtime 侧递到了 request)。
    CHECK(executor->saw_trajectory["x"]);
    CHECK(executor->saw_trajectory["y"]);

    const fs::path run_dir = WorkflowRunDir(*ledger, summary.run_id);
    REQUIRE(fs::exists(run_dir / "workflow.jsonl"));
    const auto orchestration = KindsOf(run_dir / "workflow.jsonl");
    CHECK(orchestration.front() == "run.started");
    CHECK(orchestration.back() == "run.completed");
    CHECK(std::find(orchestration.begin(), orchestration.end(), "workflow.definition.loaded") !=
          orchestration.end());
    CHECK(std::count(orchestration.begin(), orchestration.end(), "workflow.node.dispatched") == 2);
    CHECK(std::count(orchestration.begin(), orchestration.end(), "workflow.node.completed") == 2);
    CHECK(std::find(orchestration.begin(), orchestration.end(), "workflow.checkpoint.saved") !=
          orchestration.end());

    // 每只执行节点一份 node 账,首行 run.started 末行 run.completed。
    std::error_code ec;
    std::vector<std::string> node_files;
    for (const auto& entry : fs::directory_iterator(run_dir / "nodes", ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            node_files.push_back(entry.path().filename().generic_string());
        }
    }
    std::sort(node_files.begin(), node_files.end());
    REQUIRE(node_files.size() == 2);
    for (const std::string& name : node_files) {
        const auto kinds = KindsOf(run_dir / "nodes" / name);
        CHECK(kinds.front() == "run.started");
        CHECK(kinds.back() == "run.completed");
    }
    CHECK(node_files[0].find("-x-d") != std::string::npos);
    CHECK(node_files[1].find("-y-d") != std::string::npos);

    const auto report = ledger->VerifySession();
    CHECK(report.error_code.empty());
    CHECK(EveryEdgeOk(report));
}

TEST_CASE("runtime 集成 2:parallel + map——并发各路互不串线,verify 全过") {
    const char* yaml = R"YAML(
schema_version: 1
id: fanout-traj
version: 1.0.0
name: f
entry: setup
nodes:
  setup:
    type: transform
    operation: make_list
  fan:
    type: parallel
    branches: [left, right]
    join: all
  left:
    type: transform
    operation: echo
  right:
    type: transform
    operation: echo
  enrich:
    type: map
    items: "${nodes.setup.output.papers}"
    body: read_one
    max_concurrency: 4
  read_one:
    type: transform
    operation: echo
  fin:
    type: end
edges:
  - { from: setup, on: success, to: fan }
  - { from: fan, on: joined, to: enrich }
  - { from: enrich, on: success, to: fin }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto opened = OpenLedger(FreshDir("lubancode-wf-traj-fanout"));
    REQUIRE(opened.has_value());
    auto ledger = std::make_unique<lubancode::runtime::TrajectorySessionLedger>(std::move(*opened));

    auto executor = std::make_shared<BehaviorExecutor>();
    executor->behaviors["setup"] = {
        {true, "", nlohmann::json{{"papers", nlohmann::json::array({"p0", "p1", "p2"})}}}};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    options.trajectory_ledger = ledger.get();
    const WorkflowRunSummary summary = WorkflowRuntime(std::move(options)).Run(def, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);

    const fs::path run_dir = WorkflowRunDir(*ledger, summary.run_id);
    const auto orchestration = KindsOf(run_dir / "workflow.jsonl");
    // 并发路各一份 node 账(setup + left + right + map 三项),map 各路带
    // -i 路号,不串线。
    CHECK(std::count(orchestration.begin(), orchestration.end(), "workflow.branch.started") == 1);
    CHECK(std::count(orchestration.begin(), orchestration.end(), "workflow.join.completed") == 1);
    CHECK(std::count(orchestration.begin(), orchestration.end(), "workflow.node.dispatched") == 6);

    std::error_code ec;
    std::vector<std::string> node_files;
    for (const auto& entry : fs::directory_iterator(run_dir / "nodes", ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            node_files.push_back(entry.path().filename().generic_string());
        }
    }
    REQUIRE(node_files.size() == 6);
    int map_lanes = 0;
    for (const std::string& name : node_files) {
        if (name.find("-read_one-i") != std::string::npos) {
            ++map_lanes;
        }
    }
    CHECK(map_lanes == 3);
    for (const std::string& name : node_files) {
        const auto kinds = KindsOf(run_dir / "nodes" / name);
        CHECK(kinds.front() == "run.started");
        CHECK(kinds.back() == "run.completed");
    }

    const auto report = ledger->VerifySession();
    CHECK(report.error_code.empty());
    CHECK(EveryEdgeOk(report));
}

TEST_CASE("runtime 集成 3:loop + retry——重试新开文件,loop 重入新派发号") {
    const char* yaml = R"YAML(
schema_version: 1
id: loop-retry-traj
version: 1.0.0
name: lr
entry: flaky
limits:
  max_steps: 40
nodes:
  flaky:
    type: transform
    operation: echo
    retry: { attempts: 2, backoff: fixed, initial: 0s, when: [rate_limited] }
  rev:
    type: loop
    body: [draft, inspect]
    until: { op: equals, path: "${nodes.inspect.output.approved}", value: true }
    min_iterations: 1
    max_iterations: 5
    hard_limit: 6
  draft:
    type: transform
    operation: echo
    input: { previous: "${nodes.rev.output.previous}" }
  inspect:
    type: transform
    operation: echo
    input: { draft: "${nodes.draft.output}" }
  fin:
    type: end
  exhausted:
    type: end
edges:
  - { from: flaky, on: success, to: rev }
  - { from: rev, on: success, to: fin }
  - { from: rev, on: exhausted, to: exhausted }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto opened = OpenLedger(FreshDir("lubancode-wf-traj-loop"));
    REQUIRE(opened.has_value());
    auto ledger = std::make_unique<lubancode::runtime::TrajectorySessionLedger>(std::move(*opened));

    auto executor = std::make_shared<BehaviorExecutor>();
    executor->behaviors["flaky"] = {
        {false, "rate_limited", nlohmann::json()},
        {true, "", nlohmann::json{{"ok", 1}}},
    };
    // inspect 两轮 false 后 true:loop 跑满三轮。
    executor->behaviors["inspect"] = {
        {true, "", nlohmann::json{{"approved", false}}},
        {true, "", nlohmann::json{{"approved", false}}},
        {true, "", nlohmann::json{{"approved", true}}},
    };
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    options.trajectory_ledger = ledger.get();
    const WorkflowRunSummary summary = WorkflowRuntime(std::move(options)).Run(def, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);

    const fs::path run_dir = WorkflowRunDir(*ledger, summary.run_id);
    const auto orchestration = KindsOf(run_dir / "workflow.jsonl");
    CHECK(std::count(orchestration.begin(), orchestration.end(), "workflow.node.retrying") == 1);
    CHECK(std::count(orchestration.begin(), orchestration.end(),
                     "workflow.loop.iteration.started") == 3);
    CHECK(std::count(orchestration.begin(), orchestration.end(),
                     "workflow.loop.iteration.completed") == 3);

    std::error_code ec;
    std::vector<std::string> node_files;
    for (const auto& entry : fs::directory_iterator(run_dir / "nodes", ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") {
            node_files.push_back(entry.path().filename().generic_string());
        }
    }
    std::sort(node_files.begin(), node_files.end());
    // flaky 两个 attempt 各一份文件;draft/inspect 各三轮(派发号 d 区分)。
    int flaky_attempts = 0;
    int draft_dispatches = 0;
    int inspect_dispatches = 0;
    for (const std::string& name : node_files) {
        if (name.find("-flaky-d1-a1") != std::string::npos) ++flaky_attempts;
        if (name.find("-draft-d") != std::string::npos) ++draft_dispatches;
        if (name.find("-inspect-d") != std::string::npos) ++inspect_dispatches;
    }
    CHECK(flaky_attempts == 1);
    CHECK(draft_dispatches == 3);
    CHECK(inspect_dispatches == 3);
    REQUIRE(node_files.size() == 8);
    // 重试的第一份收 run.failed,第二份收 run.completed。
    const auto a1 = KindsOf(run_dir / "nodes" /
                            (summary.run_id + "-flaky-d1-a1.jsonl"));
    const auto a2 = KindsOf(run_dir / "nodes" /
                            (summary.run_id + "-flaky-d1-a2.jsonl"));
    CHECK(a1.back() == "run.failed");
    CHECK(a2.back() == "run.completed");

    const auto report = ledger->VerifySession();
    CHECK(report.error_code.empty());
    CHECK(EveryEdgeOk(report));
}

TEST_CASE("runtime 集成 4:fail closed——开不出账停在明确失败态") {
    const char* yaml = R"YAML(
schema_version: 1
id: failclosed-traj
version: 1.0.0
name: fc
entry: x
nodes:
  x:
    type: transform
    operation: echo
  fin:
    type: end
edges:
  - { from: x, on: success, to: fin }
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);

    SUBCASE("编排账开张失败:run 停在明确失败态,不落一条 stream") {
        auto opened = OpenLedger(FreshDir("lubancode-wf-traj-runfail"),
                                 [] { return std::string("io.create_failed"); });
        REQUIRE(opened.has_value());
        auto ledger =
            std::make_unique<lubancode::runtime::TrajectorySessionLedger>(std::move(*opened));
        auto executor = std::make_shared<BehaviorExecutor>();
        RuntimeOptions options;
        options.executors[NodeKind::Transform] = executor;
        options.trajectory_ledger = ledger.get();
        const WorkflowRunSummary summary = WorkflowRuntime(std::move(options)).Run(def, RunInputs{});
        CHECK(summary.state == RunState::Failed);
        CHECK(summary.error_code == "trajectory_start_failed");
        CHECK(executor->calls["x"] == 0);  // fail closed:节点一步没跑
        CHECK(CountJsonlUnder(ledger->session_dir() / "workflows") == 0);
        CHECK(ledger->VerifySession().error_code.empty());
    }

    SUBCASE("node 账开张失败:节点失败收口,不留 0 字节,verify 过") {
        auto opened = OpenLedger(FreshDir("lubancode-wf-traj-nodefail"), {},
                                 [] { return std::string("io.create_failed"); });
        REQUIRE(opened.has_value());
        auto ledger =
            std::make_unique<lubancode::runtime::TrajectorySessionLedger>(std::move(*opened));
        auto executor = std::make_shared<BehaviorExecutor>();
        RuntimeOptions options;
        options.executors[NodeKind::Transform] = executor;
        options.trajectory_ledger = ledger.get();
        const WorkflowRunSummary summary = WorkflowRuntime(std::move(options)).Run(def, RunInputs{});
        CHECK(summary.state == RunState::Failed);
        CHECK(summary.error_code == "node_failed");
        const NodeRunRecord& record = summary.nodes.at("x");
        CHECK(record.error_code == "trajectory_node_start_failed");
        CHECK(executor->calls["x"] == 0);  // fail closed:执行器没被调
        // 编排账在场(run.started/failed + node.failed),node 账不在场。
        const fs::path workflows = ledger->session_dir() / "workflows";
        REQUIRE(CountJsonlUnder(workflows) == 1);  // 只有 workflow.jsonl
        const auto report = ledger->VerifySession();
        CHECK(report.error_code.empty());
        CHECK(EveryEdgeOk(report));
    }
}
