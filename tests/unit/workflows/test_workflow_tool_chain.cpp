// 骨架拆解单批一·病二的验收钉:workflow tool 节点换 agent::RunOneTool
// 正门后,钩子账、权限门、Plan 闸、逐枚 trace 都要真过账——不许再有一条
// 绕开 hooks 的暗道。假 registry + 假 trace 收账,不出网。

#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "agent/tool_trace.hpp"
#include "tools/registry.hpp"
#include "workflow/host_executors.hpp"
#include "workflow/parser.hpp"
#include "workflow/runtime.hpp"

namespace {

// 可数、可验入参、可声明 needs_confirm 与 schema 的假工具。
class FakeTool : public lubancode::tools::Tool {
public:
    FakeTool(std::string name, std::string result, bool confirm_needed = false,
             nlohmann::json schema = nlohmann::json::object())
        : name_(std::move(name)), result_(std::move(result)), confirm_(confirm_needed), schema_(std::move(schema)) {}

    std::string name() const override { return name_; }
    std::string description() const override { return "fake"; }
    nlohmann::json input_schema() const override { return schema_; }
    bool needs_confirm() const override { return confirm_; }
    Result execute(const nlohmann::json& input) override {
        ++calls;
        last_input = input;
        return Result{result_, false};
    }

    int calls = 0;
    nlohmann::json last_input;

private:
    std::string name_;
    std::string result_;
    bool confirm_;
    nlohmann::json schema_;
};

lubancode::workflow::WorkflowDefinition ParseOrDie(const char* yaml) {
    auto parsed = lubancode::workflow::ParseWorkflowYaml(yaml);
    REQUIRE(parsed.has_value());
    return *parsed;
}

// 一枚 tool 节点的执行请求(直调执行器,不过图)。
lubancode::workflow::NodeExecRequest MakeRequest(const lubancode::workflow::WorkflowDefinition& def,
                                                 const nlohmann::json& input) {
    lubancode::workflow::NodeExecRequest request;
    request.definition = &def;
    request.node = &def.node_map.at("x");
    request.run_id = "run-1";
    request.node_run_id = "run-1-x-a1";
    request.attempt = 1;
    request.resolved_input = input;
    return request;
}

}  // namespace

TEST_SUITE("workflow-tool-chain") {

TEST_CASE("正门落 trace 账:Scheduled/Started/Finished 三栅栏,发号与身份带齐") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    auto* tool = new FakeTool("paper_search", R"({"items": ["a"]})");
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));

    std::vector<lubancode::agent::ToolTraceEvent> events;
    int ids_issued = 0;
    ToolExecutor::Options options;
    options.registry = &registry;
    options.execution_id_issuer = [&ids_issued]() { return "exec-" + std::to_string(++ids_issued); };
    options.thread_id = "thread-1";
    options.turn_id = "turn-9";
    options.callbacks.on_tool_trace = [&events](const lubancode::agent::ToolTraceEvent& event) {
        events.push_back(event);
    };
    ToolExecutor executor(std::move(options));

    const auto def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                "  x:\n    type: tool\n    tool: paper_search\n");
    const auto result = executor.Execute(MakeRequest(def, nlohmann::json{{"q", "hi"}}));
    REQUIRE(result.ok);
    REQUIRE(tool->calls == 1);

    using Kind = lubancode::agent::ToolTraceEventKind;
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == Kind::Scheduled);
    CHECK(events[1].kind == Kind::ExecutionStarted);
    CHECK(events[2].kind == Kind::ExecutionFinished);
    // 三栅栏同一枚 execution(宿主发号),批/轮/线程身份齐。
    for (const auto& event : events) {
        CHECK(event.execution_id == "exec-1");
        CHECK(event.tool_name == "paper_search");
        CHECK(event.turn_id == "turn-9");
        CHECK(event.thread_id == "thread-1");
        CHECK(event.batch_id == "run-1-x-a1");
    }
    CHECK(events[2].outcome == lubancode::agent::ToolOutcome::Succeeded);
}

TEST_CASE("PreToolUse 钩子 deny:工具不执行,稳定码落账,trace 终态 HookDenied") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    auto* tool = new FakeTool("dangerous", "不该跑", false);
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));

    std::vector<lubancode::agent::ToolTraceEvent> events;
    ToolExecutor::Options options;
    options.registry = &registry;
    options.execution_id_issuer = [] { return std::string("exec-deny"); };
    options.callbacks.on_tool_trace = [&events](const lubancode::agent::ToolTraceEvent& event) {
        events.push_back(event);
    };
    options.callbacks.on_pre_tool_use_hook = [](const std::string&, const std::string&,
                                               const nlohmann::json&) -> lubancode::agent::ToolHookDecision {
        lubancode::agent::ToolHookDecision decision;
        decision.decision = lubancode::agent::ToolHookDecision::Decision::Deny;
        decision.reason = "钩子说不";
        return decision;
    };
    ToolExecutor executor(std::move(options));

    const auto def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                "  x:\n    type: tool\n    tool: dangerous\n");
    const auto result = executor.Execute(MakeRequest(def, nlohmann::json::object()));
    CHECK_FALSE(result.ok);
    CHECK(tool->calls == 0);  // 拒在执行前
    CHECK(result.error_code == "hook.pre.denied");
    CHECK(result.error_message.find("钩子说不") != std::string::npos);
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().kind == lubancode::agent::ToolTraceEventKind::ExecutionFinished);
    CHECK(events.back().outcome == lubancode::agent::ToolOutcome::HookDenied);
}

TEST_CASE("PreToolUse allow + updatedInput:改参过 schema 复检,工具吃到改后入参") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    const nlohmann::json schema = nlohmann::json{
        {"type", "object"},
        {"properties", {{"q", {{"type", "string"}}}, {"limit", {{"type", "number"}}}}},
        {"required", nlohmann::json::array({"q"})}};
    auto* tool = new FakeTool("paper_search", R"({"ok": true})", false, schema);
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));

    ToolExecutor::Options options;
    options.registry = &registry;
    options.callbacks.on_pre_tool_use_hook = [](const std::string&, const std::string&,
                                               const nlohmann::json&) -> lubancode::agent::ToolHookDecision {
        lubancode::agent::ToolHookDecision decision;
        decision.decision = lubancode::agent::ToolHookDecision::Decision::Allow;
        decision.updated_input = nlohmann::json{{"q", "改写后的词"}, {"limit", 3}};
        return decision;
    };
    ToolExecutor executor(std::move(options));

    const auto def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                "  x:\n    type: tool\n    tool: paper_search\n");
    const auto result = executor.Execute(MakeRequest(def, nlohmann::json{{"q", "原词"}}));
    REQUIRE(result.ok);
    REQUIRE(tool->calls == 1);
    CHECK(tool->last_input["q"] == "改写后的词");
    CHECK(tool->last_input["limit"] == 3);
}

TEST_CASE("PostToolUse 反馈:追加进工具结果正文(模型与下游看得见)") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    auto* tool = new FakeTool("paper_search", "纯文本结果");
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));

    ToolExecutor::Options options;
    options.registry = &registry;
    options.callbacks.on_post_tool_use_hook = [](const std::string&, const std::string&, const nlohmann::json&,
                                                 const lubancode::tools::Tool::Result&) {
        return std::vector<std::string>{"审计追加一行"};
    };
    ToolExecutor executor(std::move(options));

    const auto def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                "  x:\n    type: tool\n    tool: paper_search\n");
    const auto result = executor.Execute(MakeRequest(def, nlohmann::json::object()));
    REQUIRE(result.ok);
    // 追加后解析不成 JSON,按 content 字段包一层;追加的反馈必须在场。
    CHECK(result.output["content"].get<std::string>().find("审计追加一行") != std::string::npos);
}

TEST_CASE("确认门旧语义保住:无门明拒、gate 拒 permission_denied、gate 放行照跑") {
    using namespace lubancode::workflow;
    const auto def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                "  x:\n    type: tool\n    tool: dangerous\n");

    SUBCASE("没人管确认门:not_configured,不挂死") {
        lubancode::tools::ToolRegistry registry;
        registry.Register(std::unique_ptr<lubancode::tools::Tool>(new FakeTool("dangerous", "x", true)));
        ToolExecutor executor(&registry);
        const auto result = executor.Execute(MakeRequest(def, nlohmann::json::object()));
        CHECK_FALSE(result.ok);
        CHECK(result.error_code == "not_configured");
    }
    SUBCASE("旧 gate 拒:permission_denied(旧稳定码)") {
        lubancode::tools::ToolRegistry registry;
        auto* tool = new FakeTool("dangerous", "x", true);
        registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));
        ToolExecutor executor(&registry, [](const std::string&, const nlohmann::json&) { return false; });
        const auto result = executor.Execute(MakeRequest(def, nlohmann::json::object()));
        CHECK_FALSE(result.ok);
        CHECK(result.error_code == "permission_denied");
        CHECK(tool->calls == 0);
    }
    SUBCASE("旧 gate 放行:照常执行") {
        lubancode::tools::ToolRegistry registry;
        auto* tool = new FakeTool("dangerous", R"({"ok":1})", true);
        registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));
        ToolExecutor executor(&registry, [](const std::string&, const nlohmann::json&) { return true; });
        const auto result = executor.Execute(MakeRequest(def, nlohmann::json::object()));
        REQUIRE(result.ok);
        CHECK(tool->calls == 1);
    }
    SUBCASE("宿主确认口(on_tool_confirm)在场时旧 gate 不抢班") {
        lubancode::tools::ToolRegistry registry;
        auto* tool = new FakeTool("dangerous", R"({"ok":1})", true);
        registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));
        ToolExecutor::Options options;
        options.registry = &registry;
        options.confirm = [](const std::string&, const nlohmann::json&) { return true; };  // 旧 gate 说放
        options.callbacks.on_tool_confirm = [](const std::string&, const std::string&,
                                               const nlohmann::json&) { return false; };  // 宿主说拒
        ToolExecutor executor(std::move(options));
        const auto result = executor.Execute(MakeRequest(def, nlohmann::json::object()));
        CHECK_FALSE(result.ok);
        CHECK(result.error_code == "permission_denied");
        CHECK(tool->calls == 0);
    }
}

TEST_CASE("Plan 闸同路:ModePolicy 拒下工具不执行,细码透传") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    auto* tool = new FakeTool("run_command", "x");
    registry.Register(std::unique_ptr<lubancode::tools::Tool>(tool));

    ToolExecutor::Options options;
    options.registry = &registry;
    options.callbacks.on_mode_policy = [](const std::string&, const nlohmann::json&) {
        return std::string("mode.denied.plan_write|Plan 模式只读");
    };
    ToolExecutor executor(std::move(options));

    const auto def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                "  x:\n    type: tool\n    tool: run_command\n");
    const auto result = executor.Execute(MakeRequest(def, nlohmann::json::object()));
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "mode.denied.plan_write");
    CHECK(result.error_message.find("Plan 模式只读") != std::string::npos);
    CHECK(tool->calls == 0);
}

TEST_CASE("工具本体失败:tool_error 旧码照旧;未知工具:tool_unavailable 照旧") {
    using namespace lubancode::workflow;
    lubancode::tools::ToolRegistry registry;
    class ErrorTool final : public lubancode::tools::Tool {
    public:
        std::string name() const override { return "boom"; }
        std::string description() const override { return "fake"; }
        nlohmann::json input_schema() const override { return nlohmann::json::object(); }
        Result execute(const nlohmann::json&) override { return Result{"炸了", true}; }
    };
    registry.Register(std::make_unique<ErrorTool>());

    ToolExecutor executor(&registry);
    const auto def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                "  x:\n    type: tool\n    tool: boom\n");
    const auto result = executor.Execute(MakeRequest(def, nlohmann::json::object()));
    CHECK_FALSE(result.ok);
    CHECK(result.error_code == "tool_error");
    CHECK(result.error_message == "炸了");

    const auto missing_def = ParseOrDie("schema_version: 1\nid: t\nversion: 1\nentry: x\nnodes:\n"
                                        "  x:\n    type: tool\n    tool: no_such\n");
    const auto missing = executor.Execute(MakeRequest(missing_def, nlohmann::json::object()));
    CHECK_FALSE(missing.ok);
    CHECK(missing.error_code == "tool_unavailable");
}

}  // TEST_SUITE(workflow-tool-chain)
