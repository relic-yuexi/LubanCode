// workflow_custom_agent_driver:自定义 Agent 单·阶段 5(Workflow 接入)的
// 冒烟驱动,不进 ctest,验收手动跑:
//   workflow_custom_agent_driver validate <agents_dir> <workflow_dir>
//       真扫 AgentCatalog(user 层目录)解析 workflow.yaml,能力表带真
//       agent_names 走 ValidateDefinition——编译期校验的正面(过)与
//       unknown_agent 的反面(点名夹具外的名字)各演一遍。
//   workflow_custom_agent_driver run <agents_dir> <workflow_dir>
//       假后端跑一幕:agent: 节点经统一 Resolver 解析、同源拼装系统提示,
//       首发请求的 system 首行、工具表、节点回执身份(resolved 名)打印
//       出来供人眼对账。
// 夹具:tests/fixtures/agents/smoke-probe.yaml 与
// tests/fixtures/workflows/custom-agent-smoke/。
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "agent/agent.hpp"
#include "agent/agent_catalog.hpp"
#include "agent/agent_definition.hpp"
#include "agent/agent_profile_resolver.hpp"
#include "api/backend.hpp"
#include "tools/registry.hpp"
#include "tools/tool.hpp"
#include "workflow/host_executors.hpp"
#include "workflow/parser.hpp"
#include "workflow/validator.hpp"

namespace {

namespace fs = std::filesystem;

class SmokeTool : public lubancode::tools::Tool {
public:
    explicit SmokeTool(std::string name) : name_(std::move(name)) {}
    std::string name() const override { return name_; }
    std::string description() const override { return "smoke tool"; }
    nlohmann::json input_schema() const override { return nlohmann::json::object(); }
    lubancode::tools::Tool::Result execute(const nlohmann::json&) override { return {"ok", false}; }

private:
    std::string name_;
};

// 假后端:首发吐一段文本结论(不开工具循环,幕短好对账)。
class SmokeBackend : public lubancode::api::Backend {
public:
    std::vector<lubancode::api::Request> requests;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request& request, const std::function<void(const lubancode::api::StreamEvent&)>& on_event,
        const std::atomic<bool>* cancel) override {
        (void)cancel;
        requests.push_back(request);
        lubancode::api::Usage usage;
        usage.input_tokens = 12;
        usage.output_tokens = 6;
        on_event(lubancode::api::MessageStart{"msg", request.model});
        on_event(lubancode::api::TextDelta{R"({"conclusion":"smoke ok"})"});
        on_event(lubancode::api::ContentBlockDone{0});
        on_event(lubancode::api::MessageDone{"end_turn", usage});
        return {};
    }
};

std::string ReadFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::string();
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fputs("用法: workflow_custom_agent_driver validate|run <agents_dir> <workflow_dir>\n", stderr);
        return 2;
    }
    const std::string mode = argv[1];
    const fs::path agents_dir = argv[2];
    const fs::path workflow_dir = argv[3];

    // 真扫 Catalog:夹具 agents 目录当 user 层。
    lubancode::agent::AgentCatalogScanRoots roots;
    roots.user_dir = agents_dir;
    const lubancode::agent::AgentCatalog catalog = lubancode::agent::LoadAgentCatalog(roots);
    std::printf("catalog: %zu 条\n", catalog.entries.size());
    for (const auto& entry : catalog.entries) {
        std::printf("  %s [%s] available=%s\n", entry.name.c_str(),
                    lubancode::agent::ToString(entry.layer).c_str(), entry.available ? "true" : "false");
    }

    const std::string workflow_yaml = ReadFile(workflow_dir / "workflow.yaml");
    if (workflow_yaml.empty()) {
        std::fputs("workflow.yaml 读不到\n", stderr);
        return 2;
    }
    const auto parsed = lubancode::workflow::ParseWorkflowYaml(workflow_yaml);
    if (!parsed.has_value()) {
        for (const auto& issue : parsed.error()) {
            std::printf("parse: %s: %s\n", issue.location.c_str(), issue.message.c_str());
        }
        return 1;
    }
    const lubancode::workflow::WorkflowDefinition def = *parsed;

    // 编译期校验:能力表带真 agent_names。
    lubancode::workflow::CapabilityTable caps;
    for (const lubancode::agent::AgentCatalogEntry* entry : catalog.Available()) {
        caps.agent_names.push_back(entry->name);
    }
    const lubancode::workflow::ValidationResult ok_result = lubancode::workflow::ValidateDefinition(def, caps);
    std::printf("validate(真名): %s\n", ok_result.ok() ? "过" : "不过");
    for (const auto& issue : ok_result.issues) {
        std::printf("  %s %s: %s\n", issue.code.c_str(), issue.path.c_str(), issue.message.c_str());
    }
    if (!ok_result.ok()) return 1;

    // 反面:点名夹具外的名字,编译期报 unknown_agent。
    lubancode::workflow::WorkflowDefinition ghost = def;
    ghost.nodes.front().agent = "no-such-agent";
    ghost.node_map.at(ghost.nodes.front().id).agent = "no-such-agent";
    const lubancode::workflow::ValidationResult ghost_result =
        lubancode::workflow::ValidateDefinition(ghost, caps);
    bool unknown_agent_seen = false;
    for (const auto& issue : ghost_result.issues) {
        if (issue.code == "unknown_agent") {
            unknown_agent_seen = true;
            std::printf("validate(鬼名): %s %s: %s\n", issue.code.c_str(), issue.path.c_str(),
                        issue.message.c_str());
        }
    }
    if (!unknown_agent_seen) {
        std::fputs("反面失守:鬼名没报 unknown_agent\n", stderr);
        return 1;
    }

    if (mode == "validate") {
        return 0;
    }

    // 跑一幕:假后端 + 统一 Resolver 的节点解析(与生产装配同构)。
    lubancode::tools::ToolRegistry registry;
    registry.Register(std::make_unique<SmokeTool>("read_file"));
    registry.Register(std::make_unique<SmokeTool>("search"));
    registry.Register(std::make_unique<SmokeTool>("run_command"));

    lubancode::agent::AgentProfile parent;
    parent.provider = "smoke-provider";
    parent.request.model = "smoke-model";
    parent.runtime.max_steps_per_turn = 8;

    SmokeBackend backend;
    lubancode::workflow::AgentExecutor::Options options;
    options.default_binding.backend = &backend;
    options.default_binding.profile = parent;
    options.registry = &registry;
    const fs::path prompt_dir = workflow_dir;
    options.task_loader = [&prompt_dir](const std::string& relative) {
        return ReadFile(prompt_dir / relative);
    };
    options.custom_agent_resolver = [&catalog, &parent, &registry](
                                        const lubancode::workflow::WorkflowNode& node,
                                        std::string& error) -> std::optional<lubancode::workflow::CustomAgentNodeResolution> {
        const lubancode::agent::AgentCatalogEntry* entry = catalog.Find(node.agent);
        if (entry == nullptr || !entry->available || !entry->definition.has_value()) {
            error = "没有名叫 \"" + node.agent + "\" 的 Agent";
            return std::nullopt;
        }
        lubancode::workflow::CustomAgentNodeResolution out;
        lubancode::agent::AgentDispatchOverrides overrides;
        if (node.step_limit > 0) overrides.max_steps_per_turn = node.step_limit;
        std::vector<std::string> parent_tools;
        for (const auto& tool : registry.All()) parent_tools.push_back(tool->name());
        out.resolved = lubancode::agent::ResolveAgentProfile(lubancode::agent::BuildWorkflowAgentResolveRequest(
            *entry->definition, parent, parent_tools, parent.runtime.max_steps_per_turn,
            lubancode::agent::AgentProfileResolveEnvironment{}, overrides));
        out.material.definition = *entry->definition;
        out.resolved_name = entry->name;
        return out;
    };
    options.subagent_prompt_material.cwd = "D:/smoke/work";
    auto agent_executor = std::make_shared<lubancode::workflow::AgentExecutor>(std::move(options));

    lubancode::workflow::RuntimeOptions runtime_options;
    runtime_options.executors[lubancode::workflow::NodeKind::Agent] = agent_executor;
    lubancode::workflow::WorkflowRuntime runtime(std::move(runtime_options));
    const lubancode::workflow::WorkflowRunSummary summary =
        runtime.Run(def, lubancode::workflow::RunInputs(nlohmann::json{{"topic", std::string("冒烟主题")}}));

    std::printf("run: state=%s\n", lubancode::workflow::ToString(summary.state).c_str());
    for (const auto& [node_id, record] : summary.nodes) {
        std::printf("  node %s: %s", node_id.c_str(), lubancode::workflow::ToString(record.state).c_str());
        if (!record.agent_name.empty()) {
            std::printf(" agent=%s", record.agent_name.c_str());
        }
        if (!record.error_code.empty()) {
            std::printf(" [%s] %s", record.error_code.c_str(), record.error_message.substr(0, 120).c_str());
        }
        std::printf("\n");
    }
    std::printf("result: %s\n", summary.result.dump().c_str());

    if (summary.state != lubancode::workflow::RunState::Succeeded) return 1;
    if (backend.requests.empty()) {
        std::fputs("假后端没收到请求\n", stderr);
        return 1;
    }
    const lubancode::api::Request& first = backend.requests.front();
    std::printf("system 首行: %.80s\n", first.system.c_str());
    std::printf("model=%s max_tokens=%d\n", first.model.c_str(), first.max_tokens.value_or(0));
    std::printf("tools:");
    for (const auto& tool : first.tools) std::printf(" %s", tool.name.c_str());
    std::printf("\n");
    // 回执身份:resolved 名进节点账(record.agent_name,与 journal 完成事件同一笔)。
    const auto probe_it = summary.nodes.find("probe");
    if (probe_it == summary.nodes.end() || probe_it->second.agent_name != "smoke-probe") {
        std::fputs("回执身份不是 resolved 名\n", stderr);
        return 1;
    }
    return 0;
}
