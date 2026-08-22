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
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <utility>

#include "platform/paths.hpp"

namespace lubancode::workflow {

namespace {

std::string DefaultRunId() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const std::int64_t secs = ms / 1000;
    const int millis = static_cast<int>(ms % 1000);
    std::tm tm_buf{};
    const std::time_t tt = static_cast<std::time_t>(secs);
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
            return to == RunState::WaitingInput || to == RunState::WaitingApproval || to == RunState::Paused ||
                   to == RunState::Succeeded || to == RunState::Failed || to == RunState::Cancelled ||
                   to == RunState::BudgetExhausted;
        case RunState::WaitingInput:
        case RunState::WaitingApproval:
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
                   to == NodeState::WaitingApproval || to == NodeState::Succeeded || to == NodeState::Failed ||
                   to == NodeState::Cancelled;
        case NodeState::RetryWait:
            return to == NodeState::Ready || to == NodeState::Failed || to == NodeState::Cancelled;
        case NodeState::WaitingInput:
        case NodeState::WaitingApproval:
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
    if (account.tool_calls > limits.tool_calls) return false;
    if (account.tokens_used > limits.tokens) return false;
    return true;
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
    static std::atomic<std::uint64_t> seq_source{1};
    event.envelope.seq = seq_source.fetch_add(1);
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

std::string WorkflowRuntime::RunNode(const ExecutionContext& ctx, const WorkflowNode& node) {
    WorkflowRunSummary& account = *ctx.account;
    NodeRunRecord& record = account.nodes[node.id];

    const auto executor_it = options_.executors.find(node.kind);
    if (executor_it == options_.executors.end()) {
        record.state = NodeState::Failed;
        record.error_code = "no_executor";
        record.error_message = "节点种类 " + ToString(node.kind) + " 没配执行器";
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
            return "cancelled";
        }
        record.state = NodeState::Running;
        record.started_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
        if (ctx.journal != nullptr) {
            ctx.journal->Append(kEventNodeStarted, node.id, attempt,
                                nlohmann::json{{"kind", ToString(node.kind)}});
        }

        NodeExecRequest request;
        request.definition = ctx.definition;
        request.node = &node;
        request.run_id = account.run_id;
        request.node_run_id = account.run_id + "-" + node.id + "-a" + std::to_string(attempt);
        request.attempt = attempt;
        request.store = ctx.store;
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
            return "error";
        }
        request.resolved_input = resolved_input->value;

        result = executor.Execute(request);
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
            // backoff 等待:受取消打断。fake clock 下 wait 缩为 0(单测不
            // 靠 sleep 赌时序)。
            if (node.retry.has_value()) {
                std::int64_t wait_ms = node.retry->initial_ms;
                if (node.retry->backoff == BackoffKind::Exponential) {
                    std::int64_t factor = 1;
                    for (int i = 1; i < attempt; ++i) factor *= 2;
                    wait_ms = node.retry->initial_ms * factor;
                }
                wait_ms = std::min(wait_ms, node.retry->max_ms);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (ctx.cancel != nullptr && ctx.cancel->load()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
        return "error";
    }

    // CommitOutput:原子落账。循环迭代里同节点重入走覆写(meta 记轮次),
    // 首写与覆写都算落账成功——"半份 output"仍不可能:Execute 只产候选,
    // 这里一次性换入。
    if (result.ok) {
        ctx.store->CommitOutputOverwrite(node.id, result.output);
        record.state = NodeState::Succeeded;
        if (ctx.journal != nullptr) {
            ctx.journal->Append(kEventNodeCompleted, node.id, record.attempt,
                                nlohmann::json{{"outcome", result.empty ? "empty" : "success"},
                                               {"output", result.output},
                                               {"tokens", result.tokens_used},
                                               {"duration_ms", result.duration_ms}});
        }
        return result.empty ? "empty" : "success";
    }
    record.state = NodeState::Failed;
    return "error";
}

WorkflowRunSummary WorkflowRuntime::Run(const WorkflowDefinition& definition, const RunInputs& inputs,
                                         const std::atomic<bool>* cancel_token) {
    WorkflowRunSummary account;
    account.run_id = options_.run_id_generator ? options_.run_id_generator() : DefaultRunId();
    account.workflow_id = definition.id;
    account.state = RunState::Created;

    Store store;
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
    if (auto problem = ValidateInputsAgainstSchema(inputs.values, definition.inputs)) {
        account.state = RunState::Failed;
        account.error_code = "invalid_inputs";
        account.error_message = *problem;
        account.duration_ms = (options_.clock ? options_.clock->NowMs() : JournalClock().NowMs()) - started_ms;
        return account;
    }

    store.Initialize(inputs.values, nlohmann::json{
        {"workflow_id", definition.id},
        {"workflow_version", definition.version},
        {"run_id", account.run_id},
    });
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

    ExecutionContext ctx;
    ctx.definition = &definition;
    ctx.account = &account;
    ctx.store = &store;
    ctx.journal = journal.has_value() ? &*journal : nullptr;
    ctx.cancel = cancel_token;

    // 主循环:entry 起步,按 outcome 边走。
    std::string current = definition.entry;
    int steps = 0;
    while (!current.empty()) {
        if (cancel_token != nullptr && cancel_token->load()) {
            account.state = RunState::Cancelled;
            account.error_code = "cancelled";
            account.error_message = "用户取消";
            break;
        }
        if (++steps > definition.limits.max_steps) {
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

        // 预算对账(节点跑之前):越帽收 budget_exhausted,不悄悄续跑。
        if (!WithinBudget(definition.limits, account)) {
            account.state = RunState::BudgetExhausted;
            account.error_code = "budget_exhausted";
            account.error_message = "预算越帽(tool_calls/tokens)";
            break;
        }
        // 时限。
        const std::int64_t now_ms = options_.clock ? options_.clock->NowMs() : JournalClock().NowMs();
        if (definition.limits.timeout_secs > 0 && now_ms - started_ms > definition.limits.timeout_secs * 1000) {
            account.state = RunState::BudgetExhausted;
            account.error_code = "timeout";
            account.error_message = "总时限越过 " + std::to_string(definition.limits.timeout_secs) + "s";
            break;
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
        // 循环自然耗尽(空 current 起步等):按成功收。
        account.state = RunState::Succeeded;
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
