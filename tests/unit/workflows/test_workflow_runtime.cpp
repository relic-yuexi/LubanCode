// Workflows 单第 2 批:Store/resolver、状态机、RunJournal、顺序图最小闭环。
//
// 调度器测试用 fake executor/fake clock,不靠 sleep 赌时序(单子验收末条)。

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include "workflow/journal.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"
#include "workflow/store.hpp"
#include "workflow/validator.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_workflow_rt_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

// fake clock:手动拨。
class FakeClock : public lubancode::workflow::JournalClock {
public:
    std::int64_t now_ms_ = 1000000;
    std::int64_t NowMs() const override { return now_ms_; }
    void Advance(std::int64_t ms) { now_ms_ += ms; }
};

// fake executor:按脚本跑——第 N 次调用返回什么,可数调用次数。
class ScriptedExecutor : public lubancode::workflow::NodeExecutor {
public:
    struct Step {
        bool ok = true;
        std::string error_code;
        std::int64_t tokens = 0;
        nlohmann::json output = nlohmann::json::object();
    };
    std::vector<Step> script;   // 依次消耗;耗尽后重复最后一格
    std::vector<int> calls_per_node;

    lubancode::workflow::NodeExecResult Execute(const lubancode::workflow::NodeExecRequest& request) override {
        ++calls;
        calls_per_node.push_back(0);
        last_input = request.resolved_input;
        inputs.push_back(request.resolved_input);
        const std::size_t index = std::min(calls - 1, script.size() - 1);
        const Step& step = script[index];
        lubancode::workflow::NodeExecResult result;
        result.ok = step.ok;
        result.error_code = step.error_code;
        result.output = step.output;
        result.tokens_used = step.tokens;
        if (cancel_token != nullptr && cancel_after_calls > 0 && calls == cancel_after_calls) {
            cancel_token->store(true);
        }
        return result;
    }

    nlohmann::json last_input = nlohmann::json::object();
    std::vector<nlohmann::json> inputs;
    std::size_t calls = 0;
    std::atomic<bool>* cancel_token = nullptr;
    std::size_t cancel_after_calls = 0;
};

class AsyncProbeExecutor : public lubancode::workflow::NodeExecutor {
public:
    lubancode::workflow::NodeExecResult Execute(
        const lubancode::workflow::NodeExecRequest& request) override {
        worker = std::this_thread::get_id();
        saw_cancel_pointer = request.cancel != nullptr;
        started.store(true);
        if (wait_for_cancel) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (request.cancel != nullptr && !request.cancel->load() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            lubancode::workflow::NodeExecResult result;
            result.error_code = request.cancel != nullptr && request.cancel->load() ? "cancelled" : "timeout";
            return result;
        }
        lubancode::workflow::NodeExecResult result;
        result.ok = true;
        result.output = nlohmann::json{{"count", 3}};
        return result;
    }

    std::atomic<bool> started{false};
    bool wait_for_cancel = false;
    bool saw_cancel_pointer = false;
    std::thread::id worker;
};

// 一张顺序图:x -> y -> end。
lubancode::workflow::WorkflowDefinition MakeLinearDef() {
    const char* yaml = R"YAML(
schema_version: 1
id: linear-flow
version: 1.0.0
name: linear
entry: x
inputs:
  type: object
  required: [topic]
  properties:
    topic: { type: string }
nodes:
  x:
    type: transform
    operation: echo
    input: { q: "${inputs.topic}" }
  y:
    type: transform
    operation: echo
    input: { from_x: "${nodes.x.output.q}", n: 2 }
  fin:
    type: end
edges:
  - { from: x, on: success, to: y }
  - { from: y, on: success, to: fin }
result:
  topic: "${inputs.topic}"
  x_q: "${nodes.x.output.q}"
)YAML";
    auto parsed = lubancode::workflow::ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_SUITE("workflows-runtime") {
TEST_CASE("状态机:迁移表合法与非法") {
    using lubancode::workflow::IsValidRunTransition;
    using RS = lubancode::workflow::RunState;
    CHECK(IsValidRunTransition(RS::Created, RS::Validating));
    CHECK(IsValidRunTransition(RS::Validating, RS::Ready));
    CHECK(IsValidRunTransition(RS::Ready, RS::Running));
    CHECK(IsValidRunTransition(RS::Running, RS::Succeeded));
    CHECK(IsValidRunTransition(RS::Running, RS::Cancelled));
    CHECK(IsValidRunTransition(RS::Running, RS::WaitingApproval));
    CHECK(IsValidRunTransition(RS::Running, RS::WaitingIo));
    CHECK(IsValidRunTransition(RS::WaitingIo, RS::Running));
    CHECK(IsValidRunTransition(RS::WaitingApproval, RS::Running));
    CHECK_FALSE(IsValidRunTransition(RS::Created, RS::Running));    // 跳步
    CHECK_FALSE(IsValidRunTransition(RS::Succeeded, RS::Running));  // 终态不出
    CHECK_FALSE(IsValidRunTransition(RS::Failed, RS::Succeeded));
    CHECK_FALSE(IsValidRunTransition(RS::Running, RS::Created));

    using lubancode::workflow::IsValidNodeTransition;
    using NS = lubancode::workflow::NodeState;
    CHECK(IsValidNodeTransition(NS::Pending, NS::Ready));
    CHECK(IsValidNodeTransition(NS::Running, NS::RetryWait));
    CHECK(IsValidNodeTransition(NS::Running, NS::WaitingIo));
    CHECK(IsValidNodeTransition(NS::RetryWait, NS::Ready));
    CHECK(IsValidNodeTransition(NS::Running, NS::Succeeded));
    CHECK_FALSE(IsValidNodeTransition(NS::Succeeded, NS::Running));
    CHECK_FALSE(IsValidNodeTransition(NS::Pending, NS::Running));  // 不许跳 ready
    CHECK_FALSE(IsValidNodeTransition(NS::Failed, NS::Ready));
}

TEST_CASE("Store:CommitOutput 只写一次,恢复往返保真") {
    lubancode::workflow::Store store;
    store.Initialize(nlohmann::json{{"topic", "gqn"}}, nlohmann::json{{"run_id", "r1"}});
    CHECK(store.CommitOutput("x", nlohmann::json{{"q", "v1"}}));
    CHECK_FALSE(store.CommitOutput("x", nlohmann::json{{"q", "v2"}}));  // 二次拒
    CHECK(store.GetOutput("x").value()["q"] == "v1");
    store.UpdateMeta("x", nlohmann::json{{"attempt", 2}});
    store.UpdateMeta("x", nlohmann::json{{"duration_ms", 42}});

    const nlohmann::json saved = store.ToJson();
    lubancode::workflow::Store restored = lubancode::workflow::Store::FromJson(saved);
    CHECK(restored.GetOutput("x").value()["q"] == "v1");
    CHECK(restored.GetMeta("x").value()["attempt"] == 2);
    CHECK(restored.GetMeta("x").value()["duration_ms"] == 42);
    CHECK(restored.inputs() == store.inputs());
}

TEST_CASE("Resolver:${...} 好坏路径") {
    using namespace lubancode::workflow;
    Store store;
    store.Initialize(nlohmann::json{{"topic", "量子纠错"}, {"limit", 40}},
                     nlohmann::json{{"run_id", "r1"}});
    store.CommitOutput("parse", nlohmann::json{{"query", "quantum error correction"},
                                               {"items", nlohmann::json::array({"a", "b"})}});

    SUBCASE("inputs 与 nodes 引用") {
        auto a = ResolveRef(store, "inputs.topic");
        REQUIRE(a.has_value());
        CHECK(*a == "量子纠错");
        auto b = ResolveRef(store, "nodes.parse.output.items.1");
        REQUIRE(b.has_value());
        CHECK(*b == "b");
        auto c = ResolveRef(store, "run.run_id");
        REQUIRE(c.has_value());
        CHECK(*c == "r1");
    }
    SUBCASE("缺字段与未来节点") {
        auto bad = ResolveRef(store, "inputs.nope");
        CHECK_FALSE(bad.has_value());
        CHECK(bad.error().message.find("inputs") != std::string::npos);
        auto future = ResolveRef(store, "nodes.ghost.output.x");
        CHECK_FALSE(future.has_value());
        CHECK(future.error().message.find("ghost") != std::string::npos);
    }
    SUBCASE("认不得的前缀与裸 nodes 引用") {
        CHECK_FALSE(ResolveRef(store, "env.HOME").has_value());
        auto bare = ResolveRef(store, "nodes.parse");
        CHECK_FALSE(bare.has_value());
    }
    SUBCASE("混排与整值") {
        auto mixed = ResolveTemplate(store, nlohmann::json("q=${inputs.topic} n=${inputs.limit}"));
        REQUIRE(mixed.has_value());
        CHECK(mixed->value.get<std::string>() == "q=量子纠错 n=40");
        // 整值单引用保持类型(数组不落字符串)。
        auto whole = ResolveTemplate(store, nlohmann::json("${nodes.parse.output.items}"));
        REQUIRE(whole.has_value());
        CHECK(whole->value.is_array());
        CHECK(whole->value.size() == 2);
    }
}

TEST_CASE("RunJournal:事件序列化/坏行/脱敏/checkpoint 原子") {
    TempDir tmp;
    using namespace lubancode::workflow;
    FakeClock clock;

    // 脱敏纯函数。
    const nlohmann::json dirty = nlohmann::json{{"api_key", "sk-123"}, {"nested", nlohmann::json{{"auth_token", "x"}}}};
    const nlohmann::json clean = SanitizeJournalPayload(dirty);
    CHECK(clean["api_key"] == "[已打码]");
    CHECK(clean["nested"]["auth_token"] == "[已打码]");

    // Start -> 事件 -> checkpoint -> Finish。
    RunJournal::StartInfo info;
    info.run_id = "run-test-1";
    info.workflow_id = "linear-flow";
    info.workflow_version = "1.0.0";
    info.content_hash = "abc123";
    info.cwd = "/tmp";
    info.definition_json = R"({"id":"linear-flow"})";
    auto journal = RunJournal::Start(tmp.Get(), info, &clock);
    REQUIRE(journal.has_value());
    journal->Append(kEventNodeStarted, "x", 1, nlohmann::json{{"kind", "transform"}});
    journal->Append(kEventNodeCompleted, "x", 1, nlohmann::json{{"output", nlohmann::json{{"q", "v"}}}});
    journal->SaveCheckpoint(journal->last_seq(), nlohmann::json{{"inputs", nlohmann::json{{"token", "sk-1"}}}});
    journal->Finish("succeeded", nlohmann::json{{"tokens", 100}});

    // 读回:事件齐、seq 单调、checkpoint 值已打码。
    const std::vector<JournalEvent> events = ReadJournalEvents(tmp.Get() / "run-test-1");
    REQUIRE(events.size() == 5);  // run_started + node_started + node_completed + checkpoint + run_completed
    CHECK(events[0].type == kEventRunStarted);
    CHECK(events[0].seq == 1);
    CHECK(events[3].type == kEventCheckpointSaved);
    CHECK(events[4].type == kEventRunCompleted);
    CHECK(events[4].data["state"] == "succeeded");

    const auto cp = ReadLatestCheckpoint(tmp.Get() / "run-test-1");
    REQUIRE(cp.has_value());
    CHECK((*cp)["inputs"]["token"] == "[已打码]");

    // manifest 落了终态。
    const std::vector<RunStatus> runs = ListRuns(tmp.Get());
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].final_state == "succeeded");
    CHECK(runs[0].content_hash == "abc123");

    // ReplayNodes:completed 的节点有 output。
    const auto replayed = ReplayNodes(events);
    CHECK(replayed.count("x") == 1);
    CHECK(replayed.at("x").state == "succeeded");
    CHECK(replayed.at("x").output["q"] == "v");

    // 坏行跳过:往 events.jsonl 追一行半截的。
    {
        std::ofstream file(tmp.Get() / "run-test-1" / "events.jsonl", std::ios::binary | std::ios::app);
        file << R"({"seq":99,"ts":123,)";  // 半截
    }
    const std::vector<JournalEvent> after = ReadJournalEvents(tmp.Get() / "run-test-1");
    CHECK(after.size() == events.size());
}

TEST_CASE("顺序图最小闭环:run 走通,result 映射,store 传值") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = MakeLinearDef();

    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [](const nlohmann::json& in) { return in; });

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = echo;
    options.run_id_generator = [] { return "run-seq-1"; };
    WorkflowRuntime runtime(options);

    RunInputs inputs;
    inputs.values = nlohmann::json{{"topic", "graph neural networks"}};
    const WorkflowRunSummary summary = runtime.Run(def, inputs);

    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(summary.result["topic"] == "graph neural networks");
    CHECK(summary.result["x_q"] == "graph neural networks");
    CHECK(summary.nodes.at("x").state == NodeState::Succeeded);
    CHECK(summary.nodes.at("y").state == NodeState::Succeeded);
}

TEST_CASE("必填输入缺失:开跑前报 invalid_inputs,不带出半场") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = MakeLinearDef();
    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [](const nlohmann::json& in) { return in; });
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = echo;
    WorkflowRuntime runtime(options);

    RunInputs inputs;  // topic 缺
    const auto summary = runtime.Run(def, inputs);
    CHECK(summary.state == RunState::Failed);
    CHECK(summary.error_code == "invalid_inputs");
    CHECK(summary.nodes.empty());  // 一个节点都没跑
}

TEST_CASE("RunInputs 聚合初始化可用") {
    lubancode::workflow::RunInputs inputs;
    inputs.values = nlohmann::json{{"a", 1}};
    CHECK(inputs.values["a"] == 1);
}

TEST_CASE("类型不合:integer 字段给了字符串,开跑前拒") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: typed
version: 1.0.0
name: t
entry: x
inputs:
  type: object
  required: [limit]
  properties:
    limit: { type: integer, minimum: 1 }
nodes:
  x:
    type: transform
    operation: echo
  fin:
    type: end
edges:
  - { from: x, on: success, to: fin }
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [](const nlohmann::json& in) { return in; });
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = echo;
    WorkflowRuntime runtime(options);
    RunInputs inputs;
    inputs.values = nlohmann::json{{"limit", "forty"}};
    const auto summary = runtime.Run(*parsed, inputs);
    CHECK(summary.state == RunState::Failed);
    CHECK(summary.error_code == "invalid_inputs");
}

TEST_CASE("重试:可重试 code 走满 attempts,不可重试 code 立刻收") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: retry-flow
version: 1.0.0
name: r
entry: x
limits:
  max_steps: 8
nodes:
  x:
    type: transform
    operation: echo
    retry: { attempts: 3, backoff: fixed, initial: 0s, when: [rate_limited] }
  fin:
    type: end
edges:
  - { from: x, on: success, to: fin }
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());

    SUBCASE("rate_limited 重试 3 次") {
        auto executor = std::make_shared<ScriptedExecutor>();
        executor->script = {{false, "rate_limited"}, {false, "rate_limited"}, {true, "", 0, {{"ok", 1}}}};
        RuntimeOptions options;
        options.executors[NodeKind::Transform] = executor;
        WorkflowRuntime runtime(options);
        const auto summary = runtime.Run(*parsed, RunInputs{});
        REQUIRE(summary.state == RunState::Succeeded);
        CHECK(executor->calls == 3);
    }
    SUBCASE("不可重试 code 一次就收") {
        auto executor = std::make_shared<ScriptedExecutor>();
        executor->script = {{false, "validation_error"}};
        RuntimeOptions options;
        options.executors[NodeKind::Transform] = executor;
        WorkflowRuntime runtime(options);
        const auto summary = runtime.Run(*parsed, RunInputs{});
        CHECK(summary.state == RunState::Failed);
        CHECK(executor->calls == 1);
        CHECK(summary.error_code == "node_failed");
    }
}

TEST_CASE("取消:cancel_token 打断,run 收 cancelled") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = MakeLinearDef();
    auto executor = std::make_shared<ScriptedExecutor>();
    executor->script = {{false, "validation_error"}};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);

    std::atomic<bool> cancel{true};  // 起跑前就取消
    const auto summary = runtime.Run(def, RunInputs(nlohmann::json{{"topic", std::string("x")}}), &cancel);
    CHECK(summary.state == RunState::Cancelled);
}

TEST_CASE("普通回边:runtime 与 validator 同口径拒绝") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: loop-flow
version: 1.0.0
name: l
entry: x
limits:
  max_steps: 3
nodes:
  x:
    type: transform
    operation: echo
  y:
    type: transform
    operation: echo
edges:
  - { from: x, on: success, to: y }
  - { from: y, on: success, to: x }
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [](const nlohmann::json& in) { return in; });
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = echo;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(*parsed, RunInputs{});
    CHECK(summary.state == RunState::Failed);
    CHECK(summary.error_code == "invalid_definition");
    CHECK(summary.error_message.find("普通边不许回环") != std::string::npos);
}

TEST_CASE("loop:按条件早停,上一轮输出传回,逐轮历史落账") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: dynamic-review
version: 1.0.0
entry: review
limits: { max_steps: 20 }
nodes:
  review:
    type: loop
    body: [draft, inspect]
    until: { op: equals, path: "${nodes.inspect.output.approved}", value: true }
    min_iterations: 1
    max_iterations: 5
    hard_limit: 8
  draft:
    type: transform
    operation: echo
    input:
      previous: "${nodes.review.output.previous}"
  inspect:
    type: transform
    operation: echo
    input:
      draft: "${nodes.draft.output}"
  fin: { type: end }
  exhausted: { type: end }
edges:
  - { from: review, on: success, to: fin }
  - { from: review, on: exhausted, to: exhausted }
result:
  review: "${nodes.review.output}"
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    auto scripted = std::make_shared<ScriptedExecutor>();
    scripted->script = {
        {true, "", 0, nlohmann::json{{"draft", 1}}},
        {true, "", 0, nlohmann::json{{"approved", false}}},
        {true, "", 0, nlohmann::json{{"draft", 2}}},
        {true, "", 0, nlohmann::json{{"approved", true}}},
    };
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = scripted;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(*parsed, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(scripted->calls == 4);
    REQUIRE(scripted->inputs.size() == 4);
    CHECK(scripted->inputs[0]["previous"].is_null());
    CHECK(scripted->inputs[2]["previous"]["outputs"]["inspect"]["approved"] == false);
    CHECK(summary.result["review"]["completed_iterations"] == 2);
    CHECK(summary.result["review"]["condition_met"] == true);
    CHECK(summary.result["review"]["history"].size() == 2);
}

TEST_CASE("loop:每轮可先跑 parallel 分支再由后续节点合案") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: parallel-plans
version: 1.0.0
entry: review
limits: { max_steps: 20 }
nodes:
  review:
    type: loop
    body: [plans, merge, gate]
    until: { op: equals, path: "${nodes.gate.output.complete}", value: true }
    max_iterations: 2
    hard_limit: 3
  plans:
    type: parallel
    branches: [a, b, c]
    join: all
  a: { type: transform, operation: echo, input: { name: a } }
  b: { type: transform, operation: echo, input: { name: b } }
  c: { type: transform, operation: echo, input: { name: c } }
  merge:
    type: transform
    operation: echo
    input:
      candidates:
        - "${nodes.a.output}"
        - "${nodes.b.output}"
        - "${nodes.c.output}"
  gate: { type: transform, operation: approve }
  fin: { type: end }
  exhausted: { type: end }
edges:
  - { from: review, on: success, to: fin }
  - { from: review, on: exhausted, to: exhausted }
result:
  merged: "${nodes.merge.output}"
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    const auto validation = ValidateDefinition(*parsed, std::nullopt);
    CHECK(validation.ok());

    auto transforms = std::make_shared<TransformExecutor>();
    transforms->Register("echo", [](const nlohmann::json& input) { return input; });
    transforms->Register("approve", [](const nlohmann::json&) {
        return nlohmann::json{{"complete", true}};
    });
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = transforms;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(*parsed, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);
    REQUIRE(summary.result["merged"]["candidates"].size() == 3);
    CHECK(summary.result["merged"]["candidates"][0]["name"] == "a");
    CHECK(summary.result["merged"]["candidates"][2]["name"] == "c");
}

TEST_CASE("loop:输入决定软帽,撞帽走 exhausted") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: bounded-review
version: 1.0.0
entry: review
inputs:
  type: object
  properties:
    review_limit: { type: integer, default: 2 }
nodes:
  review:
    type: loop
    body: [inspect]
    until: { op: equals, path: "${nodes.inspect.output.approved}", value: true }
    max_iterations: "${inputs.review_limit}"
    hard_limit: 4
  inspect: { type: transform, operation: echo }
  approved: { type: end }
  exhausted: { type: end }
edges:
  - { from: review, on: success, to: approved }
  - { from: review, on: exhausted, to: exhausted }
result:
  review: "${nodes.review.output}"
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    auto scripted = std::make_shared<ScriptedExecutor>();
    scripted->script = {{true, "", 0, nlohmann::json{{"approved", false}}}};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = scripted;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(*parsed, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(scripted->calls == 2);
    CHECK(summary.result["review"]["completed_iterations"] == 2);
    CHECK(summary.result["review"]["exhausted"] == true);
}

TEST_CASE("loop:取消后从完整轮 checkpoint 续跑") {
    using namespace lubancode::workflow;
    TempDir tmp;
    const char* yaml = R"YAML(
schema_version: 1
id: resumable-review
version: 1.0.0
entry: review
nodes:
  review:
    type: loop
    body: [draft, inspect]
    until: { op: equals, path: "${nodes.inspect.output.approved}", value: true }
    max_iterations: 3
    hard_limit: 5
  draft:
    type: transform
    operation: echo
    input: { previous: "${nodes.review.output.previous}" }
  inspect: { type: transform, operation: echo }
  fin: { type: end }
  exhausted: { type: end }
edges:
  - { from: review, on: success, to: fin }
  - { from: review, on: exhausted, to: exhausted }
result:
  review: "${nodes.review.output}"
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());

    std::atomic<bool> cancel{false};
    auto first = std::make_shared<ScriptedExecutor>();
    first->script = {
        {true, "", 0, nlohmann::json{{"draft", 1}}},
        {true, "", 0, nlohmann::json{{"approved", false}}},
    };
    first->cancel_token = &cancel;
    first->cancel_after_calls = 2;
    RuntimeOptions first_options;
    first_options.executors[NodeKind::Transform] = first;
    first_options.runs_root = tmp.Get();
    first_options.run_id_generator = [] { return "run-loop-resume"; };
    WorkflowRuntime runner(first_options);
    const auto interrupted = runner.Run(*parsed, RunInputs{}, &cancel);
    REQUIRE(interrupted.state == RunState::Cancelled);
    CHECK(first->calls == 2);

    cancel.store(false);
    auto second = std::make_shared<ScriptedExecutor>();
    second->script = {
        {true, "", 0, nlohmann::json{{"draft", 2}}},
        {true, "", 0, nlohmann::json{{"approved", true}}},
    };
    RuntimeOptions resume_options;
    resume_options.executors[NodeKind::Transform] = second;
    resume_options.runs_root = tmp.Get();
    resume_options.run_id_generator = [] { return "run-loop-resume"; };
    WorkflowRuntime resumer(resume_options);
    const auto resumed = resumer.Resume(tmp.Get() / "run-loop-resume");
    REQUIRE(resumed.has_value());
    REQUIRE(resumed->state == RunState::Succeeded);
    CHECK(second->calls == 2);
    CHECK(resumed->result["review"]["completed_iterations"] == 2);
    CHECK(second->inputs[0]["previous"]["iteration"] == 1);
}

TEST_CASE("时限:fake clock 越线收 timeout") {
    using namespace lubancode::workflow;
    WorkflowDefinition def;
    def.schema_version = 1;
    def.id = "slow-flow";
    def.version = "1.0.0";
    def.entry = "x";
    def.limits.timeout_secs = 10;
    WorkflowNode x;
    x.id = "x";
    x.kind = NodeKind::Transform;
    x.operation = "echo";
    WorkflowNode y;
    y.id = "y";
    y.kind = NodeKind::Transform;
    y.operation = "echo";
    WorkflowNode fin;
    fin.id = "fin";
    fin.kind = NodeKind::End;
    def.nodes = {x, y, fin};
    def.node_map.emplace("x", x);
    def.node_map.emplace("y", y);
    def.node_map.emplace("fin", fin);
    def.edges.push_back(WorkflowEdge{"x", "success", "y"});
    def.edges.push_back(WorkflowEdge{"y", "success", "fin"});
    def.normalized = BuildNormalizedJson(def);

    auto clock = std::make_shared<FakeClock>();
    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [&clock](const nlohmann::json& in) {
        clock->Advance(11000);  // 每步 11s:第二步前的时限检查必越线
        return in;
    });
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = echo;
    options.clock = clock;
    WorkflowRuntime runtime(options);

    const auto summary = runtime.Run(def, RunInputs{});
    CHECK(summary.state == RunState::BudgetExhausted);
    CHECK(summary.error_code == "timeout");
}

TEST_CASE("error 边与 fallback 边:失败走明边,不静默换") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: fallback-flow
version: 1.0.0
name: f
entry: primary
nodes:
  primary:
    type: transform
    operation: echo
    fallback_to: backup
  backup:
    type: transform
    operation: echo
  fin:
    type: end
edges:
  - { from: primary, on: success, to: fin }
  - { from: backup, on: success, to: fin }
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    auto executor = std::make_shared<ScriptedExecutor>();
    executor->script = {{false, "boom"}, {true, "", 0, {{"from", "backup"}}}};
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(*parsed, RunInputs{});
    CHECK(summary.state == RunState::Succeeded);
    CHECK(summary.nodes.at("primary").state == NodeState::Failed);
    CHECK(summary.nodes.at("backup").state == NodeState::Succeeded);
}

TEST_CASE("template 执行器:{{path}} 安全渲染,缺字段空串") {
    using namespace lubancode::workflow;
    TemplateExecutor executor;
    NodeExecRequest request;
    request.node = nullptr;
    request.resolved_input = nlohmann::json{
        {"render", "# {{title}}\n共 {{count}} 条\n缺的: {{nope}}"},
        {"data", nlohmann::json{{"title", "报告"}, {"count", 3}}}};
    const auto result = executor.Execute(request);
    REQUIRE(result.ok);
    CHECK(result.output["rendered"].get<std::string>() == "# 报告\n共 3 条\n缺的: ");
}

TEST_CASE("run 落 journal:headless 测试不落盘,配 runs_root 落盘") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = MakeLinearDef();
    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [](const nlohmann::json& in) { return in; });

    TempDir tmp;
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = echo;
    options.runs_root = tmp.Get();
    options.run_id_generator = [] { return "run-journal-1"; };
    WorkflowRuntime runtime(options);
    RunInputs journal_inputs(nlohmann::json{{"topic", std::string("x")}});
    const auto summary = runtime.Run(def, journal_inputs);
    CHECK(summary.state == RunState::Succeeded);

    const std::vector<RunStatus> runs = ListRuns(tmp.Get());
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].run_id == "run-journal-1");
    CHECK(runs[0].final_state == "succeeded");
    CHECK(runs[0].definition.contains("id"));
    // definition 快照能还原定义(同 hash)。
    const WorkflowDefinition restored = WorkflowDefinition::FromJson(runs[0].definition);
    CHECK(ContentHash(restored) == ContentHash(def));
}

TEST_CASE("async:I/O body 换工作线程,产物挂回外壳节点") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: async-inbox
version: 1.0.0
entry: wait
nodes:
  wait: { type: async, body: check }
  check: { type: transform, operation: probe }
  fin: { type: end }
edges:
  - { from: wait, on: success, to: fin }
result:
  count: "${nodes.wait.output.count}"
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    const auto validation = ValidateDefinition(*parsed, std::nullopt);
    REQUIRE(validation.ok());

    auto probe = std::make_shared<AsyncProbeExecutor>();
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = probe;
    WorkflowRuntime runtime(options);
    const std::thread::id caller = std::this_thread::get_id();
    const auto summary = runtime.Run(*parsed, RunInputs{});

    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(probe->worker != caller);
    CHECK(probe->saw_cancel_pointer);
    CHECK(summary.nodes.at("wait").state == NodeState::Succeeded);
    CHECK(summary.nodes.at("check").state == NodeState::Succeeded);
    CHECK(summary.result["count"] == 3);
}

TEST_CASE("async:外部取消传进 I/O body,整场收 cancelled") {
    using namespace lubancode::workflow;
    const char* yaml = R"YAML(
schema_version: 1
id: async-cancel
version: 1.0.0
entry: wait
nodes:
  wait: { type: async, body: check }
  check: { type: transform, operation: probe }
)YAML";
    auto parsed = ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());

    std::atomic<bool> cancel{false};
    auto probe = std::make_shared<AsyncProbeExecutor>();
    probe->wait_for_cancel = true;
    RuntimeOptions options;
    options.executors[NodeKind::Transform] = probe;
    WorkflowRuntime runtime(options);
    std::thread canceller([&] {
        while (!probe->started.load()) std::this_thread::yield();
        cancel.store(true);
    });
    const auto summary = runtime.Run(*parsed, RunInputs{}, &cancel);
    canceller.join();

    CHECK(summary.state == RunState::Cancelled);
    CHECK(probe->saw_cancel_pointer);
    CHECK(summary.nodes.at("wait").state == NodeState::Cancelled);
}
}  // TEST_SUITE(workflows-runtime)
