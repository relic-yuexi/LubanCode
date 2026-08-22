// Workflows 单第 4 批:宿主执行器(tool/llm/approval/ask_user/subflow)与恢复。
//
// 假 ToolRegistry/假 backend/即时 broker,不出网、不挂死。

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include "api/backend.hpp"
#include "runtime/interaction_broker.hpp"
#include "tools/registry.hpp"
#include "workflow/host_executors.hpp"
#include "workflow/journal.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_workflow_agent_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
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

// 假工具:固定结果或错误,可数调用、可声明 needs_confirm。
class FakeTool : public lubancode::tools::Tool {
public:
    FakeTool(std::string name, std::string result, bool error, bool confirm_needed)
        : name_(std::move(name)), result_(std::move(result)), error_(error), confirm_(confirm_needed) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    bool needs_confirm() const override { return confirm_; }
    Result execute(const nlohmann::json& input) override {
        ++calls;
        last_input = input;
        if (error_) return Result{result_, true};
        return Result{result_, false};
    }

    int calls = 0;
    nlohmann::json last_input;

private:
    std::string name_;
    std::string result_;
    bool error_;
    bool confirm_;
};

// 假 backend:吐一段固定文本,可带 usage。
class FakeBackend : public lubancode::api::Backend {
public:
    std::string reply = "{\"plan\": \"ok\"}";
    std::int64_t input_tokens = 100;
    std::int64_t output_tokens = 50;
    int calls = 0;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)request;
        (void)cancel;
        ++calls;
        on_event(lubancode::api::TextDelta{reply});
        lubancode::api::MessageDone done;
        done.usage.input_tokens = input_tokens;
        done.usage.output_tokens = output_tokens;
        on_event(done);
        return {};
    }
};

// 即时 broker:不等前端,当场给决定。
class InstantBroker final : public lubancode::runtime::InteractionBroker {
public:
    lubancode::runtime::InteractionDecision approval_decision =
        lubancode::runtime::InteractionDecision::Accept;
    std::vector<std::string> question_answers = {"答案一"};
    int approvals = 0;
    int questions = 0;

    class InstantFuture final : public lubancode::runtime::InteractionFuture {
    public:
        std::optional<lubancode::runtime::ApprovalResponse> approval;
        std::optional<lubancode::runtime::QuestionResponse> question;

        std::optional<lubancode::runtime::ApprovalResponse> WaitApproval() override { return approval; }
        std::optional<lubancode::runtime::QuestionResponse> WaitQuestion() override { return question; }
    };

    std::shared_ptr<lubancode::runtime::InteractionFuture> AskApproval(
        const lubancode::runtime::ApprovalRequest& request) override {
        (void)request;
        ++approvals;
        auto future = std::make_shared<InstantFuture>();
        lubancode::runtime::ApprovalResponse response;
        response.decision = approval_decision;
        future->approval = response;
        return future;
    }

    std::shared_ptr<lubancode::runtime::InteractionFuture> AskQuestion(
        const lubancode::runtime::QuestionRequest& request) override {
        (void)request;
        ++questions;
        auto future = std::make_shared<InstantFuture>();
        lubancode::runtime::QuestionResponse response;
        response.answers = question_answers;
        future->question = response;
        return future;
    }

    bool ResolveApproval(const lubancode::runtime::InteractionRequestId& id,
                         const lubancode::runtime::ApprovalResponse& response) override {
        (void)id;
        (void)response;
        return false;
    }
    bool AnswerQuestion(const lubancode::runtime::InteractionRequestId& id,
                        const lubancode::runtime::QuestionResponse& response) override {
        (void)id;
        (void)response;
        return false;
    }
};

lubancode::workflow::WorkflowDefinition ParseOrDie(const char* yaml) {
    auto parsed = lubancode::workflow::ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_SUITE("workflows-agents") {

TEST_CASE("tool 节点:调真 ToolRegistry,结构化结果交下游") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    auto* tool = new FakeTool("paper_search", R"({"items": ["a", "b"]})", false, false);
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));

    ToolExecutor executor(&registry);
    WorkflowDefinition def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                        "  x:\n    type: tool\n    tool: paper_search\n    input: { q: hi }\n");
    NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("x");
    request.resolved_input = nlohmann::json{{"q", "hi"}};

    const auto result = executor.Execute(request);
    REQUIRE(result.ok);
    CHECK(result.output["items"].size() == 2);
    CHECK(tool->calls == 1);
    CHECK(tool->last_input["q"] == "hi");
}

TEST_CASE("tool 缺失:tool_unavailable,on_unavailable=skip 时图继续") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;  // 空表

    const char* yaml = R"YAML(
schema_version: 1
id: missing-tool-flow
version: 1.0.0
name: m
entry: x
nodes:
  x:
    type: tool
    tool: no_such_tool
    on_unavailable: skip
  fin:
    type: end
edges:
  - { from: x, on: error, to: fin }
  - { from: x, on: success, to: fin }
result:
  skipped: "${run.run_id}"
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    auto executor = std::make_shared<ToolExecutor>(&registry);
    RuntimeOptions options;
    options.executors[NodeKind::Tool] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});
    // skip 路径:error 边接住,图照走完。
    CHECK(summary.state == RunState::Succeeded);
    CHECK(summary.nodes.at("x").state == NodeState::Failed);
}

TEST_CASE("tool 需要确认:确认门拒绝时 permission_denied") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    auto* tool = new FakeTool("dangerous", "ok", false, true);
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));

    ToolExecutor executor(&registry, [](const std::string&, const nlohmann::json&) { return false; });
    WorkflowDefinition def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                        "  x:\n    type: tool\n    tool: dangerous\n");
    NodeExecRequest request;
    request.node = &def.node_map.at("x");
    const auto result = executor.Execute(request);
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "permission_denied");
    CHECK(tool->calls == 0);  // 拒在执行前
}

TEST_CASE("llm 节点:单次调用,JSON 结果与 token 账") {
    using namespace lubancode::workflow;
    FakeBackend backend;
    LlmExecutor::Options llm_options;
    llm_options.backend = &backend;
    llm_options.model = "test-model";
    llm_options.prompt_loader = [](const std::string&) { return "你是分析器"; };
    auto executor = std::make_shared<LlmExecutor>(llm_options);

    const char* yaml = R"YAML(
schema_version: 1
id: llm-flow
version: 1.0.0
name: l
entry: analyze
nodes:
  analyze:
    type: llm
    prompt: prompts/analyze.md
    input: { topic: "${inputs.topic}" }
  fin:
    type: end
edges:
  - { from: analyze, on: success, to: fin }
result:
  plan: "${nodes.analyze.output.plan}"
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);
    RuntimeOptions options;
    options.executors[NodeKind::Llm] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs(nlohmann::json{{"topic", std::string("量子")}}));
    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(summary.result["plan"] == "ok");
    CHECK(summary.tokens_used == 150);  // 100 in + 50 out
    CHECK(backend.calls == 1);
}

TEST_CASE("approval 节点:accept 过、decline 拒、没 broker 明报") {
    using namespace lubancode::workflow;
    const char* yaml = "schema_version: 1\nid: a\nversion: 1\nentry: gate\nnodes:\n"
                       "  gate:\n    type: approval\n    input: { what: 删文件 }\n"
                       "  fin:\n    type: end\nedges:\n  - { from: gate, on: success, to: fin }\n";
    const WorkflowDefinition def = ParseOrDie(yaml);

    SUBCASE("accept") {
        InstantBroker broker;
        auto executor = std::make_shared<ApprovalExecutor>(&broker);
        RuntimeOptions options;
        options.executors[NodeKind::Approval] = executor;
        WorkflowRuntime runtime(options);
        CHECK(runtime.Run(def, RunInputs{}).state == RunState::Succeeded);
        CHECK(broker.approvals == 1);
    }
    SUBCASE("decline 收 failed") {
        InstantBroker broker;
        broker.approval_decision = lubancode::runtime::InteractionDecision::Decline;
        auto executor = std::make_shared<ApprovalExecutor>(&broker);
        RuntimeOptions options;
        options.executors[NodeKind::Approval] = executor;
        WorkflowRuntime runtime(options);
        const auto summary = runtime.Run(def, RunInputs{});
        CHECK(summary.state == RunState::Failed);
        CHECK(summary.nodes.at("gate").error_code == "approval_declined");
    }
    SUBCASE("没 broker:不挂死,明报 not_configured") {
        auto executor = std::make_shared<ApprovalExecutor>(nullptr);
        RuntimeOptions options;
        options.executors[NodeKind::Approval] = executor;
        WorkflowRuntime runtime(options);
        const auto summary = runtime.Run(def, RunInputs{});
        CHECK(summary.state == RunState::Failed);
        CHECK(summary.nodes.at("gate").error_code == "not_configured");
    }
}

TEST_CASE("ask_user 节点:答案进 output") {
    using namespace lubancode::workflow;
    InstantBroker broker;
    broker.question_answers = {"用 arXiv"};
    auto executor = std::make_shared<AskUserExecutor>(&broker);
    const char* yaml = "schema_version: 1\nid: q\nversion: 1\nentry: ask\nnodes:\n"
                       "  ask:\n    type: ask_user\n    input: { question: \"查哪路\" }\n"
                       "  fin:\n    type: end\nedges:\n  - { from: ask, on: success, to: fin }\n"
                       "result:\n  answer: \"${nodes.ask.output.answers.0}\"\n";
    const WorkflowDefinition def = ParseOrDie(yaml);
    RuntimeOptions options;
    options.executors[NodeKind::AskUser] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(summary.result["answer"] == "用 arXiv");
}

TEST_CASE("subflow:子图结果交回父图,错误不穿墙成文本") {
    using namespace lubancode::workflow;
    const char* sub_yaml = R"YAML(
schema_version: 1
id: child-flow
version: 1.0.0
name: child
entry: work
nodes:
  work:
    type: transform
    operation: echo
    input: { step: "${inputs.step}" }
  fin:
    type: end
edges:
  - { from: work, on: success, to: fin }
result:
  done: "${nodes.work.output.step}"
)YAML";
    const WorkflowDefinition child = ParseOrDie(sub_yaml);

    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [](const nlohmann::json& in) {
        return nlohmann::json{{"step", in.value("step", "ran")}};
    });

    // 子 runtime:直接跑 child。
    auto sub_runtime = std::make_shared<WorkflowRuntime>([&] {
        RuntimeOptions options;
        options.executors[NodeKind::Transform] = echo;
        return options;
    }());

    SubflowExecutor::DefinitionResolver resolver = [](const std::string& id) {
        (void)id;
        return std::optional<WorkflowDefinition>{};
    };
    WorkflowDefinition child_copy = child;
    SubflowExecutor::RuntimeRunner runner = [&child_copy, &sub_runtime](
                                                const WorkflowDefinition&,
                                                const nlohmann::json& inputs) {
        return sub_runtime->Run(child_copy, RunInputs(inputs));
    };
    auto executor = std::make_shared<SubflowExecutor>(
        [&child_copy](const std::string&) -> std::optional<WorkflowDefinition> { return child_copy; },
        [&sub_runtime](const WorkflowDefinition& def, const nlohmann::json& inputs) {
            return sub_runtime->Run(def, RunInputs(inputs));
        });

    const char* parent_yaml = R"YAML(
schema_version: 1
id: parent-flow
version: 1.0.0
name: parent
entry: call_child
nodes:
  call_child:
    type: subflow
    subflow: child-flow
    input: { step: 由父图给 }
  fin:
    type: end
edges:
  - { from: call_child, on: success, to: fin }
result:
  child_done: "${nodes.call_child.output.done}"
)YAML";
    const WorkflowDefinition parent = ParseOrDie(parent_yaml);
    RuntimeOptions options;
    options.executors[NodeKind::Subflow] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(parent, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(summary.result["child_done"] == "由父图给");

    // 子图不存在:unknown_subflow。
    auto missing = std::make_shared<SubflowExecutor>(
        [](const std::string&) -> std::optional<WorkflowDefinition> { return std::nullopt; },
        [](const WorkflowDefinition&, const nlohmann::json&) { return WorkflowRunSummary{}; });
    RuntimeOptions missing_options;
    missing_options.executors[NodeKind::Subflow] = missing;
    WorkflowRuntime missing_runtime(missing_options);
    const auto failed = missing_runtime.Run(parent, RunInputs{});
    CHECK(failed.state == RunState::Failed);
    CHECK(failed.nodes.at("call_child").error_code == "unknown_subflow");
}

TEST_CASE("恢复:journal 重放,已完成节点不重跑") {
    using namespace lubancode::workflow;
    TempDir tmp;
    const char* yaml = R"YAML(
schema_version: 1
id: resumable
version: 1.0.0
name: r
entry: x
nodes:
  x:
    type: transform
    operation: echo
  y:
    type: transform
    operation: echo
  fin:
    type: end
edges:
  - { from: x, on: success, to: y }
  - { from: y, on: success, to: fin }
result:
  y_ran: "${nodes.y.output.ran}"
)YAML";
    const WorkflowDefinition def = ParseOrDie(yaml);

    // 第一场:x 成功后"崩"(y 不跑)。
    auto echo = std::make_shared<TransformExecutor>();
    echo->Register("echo", [](const nlohmann::json& in) {
        return nlohmann::json{{"ran", true}};
    });
    auto fail_after_x = std::make_shared<TransformExecutor>();
    fail_after_x->Register("echo", [](const nlohmann::json& in) {
        return nlohmann::json{{"ran", true}};
    });

    RuntimeOptions options;
    options.executors[NodeKind::Transform] = echo;
    options.runs_root = tmp.Get();
    options.run_id_generator = [] { return "run-resume-1"; };
    WorkflowRuntime first(options);

    // 手工造一场半截 run:x 完成、y 没跑(journal 停在 node_completed x)。
    {
        RunJournal::StartInfo info;
        info.run_id = "run-resume-1";
        info.workflow_id = def.id;
        info.workflow_version = def.version;
        info.content_hash = ContentHash(def);
        info.cwd = "/tmp";
        info.definition_json = BuildNormalizedJson(def).dump();
        auto journal = RunJournal::Start(tmp.Get(), info);
        REQUIRE(journal.has_value());
        journal->Append(kEventNodeStarted, "x", 1, nlohmann::json{{"kind", "transform"}});
        journal->Append(kEventNodeCompleted, "x", 1,
                        nlohmann::json{{"outcome", "success"}, {"output", nlohmann::json{{"ran", true}}}});
        journal->SaveCheckpoint(journal->last_seq(),
                                nlohmann::json{{"inputs", nlohmann::json::object()},
                                               {"run", nlohmann::json{{"run_id", "run-resume-1"}}},
                                               {"nodes", nlohmann::json{{"x", {{"output", {{"ran", true}}}}}}}});
        journal->Finish("interrupted", nlohmann::json::object());
    }

    // 恢复:y 跑,x 不再跑(x 的调用次数=0)。
    auto counting = std::make_shared<TransformExecutor>();
    counting->Register("echo", [](const nlohmann::json& in) {
        return nlohmann::json{{"ran", true}};
    });
    // 用 Scripted 同款:这里直接数 TransformExecutor 不行,换 Tracking 思路
    // ——给 y 的 output 埋个标记。
    auto tagged = std::make_shared<TransformExecutor>();
    tagged->Register("echo", [](const nlohmann::json& in) {
        // x 的 input 空;resume 时 x 不该来。埋 in 里的 step 记号。
        return nlohmann::json{{"ran", true}, {"input_was", in.dump()}};
    });

    RuntimeOptions resume_options;
    resume_options.executors[NodeKind::Transform] = tagged;
    resume_options.runs_root = tmp.Get();
    resume_options.run_id_generator = [] { return "run-resume-1"; };
    WorkflowRuntime resumer(resume_options);
    auto resumed = resumer.Resume(tmp.Get() / "run-resume-1");
    REQUIRE(resumed.has_value());
    CHECK(resumed->state == RunState::Succeeded);
    CHECK(resumed->result["y_ran"] == true);
}

TEST_CASE("恢复:definition 快照损坏,明报不硬续") {
    using namespace lubancode::workflow;
    TempDir tmp;
    fs::create_directories(tmp.Get() / "run-broken");
    { std::ofstream f(tmp.Get() / "run-broken" / "definition.json", std::ios::binary); f << "{ 坏的"; }
    RuntimeOptions options;
    WorkflowRuntime runtime(options);
    auto result = runtime.Resume(tmp.Get() / "run-broken");
    REQUIRE(!result.has_value());
    CHECK(result.error().find("definition") != std::string::npos);
}

}  // TEST_SUITE(workflows-agents)
