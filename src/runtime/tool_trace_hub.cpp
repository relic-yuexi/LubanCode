// ToolTraceHub 的实现(逐枚追踪单)。

#include "runtime/tool_trace_hub.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace lubancode::runtime {

ToolTraceHub::ToolTraceHub(IdAuthority& ids, agent::SessionStore* store)
    : ToolTraceHub(ids, store, Options{}) {}

ToolTraceHub::ToolTraceHub(IdAuthority& ids, agent::SessionStore* store, const Options& options)
    : ids_(ids), store_(store), options_(options) {}



ToolTraceHub::~ToolTraceHub() = default;

std::string ToolTraceHub::NextExecutionId() {
    return ids_.NextItemId();
}

void ToolTraceHub::Install(agent::AgentLoop& loop, agent::Callbacks& callbacks, const std::string& thread_id,
                           const std::string& turn_id) {
    // execution 发号:与 Runtime item id 同源(单子:不可再造第二只计数器)。
    loop.SetExecutionIdIssuer([this] { return NextExecutionId(); });
    thread_id_ = thread_id;
    turn_id_ = turn_id;
    callbacks.on_tool_trace = [this](const agent::ToolTraceEvent& event) { OnTrace(event); };
    // 拦截查询口:RunOneTool 在 emit(execution_started) 之后、execute
    // 之前同步问一句。hub 见过"started 写盘失败 + 副作用档"时答 false,
    // RunOneTool 立即以 result_store_failed 收尾,不 execute(单子:
    // 副作用工具不得继续执行)。
    callbacks.on_tool_trace_blocked = [this](const std::string& execution_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocked_executions_.count(execution_id) != 0;
    };
    // 落盘次序关口(单子"消息落盘次序要改"):assistant 消息在执行任何
    // 工具之前 append+flush;五枚结果收齐后 user 消息 append+flush,再补
    // 各枚 result_committed。store 没开张(空指针/没 active)时这两个口
    // 空操作——老路 PersistNewMessages 仍会在轮末兜底,一个不丢。
    callbacks.on_assistant_message_ready = [this](const api::Message& message) {
        if (store_ != nullptr && store_->active()) {
            store_->AppendMessage(message);
        }
    };
    callbacks.on_tool_results_committed = [this](const std::string& batch_id, const api::Message& message) {
        if (store_ != nullptr && store_->active()) {
            store_->AppendMessage(message);
        }
        // 各枚 result_committed:本批在 recent_ 里有 finished 的都算落定。
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& event : recent_) {
            if (event.batch_id != batch_id ||
                (event.kind != agent::ToolTraceEventKind::ExecutionFinished &&
                 event.kind != agent::ToolTraceEventKind::Scheduled)) {
                continue;
            }
            agent::ToolTraceEvent committed = event;
            committed.kind = agent::ToolTraceEventKind::ResultCommitted;
            committed.outcome = agent::ToolOutcome::Succeeded;
            committed.error_code.clear();
            committed.result_ref = agent::ToolResultRef{};
            committed.undo = agent::ToolUndoToken{};
            committed.seq = ids_.NextSeq();
            committed.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
            if (store_ != nullptr && store_->active()) {
                store_->AppendToolTraceEvent(committed);
            }
        }
    };
}

bool ToolTraceHub::ShouldBlockOnFailedStart(agent::EffectClass cls) const {
    switch (cls) {
        case agent::EffectClass::ReadOnlyLocal:
        case agent::EffectClass::ReadOnlyRemote:
            return false;  // 只读档:降级执行 + 当场告警(单子)
        default:
            return true;   // 副作用档:写不落,不得继续执行
    }
}

void ToolTraceHub::OnTrace(const agent::ToolTraceEvent& event) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        recent_.push_back(event);
        // 进程内诊断账有界:留最近 512 枚,足够 /trace 看几批;真本在
        // session JSONL,这里丢了不伤恢复。
        if (recent_.size() > 512) {
            recent_.erase(recent_.begin(), recent_.end() - 512);
            dropped_count_ += 1;
        }
        if (event.kind == agent::ToolTraceEventKind::Scheduled) {
            last_batch_id_ = event.batch_id;
        }
        // agent 工具的执行区间:子代理事件的 parent 关系边由这里钉。
        // started 前置、finished/terminal 清位——子代理在这区间内投递的
        // 每枚事件都带上这枚 agent 调用的 execution_id。
        if (event.tool_name == "agent" || event.tool_name == "agent_dispatch") {
            if (event.kind == agent::ToolTraceEventKind::ExecutionStarted) {
                current_agent_execution_ = event.execution_id;
            } else if (event.kind == agent::ToolTraceEventKind::ExecutionFinished) {
                current_agent_execution_.clear();
            }
        }
    }

    // 1) 持久账:关键栅栏 append+flush。started 写失败按 effect class
    // 决定拦不拦(拦的信号走 EventSink 的 Error + LogSink,execute 不再
    // 发生——RunOneTool 在 emit(started) 之后、execute 之前没有查询口,
    // 这里用"写失败即置 fail_start_、由下一枚 started 前检查"的口径:
    // 事实上 RunOneTool 的 emit 是同步的,store 写失败时我们直接改走
    // 拦截分支:把 started 事件翻成 finished(blocked by trace_append_
    // failed)再投出去,RunOneTool 随后的 execute 照跑——所以真正的闸
    // 在这里:写不落且副作用档时,投一枚 Error 事件并把 finishOutcome
    // 置为 ResultStoreFailed,模型看到的就是"没跑成"。
    if (store_ != nullptr && store_->active()) {
        const bool durable_ok = store_->AppendToolTraceEvent(event);
        if (!durable_ok) {
            if (event.kind == agent::ToolTraceEventKind::ExecutionStarted &&
                ShouldBlockOnFailedStart(event.effect_class)) {
                // 副作用工具、started 落不住:按单子,不得继续执行。
                // 补一枚 terminal 栅栏把这一枚收成 result_store_failed,
                // 恢复侧不会把它误读成"副作用未知"。
                agent::ToolTraceEvent failed = event;
                failed.kind = agent::ToolTraceEventKind::ExecutionFinished;
                failed.outcome = agent::ToolOutcome::ResultStoreFailed;
                failed.error_code = agent::kErrSessionTraceAppendFailed;
                failed.fallback_message = "追踪账写盘失败,该工具未执行(副作用档默认拦截)";
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    blocked_executions_.insert(event.execution_id);
                    recent_.push_back(failed);
                }
                store_->AppendToolTraceEvent(failed);
                EmitRuntimeEvent(failed);
                return;  // 终态已出,不再往下分线
            }
            // 只读档或非 started 栅栏:降级执行,当场告警(UI 不拦工具)。
            if (sink_ != nullptr) {
                sink_->Emit(MakeWarningEvent(event, agent::kErrSessionTraceAppendFailed));
            }
        }
    }

    // 2) Runtime 投影(UI/app-server;失败不拦工具)。
    EmitRuntimeEvent(event);

    // 3) Workflow projection(只吃摘要,不吃正文)。
    if (projection_) {
        projection_(event);
    }
}

void ToolTraceHub::EmitRuntimeEvent(const agent::ToolTraceEvent& event) {
    if (sink_ == nullptr) {
        return;
    }
    ServerEvent server;
    server.envelope.thread_id = thread_id_;
    server.envelope.seq = ids_.NextSeq();
    server.envelope.timestamp_ms = event.timestamp_ms;
    server.kind = ServerEventKind::ItemDelta;
    server.turn_id = event.turn_id.empty() ? turn_id_ : event.turn_id;
    server.item_id = event.item_id.empty() ? event.execution_id : event.item_id;
    server.item_kind = ItemKind::Tool;
    nlohmann::json payload;
    payload["trace_event"] = agent::ToString(event.kind);
    payload["execution_id"] = event.execution_id;
    if (!event.tool_use_id.empty()) {
        payload["tool_use_id"] = event.tool_use_id;
    }
    if (!event.tool_name.empty()) {
        payload["tool_name"] = event.tool_name;
    }
    payload["outcome"] = agent::ToString(event.outcome);
    if (!event.error_code.empty()) {
        payload["error_code"] = event.error_code;
    }
    if (!event.fallback_message.empty()) {
        payload["fallback_message"] = event.fallback_message;
    }
    if (!event.batch_id.empty()) {
        payload["batch_id"] = event.batch_id;
    }
    if (event.sequence_in_batch >= 0) {
        payload["sequence_in_batch"] = event.sequence_in_batch;
    }
    if (!event.source_instance.empty()) {
        payload["source_instance"] = event.source_instance;
    }
    if (!event.parent_execution_id.empty()) {
        payload["parent_execution_id"] = event.parent_execution_id;
    }
    if (!event.retry_of.empty()) {
        payload["retry_of"] = event.retry_of;
    }
    if (!event.blocked_by.empty()) {
        payload["blocked_by"] = event.blocked_by;
    }
    if (!event.compensates.empty()) {
        payload["compensates"] = event.compensates;
    }
    server.payload = std::move(payload);
    sink_->Emit(server);
}

ServerEvent ToolTraceHub::MakeWarningEvent(const agent::ToolTraceEvent& event, const std::string& code) {
    ServerEvent warning;
    warning.envelope.thread_id = thread_id_;
    warning.envelope.seq = ids_.NextSeq();
    warning.envelope.timestamp_ms = event.timestamp_ms;
    warning.kind = ServerEventKind::Warning;
    warning.turn_id = event.turn_id.empty() ? turn_id_ : event.turn_id;
    warning.item_id = event.item_id.empty() ? event.execution_id : event.item_id;
    warning.item_kind = ItemKind::Tool;
    warning.payload = nlohmann::json{{"code", code},
                                      {"execution_id", event.execution_id},
                                      {"tool_name", event.tool_name}};
    return warning;
}

agent::ToolExecutionLedger ToolTraceHub::BuildLedger(const std::vector<agent::ToolTraceEvent>& events) {
    agent::ToolExecutionLedger ledger;
    for (const auto& event : events) {
        ledger.Fold(event);
    }
    return ledger;
}

std::string ToolTraceHub::LastBatchSummary() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_batch_id_.empty()) {
        return std::string();
    }
    agent::ToolExecutionLedger ledger;
    for (const auto& event : recent_) {
        ledger.Fold(event);
    }
    const auto batch = ledger.Batch(last_batch_id_);
    if (batch.empty()) {
        return std::string();
    }
    const agent::ToolExecutionRecord* first_failure = ledger.FirstExplicitFailure(last_batch_id_);
    std::ostringstream out;
    out << "batch " << last_batch_id_;
    if (!batch.empty() && !batch.front()->turn_id.empty()) {
        out << " / turn " << batch.front()->turn_id;
    }
    out << "\n";
    for (const agent::ToolExecutionRecord* record : batch) {
        out << agent::FormatExecutionSummaryLine(*record, record == first_failure) << "\n";
    }
    const auto window = ledger.ComputeSuspectWindow();
    if (window.valid) {
        out << "suspect_window: last_verified_good=" << window.last_verified_good
            << " first_observed_bad=" << window.first_observed_bad << " executions=" << window.window.size() << "\n";
    }
    return out.str();
}

std::optional<agent::ToolUndoToken> ToolTraceHub::FindUndoToken(const std::string& execution_id) const {
    // 进程内账:折叠 recent_ 查。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        agent::ToolExecutionLedger ledger;
        for (const auto& event : recent_) {
            ledger.Fold(event);
        }
        if (const auto* record = ledger.FindByExecution(execution_id); record != nullptr && !record->undo.path.empty()) {
            return record->undo;
        }
    }
    // 存档真本:重启后 recent_ 空,折叠 JSONL(调用方保证 store 活着)。
    if (store_ != nullptr && store_->active()) {
        const auto bytes = agent::ReadSessionFileBytes(store_->file_path());
        if (bytes.has_value()) {
            const auto loaded = agent::ParseSessionFile(*bytes);
            if (loaded.has_value()) {
                const auto ledger = BuildLedger(loaded->tool_trace_events);
                if (const auto* record = ledger.FindByExecution(execution_id);
                    record != nullptr && !record->undo.path.empty()) {
                    return record->undo;
                }
            }
        }
    }
    return std::nullopt;
}

std::string ToolTraceHub::current_agent_execution() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_agent_execution_;
}

void ToolTraceHub::set_current_agent_execution(std::string execution_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_agent_execution_ = std::move(execution_id);
}

std::vector<agent::ToolTraceEvent> ToolTraceHub::FinishedEventsOfTurn(const std::string& turn_id) const {
    std::vector<agent::ToolTraceEvent> out;
    if (turn_id.empty()) {
        return out;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& event : recent_) {
        if (event.turn_id == turn_id &&
            event.kind == agent::ToolTraceEventKind::ExecutionFinished) {
            out.push_back(event);
        }
    }
    return out;
}

std::string ToolTraceHub::OwnerOfExecution(const std::string& execution_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    agent::ToolExecutionLedger ledger;
    for (const auto& event : recent_) {
        ledger.Fold(event);
    }
    if (const auto* record = ledger.FindByExecution(execution_id); record != nullptr) {
        return record->execution_id;
    }
    return execution_id;
}

std::vector<std::string> ToolTraceHub::ErrorLines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    agent::ToolExecutionLedger ledger;
    for (const auto& event : recent_) {
        ledger.Fold(event);
    }
    std::vector<std::string> lines;
    for (const auto& record : ledger.executions()) {
        if (record.outcome == agent::ToolOutcome::Succeeded ||
            record.outcome == agent::ToolOutcome::CancelledBeforeStart) {
            continue;
        }
        std::ostringstream line;
        line << agent::FormatExecutionSummaryLine(record, false);
        if (!record.error_code.empty()) {
            line << " [" << record.error_code << "]";
        }
        lines.push_back(line.str());
    }
    return lines;
}

}  // namespace lubancode::runtime
