// 宿主侧执行器实现(自然语言编排单第 4 批)。

#include "workflow/host_executors.hpp"

#include <utility>

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
