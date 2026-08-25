// 宿主侧执行器实现(自然语言编排单第 4 批)。

#include "workflow/host_executors.hpp"

#include <set>
#include <utility>
#include <variant>

namespace lubancode::workflow {

// ---------------------------------------------------------------------------
// tool
// ---------------------------------------------------------------------------

ToolExecutor::ToolExecutor(const tools::ToolRegistry* registry, ToolConfirmGate confirm)
    : registry_(registry), confirm_(std::move(confirm)) {}

NodeExecResult ToolExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    if (registry_ == nullptr) {
        result.error_code = "not_configured";
        result.error_message = "ToolRegistry 没装配";
        return result;
    }
    tools::Tool* tool = registry_->Find(request.node->tool);
    if (tool == nullptr) {
        // capability check 的运行时半边:定义列得出,跑时没有。
        // on_unavailable 由 runtime 按定义走 fail/skip/fallback/ask。
        result.error_code = "tool_unavailable";
        result.error_message = "工具未注册: " + request.node->tool;
        return result;
    }
    if (tool->needs_confirm()) {
        if (!confirm_) {
            result.error_code = "not_configured";
            result.error_message = "工具要确认,但没人管确认门: " + request.node->tool;
            return result;
        }
        if (!confirm_(request.node->tool, request.resolved_input)) {
            result.error_code = "permission_denied";
            result.error_message = "工具被确认门拒绝: " + request.node->tool;
            return result;
        }
    }
    const tools::Tool::Result tool_result = tool->execute(request.resolved_input);
    if (tool_result.is_error) {
        result.error_code = "tool_error";
        result.error_message = tool_result.content.substr(0, 500);
        return result;
    }
    // 工具结果原是文本;能解析成 JSON 就按结构交下游,不能就包 content 字段。
    nlohmann::json parsed = nlohmann::json::object();
    bool parsed_ok = false;
    try {
        parsed = nlohmann::json::parse(tool_result.content);
        parsed_ok = true;
    } catch (...) {
    }
    result.output = parsed_ok ? parsed : nlohmann::json{{"content", tool_result.content}};
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// agent
// ---------------------------------------------------------------------------

AgentExecutor::AgentExecutor(Options options) : options_(std::move(options)) {}

NodeExecResult AgentExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    if (options_.registry == nullptr) {
        result.error_code = "not_configured";
        result.error_message = "agent 节点没有 ToolRegistry";
        return result;
    }

    std::optional<Binding> resolved;
    if (options_.resolve_binding) {
        resolved = options_.resolve_binding(*request.node);
    }
    Binding binding = resolved.has_value() ? std::move(*resolved) : options_.default_binding;
    if (binding.backend == nullptr) {
        result.error_code = "not_configured";
        result.error_message = "agent 节点没有 backend";
        return result;
    }

    const std::string task_prompt = options_.task_loader ? options_.task_loader(request.node->task) : std::string();
    if (task_prompt.empty()) {
        result.error_code = "prompt_unreadable";
        result.error_message = "task 读不到: " + request.node->task;
        return result;
    }
    binding.profile.system_prompt = task_prompt;
    if (request.node->step_limit > 0) {
        binding.profile.runtime.max_steps_per_turn = request.node->step_limit;
    }
    if (binding.profile.request.model.empty()) {
        binding.profile.request.model = binding.profile.runtime.model;
    }

    agent::Agent task_agent(*binding.backend, *options_.registry, std::move(binding.profile));
    if (!request.node->allowed_tools.empty()) {
        const auto allowed = std::make_shared<const std::set<std::string>>(
            request.node->allowed_tools.begin(), request.node->allowed_tools.end());
        task_agent.SetToolFilter([allowed](const tools::Tool& tool) {
            return allowed->contains(tool.name());
        });
        task_agent.SetToolFilterDenial("此工具不在 workflow agent 节点的 allowed_tools 里。");
    }

    std::string text;
    std::int64_t tokens = 0;
    agent::Callbacks callbacks = options_.callbacks;
    const auto outer_text = callbacks.on_text_delta;
    callbacks.on_text_delta = [&](const std::string& delta) {
        text += delta;
        if (outer_text) outer_text(delta);
    };
    const auto outer_usage = callbacks.on_usage;
    callbacks.on_usage = [&](const api::UsageReport& report) {
        tokens += api::TotalInputTokens(report.usage) + report.usage.output_tokens;
        if (outer_usage) outer_usage(report);
    };
    // workflow 没接审批宿主时，危险工具明拒；不能因回调空着便默认放行。
    if (!callbacks.on_tool_confirm && !callbacks.on_tool_confirm_async) {
        callbacks.on_tool_confirm = [](const std::string&, const std::string&, const nlohmann::json&) {
            return false;
        };
    }

    const auto outcome = task_agent.Run(request.resolved_input.dump(), callbacks, nullptr);
    result.tokens_used = tokens;
    if (!outcome.has_value()) {
        result.error_code = "agent_error";
        result.error_message = outcome.error().substr(0, 500);
        return result;
    }
    if (outcome->hit_step_limit) {
        result.error_code = "budget_exhausted";
        result.error_message = "agent 节点达到 step_limit";
        return result;
    }
    if (outcome->length_empty_output) {
        result.error_code = "output_budget_exhausted";
        result.error_message = "agent 节点输出预算耗尽，未产出正文";
        return result;
    }

    // 有的后端不逐段吐 TextDelta，只在历史收口；以 Agent 的真历史兜底。
    if (text.empty() && !task_agent.History().empty()) {
        for (const auto& block : task_agent.History().back().content) {
            if (const auto* body = std::get_if<api::TextBlock>(&block)) text += body->text;
        }
    }
    try {
        result.output = nlohmann::json::parse(text);
    } catch (...) {
        result.output = nlohmann::json{{"content", text}};
    }
    result.empty = text.empty();
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// llm
// ---------------------------------------------------------------------------

LlmExecutor::LlmExecutor(Options options) : options_(std::move(options)) {}

NodeExecResult LlmExecutor::Execute(const NodeExecRequest& request) {
    std::string system_prompt;
    if (options_.prompt_loader) {
        system_prompt = options_.prompt_loader(request.node->prompt);
    }
    return ExecuteWithPrompt(request, system_prompt);
}

NodeExecResult LlmExecutor::ExecuteWithPrompt(const NodeExecRequest& request, const std::string& prompt_text) {
    NodeExecResult result;
    if (options_.backend == nullptr) {
        result.error_code = "not_configured";
        result.error_message = "llm 节点没有 backend";
        return result;
    }
    if (prompt_text.empty()) {
        result.error_code = "prompt_unreadable";
        result.error_message = "prompt 读不到: " + request.node->prompt;
        return result;
    }

    api::Request req;
    req.model = options_.model;
    req.system = prompt_text;
    if (options_.max_output_tokens > 0) {
        req.max_tokens = static_cast<int>(options_.max_output_tokens);
    }
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{request.resolved_input.dump()});
    req.messages.push_back(std::move(user));

    std::string assembled;
    api::Usage usage;
    const auto on_event = [&](const api::StreamEvent& event) {
        if (const auto* text = std::get_if<api::TextDelta>(&event)) {
            assembled += text->text;
        } else if (const auto* done = std::get_if<api::MessageDone>(&event)) {
            usage = done->usage;
        }
    };
    const auto sent = options_.backend->send_stream(req, on_event, nullptr);
    if (!sent.has_value()) {
        result.error_code = sent.error().kind == api::ErrorKind::Cancelled ? "cancelled" : "api_error";
        result.error_message = sent.error().message.substr(0, 500);
        return result;
    }
    // token 账:输入+输出都计入 run 预算(重试也计,不开免单账)。
    result.tokens_used = api::TotalInputTokens(usage) + usage.output_tokens;

    nlohmann::json parsed = nlohmann::json::object();
    bool parsed_ok = false;
    try {
        parsed = nlohmann::json::parse(assembled);
        parsed_ok = true;
    } catch (...) {
    }
    result.output = parsed_ok ? parsed : nlohmann::json{{"content", assembled}};
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// approval / ask_user
// ---------------------------------------------------------------------------

ApprovalExecutor::ApprovalExecutor(runtime::InteractionBroker* broker) : broker_(broker) {}

NodeExecResult ApprovalExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    if (broker_ == nullptr) {
        result.error_code = "not_configured";
        result.error_message = "approval 节点没有 InteractionBroker(不挂死,明报)";
        return result;
    }
    runtime::ApprovalRequest approval;
    approval.tool_name = "workflow_approval:" + request.node->id;
    approval.input = request.resolved_input;
    approval.reason = "workflow 节点 " + request.node->id + " 请求审批";
    auto future = broker_->AskApproval(approval);
    const auto decision = future->WaitApproval();
    if (!decision.has_value()) {
        result.error_code = "cancelled";
        result.error_message = "审批悬空收口(没人可答)";
        return result;
    }
    switch (decision->decision) {
        case runtime::InteractionDecision::Accept:
        case runtime::InteractionDecision::AcceptForSession:
            result.output = nlohmann::json{{"approved", true}};
            result.ok = true;
            return result;
        case runtime::InteractionDecision::Decline:
            result.error_code = "approval_declined";
            result.error_message = decision->reason.empty() ? "用户拒绝" : decision->reason;
            return result;
        case runtime::InteractionDecision::Cancel:
            result.error_code = "cancelled";
            result.error_message = "审批被取消";
            return result;
    }
    result.error_code = "cancelled";
    return result;
}

AskUserExecutor::AskUserExecutor(runtime::InteractionBroker* broker) : broker_(broker) {}

NodeExecResult AskUserExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    if (broker_ == nullptr) {
        result.error_code = "not_configured";
        result.error_message = "ask_user 节点没有 InteractionBroker(不挂死,明报)";
        return result;
    }
    runtime::QuestionRequest question;
    question.question = request.resolved_input.value("question", std::string("请补充:"));
    if (const auto header = request.resolved_input.find("header");
        header != request.resolved_input.end() && header->is_string()) {
        question.header = header->get<std::string>();
    }
    auto future = broker_->AskQuestion(question);
    const auto answer = future->WaitQuestion();
    if (!answer.has_value() || answer->answers.empty()) {
        result.error_code = "cancelled";
        result.error_message = "提问悬空收口";
        return result;
    }
    result.output = nlohmann::json{{"answers", answer->answers}};
    result.ok = true;
    return result;
}

// ---------------------------------------------------------------------------
// skill
// ---------------------------------------------------------------------------

SkillExecutor::SkillExecutor(std::shared_ptr<LlmExecutor> llm, std::map<std::string, std::string> skill_bodies)
    : llm_(std::move(llm)), skill_bodies_(std::move(skill_bodies)) {}

NodeExecResult SkillExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    const auto it = skill_bodies_.find(request.node->skill);
    if (it == skill_bodies_.end()) {
        result.error_code = "unknown_skill";
        result.error_message = "skill 不存在: " + request.node->skill;
        return result;
    }
    if (llm_ == nullptr) {
        result.error_code = "not_configured";
        result.error_message = "skill 节点没有 llm 执行器托底";
        return result;
    }
    // 把 SKILL.md 装进执行上下文:不改图、不执行文本,只作为章法喂给
    // 单次模型调用(单子"Workflow 与 Skill":不把 Skill 文本当代码跑)。
    return llm_->ExecuteWithPrompt(request, it->second);
}

// ---------------------------------------------------------------------------
// subflow
// ---------------------------------------------------------------------------

SubflowExecutor::SubflowExecutor(DefinitionResolver resolver, RuntimeRunner runner)
    : resolver_(std::move(resolver)), runner_(std::move(runner)) {}

NodeExecResult SubflowExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    auto def = resolver_(request.node->subflow_id);
    if (!def.has_value()) {
        result.error_code = "unknown_subflow";
        result.error_message = "workflow 不存在: " + request.node->subflow_id;
        return result;
    }
    // 输入显式映射(单子:子 Workflow 只拿显式映射的输入,不默认继承)。
    const WorkflowRunSummary sub = runner_(*def, request.resolved_input);
    if (sub.state == RunState::Succeeded) {
        result.output = sub.result;
        result.ok = true;
        result.tokens_used = sub.tokens_used;
        return result;
    }
    // 错误不能穿墙变成一串文本(单子 Edge 一节):稳定 code 交回父图。
    result.error_code = "subflow_" + ToString(sub.state);
    result.error_message = sub.error_code + ": " + sub.error_message;
    return result;
}

}  // namespace lubancode::workflow
