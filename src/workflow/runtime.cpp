// WorkflowRuntime 实现(自然语言编排单第 2 批)。
//
// 首版引擎:按 outcome 边走的解释器。ready 队列就是"当前节点",顺序图
// 天然单线程;parallel(第 3 批)在同一只调度循环里开线程池。取消经
// cancel_token 逐节点检查;预算(步数/tool_calls/tokens/时限)每节点
// 终态后对账,越帽收成 budget_exhausted,不悄悄续跑。

#include "workflow/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

#include "platform/paths.hpp"
#include "platform/wall_clock.hpp"
#include "runtime/budget_gate.hpp"
#include "runtime/id_authority.hpp"
#include "runtime/retry_backoff.hpp"
#include "workflow/validator.hpp"

namespace lubancode::workflow {

namespace {

std::string DefaultRunId() {
    // run id 的形状不动(时间戳前缀是 ListRuns 倒序的排序键);钟批五起
    // 读 platform 统一墙钟(口径不变,只收源)。
    const std::int64_t ms = platform::WallClockNowMs();
    const std::int64_t secs = ms / 1000;
    const int millis = static_cast<int>(ms % 1000);
    std::tm tm_buf{};
    const std::time_t tt = platform::WallClockToTimeT(ms);
#if defined(_WIN32)
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_buf);
    std::ostringstream id;
    id << "run-" << stamp;
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "%03d", millis);
    id << "-" << suffix;
    std::random_device rd;
    id << "-" << (rd() & 0xffff);
    return id.str();
}

// 输入对账(ValidateInputs):required 缺字段、类型不合在开跑前报,
// 不跑到一半才炸(单子"运行时"测试清单)。
std::optional<std::string> ValidateInputsAgainstSchema(const nlohmann::json& values,
                                                       const nlohmann::json& schema) {
    if (!schema.is_object() || schema.empty()) return std::nullopt;
    if (const auto required = schema.find("required"); required != schema.end() && required->is_array()) {
        for (const auto& field : *required) {
            if (!field.is_string()) continue;
            if (!values.contains(field.get<std::string>()) || values[field.get<std::string>()].is_null()) {
                return "inputs." + field.get<std::string>() + " 缺必填字段";
            }
        }
    }
    if (const auto props = schema.find("properties"); props != schema.end() && props->is_object()) {
        for (auto it = props->begin(); it != props->end(); ++it) {
            const auto value = values.find(it.key());
            if (value == values.end() || value->is_null()) continue;
            const std::string* type = nullptr;
            if (const auto t = it->find("type"); t != it->end() && t->is_string()) {
                type = &t->get_ref<const std::string&>();
            }
            if (type == nullptr) continue;
            bool ok = true;
            if (*type == "string") ok = value->is_string();
            else if (*type == "integer") ok = value->is_number_integer();
            else if (*type == "number") ok = value->is_number();
            else if (*type == "boolean") ok = value->is_boolean();
            else if (*type == "array") ok = value->is_array();
            else if (*type == "object") ok = value->is_object();
            if (!ok) {
                return "inputs." + it.key() + " 期望 " + *type + ",给的是 " + value->type_name();
            }
        }
    }
    return std::nullopt;
}

nlohmann::json ApplyInputDefaults(const nlohmann::json& values, const nlohmann::json& schema) {
    nlohmann::json result = values.is_object() ? values : nlohmann::json::object();
    if (!schema.is_object()) return result;
    const auto properties = schema.find("properties");
    if (properties == schema.end() || !properties->is_object()) return result;
    for (auto it = properties->begin(); it != properties->end(); ++it) {
        if (result.contains(it.key()) || !it->is_object()) continue;
        const auto fallback = it->find("default");
        if (fallback != it->end()) result[it.key()] = *fallback;
    }
    return result;
}

}  // namespace

// ---- 状态机 -----------------------------------------------------------------

std::string ToString(RunState state) {
    switch (state) {
        case RunState::Created: return "created";
        case RunState::Validating: return "validating";
        case RunState::Ready: return "ready";
        case RunState::Running: return "running";
        case RunState::WaitingInput: return "waiting_input";
        case RunState::WaitingApproval: return "waiting_approval";
        case RunState::WaitingIo: return "waiting_io";
        case RunState::Paused: return "paused";
        case RunState::Succeeded: return "succeeded";
        case RunState::Failed: return "failed";
        case RunState::Cancelled: return "cancelled";
        case RunState::BudgetExhausted: return "budget_exhausted";
    }
    return "created";
}

bool ParseRunState(const std::string& s, RunState& out) {
    if (s == "created") { out = RunState::Created; return true; }
    if (s == "validating") { out = RunState::Validating; return true; }
    if (s == "ready") { out = RunState::Ready; return true; }
    if (s == "running") { out = RunState::Running; return true; }
    if (s == "waiting_input") { out = RunState::WaitingInput; return true; }
    if (s == "waiting_approval") { out = RunState::WaitingApproval; return true; }
    if (s == "waiting_io") { out = RunState::WaitingIo; return true; }
    if (s == "paused") { out = RunState::Paused; return true; }
    if (s == "succeeded") { out = RunState::Succeeded; return true; }
    if (s == "failed") { out = RunState::Failed; return true; }
    if (s == "cancelled") { out = RunState::Cancelled; return true; }
    if (s == "budget_exhausted") { out = RunState::BudgetExhausted; return true; }
    return false;
}

bool IsTerminalRunState(RunState state) {
    return state == RunState::Succeeded || state == RunState::Failed || state == RunState::Cancelled ||
           state == RunState::BudgetExhausted;
}

std::string ToString(NodeState state) {
    switch (state) {
        case NodeState::Pending: return "pending";
        case NodeState::Ready: return "ready";
        case NodeState::Running: return "running";
        case NodeState::RetryWait: return "retry_wait";
        case NodeState::WaitingInput: return "waiting_input";
        case NodeState::WaitingApproval: return "waiting_approval";
        case NodeState::WaitingIo: return "waiting_io";
        case NodeState::Succeeded: return "succeeded";
        case NodeState::Skipped: return "skipped";
        case NodeState::Failed: return "failed";
        case NodeState::Cancelled: return "cancelled";
        case NodeState::Interrupted: return "interrupted";
    }
    return "pending";
}

bool ParseNodeState(const std::string& s, NodeState& out) {
    if (s == "pending") { out = NodeState::Pending; return true; }
    if (s == "ready") { out = NodeState::Ready; return true; }
    if (s == "running") { out = NodeState::Running; return true; }
    if (s == "retry_wait") { out = NodeState::RetryWait; return true; }
    if (s == "waiting_input") { out = NodeState::WaitingInput; return true; }
    if (s == "waiting_approval") { out = NodeState::WaitingApproval; return true; }
    if (s == "waiting_io") { out = NodeState::WaitingIo; return true; }
    if (s == "succeeded") { out = NodeState::Succeeded; return true; }
    if (s == "skipped") { out = NodeState::Skipped; return true; }
    if (s == "failed") { out = NodeState::Failed; return true; }
    if (s == "cancelled") { out = NodeState::Cancelled; return true; }
    if (s == "interrupted") { out = NodeState::Interrupted; return true; }
    return false;
}

bool IsTerminalNodeState(NodeState state) {
    return state == NodeState::Succeeded || state == NodeState::Skipped || state == NodeState::Failed ||
           state == NodeState::Cancelled;
}

bool IsValidRunTransition(RunState from, RunState to) {
    if (from == to) return false;
    if (IsTerminalRunState(from)) return false;  // 终态不出
    switch (from) {
        case RunState::Created:
            return to == RunState::Validating || to == RunState::Failed || to == RunState::Cancelled;
        case RunState::Validating:
            return to == RunState::Ready || to == RunState::Failed || to == RunState::Cancelled;
        case RunState::Ready:
            return to == RunState::Running || to == RunState::Failed || to == RunState::Cancelled;
        case RunState::Running:
            return to == RunState::WaitingInput || to == RunState::WaitingApproval || to == RunState::WaitingIo ||
                   to == RunState::Paused ||
                   to == RunState::Succeeded || to == RunState::Failed || to == RunState::Cancelled ||
                   to == RunState::BudgetExhausted;
        case RunState::WaitingInput:
        case RunState::WaitingApproval:
        case RunState::WaitingIo:
        case RunState::Paused:
            return to == RunState::Running || to == RunState::Succeeded || to == RunState::Failed ||
                   to == RunState::Cancelled || to == RunState::BudgetExhausted;
        default:
            return false;
    }
}

bool IsValidNodeTransition(NodeState from, NodeState to) {
    if (from == to) return false;
    if (IsTerminalNodeState(from)) return false;
    switch (from) {
        case NodeState::Pending:
            return to == NodeState::Ready || to == NodeState::Skipped || to == NodeState::Cancelled;
        case NodeState::Ready:
            return to == NodeState::Running || to == NodeState::Skipped || to == NodeState::Cancelled;
        case NodeState::Running:
            return to == NodeState::RetryWait || to == NodeState::WaitingInput ||
                   to == NodeState::WaitingApproval || to == NodeState::WaitingIo ||
                   to == NodeState::Succeeded || to == NodeState::Failed ||
                   to == NodeState::Cancelled;
        case NodeState::RetryWait:
            return to == NodeState::Ready || to == NodeState::Failed || to == NodeState::Cancelled;
        case NodeState::WaitingInput:
        case NodeState::WaitingApproval:
        case NodeState::WaitingIo:
            return to == NodeState::Running || to == NodeState::Succeeded || to == NodeState::Failed ||
                   to == NodeState::Cancelled;
        case NodeState::Interrupted:
            return to == NodeState::Ready || to == NodeState::Cancelled;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// WorkflowRuntime
// ---------------------------------------------------------------------------

WorkflowRuntime::WorkflowRuntime(RuntimeOptions options) : options_(std::move(options)) {}

bool WorkflowRuntime::WithinBudget(const WorkflowLimits& limits, const WorkflowRunSummary& account) const {
    // 预算对账(批五):tool_calls/tokens 两尺声明进公共预算闸,Overrun
    // 口径(已越帽才拦,严格 >)——与旧逐尺短路同序同文。
    runtime::BudgetGate gate(runtime::BudgetScales{
        .count = static_cast<std::int64_t>(limits.tool_calls),
        .tokens = limits.tokens,
    });
    return !gate.OverrunCount(account.tool_calls) && !gate.OverrunTokens(account.tokens_used);
}

std::string WorkflowRuntime::NextNodeFor(const WorkflowDefinition& def, const std::string& node_id,
                                         const std::string& outcome) const {
    for (const auto& edge : def.edges) {
        if (edge.from == node_id && edge.outcome == outcome) return edge.to;
    }
    return std::string();
}

void WorkflowRuntime::EmitRunEvent(const WorkflowRunSummary& account, const char* payload_type,
                                   nlohmann::json payload) {
    if (options_.event_sink == nullptr) return;
    runtime::ServerEvent event;
    event.envelope.thread_id = options_.thread_id.empty() ? "workflow" : options_.thread_id;
    // 信封 seq(批五):发号局只此一家,本文件那只 static 计数器收编。
    // 默认进程级(单调跨 runtime 实例);装配层可注入专属局。
    runtime::IdAuthority& ids =
        options_.id_authority != nullptr ? *options_.id_authority : runtime::ProcessIdAuthority();
    event.envelope.seq = ids.NextSeq();
    event.envelope.timestamp_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
    event.kind = runtime::ServerEventKind::ItemDelta;
    event.item_kind = runtime::ItemKind::Command;
    event.item_id = account.run_id;
    payload["type"] = payload_type;
    payload["workflow_id"] = account.workflow_id;
    payload["run_id"] = account.run_id;
    event.payload = std::move(payload);
    options_.event_sink->Emit(event);
}

std::expected<nlohmann::json, ResolveError> WorkflowRuntime::BuildResult(const WorkflowDefinition& def,
                                                                          const Store& store) const {
    nlohmann::json out = nlohmann::json::object();
    if (!def.result.is_object()) return out;
    for (auto it = def.result.begin(); it != def.result.end(); ++it) {
        auto resolved = ResolveTemplate(store, it.value());
        if (!resolved.has_value()) {
            ResolveError err = resolved.error();
            err.path = "result." + it.key() + " " + err.path;
            return std::unexpected(err);
        }
        out[it.key()] = std::move(resolved->value);
    }
    return out;
}

std::string WorkflowRuntime::RunNode(const ExecutionContext& ctx, const WorkflowNode& node,
                                     nlohmann::json* committed_output) {
    WorkflowRunSummary& account = *ctx.account;
    // 并行分支共写 account.nodes:持锁落记录。单线程路径(顺序图)锁空着,
    // 裸引用照旧——顺序图不为并行付锁钱。
    std::unique_lock<std::mutex> nodes_lock;
    if (ctx.nodes_mutex != nullptr) nodes_lock = std::unique_lock<std::mutex>(*ctx.nodes_mutex);
    NodeRunRecord& record = account.nodes[node.id];
    const auto emit_node_event = [&](const char* type, nlohmann::json payload) {
        const int event_attempt = payload.value("attempt", (std::max)(1, record.attempt));
        payload["node_id"] = node.id;
        payload["label"] = node.label;
        payload["kind"] = ToString(node.kind);
        payload["attempt"] = event_attempt;
        payload["node_run_id"] =
            account.run_id + "-" + node.id + "-a" + std::to_string(event_attempt);
        EmitRunEvent(account, type, std::move(payload));
    };

    const auto executor_it = options_.executors.find(node.kind);
    if (executor_it == options_.executors.end()) {
        record.state = NodeState::Failed;
        record.error_code = "no_executor";
        record.error_message = "节点种类 " + ToString(node.kind) + " 没配执行器";
        emit_node_event(kEventNodeCompleted,
                        nlohmann::json{{"outcome", "error"}, {"code", record.error_code}});
        return "error";
    }
    NodeExecutor& executor = *executor_it->second;

    record.state = NodeState::Ready;
    const int max_attempts = node.retry.has_value() ? std::max(1, node.retry->attempts) : 1;
    NodeExecResult result;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        record.attempt = attempt;
        // 取消检查:每个 attempt 之前看一眼。
        if (ctx.cancel != nullptr && ctx.cancel->load()) {
            record.state = NodeState::Cancelled;
            emit_node_event(kEventNodeCompleted, nlohmann::json{{"outcome", "cancelled"}});
            return "cancelled";
        }
        record.state = NodeState::Running;
        record.started_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
        if (ctx.journal != nullptr) {
            ctx.journal->Append(kEventNodeStarted, node.id, attempt,
                                nlohmann::json{{"kind", ToString(node.kind)}});
        }
        // Execute 不持账面锁:真活儿(工具/网络/模型)并发跑,只有账本串行。
        if (nodes_lock.owns_lock()) nodes_lock.unlock();

        NodeExecRequest request;
        request.definition = ctx.definition;
        request.node = &node;
        request.run_id = account.run_id;
        request.node_run_id = account.run_id + "-" + node.id + "-a" + std::to_string(attempt);
        request.attempt = attempt;
        request.store = ctx.store;
        request.cancel = ctx.cancel;
        auto resolved_input = ResolveTemplate(*ctx.store, node.input);
        if (!resolved_input.has_value()) {
            record.state = NodeState::Failed;
            record.error_code = "resolve_input";
            record.error_message = resolved_input.error().path + ": " + resolved_input.error().message;
            if (ctx.journal != nullptr) {
                ctx.journal->Append(kEventNodeCompleted, node.id, attempt,
                                    nlohmann::json{{"outcome", "error"},
                                                   {"code", record.error_code},
                                                   {"error", record.error_message}});
            }
            emit_node_event(kEventNodeCompleted,
                            nlohmann::json{{"outcome", "error"}, {"code", record.error_code}});
            return "error";
        }
        request.resolved_input = resolved_input->value;

        // 到这里，输入已解好、执行器也找着了；此刻才算真正交办。终端据
        // 这条回执告诉用户“中书省已经接到”，不拿预备状态冒充已发送。
        emit_node_event(kEventNodeStarted,
                        nlohmann::json{{"attempt", attempt}, {"max_attempts", max_attempts}});
        result = executor.Execute(request);
        if (nodes_lock.owns_lock() == false && ctx.nodes_mutex != nullptr) {
            nodes_lock.lock();
        }
        account.tokens_used += result.tokens_used;
        if (!result.ok) {
            account.tool_calls += 1;
        }
        record.ended_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
        record.tokens_used += result.tokens_used;

        if (result.ok) break;
        // 可重试判定:稳定 code 白名单;空白名单认默认可重试档(超时/
        // 限流/瞬时网络一类执行器报上来的)。
        bool retryable = false;
        if (node.retry.has_value() && attempt < max_attempts) {
            if (node.retry->when.empty()) {
                retryable = result.error_code == "timeout" || result.error_code == "rate_limited" ||
                            result.error_code == "transient_network" || result.error_code == "transient";
            } else {
                retryable = std::find(node.retry->when.begin(), node.retry->when.end(), result.error_code) !=
                            node.retry->when.end();
            }
        }
        if (retryable) {
            record.state = NodeState::RetryWait;
            if (ctx.journal != nullptr) {
                ctx.journal->Append(kEventNodeRetrying, node.id, attempt,
                                    nlohmann::json{{"code", result.error_code},
                                                   {"attempt", attempt},
                                                   {"max_attempts", max_attempts}});
            }
            emit_node_event(kEventNodeRetrying,
                            nlohmann::json{{"attempt", attempt},
                                           {"max_attempts", max_attempts},
                                           {"code", result.error_code}});
            // backoff 等待:受取消打断。fake clock 下 wait 缩为 0(单测不
            // 靠 sleep 赌时序)。批五:阶梯与等待走公共退避件;定义里的
            // jitter 仍不启用(声明了未接线是现状,开了改节奏,另立一批)。
            if (node.retry.has_value()) {
                runtime::BackoffPolicy policy;
                policy.kind = node.retry->backoff == BackoffKind::Exponential
                                  ? runtime::BackoffPolicy::Kind::Exponential
                                  : runtime::BackoffPolicy::Kind::Fixed;
                policy.initial_ms = node.retry->initial_ms;
                policy.max_ms = node.retry->max_ms;
                policy.jitter = false;
                const auto wait = runtime::BackoffWaitMs(policy, static_cast<std::uint32_t>(attempt));
                if (wait.has_value()) {
                    // 取消打断只截断等待;收口由下一轮 attempt 顶上的取消
                    // 检查做(旧形状如此,不在这里提前 return)。
                    (void)runtime::WaitBackoffCancellable(*wait, ctx.cancel);
                }
            }
            record.state = NodeState::Ready;
            continue;
        }
        record.state = NodeState::Failed;
        record.error_code = result.error_code;
        record.error_message = result.error_message;
        if (ctx.journal != nullptr) {
            ctx.journal->Append(kEventNodeCompleted, node.id, attempt,
                                nlohmann::json{{"outcome", "error"},
                                               {"code", result.error_code},
                                               {"error", result.error_message}});
        }
        emit_node_event(kEventNodeCompleted,
                        nlohmann::json{{"outcome", "error"},
                                       {"code", result.error_code},
                                       {"duration_ms", result.duration_ms},
                                       {"tokens", result.tokens_used}});
        return "error";
    }

    // CommitOutput:原子落账。循环迭代里同节点重入走覆写(meta 记轮次),
    // 首写与覆写都算落账成功——"半份 output"仍不可能:Execute 只产候选,
    // 这里一次性换入。
    if (result.ok) {
        ctx.store->CommitOutputOverwrite(node.id, result.output);
        // 本项产物当场交还调用方(map 并发用):store 的 body 键在并发下是
        // 共用垫,回头 GetOutput 取到的是"最后一只 commit 的",不是自己那份。
        if (committed_output != nullptr) *committed_output = result.output;
        record.state = NodeState::Succeeded;
        if (ctx.journal != nullptr) {
            ctx.journal->Append(kEventNodeCompleted, node.id, record.attempt,
                                nlohmann::json{{"outcome", result.empty ? "empty" : "success"},
                                               {"output", result.output},
                                               {"tokens", result.tokens_used},
                                               {"duration_ms", result.duration_ms}});
        }
        emit_node_event(kEventNodeCompleted,
                        nlohmann::json{{"outcome", result.empty ? "empty" : "success"},
                                       {"duration_ms", result.duration_ms},
                                       {"tokens", result.tokens_used}});
        return result.empty ? "empty" : "success";
    }
    record.state = NodeState::Failed;
    emit_node_event(kEventNodeCompleted, nlohmann::json{{"outcome", "error"}});
    return "error";
}

std::string WorkflowRuntime::RunAsync(const ExecutionContext& ctx, const WorkflowNode& node) {
    WorkflowRunSummary& account = *ctx.account;
    const auto body_it = ctx.definition->node_map.find(node.async_body);
    if (body_it == ctx.definition->node_map.end()) return "error";  // validator 已给细账

    {
        std::scoped_lock lock(*ctx.nodes_mutex);
        NodeRunRecord& record = account.nodes[node.id];
        record.node_id = node.id;
        record.attempt = 1;
        record.state = NodeState::WaitingIo;
        record.started_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
    }
    account.state = RunState::WaitingIo;
    if (ctx.journal != nullptr) {
        ctx.journal->Append(kEventNodeStarted, node.id, 1,
                            nlohmann::json{{"kind", "async"}});
        ctx.journal->Append(kEventNodeWaiting, node.id, 1,
                            nlohmann::json{{"kind", "io"}, {"body", node.async_body}});
    }

    if (++*ctx.steps > ctx.definition->limits.max_steps) {
        account.state = RunState::BudgetExhausted;
        std::scoped_lock lock(*ctx.nodes_mutex);
        NodeRunRecord& record = account.nodes[node.id];
        record.state = NodeState::Failed;
        record.error_code = "max_steps";
        record.error_message = "async body 越过 max_steps";
        return "budget_exhausted";
    }

    std::atomic<bool> body_cancel{false};
    ExecutionContext body_ctx = ctx;
    body_ctx.cancel = &body_cancel;
    const WorkflowNode& body = body_it->second;
    auto future = std::async(std::launch::async, [this, body_ctx, &body]() {
        nlohmann::json output;
        const std::string outcome = RunNode(body_ctx, body, &output);
        return std::pair<std::string, nlohmann::json>{outcome, std::move(output)};
    });

    bool cancelled = false;
    bool timed_out = false;
    while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
        if (ctx.cancel != nullptr && ctx.cancel->load()) {
            cancelled = true;
            body_cancel.store(true);
        }
        const std::int64_t now_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
        if (ctx.definition->limits.timeout_secs > 0 &&
            now_ms - ctx.started_ms > ctx.definition->limits.timeout_secs * 1000) {
            timed_out = true;
            body_cancel.store(true);
        }
    }
    auto [outcome, output] = future.get();
    account.state = RunState::Running;

    std::scoped_lock lock(*ctx.nodes_mutex);
    NodeRunRecord& record = account.nodes[node.id];
    record.ended_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
    if (cancelled) {
        record.state = NodeState::Cancelled;
        record.error_code = "cancelled";
        record.error_message = "async 等待被取消";
        return "cancelled";
    }
    if (timed_out) {
        record.state = NodeState::Failed;
        record.error_code = "timeout";
        record.error_message = "async 等待撞到 workflow 总时限";
        account.state = RunState::BudgetExhausted;
        account.error_code = "timeout";
        account.error_message = record.error_message;
        return "budget_exhausted";
    }
    if (outcome == "success" || outcome == "empty") {
        ctx.store->CommitOutputOverwrite(node.id, output);
        record.state = NodeState::Succeeded;
        if (ctx.journal != nullptr) {
            ctx.journal->Append(kEventNodeCompleted, node.id, 1,
                                nlohmann::json{{"outcome", outcome}, {"output", output}});
        }
        return outcome;
    }
    if (outcome == "cancelled") {
        record.state = NodeState::Cancelled;
        record.error_code = "cancelled";
        return "cancelled";
    }
    const NodeRunRecord& body_record = account.nodes.at(node.async_body);
    record.state = outcome == "skipped" ? NodeState::Skipped : NodeState::Failed;
    record.error_code = body_record.error_code;
    record.error_message = body_record.error_message;
    return outcome;
}

// ---------------------------------------------------------------------------
// 并行、map、reduce、switch(第 3 批)
// ---------------------------------------------------------------------------

namespace {

// ConditionOp 评估(纯函数,switch 用):只读结构化值,禁 eval。
bool EvaluateCondition(lubancode::workflow::ConditionOp op, const nlohmann::json* value,
                       const nlohmann::json& literal) {
    using CO = lubancode::workflow::ConditionOp;
    switch (op) {
        case CO::Exists:
            return value != nullptr && !value->is_null();
        case CO::NotExists:
            return value == nullptr || value->is_null();
        case CO::Equals:
            return value != nullptr && *value == literal;
        case CO::NotEquals:
            return value == nullptr || !(*value == literal);
        case CO::GreaterThan:
            return value != nullptr && value->is_number() && literal.is_number() &&
                   value->get<double>() > literal.get<double>();
        case CO::LessThan:
            return value != nullptr && value->is_number() && literal.is_number() &&
                   value->get<double>() < literal.get<double>();
        case CO::Contains:
            if (value == nullptr) return false;
            if (value->is_array()) {
                for (const auto& item : *value) {
                    if (item == literal) return true;
                }
                return false;
            }
            if (value->is_string() && literal.is_string()) {
                return value->get<std::string>().find(literal.get<std::string>()) != std::string::npos;
            }
            return false;
        case CO::StartsWith:
            return value != nullptr && value->is_string() && literal.is_string() &&
                   value->get<std::string>().rfind(literal.get<std::string>(), 0) == 0;
        case CO::NonEmpty:
            if (value == nullptr) return false;
            if (value->is_array()) return !value->empty();
            if (value->is_string()) return !value->get<std::string>().empty();
            return false;
    }
    return false;
}

// 从 ${...} 字符串剥出内层路径(非引用原样返回,当字面量路径走 store 下钻)。
std::string StripRefBraces(const std::string& path) {
    if (path.size() > 4 && path.front() == '$' && path[1] == '{' && path.back() == '}') {
        return path.substr(2, path.size() - 3);
    }
    return path;
}

}  // namespace

std::string WorkflowRuntime::EvaluateSwitch(const WorkflowDefinition& def, const WorkflowNode& node,
                                            const Store& store) const {
    for (const auto& c : node.conditions) {
        const std::string ref = StripRefBraces(c.path);
        // switch 条件只读 store(inputs/nodes/run);不中就落下一条。
        std::optional<nlohmann::json> resolved;
        if (auto r = ResolveRef(store, ref); r.has_value()) {
            resolved = std::move(*r);
        }
        const nlohmann::json* value = resolved.has_value() ? &*resolved : nullptr;
        if (EvaluateCondition(c.op, value, c.literal)) {
            return c.to;
        }
    }
    return node.default_to;  // 可空:都不中且无 default -> 空(主循环收 skipped)
}

std::string WorkflowRuntime::RunLoop(const ExecutionContext& ctx, const WorkflowNode& node) {
    WorkflowRunSummary& account = *ctx.account;
    const WorkflowDefinition& def = *ctx.definition;
    NodeRunRecord& record = account.nodes[node.id];
    record.node_id = node.id;
    record.state = NodeState::Running;
    record.started_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();

    const auto fail = [&](const std::string& code, const std::string& message) {
        record.state = NodeState::Failed;
        record.error_code = code;
        record.error_message = message;
        return std::string("error");
    };
    const auto resolve_bound = [&](const nlohmann::json& spec, const char* field)
        -> std::expected<int, std::string> {
        auto resolved = ResolveTemplate(*ctx.store, spec);
        if (!resolved.has_value()) {
            return std::unexpected(std::string(field) + " 解析失败: " + resolved.error().message);
        }
        if (!resolved->value.is_number_integer()) {
            return std::unexpected(std::string(field) + " 必须解析成整数");
        }
        const int value = resolved->value.get<int>();
        if (value <= 0) return std::unexpected(std::string(field) + " 必须大于 0");
        return value;
    };

    const auto min_iterations = resolve_bound(node.loop_min_iterations, "min_iterations");
    if (!min_iterations.has_value()) return fail("bad_loop_min", min_iterations.error());
    const auto max_iterations = resolve_bound(node.loop_max_iterations, "max_iterations");
    if (!max_iterations.has_value()) return fail("bad_loop_max", max_iterations.error());
    if (*min_iterations > *max_iterations) {
        return fail("loop_min_exceeds_max", "min_iterations 不能大于 max_iterations");
    }
    if (*max_iterations > node.loop_hard_limit) {
        return fail("loop_limit_exceeds_hard_limit",
                    "max_iterations 越过 hard_limit(" + std::to_string(node.loop_hard_limit) + ")");
    }

    int completed = 0;
    nlohmann::json history = nlohmann::json::array();
    nlohmann::json previous = nullptr;
    if (const auto saved = ctx.store->GetOutput(node.id); saved.has_value() && saved->is_object()) {
        completed = saved->value("completed_iterations", 0);
        if (const auto it = saved->find("history"); it != saved->end() && it->is_array()) history = *it;
        if (const auto it = saved->find("last"); it != saved->end()) previous = *it;
        if (saved->value("condition_met", false) && completed >= *min_iterations) {
            record.state = NodeState::Succeeded;
            return "success";
        }
        if (saved->value("exhausted", false) || completed >= *max_iterations) {
            record.state = NodeState::Succeeded;
            return "exhausted";
        }
    }

    while (completed < *max_iterations) {
        if (ctx.cancel != nullptr && ctx.cancel->load()) {
            record.state = NodeState::Cancelled;
            return "cancelled";
        }
        const int iteration = completed + 1;
        nlohmann::json loop_context{{"iteration", iteration},
                                    {"completed_iterations", completed},
                                    {"previous", previous},
                                    {"history", history},
                                    {"condition_met", false},
                                    {"exhausted", false}};
        ctx.store->CommitOutputOverwrite(node.id, loop_context);
        ctx.store->UpdateMeta(node.id, nlohmann::json{{"iteration", iteration}, {"complete", false}});
        if (ctx.journal != nullptr) {
            ctx.journal->Append(kEventLoopIterationStarted, node.id, iteration,
                                nlohmann::json{{"iteration", iteration}});
        }

        nlohmann::json outputs = nlohmann::json::object();
        for (const auto& body_id : node.loop_body) {
            if (ctx.cancel != nullptr && ctx.cancel->load()) {
                record.state = NodeState::Cancelled;
                return "cancelled";
            }
            if (ctx.steps != nullptr && ++*ctx.steps > def.limits.max_steps) {
                account.state = RunState::BudgetExhausted;
                account.error_code = "max_steps";
                account.error_message =
                    "步数越过 max_steps(" + std::to_string(def.limits.max_steps) + ")";
                record.state = NodeState::Failed;
                record.error_code = "max_steps";
                record.error_message = account.error_message;
                return "budget_exhausted";
            }
            if (!WithinBudget(def.limits, account)) {
                account.state = RunState::BudgetExhausted;
                account.error_code = "budget_exhausted";
                account.error_message = "预算越帽(tool_calls/tokens)";
                record.state = NodeState::Failed;
                return "budget_exhausted";
            }
            const auto body = def.node_map.find(body_id);
            if (body == def.node_map.end()) return fail("unknown_loop_body", "loop body 节点不存在: " + body_id);
            ctx.store->UpdateMeta(body_id, nlohmann::json{{"iteration", iteration}});
            nlohmann::json output;
            const std::string outcome = RunNode(ctx, body->second, &output);
            if (outcome == "cancelled") {
                record.state = NodeState::Cancelled;
                return "cancelled";
            }
            if (outcome == "error" || outcome == "skipped") {
                return fail("loop_body_failed", body_id + " 返回 " + outcome);
            }
            outputs[body_id] = std::move(output);
        }

        std::optional<nlohmann::json> resolved;
        if (auto value = ResolveRef(*ctx.store, StripRefBraces(node.loop_until->path)); value.has_value()) {
            resolved = std::move(*value);
        }
        const bool condition_met =
            EvaluateCondition(node.loop_until->op, resolved.has_value() ? &*resolved : nullptr,
                              node.loop_until->literal);
        completed = iteration;
        nlohmann::json last{{"iteration", iteration}, {"outputs", std::move(outputs)},
                            {"condition_met", condition_met}};
        history.push_back(last);
        previous = last;
        const bool exhausted = completed >= *max_iterations && !(condition_met && completed >= *min_iterations);
        nlohmann::json output{{"iteration", iteration},
                              {"completed_iterations", completed},
                              {"previous", history.size() > 1 ? history[history.size() - 2] : nlohmann::json(nullptr)},
                              {"last", last},
                              {"history", history},
                              {"condition_met", condition_met},
                              {"exhausted", exhausted}};
        ctx.store->CommitOutputOverwrite(node.id, output);
        ctx.store->UpdateMeta(node.id, nlohmann::json{{"iteration", iteration}, {"complete", true}});
        if (ctx.journal != nullptr) {
            ctx.journal->Append(kEventLoopIterationCompleted, node.id, iteration,
                                nlohmann::json{{"iteration", iteration},
                                               {"condition_met", condition_met},
                                               {"exhausted", exhausted},
                                               {"output", output}});
            ctx.journal->SaveCheckpoint(ctx.journal->last_seq(), ctx.store->ToJson());
        }
        if (condition_met && completed >= *min_iterations) {
            record.state = NodeState::Succeeded;
            record.ended_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
            if (ctx.journal != nullptr) {
                ctx.journal->Append(kEventNodeCompleted, node.id, completed,
                                    nlohmann::json{{"outcome", "success"}, {"output", output}});
            }
            return "success";
        }
    }

    record.state = NodeState::Succeeded;
    record.ended_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
    if (ctx.journal != nullptr) {
        ctx.journal->Append(kEventNodeCompleted, node.id, completed,
                            nlohmann::json{{"outcome", "exhausted"},
                                           {"output", ctx.store->GetOutput(node.id).value_or(nlohmann::json::object())}});
    }
    return "exhausted";
}

std::string WorkflowRuntime::RunParallel(const ExecutionContext& ctx, const WorkflowNode& node) {
    WorkflowRunSummary& account = *ctx.account;
    const WorkflowDefinition& def = *ctx.definition;

    const int global_cap = std::max(1, def.limits.max_concurrency);
    const int node_cap = node.max_concurrency > 0 ? node.max_concurrency : global_cap;
    // 全局、节点两层帽取最小(provider/tool 层帽由各执行器自己再收)。
    const int effective_cap = std::min(global_cap, node_cap);

    NodeRunRecord& record = account.nodes[node.id];
    record.state = NodeState::Running;
    if (ctx.journal != nullptr) {
        ctx.journal->Append(kEventBranchStarted, node.id, 0,
                            nlohmann::json{{"branches", node.branches}, {"cap", effective_cap}});
    }

    struct BranchOutcome {
        std::string outcome;
    };
    const std::size_t count = node.branches.size();
    std::vector<BranchOutcome> results(count);
    std::atomic<std::size_t> next_index{0};
    std::atomic<int> succeeded{0};
    std::atomic<int> failed{0};
    std::atomic<bool> cancelled{false};
    std::mutex node_mutex;  // account.nodes 并行写互斥

    const auto worker = [&]() {
        while (true) {
            if (ctx.cancel != nullptr && ctx.cancel->load()) {
                cancelled.store(true);
                return;
            }
            const std::size_t index = next_index.fetch_add(1);
            if (index >= count) return;
            const std::string& branch_id = node.branches[index];
            const auto branch_it = def.node_map.find(branch_id);
            if (branch_it == def.node_map.end()) {
                std::lock_guard<std::mutex> lock(node_mutex);
                NodeRunRecord& br = account.nodes[branch_id];
                br.state = NodeState::Failed;
                br.error_code = "unknown_node";
                br.error_message = "parallel 分支不存在";
                results[index].outcome = "error";
                failed.fetch_add(1);
                continue;
            }
            // 分支可以是一条链:沿 success 边跑到头。
            std::string cursor = branch_id;
            std::string branch_outcome;
            int guard = 0;
            while (!cursor.empty()) {
                if (++guard > def.limits.max_steps) {
                    branch_outcome = "error";
                    break;
                }
                const auto step_it = def.node_map.find(cursor);
                if (step_it == def.node_map.end()) {
                    branch_outcome = "error";
                    break;
                }
                const WorkflowNode& step = step_it->second;
                // RunNode 内部只碰自己名下的 Store 分区与 account.nodes[自己]
                //(std::map 写入互斥由 nodes_mutex_ 担着),分支间不互踩。
                const std::string outcome = RunNode(ctx, step);
                if (outcome == "error" || outcome == "cancelled") {
                    branch_outcome = outcome;
                    break;
                }
                if (outcome == "skipped") {
                    branch_outcome = "skipped";
                    break;
                }
                cursor = NextNodeFor(def, cursor, outcome);
                if (cursor.empty()) {
                    branch_outcome = "success";
                    break;
                }
            }
            results[index].outcome = branch_outcome.empty() ? "success" : branch_outcome;
            if (branch_outcome == "success") succeeded.fetch_add(1);
            if (branch_outcome == "error") failed.fetch_add(1);
        }
    };

    const std::size_t threads = std::min<std::size_t>(static_cast<std::size_t>(effective_cap), count);
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    // 汇合结果:按定义顺序收,不按谁先回来排(单子 Store 一节)。
    nlohmann::json outputs = nlohmann::json::array();
    nlohmann::json unavailable = nlohmann::json::array();
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& branch_id = node.branches[i];
        nlohmann::json entry = nlohmann::json::object();
        entry["branch"] = branch_id;
        entry["outcome"] = results[i].outcome;
        if (auto output = ctx.store->GetOutput(branch_id)) {
            entry["output"] = *output;
        }
        if (results[i].outcome == "skipped" || results[i].outcome == "error") {
            unavailable.push_back(branch_id);
            if (results[i].outcome == "skipped") {
                account.unavailable_sources.push_back(branch_id);
            }
        }
        outputs.push_back(std::move(entry));
    }
    ctx.store->CommitOutputOverwrite(node.id, nlohmann::json{{"outputs", outputs}, {"unavailable", unavailable}});
    if (ctx.journal != nullptr) {
        ctx.journal->Append(kEventJoinCompleted, node.id, 0,
                            nlohmann::json{{"join", ToString(node.join)},
                                           {"succeeded", succeeded.load()},
                                           {"failed", failed.load()},
                                           {"unavailable", unavailable}});
    }

    // join 政策(单子"并行与汇合规矩"五种)。
    if (cancelled.load()) {
        record.state = NodeState::Cancelled;
        return "cancelled";
    }
    switch (node.join) {
        case JoinPolicy::All:
            if (failed.load() > 0 || static_cast<int>(count) != succeeded.load()) {
                record.state = NodeState::Failed;
                return "error";
            }
            record.state = NodeState::Succeeded;
            return "success";
        case JoinPolicy::AllSettled:
            // 全等完,成功与失败一并交下游(论文多源检索缺一路的用法)。
            record.state = NodeState::Succeeded;
            return "success";
        case JoinPolicy::Any:
            if (succeeded.load() > 0) {
                record.state = NodeState::Succeeded;
                return "success";
            }
            record.state = NodeState::Failed;
            return "error";
        case JoinPolicy::Quorum: {
            const int needed = std::max(1, node.join_quorum);
            if (succeeded.load() >= needed) {
                record.state = NodeState::Succeeded;
                return "success";
            }
            record.state = NodeState::Failed;
            return "error";
        }
        case JoinPolicy::Race:
            // 首个终态便过关,成功失败都算(实现上等全部回来再判;定义时
            // 明示用途窄,单子原文)。
            record.state = succeeded.load() > 0 ? NodeState::Succeeded : NodeState::Failed;
            return succeeded.load() > 0 ? "success" : "error";
    }
    record.state = NodeState::Succeeded;
    return "success";
}

std::string WorkflowRuntime::RunMap(const ExecutionContext& ctx, const WorkflowNode& node) {
    WorkflowRunSummary& account = *ctx.account;
    const WorkflowDefinition& def = *ctx.definition;

    NodeRunRecord& record = account.nodes[node.id];
    record.state = NodeState::Running;

    // items 展开。
    auto items = ResolveRef(*ctx.store, StripRefBraces(node.items_ref));
    if (!items.has_value() || !items->is_array()) {
        record.state = NodeState::Failed;
        record.error_code = "bad_items";
        record.error_message = "map/foreach 的 items 不是数组: " + node.items_ref;
        return "error";
    }
    const std::size_t count = items->size();
    // 展开上限(单子:展开后可能越过 max_nodes/max_steps,validator 已查
    // 静态形状;这里对运行时数据再收一道)。
    if (static_cast<int>(count) > def.limits.max_nodes) {
        record.state = NodeState::Failed;
        record.error_code = "map_too_large";
        record.error_message = "map 展开 " + std::to_string(count) + " 项,越过 max_nodes(" +
                               std::to_string(def.limits.max_nodes) + ")";
        return "error";
    }
    const auto body_it = def.node_map.find(node.map_body);
    if (body_it == def.node_map.end()) {
        record.state = NodeState::Failed;
        record.error_code = "unknown_body";
        record.error_message = "map body 节点不存在: " + node.map_body;
        return "error";
    }

    // 落位规矩(跨平台实锤过的一课):worker 不共写 store 键再读回——
    // RunNode 末尾的 CommitOutputOverwrite(body.id) 在并发下会互相覆写,
    // 随后 GetOutput(body.id) 取到的是"最后一只 commit 的",不是自己那份,
    // mapped 槽位就错。每只 worker 各写各的下标槽(slot.outputs),全 join
    // 完再按 items 顺序拼装;store 里那只 body 键只当最后一项的落账。
    struct MapSlot {
        nlohmann::json output;   // 本项产物(items 顺序落位,不按完成时间)
        bool done = false;       // 本项终态(success/empty)与否
    };
    std::vector<MapSlot> slots(count);  // 预分配,worker 只写自己的下标
    int failures = 0;
    const bool sequential = node.kind == NodeKind::Foreach;
    std::mutex slots_mutex;  // failures 计数与诊断类共写互斥(json 非线程安全)

    const auto run_item = [&](std::size_t index) -> bool {
        if (ctx.cancel != nullptr && ctx.cancel->load()) return false;
        // 逐项把 item 塞进 body 的 input(item 字段),跑一遍 body 链。
        // body 拷贝只在 RunNode 期间有效:产物当场取走落进自己槽,
        // 不回头读 store(body 键在并发下是共用垫,读它会拿别人的)。
        WorkflowNode body = body_it->second;  // 拷一份:input 覆写 item
        nlohmann::json item_input = body.input.is_object() ? body.input : nlohmann::json::object();
        item_input["item"] = (*items)[index];
        item_input["index"] = index;
        body.input = item_input;
        const std::string outcome = RunNode(ctx, body, &slots[index].output);
        if (outcome == "success" || outcome == "empty") {
            slots[index].done = true;
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(slots_mutex);
            failures += 1;
        }
        return false;
    };

    if (sequential || count <= 1) {
        for (std::size_t i = 0; i < count; ++i) {
            if (!run_item(i)) {
                if (ctx.cancel != nullptr && ctx.cancel->load()) {
                    record.state = NodeState::Cancelled;
                    return "cancelled";
                }
                break;  // foreach 顺次:一项败就停(有依赖的活)
            }
        }
    } else {
        // map 并发:线程池 + 全局帽。
        const int cap = std::min(std::max(1, def.limits.max_concurrency),
                                 node.max_concurrency > 0 ? node.max_concurrency
                                                          : std::max(1, def.limits.max_concurrency));
        std::atomic<std::size_t> next_index{0};
        std::vector<std::thread> pool;
        const std::size_t threads = std::min<std::size_t>(static_cast<std::size_t>(cap), count);
        const auto worker = [&]() {
            while (true) {
                const std::size_t index = next_index.fetch_add(1);
                if (index >= count) return;
                (void)run_item(index);
                if (ctx.cancel != nullptr && ctx.cancel->load()) return;
            }
        };
        pool.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }

    if (ctx.cancel != nullptr && ctx.cancel->load()) {
        record.state = NodeState::Cancelled;
        return "cancelled";
    }
    // join 后拼装:预分配的 array,逐槽按下标落——不靠 operator[] 的
    // 缺省插入语义(libstdc++/libc++ 行为虽同,显式 resize 更不给巧合留门)。
    nlohmann::json mapped = nlohmann::json::array();
    for (std::size_t i = 0; i < count; ++i) {
        mapped.push_back(slots[i].done ? std::move(slots[i].output) : nlohmann::json());
    }
    ctx.store->CommitOutputOverwrite(node.id,
                                     nlohmann::json{{"items", mapped}, {"failures", failures}});
    record.state = failures > 0 && node.kind == NodeKind::Foreach ? NodeState::Failed : NodeState::Succeeded;
    return failures > 0 && node.kind == NodeKind::Foreach ? "error" : "success";
}

std::string WorkflowRuntime::RunReduce(const ExecutionContext& ctx, const WorkflowNode& node) {
    WorkflowRunSummary& account = *ctx.account;
    NodeRunRecord& record = account.nodes[node.id];
    record.state = NodeState::Running;

    // reduce 的 items:node.items_ref 没写就找 input.items(定义可以两处写)。
    std::string items_ref = node.items_ref;
    if (items_ref.empty() && node.input.is_object() && node.input.contains("items") &&
        node.input["items"].is_string()) {
        items_ref = node.input["items"].get<std::string>();
    }
    auto items = ResolveRef(*ctx.store, StripRefBraces(items_ref));
    if (!items.has_value() || !items->is_array()) {
        record.state = NodeState::Failed;
        record.error_code = "bad_items";
        record.error_message = "reduce 的 items 不是数组";
        return "error";
    }
    const auto body_it = ctx.definition->node_map.find(node.reduce_body);
    if (body_it == ctx.definition->node_map.end()) {
        record.state = NodeState::Failed;
        record.error_code = "unknown_body";
        record.error_message = "reduce body 节点不存在: " + node.reduce_body;
        return "error";
    }
    nlohmann::json acc = nlohmann::json();
    if (!node.initial_ref.empty()) {
        if (auto init = ResolveRef(*ctx.store, StripRefBraces(node.initial_ref)); init.has_value()) {
            acc = *init;
        }
    }
    // 稳定次序:items 顺序,不按完成时间(单子:reduce 按稳定次序汇总)。
    for (const auto& item : *items) {
        if (ctx.cancel != nullptr && ctx.cancel->load()) {
            record.state = NodeState::Cancelled;
            return "cancelled";
        }
        WorkflowNode body = body_it->second;
        nlohmann::json body_input = body.input.is_object() ? body.input : nlohmann::json::object();
        body_input["acc"] = acc;
        body_input["item"] = item;
        body.input = body_input;
        const std::string outcome = RunNode(ctx, body);
        if (outcome != "success") {
            record.state = NodeState::Failed;
            record.error_code = "reduce_step_failed";
            return "error";
        }
        if (auto output = ctx.store->GetOutput(body.id)) {
            acc = *output;
        }
    }
    ctx.store->CommitOutputOverwrite(node.id, acc);
    record.state = NodeState::Succeeded;
    return "success";
}

WorkflowRunSummary WorkflowRuntime::Run(const WorkflowDefinition& definition, const RunInputs& inputs,
                                         const std::atomic<bool>* cancel_token) {
    Store store;
    // 预置为空(新跑):全部节点从头跑。
    return RunWithStore(definition, inputs.values, std::move(store), {}, cancel_token, false);
}

std::expected<WorkflowRunSummary, std::string> WorkflowRuntime::Resume(
    const std::filesystem::path& run_dir, const std::atomic<bool>* cancel_token) {
    // 1) definition 快照:归一化定义必须还原得动(定义没了也续得上)。
    std::ifstream def_file(run_dir / "definition.json", std::ios::binary);
    if (!def_file) {
        return std::unexpected("run 目录没有 definition 快照: " + lubancode::platform::PathToUtf8(run_dir));
    }
    nlohmann::json def_json;
    try {
        def_json = nlohmann::json::parse(def_file);
    } catch (const std::exception& e) {
        return std::unexpected(std::string("definition 快照解析失败: ") + e.what());
    }
    WorkflowDefinition definition;
    try {
        definition = WorkflowDefinition::FromJson(def_json);
    } catch (const std::exception& e) {
        return std::unexpected(std::string("definition 快照还原失败: ") + e.what());
    }

    // 2) journal 重放:已完成节点(有 output)不重跑。
    const std::vector<JournalEvent> events = ReadJournalEvents(run_dir);
    const auto replayed = ReplayNodes(events);
    const auto checkpoint = ReadLatestCheckpoint(run_dir);
    nlohmann::json inputs_json = nlohmann::json::object();
    std::map<std::string, NodeRunRecord> precompleted;
    if (checkpoint.has_value() && checkpoint->is_object()) {
        if (const auto it = checkpoint->find("inputs"); it != checkpoint->end() && it->is_object()) {
            inputs_json = *it;
        }
        // checkpoint 里的节点 output 全部认账(它只在节点终态后写)。
        if (const auto nodes = checkpoint->find("nodes"); nodes != checkpoint->end() && nodes->is_object()) {
            for (auto node = nodes->begin(); node != nodes->end(); ++node) {
                NodeRunRecord record;
                record.node_id = node.key();
                record.state = NodeState::Succeeded;
                precompleted.emplace(node.key(), std::move(record));
            }
        }
    }
    // journal 事件比 checkpoint 新的部分也认(node_completed 有 output)。
    for (const auto& [node_id, node] : replayed) {
        if (node.state == "succeeded") {
            precompleted.insert_or_assign(node_id, NodeRunRecord{node_id, NodeState::Succeeded});
        }
    }
    // loop 自己要读 checkpoint 里的轮次再判续跑/收口,不能被普通的
    // "已有 output = 已完成"规则跳过。body 由 loop 直接调度,也不走主图跳过口。
    for (const auto& node : definition.nodes) {
        if (node.kind == NodeKind::Loop) precompleted.erase(node.id);
    }

    Store store;
    if (checkpoint.has_value()) {
        store = Store::FromJson(*checkpoint);
    } else {
        store.Initialize(inputs_json, nlohmann::json{{"run_id", std::string()}});
    }
    return RunWithStore(definition, inputs_json, std::move(store), precompleted, cancel_token, true);
}

WorkflowRunSummary WorkflowRuntime::RunWithStore(const WorkflowDefinition& definition,
                                                  const nlohmann::json& inputs, Store&& preloaded,
                                                  const std::map<std::string, NodeRunRecord>& precompleted,
                                                  const std::atomic<bool>* cancel_token, bool resuming) {
    WorkflowRunSummary account;
    account.run_id = options_.run_id_generator ? options_.run_id_generator() : DefaultRunId();
    account.workflow_id = definition.id;
    account.state = RunState::Created;
    for (const auto& [id, record] : precompleted) {
        account.nodes.emplace(id, record);
    }

    Store store = std::move(preloaded);
    const nlohmann::json effective_inputs = ApplyInputDefaults(inputs, definition.inputs);
    const std::int64_t started_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();

    // 取消最先看:用户都撤了,校验也不必做。
    if (cancel_token != nullptr && cancel_token->load()) {
        account.state = RunState::Cancelled;
        account.error_code = "cancelled";
        account.error_message = "用户取消";
        account.duration_ms = (options_.clock ? options_.clock->NowMs() : JournalClock().NowMs()) - started_ms;
        return account;
    }

    // validating:输入 schema 对账。
    account.state = RunState::Validating;
    const ValidationResult validation = ValidateDefinition(definition, std::nullopt);
    if (!validation.ok()) {
        account.state = RunState::Failed;
        account.error_code = "invalid_definition";
        const ValidationIssue& issue = validation.issues.front();
        account.error_message = issue.path + ": " + issue.message;
        account.duration_ms = (options_.clock ? options_.clock->NowMs() : JournalClock().NowMs()) - started_ms;
        return account;
    }
    if (auto problem = ValidateInputsAgainstSchema(effective_inputs, definition.inputs)) {
        account.state = RunState::Failed;
        account.error_code = "invalid_inputs";
        account.error_message = *problem;
        account.duration_ms = (options_.clock ? options_.clock->NowMs() : JournalClock().NowMs()) - started_ms;
        return account;
    }

    if (!resuming) {
        store.Initialize(effective_inputs, nlohmann::json{
            {"workflow_id", definition.id},
            {"workflow_version", definition.version},
            {"run_id", account.run_id},
        });
    }
    account.state = RunState::Ready;

    // journal(可选)。
    std::optional<RunJournal> journal;
    if (!options_.runs_root.empty()) {
        RunJournal::StartInfo info;
        info.run_id = account.run_id;
        info.workflow_id = definition.id;
        info.workflow_version = definition.version;
        info.content_hash = ContentHash(definition);
        info.cwd = lubancode::platform::PathToUtf8(std::filesystem::current_path());
        info.definition_json = BuildNormalizedJson(definition).dump();
        auto started = RunJournal::Start(options_.runs_root, info, options_.clock.get());
        if (started.has_value()) {
            journal.emplace(std::move(*started));
        } else {
            std::cerr << "[workflow] run journal 起不成,本跑不留档: " << started.error() << "\n";
        }
    }

    account.state = RunState::Running;
    EmitRunEvent(account, kEventRunStarted, nlohmann::json{{"state", ToString(account.state)}});

    int steps = 0;
    ExecutionContext ctx;
    ctx.definition = &definition;
    ctx.account = &account;
    ctx.store = &store;
    ctx.journal = journal.has_value() ? &*journal : nullptr;
    ctx.cancel = cancel_token;
    ctx.steps = &steps;
    ctx.started_ms = started_ms;
    std::mutex nodes_mutex;  // 并行分支共写账本的那把锁
    ctx.nodes_mutex = &nodes_mutex;

    // 主循环:entry 起步,按 outcome 边走。
    std::string current = definition.entry;
    while (!current.empty()) {
        if (cancel_token != nullptr && cancel_token->load()) {
            account.state = RunState::Cancelled;
            account.error_code = "cancelled";
            account.error_message = "用户取消";
            break;
        }
        // 步数闸(批五):count 尺声明进公共预算闸,Overrun 口径(已越帽,
        // 严格 >)——(++steps > max_steps) 逐字节同判。
        if (++steps;
            runtime::BudgetGate(runtime::BudgetScales{
                .count = static_cast<std::int64_t>(definition.limits.max_steps),
            }).OverrunCount(steps)) {
            account.state = RunState::BudgetExhausted;
            account.error_code = "max_steps";
            account.error_message =
                "步数越过 max_steps(" + std::to_string(definition.limits.max_steps) + ")";
            break;
        }
        const auto node_it = definition.node_map.find(current);
        if (node_it == definition.node_map.end()) {
            account.state = RunState::Failed;
            account.error_code = "unknown_node";
            account.error_message = "走到的节点不存在: " + current;
            break;
        }
        const WorkflowNode& node = node_it->second;

        // end / checkpoint 是控制节点,不走执行器。
        if (node.kind == NodeKind::End) {
            auto result = BuildResult(definition, store);
            if (!result.has_value()) {
                account.state = RunState::Failed;
                account.error_code = "resolve_result";
                account.error_message = result.error().path + ": " + result.error().message;
                break;
            }
            account.result = std::move(*result);
            account.state = RunState::Succeeded;
            break;
        }
        if (node.kind == NodeKind::Checkpoint) {
            if (journal.has_value()) {
                journal->SaveCheckpoint(journal->last_seq(), store.ToJson());
            }
            current = NextNodeFor(definition, node.id, "success");
            continue;
        }
        // switch:按结构化条件选路(受限表达式,禁 eval)。
        if (node.kind == NodeKind::Switch) {
            current = EvaluateSwitch(definition, node, store);
            if (current.empty()) {
                account.state = RunState::Succeeded;  // 都不中且无 default:skipped 收
            }
            continue;
        }
        // loop:body 每轮顺次跑,until 命中即走 success;撞 max 走 exhausted。
        if (node.kind == NodeKind::Loop) {
            const std::string loop_outcome = RunLoop(ctx, node);
            if (loop_outcome == "cancelled") {
                account.state = RunState::Cancelled;
                account.error_code = "cancelled";
                break;
            }
            if (loop_outcome == "budget_exhausted") break;
            if (loop_outcome == "error") {
                account.state = RunState::Failed;
                account.error_code = "loop_failed";
                account.error_message = node.id + ": " + account.nodes[node.id].error_message;
                break;
            }
            current = NextNodeFor(definition, node.id, loop_outcome);
            if (current.empty()) {
                account.state = RunState::Failed;
                account.error_code = "loop_outcome_unhandled";
                account.error_message = node.id + " 的 " + loop_outcome + " 没接出边";
                break;
            }
            continue;
        }
        // async:body 在工作线程跑;外壳只等这一只 I/O 活,并非 fan-out。
        if (node.kind == NodeKind::Async) {
            const std::string async_outcome = RunAsync(ctx, node);
            if (async_outcome == "cancelled") {
                account.state = RunState::Cancelled;
                account.error_code = "cancelled";
                break;
            }
            if (async_outcome == "budget_exhausted") break;
            if (async_outcome == "error") {
                std::string next = NextNodeFor(definition, node.id, "error");
                if (next.empty()) {
                    account.state = RunState::Failed;
                    account.error_code = "async_failed";
                    account.error_message = node.id + ": " + account.nodes[node.id].error_message;
                    break;
                }
                current = next;
                continue;
            }
            current = NextNodeFor(definition, node.id, async_outcome);
            if (current.empty()) {
                auto result = BuildResult(definition, store);
                if (result.has_value()) {
                    account.result = std::move(*result);
                    account.state = RunState::Succeeded;
                } else {
                    account.state = RunState::Failed;
                    account.error_code = "resolve_result";
                    account.error_message = result.error().path + ": " + result.error().message;
                }
                break;
            }
            continue;
        }
        // parallel:分支齐跑,join 政策收束(第 3 批)。
        if (node.kind == NodeKind::Parallel || node.kind == NodeKind::Join) {
            const std::string join_outcome = RunParallel(ctx, node);
            if (join_outcome == "cancelled") {
                account.state = RunState::Cancelled;
                account.error_code = "cancelled";
                break;
            }
            if (join_outcome == "error") {
                account.state = RunState::Failed;
                account.error_code = "join_failed";
                const NodeRunRecord* worst = nullptr;
                for (const auto& b : node.branches) {
                    const auto it = account.nodes.find(b);
                    if (it != account.nodes.end() && it->second.state == NodeState::Failed) {
                        if (worst == nullptr) worst = &it->second;
                    }
                }
                account.error_message = node.id + ": 分支失败(" +
                                        (worst != nullptr ? worst->error_code : std::string("?")) + ")";
                break;
            }
            current = NextNodeFor(definition, node.id, join_outcome.empty() ? "joined" : join_outcome);
            // 旧定义与 schema 文档把 parallel 的汇合边写作 joined，执行器
            // 内部则用 success 表示 join 策略通过。两种词都已流通过，先认
            // 执行器原词，再认 joined，免得图在汇合处无声断掉。
            if (current.empty() && join_outcome == "success") {
                current = NextNodeFor(definition, node.id, "joined");
            }
            continue;
        }
        // map/foreach:数组拆项,逐项跑 body(map 并发,foreach 顺次)。
        if (node.kind == NodeKind::Map || node.kind == NodeKind::Foreach) {
            const std::string map_outcome = RunMap(ctx, node);
            if (map_outcome == "cancelled") {
                account.state = RunState::Cancelled;
                account.error_code = "cancelled";
                break;
            }
            if (map_outcome == "error") {
                account.state = RunState::Failed;
                account.error_code = "map_failed";
                account.error_message = node.id + ": map 展开或执行失败";
                break;
            }
            current = NextNodeFor(definition, node.id, map_outcome);
            continue;
        }
        // reduce:按定义顺序汇总(第 3 批:吃 map/parallel 的结果数组)。
        if (node.kind == NodeKind::Reduce) {
            const std::string reduce_outcome = RunReduce(ctx, node);
            if (reduce_outcome == "error") {
                account.state = RunState::Failed;
                account.error_code = "reduce_failed";
                const NodeRunRecord& reduce_record = account.nodes[node.id];
                account.error_message =
                    node.id + ": " + reduce_record.error_code + " " + reduce_record.error_message;
                break;
            }
            current = NextNodeFor(definition, node.id, reduce_outcome);
            continue;
        }

        // 预算对账(节点跑之前):越帽收 budget_exhausted,不悄悄续跑。
        if (!WithinBudget(definition.limits, account)) {
            account.state = RunState::BudgetExhausted;
            account.error_code = "budget_exhausted";
            account.error_message = "预算越帽(tool_calls/tokens)";
            break;
        }
        // 时限(批五):elapsed 尺声明进公共预算闸,Overrun 口径(严格 >,
        // 与旧判同线);timeout_secs <= 0 = 不设尺。
        const std::int64_t now_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
        const runtime::BudgetGate timeout_gate(runtime::BudgetScales{
            .elapsed_ms = definition.limits.timeout_secs > 0
                              ? std::optional<std::int64_t>(definition.limits.timeout_secs * 1000)
                              : std::nullopt,
        });
        if (timeout_gate.OverrunElapsed(now_ms - started_ms)) {
            account.state = RunState::BudgetExhausted;
            account.error_code = "timeout";
            account.error_message = "总时限越过 " + std::to_string(definition.limits.timeout_secs) + "s";
            break;
        }

        // 恢复路径:已成功的节点不再跑(副作用不重复做,单子"Run Journal"
        // 一节),沿 success 边直接过。
        if (resuming && account.nodes.count(node.id) > 0 &&
            account.nodes.at(node.id).state == NodeState::Succeeded) {
            current = NextNodeFor(definition, node.id, "success");
            continue;
        }
        const std::string outcome = RunNode(ctx, node);
        // skip 的节点(unavailable)计入缺失账(单子验收:报告明写缺了谁)。
        if (outcome == "skipped") {
            account.unavailable_sources.push_back(node.id);
        }
        if (outcome == "cancelled") {
            account.state = RunState::Cancelled;
            account.error_code = "cancelled";
            break;
        }
        if (outcome == "error") {
            // fallback 是一条明边(单子:fallback 是一条明边或一只明节点,
            // 不得在深处静默换工具)。节点字段 fallback_to 就是这条明边;
            // 其次才看 outcome=error 的边。
            std::string next;
            if (!node.fallback_to.empty() && definition.node_map.count(node.fallback_to) > 0) {
                next = node.fallback_to;
            } else {
                next = NextNodeFor(definition, node.id, "error");
            }
            if (next.empty()) {
                account.state = RunState::Failed;
                account.error_code = "node_failed";
                const NodeRunRecord& record = account.nodes[node.id];
                account.error_message = node.id + ": " + record.error_code + " " + record.error_message;
                break;
            }
            current = next;
            continue;
        }
        current = NextNodeFor(definition, node.id, outcome);
        if (current.empty()) {
            // 没有 success 出边:视为图走完(没写 end 节点的小图)。
            auto result = BuildResult(definition, store);
            if (result.has_value()) {
                account.result = std::move(*result);
                account.state = RunState::Succeeded;
            } else {
                account.state = RunState::Failed;
                account.error_code = "resolve_result";
                account.error_message = result.error().path + ": " + result.error().message;
            }
            break;
        }
    }

    if (account.state == RunState::Running) {
        // 结构节点(loop/parallel/map/reduce)可能把 current 走到空。不能只把
        // 状态改成成功而漏掉 result；统一在这里结算，引用断了也明白报错。
        auto result = BuildResult(definition, store);
        if (result.has_value()) {
            account.result = std::move(*result);
            account.state = RunState::Succeeded;
        } else {
            account.state = RunState::Failed;
            account.error_code = "resolve_result";
            account.error_message = result.error().path + ": " + result.error().message;
        }
    }

    account.duration_ms = (options_.clock ? options_.clock->NowMs() : JournalClock().NowMs()) - started_ms;
    EmitRunEvent(account, kEventRunCompleted,
                 nlohmann::json{{"state", ToString(account.state)},
                                {"error", account.error_message},
                                {"tokens", account.tokens_used},
                                {"duration_ms", account.duration_ms}});
    if (journal.has_value()) {
        journal->SaveCheckpoint(journal->last_seq(), store.ToJson());
        journal->Finish(ToString(account.state),
                        nlohmann::json{{"result", account.result},
                                       {"tokens", account.tokens_used},
                                       {"duration_ms", account.duration_ms}});
    }
    return account;
}

// ---------------------------------------------------------------------------
// 内建执行器
// ---------------------------------------------------------------------------

void TransformExecutor::Register(const std::string& operation, TransformFn fn) {
    transforms_[operation] = std::move(fn);
}

NodeExecResult TransformExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    const auto it = transforms_.find(request.node->operation);
    if (it == transforms_.end()) {
        result.error_code = "unknown_transform";
        result.error_message = "变换未注册: " + request.node->operation + "(不认魔法字符串)";
        return result;
    }
    try {
        result.output = it->second(request.resolved_input);
        result.ok = true;
    } catch (const std::exception& e) {
        result.error_code = "transform_failed";
        result.error_message = e.what();
    }
    return result;
}

namespace {

// {{path}} 点路径取值;缺字段空串。
std::string RenderTemplateText(const std::string& text, const nlohmann::json& data) {
    std::string out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t open = text.find("{{", pos);
        if (open == std::string::npos) {
            out += text.substr(pos);
            break;
        }
        out += text.substr(pos, open - pos);
        const std::size_t close = text.find("}}", open);
        if (close == std::string::npos) {
            out += text.substr(open);
            break;
        }
        std::string path = text.substr(open + 2, close - open - 2);
        // trim
        const auto not_space = [](char c) { return c != ' ' && c != '\t'; };
        const std::size_t b = std::find_if(path.begin(), path.end(), not_space) - path.begin();
        const std::size_t e = std::find_if(path.rbegin(), path.rend(), not_space).base() - path.begin();
        path = path.substr(b, e - b);

        const nlohmann::json* current = &data;
        std::size_t start = 0;
        bool found = true;
        while (start <= path.size() && !path.empty()) {
            const std::size_t dot = path.find('.', start);
            const std::string seg =
                path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (current->is_object() && current->contains(seg)) {
                current = &(*current)[seg];
            } else {
                found = false;
                break;
            }
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        if (found && !path.empty()) {
            out += current->is_string() ? current->get<std::string>() : current->dump();
        }
        pos = close + 2;
    }
    return out;
}

}  // namespace

NodeExecResult TemplateExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    // 模板正文从 input.render 或 input.text 取(定义里 template: 路径指包内
    // 文件,由宿主在装配时读进来塞 input;这里不碰盘)。
    std::string text;
    if (request.resolved_input.is_object()) {
        if (const auto it = request.resolved_input.find("render"); it != request.resolved_input.end() && it->is_string()) {
            text = it->get<std::string>();
        } else if (const auto t = request.resolved_input.find("text");
                   t != request.resolved_input.end() && t->is_string()) {
            text = t->get<std::string>();
        }
    } else if (request.resolved_input.is_string()) {
        text = request.resolved_input.get<std::string>();
    }
    nlohmann::json data = nlohmann::json::object();
    if (request.resolved_input.is_object()) {
        if (const auto it = request.resolved_input.find("data"); it != request.resolved_input.end()) {
            data = *it;
        }
    }
    result.output = nlohmann::json{{"rendered", RenderTemplateText(text, data)}};
    result.ok = true;
    return result;
}

NodeExecResult EchoExecutor::Execute(const NodeExecRequest& request) {
    NodeExecResult result;
    result.output = request.resolved_input;
    result.ok = true;
    return result;
}

}  // namespace lubancode::workflow
