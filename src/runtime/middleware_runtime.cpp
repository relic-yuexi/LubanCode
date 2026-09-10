// 统一 runtime 派发点实现(LuaHook 单 P0-B)。三入口(CLI/one-shot/
// app-server)共用;空核 = 恒等结果,零注册零改写。装配(lua 工厂)在
// middleware_assembly.cpp——那件碰 runtime 侧 Lua 适配器,归 runtime 库。
#include "runtime/middleware_runtime.hpp"

#include <utility>

namespace lubancode::runtime {

namespace {

using hooks::middleware::DispatchOutcome;
using hooks::middleware::DispatchTrigger;
using hooks::middleware::HookPoint;
using hooks::middleware::MiddlewareDispatcher;

MiddlewareDispatcher* MiddlewareOf(hooks::HookDispatcher* dispatcher) {
    return dispatcher != nullptr ? dispatcher->middleware() : nullptr;
}

DispatchTrigger MakeTrigger(const MiddlewareHookContext& context, nlohmann::json input,
                            std::optional<hooks::middleware::Stage> stage_filter = std::nullopt) {
    DispatchTrigger trigger;
    trigger.input = std::move(input);
    trigger.origin = context.origin;
    trigger.purpose = context.purpose;
    trigger.delivery_mode = context.delivery_mode;
    trigger.turn_id = context.turn_id;
    trigger.step_id = context.step_id;
    trigger.action_id = context.action_id;
    trigger.request_id = context.request_id;
    trigger.cancel = context.cancel;
    trigger.stage_filter = stage_filter;
    return trigger;
}

// 结局 → 闸门语义:业务 deny 与 required 失败都拦(deny 与脚本错误分开,
// 但对宿主都是"本轮不继续"); Completed 放行。
bool OutcomeBlocks(const DispatchOutcome& outcome, std::string* code, std::string* reason) {
    if (outcome.kind == DispatchOutcome::Kind::Denied) {
        if (code != nullptr) {
            *code = outcome.deny_code;
        }
        if (reason != nullptr) {
            *reason = outcome.deny_message;
        }
        return true;
    }
    if (outcome.kind == DispatchOutcome::Kind::Failed) {
        if (code != nullptr) {
            *code = outcome.error_code;
        }
        if (reason != nullptr) {
            *reason = outcome.error_detail;
        }
        return true;
    }
    return false;
}

bool HasSelectedFor(const hooks::HookDispatcher* dispatcher, HookPoint point) {
    const MiddlewareDispatcher* middleware = dispatcher != nullptr ? dispatcher->middleware() : nullptr;
    if (middleware == nullptr) {
        return false;
    }
    return !middleware->registry().Selected(point).empty();
}

// api::Message 的内容块 → 中立 json(估算快照口径:非文本块保类型占位)。
nlohmann::json ContentBlockToJson(const api::ContentBlock& block) {
    if (const auto* text = std::get_if<api::TextBlock>(&block)) {
        return nlohmann::json{{"type", "text"}, {"text", text->text}};
    }
    if (const auto* image = std::get_if<api::ImageBlock>(&block)) {
        // 媒体不进 bytes/4:类型占位,估算器再剥并标 unestimatedModalities。
        return nlohmann::json{{"type", "image"}, {"media_type", image->media_type},
                              {"filename", image->filename}};
    }
    if (const auto* use = std::get_if<api::ToolUseBlock>(&block)) {
        return nlohmann::json{{"type", "tool_use"}, {"id", use->id}, {"name", use->name},
                              {"input", use->input}};
    }
    if (const auto* result = std::get_if<api::ToolResultBlock>(&block)) {
        nlohmann::json out = nlohmann::json{{"type", "tool_result"},
                                            {"tool_use_id", result->tool_use_id},
                                            {"content", result->content},
                                            {"is_error", result->is_error}};
        if (result->structured_content.has_value()) {
            out["structured_content"] = *result->structured_content;
        }
        return out;
    }
    if (const auto* thinking = std::get_if<api::ThinkingBlock>(&block)) {
        // 计量包含实际回传的明文 reasoning(§4.36);签名不是思考 token 的
        // 计量物,字节长度照算(代理口径,不冒充精确预算)。
        return nlohmann::json{{"type", "thinking"}, {"text", thinking->text},
                              {"signature", thinking->signature}};
    }
    if (const auto* model_image = std::get_if<api::ModelImageBlock>(&block)) {
        return nlohmann::json{{"type", "image"}, {"filename", model_image->filename},
                              {"sha256", model_image->sha256}};
    }
    if (const auto* server_use = std::get_if<api::ServerToolUseBlock>(&block)) {
        return nlohmann::json{{"type", "server_tool_use"}, {"id", server_use->id},
                              {"name", server_use->name}, {"input", server_use->input}};
    }
    if (const auto* server_result = std::get_if<api::ServerToolResultBlock>(&block)) {
        return nlohmann::json{{"type", "server_tool_result"},
                              {"tool_use_id", server_result->tool_use_id},
                              {"content", server_result->content}};
    }
    return nlohmann::json{{"type", "unknown"}};
}

}  // namespace

// ---------------------------------------------------------------------------
// PreUser
// ---------------------------------------------------------------------------

PreUserGate RunPreUserMiddleware(hooks::HookDispatcher* dispatcher, const std::string& user_text,
                                 const MiddlewareHookContext& context,
                                 hooks::middleware::MiddlewareEventSink* sink) {
    PreUserGate gate;
    gate.prompt = user_text;
    MiddlewareDispatcher* middleware = MiddlewareOf(dispatcher);
    if (middleware == nullptr) {
        return gate;  // 零行为:与迁移前逐字节等价
    }
    DispatchTrigger trigger = MakeTrigger(context, nlohmann::json{{"prompt", user_text}});
    // 链尾 = 宿主接纳位:返回待接纳候选(§四)。
    DispatchOutcome outcome = middleware->Dispatch(HookPoint::PreUser, trigger,
                                                   [](const nlohmann::json& input) { return input; }, sink);
    gate.dispatched = true;
    gate.outcome = std::move(outcome);
    if (OutcomeBlocks(gate.outcome, &gate.block_code, &gate.block_reason)) {
        gate.blocked = true;
        return gate;
    }
    // 采用后的工作版本:改写经 next(candidate) 落账为 input.rewrite 效果,
    // adopted_input 是喂给链尾的版本。
    if (gate.outcome.adopted_input.is_object() && gate.outcome.adopted_input.contains("prompt") &&
        gate.outcome.adopted_input["prompt"].is_string()) {
        gate.prompt = gate.outcome.adopted_input["prompt"].get<std::string>();
        gate.rewritten = gate.prompt != user_text;
    }
    gate.additional_context = gate.outcome.context_appends;
    return gate;
}

// ---------------------------------------------------------------------------
// PostUser
// ---------------------------------------------------------------------------

PostUserAppend RunPostUserMiddleware(hooks::HookDispatcher* dispatcher, const std::string& admitted_prompt,
                                     const MiddlewareHookContext& context,
                                     hooks::middleware::MiddlewareEventSink* sink) {
    PostUserAppend append;
    MiddlewareDispatcher* middleware = MiddlewareOf(dispatcher);
    if (middleware == nullptr) {
        return append;
    }
    DispatchTrigger trigger = MakeTrigger(context, nlohmann::json{{"prompt", admitted_prompt}});
    DispatchOutcome outcome = middleware->Dispatch(HookPoint::PostUser, trigger,
                                                   [](const nlohmann::json& input) { return input; }, sink);
    append.dispatched = true;
    append.outcome = std::move(outcome);
    if (OutcomeBlocks(append.outcome, &append.block_code, &append.block_reason)) {
        append.blocked = true;  // 原 user 保留;required 未成功不得继续发送(§4.47)
        return append;
    }
    append.context_appends = append.outcome.context_appends;
    return append;
}

// ---------------------------------------------------------------------------
// PreRequest 三段
// ---------------------------------------------------------------------------

PreRequestStages RunPreRequestMiddleware(hooks::HookDispatcher* dispatcher,
                                         const nlohmann::json& request_snapshot,
                                         std::uint64_t context_window_tokens,
                                         std::uint64_t output_reserve_tokens,
                                         const MiddlewareHookContext& context,
                                         hooks::middleware::MiddlewareEventSink* sink) {
    using hooks::middleware::Stage;
    PreRequestStages stages;
    MiddlewareDispatcher* middleware = MiddlewareOf(dispatcher);
    if (middleware == nullptr) {
        return stages;
    }
    stages.dispatched = true;

    // ---- 段一 mutate:改输入的最后机会(§4.36)。----
    DispatchTrigger mutate_trigger = MakeTrigger(context, request_snapshot, Stage::Mutate);
    stages.mutate_outcome =
        middleware->Dispatch(HookPoint::PreRequest, mutate_trigger,
                             [](const nlohmann::json& input) { return input; }, sink);
    stages.adopted_input = stages.mutate_outcome.adopted_input;
    if (stages.mutate_outcome.kind == DispatchOutcome::Kind::Failed ||
        stages.mutate_outcome.kind == DispatchOutcome::Kind::Denied) {
        // required 失败/业务拒绝:请求不得绕过(§4.36)。
        stages.decision = "reject";
        stages.reason = stages.mutate_outcome.kind == DispatchOutcome::Kind::Denied
                            ? stages.mutate_outcome.deny_message
                            : stages.mutate_outcome.error_detail;
        return stages;
    }
    // mutate 段收尾 = freeze(宿主提交边界;§4.36)。改写被采用 → 本批不
    // 重建请求,调用方换输入版本重新准备。
    stages.reprepare_required =
        stages.adopted_input.is_object() && stages.adopted_input != request_snapshot;
    if (stages.reprepare_required) {
        stages.decision = "reprepare";
        stages.reason = "PreRequest/mutate 段采用了输入改写;须按新输入版本重新准备请求";
        return stages;
    }

    // ---- 段二 estimate:估算在冻结快照上跑(内置槽位或同名替换实现)。----
    DispatchTrigger estimate_trigger = MakeTrigger(context, request_snapshot, Stage::Estimate);
    stages.estimate_outcome =
        middleware->Dispatch(HookPoint::PreRequest, estimate_trigger,
                             [](const nlohmann::json& input) { return input; }, sink);
    if (stages.estimate_outcome.kind != DispatchOutcome::Kind::Completed ||
        !stages.estimate_outcome.value.is_object() ||
        !stages.estimate_outcome.value.contains("estimatedInputTokens")) {
        // 估算缺场/失败/形状不合合同:不得绕过检查直接发送(§4.36)。
        stages.decision = "reject";
        stages.reason = stages.estimate_outcome.kind == DispatchOutcome::Kind::Failed
                            ? "估算段失败: " + stages.estimate_outcome.error_detail
                            : "估算段未产出结构化结果(estimatedInputTokens 缺失)";
        return stages;
    }
    stages.token_estimate = stages.estimate_outcome.value;

    // ---- 段三 capacity:消费估算,给准入决定。----
    nlohmann::json capacity_input = request_snapshot;
    capacity_input["tokenEstimate"] = stages.token_estimate;
    capacity_input["outputReserveTokens"] = output_reserve_tokens;
    capacity_input["contextWindowTokens"] = context_window_tokens;
    DispatchTrigger capacity_trigger = MakeTrigger(context, std::move(capacity_input), Stage::Capacity);
    stages.capacity_outcome =
        middleware->Dispatch(HookPoint::PreRequest, capacity_trigger,
                             [](const nlohmann::json& input) { return input; }, sink);
    if (stages.capacity_outcome.kind != DispatchOutcome::Kind::Completed) {
        stages.decision = "reject";
        stages.reason = stages.capacity_outcome.kind == DispatchOutcome::Kind::Denied
                            ? stages.capacity_outcome.deny_message
                            : "容量段失败: " + stages.capacity_outcome.error_detail;
        return stages;
    }
    // 决定从已采用的 admission.decision 效果里读(计划序最后一枚)。
    for (const auto& record : stages.capacity_outcome.records) {
        for (const auto& effect : record.effects) {
            if (effect.applied && effect.type == "admission.decision" && effect.payload.contains("decision") &&
                effect.payload["decision"].is_string()) {
                stages.decision = effect.payload["decision"].get<std::string>();
                if (effect.payload.contains("reason") && effect.payload["reason"].is_string()) {
                    stages.reason = effect.payload["reason"].get<std::string>();
                }
            }
        }
    }
    if (stages.decision.empty()) {
        // 容量段跑了但没给决定:视为不放心,拒绝发送(fail closed)。
        stages.decision = "reject";
        stages.reason = "容量段未返回准入决定";
    }
    return stages;
}

// ---------------------------------------------------------------------------
// 快照投影与判据
// ---------------------------------------------------------------------------

nlohmann::json BuildRequestSnapshotJson(const api::Request& request) {
    nlohmann::json snapshot = nlohmann::json::object();
    snapshot["model"] = request.model;
    snapshot["system"] = request.system;
    nlohmann::json messages = nlohmann::json::array();
    for (const auto& message : request.messages) {
        nlohmann::json out = nlohmann::json::object();
        out["role"] = message.role == api::Role::Assistant ? "assistant" : "user";
        nlohmann::json blocks = nlohmann::json::array();
        for (const auto& block : message.content) {
            blocks.push_back(ContentBlockToJson(block));
        }
        out["content"] = std::move(blocks);
        messages.push_back(std::move(out));
    }
    snapshot["messages"] = std::move(messages);
    nlohmann::json tools = nlohmann::json::array();
    for (const auto& tool : request.tools) {
        tools.push_back(nlohmann::json{{"name", tool.name},
                                       {"description", tool.description},
                                       {"input_schema", tool.input_schema}});
    }
    snapshot["tools"] = std::move(tools);
    if (request.max_tokens.has_value()) {
        snapshot["max_tokens"] = *request.max_tokens;
    }
    return snapshot;
}

bool HasPreRequestMiddleware(const hooks::HookDispatcher* dispatcher) {
    return HasSelectedFor(dispatcher, HookPoint::PreRequest);
}

bool HasUserMiddleware(const hooks::HookDispatcher* dispatcher) {
    return HasSelectedFor(dispatcher, HookPoint::PreUser) || HasSelectedFor(dispatcher, HookPoint::PostUser);
}

// ---------------------------------------------------------------------------
// UI 投影
// ---------------------------------------------------------------------------

nlohmann::json DescribeDispatchForUi(const hooks::middleware::DispatchOutcome& outcome) {
    nlohmann::json out = nlohmann::json::object();
    out["dispatchId"] = outcome.dispatch_id;
    out["registryRevision"] = outcome.registry_revision;
    const char* kind = "completed";
    if (outcome.kind == DispatchOutcome::Kind::Denied) {
        kind = "denied";
    } else if (outcome.kind == DispatchOutcome::Kind::Failed) {
        kind = "failed";
    }
    out["outcome"] = kind;
    if (!outcome.deny_code.empty()) {
        out["denyCode"] = outcome.deny_code;
    }
    if (!outcome.error_code.empty()) {
        out["errorCode"] = outcome.error_code;
    }
    nlohmann::json invocations = nlohmann::json::array();
    for (const auto& record : outcome.records) {
        nlohmann::json item = nlohmann::json::object();
        item["hook"] = record.key;
        item["source"] = record.source_label;
        item["kind"] = record.handler_kind;
        item["outcome"] = record.outcome;
        if (record.next_consumed) {
            item["nextConsumed"] = true;
        }
        if (record.next_calls > 0) {
            item["nextCalls"] = record.next_calls;
        }
        if (!record.error_code.empty()) {
            item["errorCode"] = record.error_code;
        }
        if (record.duration_ms > 0) {
            item["durationMs"] = record.duration_ms;
        }
        nlohmann::json effects = nlohmann::json::array();
        for (const auto& effect : record.effects) {
            effects.push_back(nlohmann::json{{"type", effect.type},
                                             {"applied", effect.applied},
                                             {"reason", effect.reject_reason}});
        }
        if (!effects.empty()) {
            item["effects"] = std::move(effects);
        }
        invocations.push_back(std::move(item));
    }
    out["invocations"] = std::move(invocations);
    return out;
}

}  // namespace lubancode::runtime
