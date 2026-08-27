// 宿主侧执行器实现(自然语言编排单第 4 批;批一封暗道:tool 走
// agent::RunOneTool 正门,llm 走 agent::SampleModel 原语;批五乙降策略:
// agent 节点的 turn 推进走 agent::DriveTurn,不再自家直调 Agent::Run)。

#include "workflow/host_executors.hpp"

#include <chrono>
#include <set>
#include <utility>
#include <variant>

#include "agent/sample_model.hpp"
#include "agent/tool_trace.hpp"  // kErrPermissionDeclined:旧稳定码的映射锚
#include "agent/turn_harness.hpp"  // DriveTurn:agent 节点的 turn 推进正门(批五乙)
#include "platform/wall_clock.hpp"  // 统一墙钟(批五):trace 批头事件的钟同源
#include "runtime/turn_event_adapter.hpp"

namespace lubancode::workflow {

// ---------------------------------------------------------------------------
// tool
// ---------------------------------------------------------------------------

ToolExecutor::ToolExecutor(Options options) : options_(std::move(options)) {}

ToolExecutor::ToolExecutor(tools::ToolRegistry* registry, ToolConfirmGate confirm) {
    options_.registry = registry;
    options_.confirm = std::move(confirm);
}

NodeExecResult ToolExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    if (options_.registry == nullptr) {
        result.error_code = "not_configured";
        result.error_message = "ToolRegistry 没装配";
        return result;
    }
    tools::Tool* tool = options_.registry->Find(request.node->tool);
    if (tool == nullptr) {
        // capability check 的运行时半边:定义列得出,跑时没有。
        // on_unavailable 由 runtime 按定义走 fail/skip/fallback/ask。
        result.error_code = "tool_unavailable";
        result.error_message = "工具未注册: " + request.node->tool;
        return result;
    }

    // 装配链:宿主带来的钩子/权限/trace 原样用;旧 confirm gate 只在宿主
    // 没给确认口时兜底(不越权覆盖正门装配)。
    agent::TurnWiring chain = options_.callbacks;
    if (!chain.on_tool_confirm && !chain.on_tool_confirm_async && options_.confirm) {
        chain.on_tool_confirm = [gate = options_.confirm](const std::string&, const std::string& name,
                                                          const nlohmann::json& input) {
            return gate(name, input);
        };
    }
    // 旧路的守门语义照旧:needs_confirm 的工具既没有宿主确认口也没有旧
    // gate 时,明拒 not_configured——RunOneTool 缺省放行,这里不许静默跟放。
    if (tool->needs_confirm() && !chain.on_tool_confirm && !chain.on_tool_confirm_async) {
        result.error_code = "not_configured";
        result.error_message = "工具要确认,但没人管确认门: " + request.node->tool;
        return result;
    }

    // trace 上下文:发号口与事件口都装上才追踪(缺一 = 没装配,全空操作)。
    const bool trace_armed = options_.execution_id_issuer && chain.on_tool_trace;
    agent::ToolTraceContext trace_ctx;
    if (trace_armed) {
        trace_ctx.execution_id = options_.execution_id_issuer();
        trace_ctx.thread_id = options_.thread_id;
        trace_ctx.turn_id = !options_.turn_id.empty() ? options_.turn_id : request.run_id;
        trace_ctx.batch_id = request.node_run_id;
        trace_ctx.sequence_in_batch = 0;
        // 排队栅栏先落一枚(与 AgentLoop 的批次头同款):/trace 的批次账
        // 与恢复侧都靠它认批。
        agent::ToolTraceEvent scheduled;
        scheduled.kind = agent::ToolTraceEventKind::Scheduled;
        scheduled.execution_id = trace_ctx.execution_id;
        scheduled.item_id = trace_ctx.execution_id;
        scheduled.tool_use_id = request.node_run_id;
        scheduled.tool_name = request.node->tool;
        scheduled.batch_id = trace_ctx.batch_id;
        scheduled.sequence_in_batch = 0;
        scheduled.turn_id = trace_ctx.turn_id;
        scheduled.thread_id = trace_ctx.thread_id;
        // 批五:统一墙钟(口径不变,只收源)。
        scheduled.timestamp_ms = platform::WallClockNowMs();
        chain.on_tool_trace(scheduled);
    }

    // 正门一发:PreToolUse(schema 复检)/Plan 闸/确认档/执行/PostToolUse/
    // 编码清洗/finished 栅栏全在这条链里。tool_use_id 用本 attempt 的
    // node_run_id 宿主合成(模型没给,与 PTC 的 "ptc-N" 同一作法)。
    api::ToolUseBlock call;
    call.id = !request.node_run_id.empty() ? request.node_run_id : "wf-" + request.node->id;
    call.name = request.node->tool;
    call.input = request.resolved_input;
    const tools::Tool::Result tool_result = agent::RunOneTool(
        *options_.registry, call, chain, /*tool_filter=*/nullptr, std::string(),
        trace_armed ? &trace_ctx : nullptr);
    if (tool_result.is_error) {
        // 稳定码映射:旧两码(permission_denied/tool_error)不动,新路才有
        // 的失败形态(钩子拦/Plan 拒/schema 打回)按 RunOneTool 给的稳定码
        // 透传,journal 与 on_error 边看得见细因。
        result.error_code = tool_result.error_code == agent::kErrPermissionDeclined
                                ? std::string("permission_denied")
                                : (tool_result.error_code.empty() ? std::string("tool_error")
                                                                  : tool_result.error_code);
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
    // 工具可见性(病十三的方向):allowed_tools 的白名单写进皮,不再走
    // loop 级 setter。
    if (!request.node->allowed_tools.empty()) {
        const auto allowed = std::make_shared<const std::set<std::string>>(
            request.node->allowed_tools.begin(), request.node->allowed_tools.end());
        binding.profile.tool_filter = [allowed](const tools::Tool& tool) {
            return allowed->contains(tool.name());
        };
        binding.profile.tool_filter_denial = "此工具不在 workflow agent 节点的 allowed_tools 里。";
    }

    agent::Agent task_agent(*binding.backend, *options_.registry, std::move(binding.profile));

    std::string text;
    std::int64_t tokens = 0;
    // 事件流(批二余款:显示出水只有这只口)。适配器常起——结果要观察正文
    // 与 usage 记账(result 的 text/tokens 从流里取);装了 sink 的节点再把
    // 同一份流落会话 sink——turn_id 用本次 run 的 run_id(节点嵌套轮,与
    // ToolExecutor 的 trace 上下文同口径)。控制口(确认/钩子)原样走
    // TurnWiring。
    agent::TurnWiring wiring = options_.callbacks;
    runtime::TurnEventAdapter events(!options_.thread_id.empty() ? options_.thread_id : std::string("workflow"),
                                     options_.ids != nullptr ? *options_.ids : runtime::ProcessIdAuthority());
    {
        runtime::EventSink* sink = options_.event_sink;
        std::string* text_out = &text;
        std::int64_t* tokens_out = &tokens;
        events.Attach([sink, text_out, tokens_out](const runtime::ServerEvent& event) {
            switch (event.kind) {
                case runtime::ServerEventKind::ItemDelta:
                    if (event.item_kind == runtime::ItemKind::Text) {
                        *text_out += event.text;
                    }
                    break;
                case runtime::ServerEventKind::UsageUpdated:
                    *tokens_out += event.payload.value("input_tokens", std::int64_t{0}) +
                                   event.payload.value("cache_read_tokens", std::int64_t{0}) +
                                   event.payload.value("cache_creation_tokens", std::int64_t{0}) +
                                   event.payload.value("output_tokens", std::int64_t{0});
                    break;
                default:
                    break;
            }
            if (sink != nullptr) {
                sink->Emit(event);
            }
        });
        events.Start(request.run_id);
    }
    wiring.events = &events;
    // workflow 没接审批宿主时,危险工具明拒;不能因回调空着便默认放行。
    if (!wiring.on_tool_confirm && !wiring.on_tool_confirm_async) {
        wiring.on_tool_confirm = [](const std::string&, const std::string&, const nlohmann::json&) {
            return false;
        };
    }

    // turn 推进走 TurnHarness 的续投外环(批五乙:三外壳降策略——workflow
    // 的 agent 节点不再自家直调 Agent::Run,run turn 的路全仓只 harness 一
    // 份;批二余款:Callbacks 退役,装配走 TurnWiring)。单轮即收:没续投
    // 源、没墙钟、没取消链(取消在 runtime 的节点边界看,与从前一致)。
    api::Message task_input;
    task_input.role = api::Role::User;
    task_input.content.push_back(api::TextBlock{request.resolved_input.dump()});
    agent::DriveOptions drive_options;
    drive_options.cancel = request.cancel;
    const agent::DriveReport drive =
        agent::DriveTurn(task_agent, wiring, std::move(task_input), std::move(drive_options));

    // 事件流收口:DriveTurn 一返回就按收场分型 Finish(后面的早退分支各
    // 走各的 error_code,终态映射在这定死:报错/预算尽 = Failed,打断 =
    // Cancelled,其余 Succeeded)。
    const bool empty_output = drive.final_round.has_value() && drive.final_round->length_empty_output;
    events.Finish(!drive.ok || drive.hit_step_limit || empty_output
                      ? runtime::Outcome::Failed
                  : drive.cancelled ? runtime::Outcome::Cancelled
                                    : runtime::Outcome::Succeeded,
                  drive.ok ? std::string() : drive.error);
    result.tokens_used = tokens;
    if (!drive.ok) {
        result.error_code = "agent_error";
        result.error_message = drive.error.substr(0, 500);
        return result;
    }
    if (drive.hit_step_limit) {
        result.error_code = "budget_exhausted";
        result.error_message = "agent 节点达到 step_limit";
        return result;
    }
    if (empty_output) {
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

    // 采样走 SampleModel 原语(批一·病四):路只有一条,提示拼装仍在本层。
    // 旧路的语义原样:无看门狗、无取消;流错折 Api;Cancelled 单列。
    agent::SampleRequest sample;
    sample.model = options_.model;
    sample.system = prompt_text;
    if (options_.max_output_tokens > 0) {
        sample.max_tokens = static_cast<int>(options_.max_output_tokens);
    }
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{request.resolved_input.dump()});
    sample.messages.push_back(std::move(user));

    agent::SampleOptions sample_options;
    sample_options.cancel = request.cancel;
    const agent::SampleResult sampled = agent::SampleModel(*options_.backend, sample, sample_options);
    if (!sampled.ok) {
        result.error_code =
            sampled.error.kind == api::ErrorKind::Cancelled ? "cancelled" : "api_error";
        result.error_message = sampled.error.message.substr(0, 500);
        return result;
    }
    // token 账:输入+输出都计入 run 预算(重试也计,不开免单账)。
    result.tokens_used = api::TotalInputTokens(sampled.usage) + sampled.usage.output_tokens;

    nlohmann::json parsed = nlohmann::json::object();
    bool parsed_ok = false;
    try {
        parsed = nlohmann::json::parse(sampled.text);
        parsed_ok = true;
    } catch (...) {
    }
    result.output = parsed_ok ? parsed : nlohmann::json{{"content", sampled.text}};
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
