// 宿主侧执行器实现(自然语言编排单第 4 批;批一封暗道:tool 走
// agent::RunOneTool 正门,llm 走 agent::SampleModel 原语;批五乙降策略:
// agent 节点的 turn 推进走 agent::DriveTurn,不再自家直调 Agent::Run)。

#include "workflow/host_executors.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <utility>
#include <variant>

#include "agent/prompt_assembler.hpp"  // AssembleSystemPrompt:自定义 Agent 的同源拼装
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

    // ---- 自定义 Agent(`agent: <name>`,阶段 5)---------------------------
    // 名字非空时先走统一解析:宿主查 AgentCatalog(canonical/裸名;包内
    // 短引用已在 Package 挂载层折成 canonical),解析不过(查无此名、
    // unavailable、定义或环境有错)在这里失败——运行时首知即报,不静默
    // 退回 default binding,也不悄悄换 general-purpose。
    std::optional<CustomAgentNodeResolution> custom;
    const bool is_custom = !request.node->agent.empty();
    if (is_custom) {
        if (!options_.custom_agent_resolver) {
            result.error_code = "not_configured";
            result.error_message = "agent 节点点名了自定义 Agent,但宿主没接解析口: " + request.node->agent;
            return result;
        }
        std::string resolve_error;
        custom = options_.custom_agent_resolver(*request.node, resolve_error);
        if (!custom.has_value()) {
            result.error_code = "agent_unresolved";
            result.error_message = "自定义 Agent 解析不过: " + request.node->agent +
                                   (resolve_error.empty() ? std::string("(查无此名,可用清单看 /agents)")
                                                          : ": " + resolve_error);
            return result;
        }
        if (!custom->resolved.ok()) {
            result.error_code = "agent_unresolved";
            result.error_message = "自定义 Agent \"" + request.node->agent + "\" 解析不过(定义或环境有错;先 " +
                                   "/agent doctor " + request.node->agent + " 看诊断):\n" +
                                   agent::FormatResolutionIssues(custom->resolved.issues);
            return result;
        }
        result.agent_name = custom->resolved_name;
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
    if (is_custom) {
        // 统一解析的皮整份接管:provider/request/runtime/四段开关/工具谓词
        // 都在 Resolver 里合并完(与 agent 工具路同一只 AgentProfileResolver,
        // 两路逐字段一致的账钉在对账册)。backend 仍走宿主递的这条。
        binding.profile = custom->resolved.profile;
    }

    const std::string task_prompt = options_.task_loader ? options_.task_loader(request.node->task) : std::string();
    if (task_prompt.empty()) {
        result.error_code = "prompt_unreadable";
        result.error_message = "task 读不到: " + request.node->task;
        return result;
    }
    if (is_custom) {
        // 系统提示与 agent 工具路同源(阶段 5 的验收线):同一只
        // BuildSubagentPromptOptions + AssembleSystemPrompt + 预装技能段
        // ——Prompt Profile 五层回路、能力推导、AGENTS.md 继承、魂启停
        // 全按 Resolver 的决议走。task 是任务指令,不进系统提示(与
        // agent 工具路的用户 prompt 同位,进 user message)。
        const SubagentPromptMaterial& mat = options_.subagent_prompt_material;
        agent::ResolvedAgentProfile prompt_resolved = custom->resolved;
        if (!request.node->allowed_tools.empty()) {
            // 节点白名单压过 YAML 的 allow(契约 §4.8:调用方显式 > 定义
            // 缺省):谓词重设,effective_tools 同步收窄——prompt 的能力
            // 推导按交集算,文案不吹不存在的工具。
            std::set<std::string> allowed(request.node->allowed_tools.begin(), request.node->allowed_tools.end());
            std::vector<std::string> intersected;
            for (const std::string& name : prompt_resolved.effective_tools) {
                if (allowed.count(name) > 0) intersected.push_back(name);
            }
            prompt_resolved.effective_tools = std::move(intersected);
        }
        binding.profile.system_prompt =
            agent::AssembleSystemPrompt(tools::BuildSubagentPromptOptions(
                mat.cwd, custom->resolved_name, mat.prompts_dir, mat.project_prompts_dir,
                mat.project_instructions, mat.skills_segment, binding.profile, &custom->material,
                &prompt_resolved, mat.package_profile_roots)) +
            tools::AppendPreloadedSkills(custom->material.definition.skills_preload,
                                         custom->material.preloaded_skills);
    } else {
        binding.profile.system_prompt = task_prompt;
    }
    if (!is_custom && request.node->step_limit > 0) {
        // 自定义路的步数在 Resolver 里并过(节点 step_limit 走 overrides
        // 三级:入参 > YAML > 父步数),这里不重设,免得两笔账打架。
        binding.profile.runtime.max_steps_per_turn = request.node->step_limit;
    }
    // 工具可见性(病十三的方向):allowed_tools 的白名单写进皮,不再走
    // loop 级 setter。自定义路的 YAML allow/deny 已由 Resolver 装好,节点
    // 白名单(给了的话)压过它——同一道门,非自定义路行为一字不动。
    if (!request.node->allowed_tools.empty()) {
        const auto allowed = std::make_shared<const std::set<std::string>>(
            request.node->allowed_tools.begin(), request.node->allowed_tools.end());
        binding.profile.tool_filter = [allowed](const tools::Tool& tool) {
            return allowed->contains(tool.name());
        };
        binding.profile.tool_filter_denial = "此工具不在 workflow agent 节点的 allowed_tools 里。";
    } else if (is_custom && binding.profile.tool_filter == nullptr) {
        // Resolver 的 allow/deny 全空时旧语义是全放行(自定义 Agent 不吃
        // 装配层的延迟过滤)——与 agent 工具路同一笔账。
        binding.profile.tool_filter = [](const tools::Tool&) { return true; };
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
        const std::string node_run_id = request.node_run_id;
        const std::string node_id = request.node->id;
        const std::string node_label = request.node->label;
        events.Attach([sink, text_out, tokens_out, node_run_id, node_id,
                       node_label](const runtime::ServerEvent& event) {
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
                runtime::ServerEvent forwarded = event;
                if (!forwarded.payload.is_object()) forwarded.payload = nlohmann::json::object();
                forwarded.payload["workflow_node_run_id"] = node_run_id;
                forwarded.payload["workflow_node_id"] = node_id;
                forwarded.payload["workflow_node_label"] = node_label;
                sink->Emit(forwarded);
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
        // 拒词也说明白:缺省那句"用户拒绝"是假话——根本没人可拒。
        wiring.on_tool_denial_text = [](const std::string&, const std::string& name) {
            return "workflow agent 节点未接审批宿主，已拒绝 " + name;
        };
    }
    // 权限下限接线(阶段 5,R 单遗留):Resolver 已校验"不许放宽"(越宽在
    // 解析口明拒),"收窄生效"在这半截——自定义 Agent 的定义档比会话档
    // 严时(父 yolo 子 confirm),确认回调换成宿主的"带下限"口,会话档
    // 向下并到下限再裁定,该问就真把确认拉回。宿主没接 floored 口(旧
    // 装配)或档不比父严时,原样转发,行为不变。与 agent 工具路的
    // Hooks::on_tool_confirm_floored 同一先例(0.26.96)。
    if (custom.has_value() && custom->permission_floor.has_value() && wiring.on_tool_confirm_floored) {
        auto floored = wiring.on_tool_confirm_floored;
        const agent::AgentPermissionMode floor = *custom->permission_floor;
        wiring.on_tool_confirm = [floored, floor](const std::string& tool_use_id, const std::string& name,
                                                  const nlohmann::json& input) {
            return floored(tool_use_id, name, input, floor);
        };
    }

    // turn 推进走 TurnHarness。面板补充只在一轮正常收口后取，取到便另开
    // 一轮；送不成由 batch.restore 退回原队列。
    api::Message task_input;
    task_input.role = api::Role::User;
    if (is_custom) {
        // 自定义路(阶段 5):task 是任务指令,与 agent 工具路的用户 prompt
        // 同位——正文在前、节点的 resolved input 在后,同一条 user message。
        std::string user_text = task_prompt;
        if (!request.resolved_input.empty()) {
            if (!user_text.empty()) user_text += "\n\n";
            user_text += request.resolved_input.dump();
        }
        task_input.content.push_back(api::TextBlock{std::move(user_text)});
    } else {
        task_input.content.push_back(api::TextBlock{request.resolved_input.dump()});
    }
    agent::DriveOptions drive_options;
    drive_options.cancel = request.cancel;
    int steering_round = 1;
    if (options_.steering) {
        drive_options.continuation = [this, &request, &events, &text,
                                      &steering_round]() -> std::optional<agent::ContinuationBatch> {
            // 先收当前文字条目，再把用户补充记进详情账；下一轮正文才会排
            // 在补充之后，不会倒插回上一张卡。
            events.Finish(runtime::Outcome::Succeeded);
            auto batch = options_.steering(request);
            if (!batch.has_value()) return std::nullopt;
            text.clear();  // workflow 输出认最后一轮，旧轮仍留在事件账里。
            ++steering_round;
            events.Start(request.run_id + "-r" + std::to_string(steering_round));
            agent::ContinuationBatch out;
            out.input = std::move(batch->input);
            out.restore = std::move(batch->restore);
            return out;
        };
    }
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
    std::optional<Binding> resolved;
    if (options_.resolve_binding) resolved = options_.resolve_binding(*request.node);
    Binding binding;
    if (resolved.has_value()) {
        binding = std::move(*resolved);
    } else {
        binding.backend = options_.backend;
        binding.model = options_.model;
        binding.reasoning_effort = options_.reasoning_effort;
    }
    if (binding.backend == nullptr) {
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
    sample.model = binding.model;
    sample.reasoning_effort = binding.reasoning_effort;
    sample.system = prompt_text;
    if (options_.max_output_tokens > 0) {
        sample.max_tokens = static_cast<int>(options_.max_output_tokens);
    }
    api::Message user;
    user.role = api::Role::User;
    user.content.push_back(api::TextBlock{request.resolved_input.dump()});
    sample.messages.push_back(std::move(user));

    runtime::TurnEventAdapter events(!options_.thread_id.empty() ? options_.thread_id : std::string("workflow"),
                                     options_.ids != nullptr ? *options_.ids : runtime::ProcessIdAuthority());
    if (options_.event_sink != nullptr) {
        runtime::EventSink* sink = options_.event_sink;
        const std::string node_run_id = request.node_run_id;
        const std::string node_id = request.node->id;
        const std::string node_label = request.node->label;
        events.Attach([sink, node_run_id, node_id, node_label](const runtime::ServerEvent& event) {
            runtime::ServerEvent forwarded = event;
            if (!forwarded.payload.is_object()) forwarded.payload = nlohmann::json::object();
            forwarded.payload["workflow_node_run_id"] = node_run_id;
            forwarded.payload["workflow_node_id"] = node_id;
            forwarded.payload["workflow_node_label"] = node_label;
            sink->Emit(forwarded);
        });
    }

    agent::SampleOptions sample_options;
    sample_options.cancel = request.cancel;
    std::string final_text;
    std::function<void()> inflight_restore;
    int round = 1;
    for (;;) {
        events.Start(round == 1 ? request.run_id : request.run_id + "-r" + std::to_string(round));
        events.OnModelStepStarted(round - 1);
        const agent::SampleResult sampled = agent::SampleModel(*binding.backend, sample, sample_options);
        result.tokens_used += api::TotalInputTokens(sampled.usage) + sampled.usage.output_tokens;
        if (sampled.usage_reported) {
            api::UsageReport usage;
            usage.usage = sampled.usage;
            usage.step_index = round - 1;
            usage.model = binding.model;
            events.OnUsage(usage);
        }
        if (!sampled.ok) {
            if (inflight_restore) inflight_restore();
            events.Finish(sampled.error.kind == api::ErrorKind::Cancelled
                              ? runtime::Outcome::Cancelled
                              : runtime::Outcome::Failed,
                          sampled.error.message);
            result.error_code =
                sampled.error.kind == api::ErrorKind::Cancelled ? "cancelled" : "api_error";
            result.error_message = sampled.error.message.substr(0, 500);
            return result;
        }
        inflight_restore = nullptr;  // 这一批已经随模型请求送达。
        final_text = sampled.text;
        events.OnTextDelta(sampled.text);
        events.Finish(runtime::Outcome::Succeeded);

        auto steering = options_.steering ? options_.steering(request) : std::nullopt;
        if (!steering.has_value()) break;

        api::Message assistant;
        assistant.role = api::Role::Assistant;
        assistant.content.push_back(api::TextBlock{sampled.text});
        sample.messages.push_back(std::move(assistant));
        api::Message followup;
        followup.role = api::Role::User;
        followup.content.push_back(api::TextBlock{steering->input});
        sample.messages.push_back(std::move(followup));
        inflight_restore = std::move(steering->restore);
        ++round;

        if (request.cancel != nullptr && request.cancel->load(std::memory_order_acquire)) {
            if (inflight_restore) inflight_restore();
            result.error_code = "cancelled";
            result.error_message = "用户取消";
            return result;
        }
    }

    nlohmann::json parsed = nlohmann::json::object();
    bool parsed_ok = false;
    try {
        parsed = nlohmann::json::parse(final_text);
        parsed_ok = true;
    } catch (...) {
    }
    result.output = parsed_ok ? parsed : nlohmann::json{{"content", final_text}};
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
    const auto answer_matches = [&request](const char* field,
                                           const std::vector<std::string>& answers) {
        const auto configured = request.resolved_input.find(field);
        if (configured == request.resolved_input.end() || !configured->is_array()) return false;
        for (const auto& candidate : *configured) {
            if (!candidate.is_string()) continue;
            if (std::find(answers.begin(), answers.end(), candidate.get<std::string>()) != answers.end()) {
                return true;
            }
        }
        return false;
    };
    const bool review_approved = request.resolved_input.value("review_approved", true);
    bool delegated_before = false;
    if (const auto previous = request.resolved_input.find("previous");
        previous != request.resolved_input.end() && previous->is_object()) {
        const auto outputs = previous->find("outputs");
        if (outputs != previous->end() && outputs->is_object()) {
            const auto prior = outputs->find(request.node->id);
            delegated_before = prior != outputs->end() && prior->is_object() &&
                               prior->value("delegated", false);
        }
    }
    // 皇帝已把余下细节交给规划者后，后续轮只走“修订 -> 独立复审”。
    // 复审没过便再修，不再把新问题一张张弹回御前。
    if (delegated_before) {
        result.output = nlohmann::json{{"answers", nlohmann::json::array()},
                                       {"skipped", true},
                                       {"delegated", true},
                                       {"approved", review_approved},
                                       {"complete", review_approved},
                                       {"overridden", false}};
        result.ok = true;
        return result;
    }
    // 澄清 loop 用：上游已经判明“信息够了”时，不再弹一张多余菜单。
    if (request.resolved_input.value("skip_when", false)) {
        result.output = nlohmann::json{{"answers", nlohmann::json::array()},
                                       {"skipped", true},
                                       {"delegated", false},
                                       {"approved", true},
                                       {"complete", true},
                                       {"overridden", false}};
        result.ok = true;
        return result;
    }
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
    if (const auto multi = request.resolved_input.find("multi_select");
        multi != request.resolved_input.end() && multi->is_boolean()) {
        question.multi_select = multi->get<bool>();
    }
    if (const auto options = request.resolved_input.find("options");
        options != request.resolved_input.end() && options->is_array()) {
        for (const auto& option : *options) {
            runtime::QuestionOption parsed;
            if (option.is_string()) {
                parsed.label = option.get<std::string>();
            } else if (option.is_object()) {
                parsed.label = option.value("label", std::string());
                parsed.description = option.value("description", std::string());
            }
            if (!parsed.label.empty()) question.options.push_back(std::move(parsed));
        }
    }
    auto future = broker_->AskQuestion(question);
    const auto answer = future->WaitQuestion();
    if (!answer.has_value() || answer->answers.empty()) {
        result.error_code = "cancelled";
        result.error_message = "提问悬空收口";
        return result;
    }
    const bool delegated = answer_matches("delegate_answers", answer->answers);
    // 墨敕(override_answers):门下已驳(review_approved=false)时皇帝仍可
    // 越权放行,命中即拍板,不掺和 approve 的账。
    const bool overridden = answer_matches("override_answers", answer->answers);
    const bool approved = overridden || (review_approved && answer_matches("approve_answers", answer->answers));
    result.output = nlohmann::json{{"answers", answer->answers},
                                   {"delegated", delegated},
                                   {"approved", approved},
                                   // 委托至少再过一轮规划与门下复审，不能当场越闸。
                                   {"complete", approved},
                                   // 恒在键:下游模板不判存在,直接读。
                                   {"overridden", overridden}};
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
