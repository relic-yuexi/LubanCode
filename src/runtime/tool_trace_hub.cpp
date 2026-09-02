// ToolTraceHub 的实现(逐枚追踪单)。

#include "runtime/tool_trace_hub.hpp"

#include "agent/agent.hpp"  // Agent:Install 要碰成员(批四自立门户后 loop.hpp 只剩前向声明)

#include <algorithm>
#include <sstream>
#include <utility>

#include "platform/wall_clock.hpp"  // 统一墙钟(批五):trace ts 的钟同源五套台账

namespace lubancode::runtime {

ToolTraceHub::ToolTraceHub(IdAuthority& ids) : ToolTraceHub(ids, Options{}) {}

ToolTraceHub::ToolTraceHub(IdAuthority& ids, const Options& options)
    : ids_(ids), options_(options) {}

ToolTraceHub::~ToolTraceHub() = default;

std::string ToolTraceHub::NextExecutionId() {
    return ids_.NextItemId();
}

void ToolTraceHub::Install(agent::Agent& loop, agent::TurnWiring& wiring, const std::string& thread_id,
                           const std::string& turn_id) {
    // execution 发号:与 Runtime item id 同源(单子:不可再造第二只计数器)。
    // 批四·病十二:发号口是接线,进 AgentWiring——其余接线(inbox/压力钩)
    // 是装配层先灌好的,这里照原样带上,只换发号这一路。
    agent::AgentWiring agent_wiring = loop.wiring();
    agent_wiring.execution_id_issuer = [this] { return NextExecutionId(); };
    loop.SetWiring(std::move(agent_wiring));
    thread_id_ = thread_id;
    turn_id_ = turn_id;
    wiring.on_tool_trace = [this](const agent::ToolTraceEvent& event) { OnTrace(event); };
    // 拦截查询口:RunOneTool 在 emit(execution_started) 之后、execute
    // 之前同步问一句。hub 见过"started 写盘失败 + 副作用档"时答 false,
    // RunOneTool 立即以 result_store_failed 收尾,不 execute(单子:
    // 副作用工具不得继续执行)。
    wiring.on_tool_trace_blocked = [this](const std::string& execution_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return blocked_executions_.count(execution_id) != 0;
    };
    // P0-6:旧 SessionStore 的轮内消息 append 关口已删——消息事实由
    // model.output.completed/tool.result.committed typed 事件承载,轨迹桥
    // 从批次尾口拿正文。on_assistant_message_ready 因此只剩占位(接口
    // 保留,AgentLoop 的挂点不动)。
    wiring.on_assistant_message_ready = [](const api::Message&) {};
    wiring.on_tool_results_committed = [this](const std::string& batch_id, const api::Message& message) {
        // 正文进 tool.result.committed 事件(轨迹桥落账;没挂轨迹的会话
        // 只有进程内 recent_ 诊断账)。
        if (trajectory_ != nullptr) {
            trajectory_->OnToolResultsCommitted(batch_id, message);
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
                current_agent_call_id_ = event.tool_use_id;
            } else if (event.kind == agent::ToolTraceEventKind::ExecutionFinished) {
                current_agent_execution_.clear();
                current_agent_call_id_.clear();
            }
        }
    }

    // 1) 持久账(P0-6 后唯一路):轨迹桥同步落账。started 提交完再问一次
    //    ShouldBlockExecution——桥的拦截集合在提交时才填得上,先问后提交
    //    永远问着空集(P0-4 修的次序缺陷)。拦下且副作用档,同一道闸拦
    //    执行:把 started 翻成 finished(result_store_failed),模型看到的
    //    就是"没跑成"。
    if (trajectory_ != nullptr) {
        trajectory_->OnToolTrace(event);
        if (event.kind == agent::ToolTraceEventKind::ExecutionStarted &&
            trajectory_->ShouldBlockExecution(event) && ShouldBlockOnFailedStart(event.effect_class)) {
            agent::ToolTraceEvent failed = event;
            failed.kind = agent::ToolTraceEventKind::ExecutionFinished;
            failed.outcome = agent::ToolOutcome::ResultStoreFailed;
            failed.error_code = agent::kErrSessionTraceAppendFailed;
            failed.fallback_message = "轨迹账写盘失败或磁盘 reserve 不足,该工具未执行(副作用档默认拦截)";
            {
                std::lock_guard<std::mutex> lock(mutex_);
                blocked_executions_.insert(event.execution_id);
                recent_.push_back(failed);
            }
            trajectory_->OnToolTrace(failed);  // 轨迹侧翻 cancelled 落账
            EmitRuntimeEvent(failed);
            return;  // 终态已出,不再往下分线
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

agent::ToolExecutionLedger ToolTraceHub::BuildRecentLedger() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return BuildLedger(recent_);
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
    // 进程内账:折叠 recent_ 查。(P0-6:旧存档真本回落已删——撤销令牌
    // 的持久真账在 trajectory Journal,跨进程查令牌走事件账侧。)
    std::lock_guard<std::mutex> lock(mutex_);
    agent::ToolExecutionLedger ledger;
    for (const auto& event : recent_) {
        ledger.Fold(event);
    }
    if (const auto* record = ledger.FindByExecution(execution_id); record != nullptr && !record->undo.path.empty()) {
        return record->undo;
    }
    return std::nullopt;
}

bool ToolTraceHub::IsExecutionBlocked(const std::string& execution_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocked_executions_.count(execution_id) != 0;
}

std::string ToolTraceHub::current_agent_execution() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_agent_execution_;
}

std::string ToolTraceHub::current_agent_call_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_agent_call_id_;
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
