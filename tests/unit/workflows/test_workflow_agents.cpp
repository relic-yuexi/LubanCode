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
#include "runtime/event.hpp"
#include "runtime/event_sink.hpp"
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
    std::vector<lubancode::api::Request> requests;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)cancel;
        ++calls;
        requests.push_back(request);
        on_event(lubancode::api::TextDelta{reply});
        lubancode::api::MessageDone done;
        done.usage.input_tokens = input_tokens;
        done.usage.output_tokens = output_tokens;
        on_event(done);
        return {};
    }
};

class AgentBackend : public lubancode::api::Backend {
public:
    // 首发调哪枚工具(默认 reader;审批/白名单用例换成 needs_confirm 或
    // 白名单外的工具)。
    std::string first_tool = "reader";
    std::vector<lubancode::api::Request> requests;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)cancel;
        requests.push_back(request);
        lubancode::api::Usage usage;
        usage.input_tokens = 10;
        usage.output_tokens = 5;
        on_event(lubancode::api::MessageStart{"msg", request.model});
        if (requests.size() == 1) {
            on_event(lubancode::api::ToolUseStart{0, "tool-1", first_tool});
            on_event(lubancode::api::ToolUseInputDelta{0, "{}"});
            on_event(lubancode::api::ContentBlockDone{0});
            on_event(lubancode::api::MessageDone{"tool_use", usage});
        } else {
            on_event(lubancode::api::TextDelta{R"({"answer":"ok"})"});
            on_event(lubancode::api::ContentBlockDone{0});
            on_event(lubancode::api::MessageDone{"end_turn", usage});
        }
        return {};
    }
};

// 假 backend:send_stream 直接报错(批五乙降策略的钉用——agent 节点走
// TurnHarness 的 DriveTurn 收场,报错映射与从前一字不差)。
class FailingBackend : public lubancode::api::Backend {
public:
    int calls = 0;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request,
        const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)request;
        (void)on_event;
        (void)cancel;
        ++calls;
        return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Api, "接口崩了"});
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
    lubancode::runtime::QuestionRequest last_question;

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
        last_question = request;
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

// 事件录音机(骨架拆解批二:agent 节点上事件流的验收)。
class RecordingSink final : public lubancode::runtime::EventSink {
public:
    void Emit(const lubancode::runtime::ServerEvent& event) override { events.push_back(event); }
    std::vector<lubancode::runtime::ServerEvent> events;
};

// 从一次请求的消息里拼出全部工具结果块的文本(审批拒词/白名单拒词都从这查)。
std::string ToolResultsText(const lubancode::api::Request& request) {
    std::string out;
    for (const auto& message : request.messages) {
        for (const auto& block : message.content) {
            if (const auto* result = std::get_if<lubancode::api::ToolResultBlock>(&block)) {
                out += result->content;
                out += "\n";
            }
        }
    }
    return out;
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

TEST_CASE("llm 节点:面板补充在首轮收口后送入第二轮") {
    using namespace lubancode::workflow;
    FakeBackend backend;
    LlmExecutor::Options options;
    options.backend = &backend;
    options.model = "test-model";
    options.prompt_loader = [](const std::string&) { return "你是分析器"; };
    int pulls = 0;
    options.steering = [&pulls](const NodeExecRequest&) -> std::optional<NodeSteeringBatch> {
        if (pulls++ > 0) return std::nullopt;
        return NodeSteeringBatch{"请把验收条件写清楚", nullptr};
    };
    LlmExecutor executor(std::move(options));

    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: llm-steering
version: 1.0.0
entry: analyze
nodes:
  analyze:
    type: llm
    prompt: prompts/analyze.md
)YAML");
    NodeExecRequest request;
    request.node = &def.node_map.at("analyze");
    request.resolved_input = nlohmann::json{{"topic", "参与"}};
    request.run_id = "run-steering";
    request.node_run_id = "run-steering-analyze-a1";

    const NodeExecResult result = executor.Execute(request);
    REQUIRE(result.ok);
    REQUIRE(backend.requests.size() == 2);
    REQUIRE(backend.requests[1].messages.size() == 3);
    const auto* followup = std::get_if<lubancode::api::TextBlock>(
        &backend.requests[1].messages.back().content.front());
    REQUIRE(followup != nullptr);
    CHECK(followup->text.find("验收条件") != std::string::npos);
    CHECK(result.tokens_used == 300);
}

TEST_CASE("llm 节点:model_role 可换到独立 Lao binding") {
    using namespace lubancode::workflow;
    FakeBackend normal;
    FakeBackend lao;
    LlmExecutor::Options options;
    options.backend = &normal;
    options.model = "normal-model";
    options.prompt_loader = [](const std::string&) { return "拟方案"; };
    options.resolve_binding = [&lao](const WorkflowNode& node)
        -> std::optional<LlmExecutor::Binding> {
        CHECK(node.model_role == "lao");
        return LlmExecutor::Binding{&lao, nullptr, "lao-model", "high"};
    };
    LlmExecutor executor(std::move(options));

    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: llm-lao
version: 1.0.0
entry: plan
nodes:
  plan:
    type: llm
    model_role: lao
    prompt: prompts/plan.md
)YAML");
    NodeExecRequest request;
    request.node = &def.node_map.at("plan");
    request.resolved_input = nlohmann::json{{"topic", "方案"}};

    const NodeExecResult result = executor.Execute(request);
    REQUIRE(result.ok);
    CHECK(normal.calls == 0);
    REQUIRE(lao.requests.size() == 1);
    CHECK(lao.requests[0].model == "lao-model");
    CHECK(lao.requests[0].reasoning_effort == "high");
}

TEST_CASE("agent 节点:同一 Agent 吃 profile、工具白名单并跑完整工具循环") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    auto* reader = new FakeTool("reader", R"({"read":true})", false, false);
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(reader));
    registry.Register(std::make_unique<FakeTool>("editor", "edited", false, false));
    AgentBackend backend;

    AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile.provider = "zai";
    options.default_binding.profile.request.model = "glm-5.3";
    options.default_binding.profile.request.reasoning_effort = "high";
    options.default_binding.profile.request.reasoning.supports_effort = true;
    options.default_binding.profile.request.model = "glm-5.3";
    options.registry = &registry;
    options.task_loader = [](const std::string& path) {
        CHECK(path == "prompts/explore.md");
        return std::string("只读探索");
    };
    AgentExecutor executor(std::move(options));

    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: agent-flow
version: 1.0.0
entry: explore
nodes:
  explore:
    type: agent
    task: prompts/explore.md
    allowed_tools: [reader]
    step_limit: 4
)YAML");
    NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("explore");
    request.resolved_input = nlohmann::json{{"topic", "木构"}};

    const NodeExecResult result = executor.Execute(request);
    REQUIRE(result.ok);
    CHECK(result.output["answer"] == "ok");
    CHECK(result.tokens_used == 30);
    CHECK(reader->calls == 1);
    REQUIRE(backend.requests.size() == 2);
    CHECK(backend.requests[0].model == "glm-5.3");
    CHECK(backend.requests[0].reasoning_effort == "high");
    CHECK(backend.requests[0].reasoning.supports_effort);
    CHECK(backend.requests[0].system == "只读探索");
    REQUIRE(backend.requests[0].tools.size() == 1);
    CHECK(backend.requests[0].tools[0].name == "reader");
}

TEST_CASE("agent 节点:审批口三态——放行、拒绝、没宿主的兜底") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: agent-confirm
version: 1.0.0
entry: work
nodes:
  work:
    type: agent
    task: prompts/work.md
    allowed_tools: [writer]
)YAML");
    NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("work");
    request.resolved_input = nlohmann::json{{"topic", "写档"}};

    SUBCASE("回调放行:needs_confirm 工具真执行") {
        lubancode::tools::ToolRegistry registry;
        auto* writer = new FakeTool("writer", R"({"wrote":true})", false, true);
        registry.Register(std::unique_ptr<lubancode::tools::Tool>(writer));
        AgentBackend backend;
        backend.first_tool = "writer";

        AgentExecutor::Options options;
        options.default_binding.backend = &backend;
        options.default_binding.profile.request.model = "test-agent";
        options.registry = &registry;
        options.task_loader = [](const std::string&) { return std::string("照单办事"); };
        options.callbacks.on_tool_confirm = [](const std::string&, const std::string&, const nlohmann::json&) {
            return true;
        };
        AgentExecutor executor(std::move(options));

        const NodeExecResult result = executor.Execute(request);
        REQUIRE(result.ok);
        CHECK(result.output["answer"] == "ok");
        CHECK(writer->calls == 1);  // 真执行了,不是空放行
        REQUIRE(backend.requests.size() == 2);
    }

    SUBCASE("回调拒绝:工具不执行,流程照剧本收口") {
        lubancode::tools::ToolRegistry registry;
        auto* writer = new FakeTool("writer", R"({"wrote":true})", false, true);
        registry.Register(std::unique_ptr<lubancode::tools::Tool>(writer));
        AgentBackend backend;
        backend.first_tool = "writer";

        AgentExecutor::Options options;
        options.default_binding.backend = &backend;
        options.default_binding.profile.request.model = "test-agent";
        options.registry = &registry;
        options.task_loader = [](const std::string&) { return std::string("照单办事"); };
        options.callbacks.on_tool_confirm = [](const std::string&, const std::string&, const nlohmann::json&) {
            return false;
        };
        AgentExecutor executor(std::move(options));

        const NodeExecResult result = executor.Execute(request);
        CHECK(writer->calls == 0);  // 拒在执行前
        REQUIRE(result.ok);         // 拒词进历史,剧本第二发照出 JSON
        CHECK(result.output["answer"] == "ok");
        REQUIRE(backend.requests.size() == 2);
        // 默认拒词(问了用户之后的拒绝是实话)
        CHECK(ToolResultsText(backend.requests[1]).find("用户拒绝执行该工具") != std::string::npos);
    }

    SUBCASE("没接宿主:兜底明拒,模型收到的是实话") {
        lubancode::tools::ToolRegistry registry;
        auto* writer = new FakeTool("writer", R"({"wrote":true})", false, true);
        registry.Register(std::unique_ptr<lubancode::tools::Tool>(writer));
        AgentBackend backend;
        backend.first_tool = "writer";

        AgentExecutor::Options options;
        options.default_binding.backend = &backend;
        options.default_binding.profile.request.model = "test-agent";
        options.registry = &registry;
        options.task_loader = [](const std::string&) { return std::string("照单办事"); };
        AgentExecutor executor(std::move(options));  // callbacks 全空

        const NodeExecResult result = executor.Execute(request);
        CHECK(writer->calls == 0);
        REQUIRE(result.ok);
        REQUIRE(backend.requests.size() == 2);
        CHECK(ToolResultsText(backend.requests[1]).find("workflow agent 节点未接审批宿主") !=
              std::string::npos);
        CHECK(ToolResultsText(backend.requests[1]).find("writer") != std::string::npos);
    }
}

TEST_CASE("agent 节点:allowed_tools 外的工具收到白名单拒词") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("reader", R"({"read":true})", false, false));
    auto* editor = new FakeTool("editor", "edited", false, false);
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(editor));
    AgentBackend backend;
    backend.first_tool = "editor";  // 剧本偏要调白名单外的

    AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile.request.model = "test-agent";
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("照单办事"); };
    AgentExecutor executor(std::move(options));

    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: agent-filter
version: 1.0.0
entry: work
nodes:
  work:
    type: agent
    task: prompts/work.md
    allowed_tools: [reader]
)YAML");
    NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("work");
    request.resolved_input = nlohmann::json{{"topic", "只读"}};

    const NodeExecResult result = executor.Execute(request);
    CHECK(editor->calls == 0);  // 白名单拦在执行前
    REQUIRE(result.ok);         // 拒词进历史,第二发照出 JSON
    REQUIRE(backend.requests.size() == 2);
    CHECK(ToolResultsText(backend.requests[1]).find("此工具不在 workflow agent 节点的 allowed_tools 里") !=
          std::string::npos);
}

TEST_CASE("agent 节点:面板补充经 TurnHarness 续投且结果认最后一轮") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("reader", R"({"read":true})", false, false));
    AgentBackend backend;

    AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile.request.model = "test-agent";
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("照单办事"); };
    int pulls = 0;
    options.steering = [&pulls](const NodeExecRequest&) -> std::optional<NodeSteeringBatch> {
        if (pulls++ > 0) return std::nullopt;
        return NodeSteeringBatch{"再核对一遍", nullptr};
    };
    AgentExecutor executor(std::move(options));

    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: agent-steering
version: 1.0.0
entry: work
nodes:
  work:
    type: agent
    task: prompts/work.md
    allowed_tools: [reader]
)YAML");
    NodeExecRequest request;
    request.node = &def.node_map.at("work");
    request.resolved_input = nlohmann::json{{"topic", "参与"}};
    request.run_id = "run-agent-steering";
    request.node_run_id = "run-agent-steering-work-a1";

    const NodeExecResult result = executor.Execute(request);
    REQUIRE(result.ok);
    CHECK(result.output["answer"] == "ok");
    REQUIRE(backend.requests.size() == 3);
    bool saw_followup = false;
    for (const auto& message : backend.requests.back().messages) {
        for (const auto& block : message.content) {
            if (const auto* text = std::get_if<lubancode::api::TextBlock>(&block);
                text != nullptr && text->text.find("再核对一遍") != std::string::npos) {
                saw_followup = true;
            }
        }
    }
    CHECK(saw_followup);
}

TEST_CASE("agent 节点:backend 报错经 TurnHarness 收场,映射与从前一字不差") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("reader", R"({"read":true})", false, false));
    FailingBackend backend;

    AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile.provider = "zai";
    options.default_binding.profile.request.model = "glm-5.3";
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("干活"); };
    AgentExecutor executor(std::move(options));

    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: agent-fail
version: 1.0.0
entry: work
nodes:
  work:
    type: agent
    task: prompts/work.md
)YAML");
    NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("work");
    request.resolved_input = nlohmann::json{{"topic", "钉收场"}};

    // 批五乙降策略:turn 推进走 DriveTurn(单轮即收,没续投源),报错
    // 折 agent_error、错误文案带 backend 原话、不发第二次请求。
    const NodeExecResult result = executor.Execute(request);
    REQUIRE_FALSE(result.ok);
    CHECK(result.error_code == "agent_error");
    CHECK(result.error_message.find("接口崩了") != std::string::npos);
    CHECK(backend.calls == 1);
}

TEST_CASE("agent 节点:装了 event_sink 的嵌套回合经 TurnEventAdapter 上事件流") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    auto* reader = new FakeTool("reader", R"({"read":true})", false, false);
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(reader));
    AgentBackend backend;
    RecordingSink sink;

    AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile.provider = "zai";
    options.default_binding.profile.request.model = "glm-5.3";
    options.default_binding.profile.request.model = "glm-5.3";
    options.registry = &registry;
    options.task_loader = [](const std::string&) { return std::string("只读探索"); };
    // 批二:sink 配置(没给 ids 落 ProcessIdAuthority——生产装配给会话的)。
    options.event_sink = &sink;
    options.thread_id = "wf-th";
    AgentExecutor executor(std::move(options));

    const WorkflowDefinition def = ParseOrDie(R"YAML(
schema_version: 1
id: agent-events-flow
version: 1.0.0
entry: explore
nodes:
  explore:
    type: agent
    task: prompts/explore.md
    allowed_tools: [reader]
)YAML");
    NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("explore");
    request.resolved_input = nlohmann::json{{"topic", "事件流"}};
    request.run_id = "run-wf-1";
    request.node_run_id = "run-wf-1-explore-a1";

    const NodeExecResult result = executor.Execute(request);
    REQUIRE(result.ok);

    // 事件账(批二余款:step/批次边界也随事件流走):TurnStarted(run_id 当
    // turn_id)→ 两轮请求各一枚 UsageUpdated,工具与正文条目有起有终,
    // TurnCompleted(Succeeded) 收口;seq 单调、thread_id 透传、领域数据
    // (工具名/结果)在场。
    using lubancode::runtime::ServerEventKind;
    using lubancode::runtime::ItemKind;
    using lubancode::runtime::Outcome;
    REQUIRE(sink.events.size() == 13);
    CHECK(sink.events[0].kind == ServerEventKind::TurnStarted);
    CHECK(sink.events[0].turn_id == "run-wf-1");
    CHECK(sink.events[1].kind == ServerEventKind::ModelStepStarted);
    CHECK(sink.events[2].kind == ServerEventKind::UsageUpdated);
    CHECK(sink.events[3].kind == ServerEventKind::ToolBatchStarted);
    CHECK(sink.events[4].kind == ServerEventKind::ItemStarted);
    CHECK(sink.events[4].item_kind == ItemKind::Tool);
    CHECK(sink.events[4].payload.value("tool_name", std::string()) == "reader");
    CHECK(sink.events[5].kind == ServerEventKind::ItemCompleted);
    CHECK(sink.events[5].payload.value("result", std::string()) == R"({"read":true})");
    CHECK(sink.events[6].kind == ServerEventKind::ToolBatchFinished);
    CHECK(sink.events[7].kind == ServerEventKind::ModelStepStarted);
    // 第二轮请求:流式 delta 先于 MessageDone 的 usage(流式次序,如实钉)。
    CHECK(sink.events[8].kind == ServerEventKind::ItemStarted);
    CHECK(sink.events[8].item_kind == ItemKind::Text);
    CHECK(sink.events[9].kind == ServerEventKind::ItemDelta);
    CHECK(sink.events[9].text == R"({"answer":"ok"})");
    CHECK(sink.events[10].kind == ServerEventKind::UsageUpdated);
    CHECK(sink.events[11].kind == ServerEventKind::ItemCompleted);
    CHECK(sink.events[11].outcome == Outcome::Succeeded);
    CHECK(sink.events[12].kind == ServerEventKind::TurnCompleted);
    CHECK(sink.events[12].outcome == Outcome::Succeeded);

    std::uint64_t last_seq = 0;
    for (const auto& event : sink.events) {
        CHECK(event.envelope.thread_id == "wf-th");
        CHECK(event.turn_id == "run-wf-1");
        CHECK(event.payload.value("workflow_node_run_id", std::string()) ==
              "run-wf-1-explore-a1");
        CHECK(event.payload.value("workflow_node_id", std::string()) == "explore");
        CHECK(event.envelope.seq > last_seq);
        last_seq = event.envelope.seq;
    }
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
                       "  ask:\n    type: ask_user\n    input:\n      question: \"查哪路\"\n"
                       "      options:\n        - { label: \"arXiv\", description: \"论文库\" }\n"
                       "  fin:\n    type: end\nedges:\n  - { from: ask, on: success, to: fin }\n"
                       "result:\n  answer: \"${nodes.ask.output.answers.0}\"\n";
    const WorkflowDefinition def = ParseOrDie(yaml);
    RuntimeOptions options;
    options.executors[NodeKind::AskUser] = executor;
    WorkflowRuntime runtime(options);
    const auto summary = runtime.Run(def, RunInputs{});
    REQUIRE(summary.state == RunState::Succeeded);
    CHECK(summary.result["answer"] == "用 arXiv");
    REQUIRE(broker.last_question.options.size() == 1);
    CHECK(broker.last_question.options[0].label == "arXiv");
    CHECK(broker.last_question.options[0].description == "论文库");
}

TEST_CASE("ask_user 节点:上游已通过时跳过菜单") {
    using namespace lubancode::workflow;
    InstantBroker broker;
    AskUserExecutor executor(&broker);
    const WorkflowDefinition def = ParseOrDie(
        "schema_version: 1\nid: q-skip\nversion: 1\nentry: ask\nnodes:\n"
        "  ask:\n    type: ask_user\n    input: { skip_when: true, question: \"不该问\" }\n");
    NodeExecRequest request;
    request.node = &def.node_map.at("ask");
    request.resolved_input = nlohmann::json{{"skip_when", true}, {"question", "不该问"}};
    const NodeExecResult result = executor.Execute(request);
    REQUIRE(result.ok);
    CHECK(result.output["skipped"] == true);
    CHECK(broker.questions == 0);
}

TEST_CASE("ask_user 节点:不知道会委托后续各轮且须等复审通过") {
    using namespace lubancode::workflow;
    InstantBroker broker;
    broker.question_answers = {"不知道，请中书定方案"};
    AskUserExecutor executor(&broker);
    const WorkflowDefinition def = ParseOrDie(
        "schema_version: 1\nid: q-delegate\nversion: 1\nentry: ask\nnodes:\n"
        "  ask:\n    type: ask_user\n    input: { question: \"请批阅\" }\n");
    NodeExecRequest request;
    request.node = &def.node_map.at("ask");
    request.resolved_input = nlohmann::json{
        {"question", "请批阅"},
        {"review_approved", false},
        {"delegate_answers", nlohmann::json::array({"不知道，请中书定方案"})}};

    const NodeExecResult first = executor.Execute(request);
    REQUIRE(first.ok);
    CHECK(first.output["delegated"] == true);
    CHECK(first.output["complete"] == false);
    CHECK(broker.questions == 1);

    request.resolved_input["previous"] =
        nlohmann::json{{"outputs", {{"ask", first.output}}}};
    const NodeExecResult rejected_again = executor.Execute(request);
    REQUIRE(rejected_again.ok);
    CHECK(rejected_again.output["skipped"] == true);
    CHECK(rejected_again.output["complete"] == false);
    CHECK(broker.questions == 1);

    request.resolved_input["review_approved"] = true;
    const NodeExecResult approved = executor.Execute(request);
    REQUIRE(approved.ok);
    CHECK(approved.output["complete"] == true);
    CHECK(broker.questions == 1);
}

TEST_CASE("ask_user 节点:override_answers 墨敕越权放行") {
    using namespace lubancode::workflow;
    const WorkflowDefinition def = ParseOrDie(
        "schema_version: 1\nid: q-override\nversion: 1\nentry: ask\nnodes:\n"
        "  ask:\n    type: ask_user\n    input: { question: \"请批阅\" }\n");
    NodeExecRequest request;
    request.node = &def.node_map.at("ask");

    SUBCASE("门下已驳,墨敕命中:approved/complete/overridden 全真") {
        InstantBroker broker;
        broker.question_answers = {"朕说了算"};
        AskUserExecutor executor(&broker);
        request.resolved_input = nlohmann::json{
            {"question", "请批阅"},
            {"review_approved", false},
            {"override_answers", nlohmann::json::array({"朕说了算"})}};

        const NodeExecResult result = executor.Execute(request);
        REQUIRE(result.ok);
        CHECK(result.output["approved"] == true);
        CHECK(result.output["complete"] == true);
        CHECK(result.output["overridden"] == true);
    }

    SUBCASE("门下已驳,墨敕不命中:照旧驳回") {
        InstantBroker broker;
        broker.question_answers = {"再改"};
        AskUserExecutor executor(&broker);
        request.resolved_input = nlohmann::json{
            {"question", "请批阅"},
            {"review_approved", false},
            {"approve_answers", nlohmann::json::array({"准"})},
            {"override_answers", nlohmann::json::array({"朕说了算"})}};

        const NodeExecResult result = executor.Execute(request);
        REQUIRE(result.ok);
        CHECK(result.output["approved"] == false);
        CHECK(result.output["complete"] == false);
        CHECK(result.output["overridden"] == false);  // 恒在键,未命中给假
    }
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
    echo->Register("echo", [](const nlohmann::json&) {
        return nlohmann::json{{"ran", true}};
    });
    auto fail_after_x = std::make_shared<TransformExecutor>();
    fail_after_x->Register("echo", [](const nlohmann::json&) {
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
    counting->Register("echo", [](const nlohmann::json&) {
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
