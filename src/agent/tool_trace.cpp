// tool_trace.hpp 的实现。纯函数:序列化/折叠/分类/归因/摘要。

#include "agent/tool_trace.hpp"

#include <algorithm>
#include <sstream>

#include "skills/workflow_recorder.hpp"  // skills::RedactSecrets:预览打码(纯函数,不带磁盘件)
#include "hooks/hash.hpp"               // Sha256Hex:结果/入参摘要锚
#include "platform/json_safe.hpp"       // DumpJsonSanitized:追踪 JSONL 的末道编码闸
#include "platform/text_encoding.hpp"   // UTF-8 安全截头尾

namespace lubancode::agent {

// ---------------------------------------------------------------------------
// 枚举 <-> 稳定字符串
// ---------------------------------------------------------------------------

std::string ToString(ToolSourceKind kind) {
    switch (kind) {
        case ToolSourceKind::Builtin: return "builtin";
        case ToolSourceKind::Mcp: return "mcp";
        case ToolSourceKind::Lsp: return "lsp";
        case ToolSourceKind::PluginLua: return "plugin-lua";
        case ToolSourceKind::PluginNative: return "plugin-native";
        case ToolSourceKind::Agent: return "agent";
        case ToolSourceKind::Ptc: return "ptc";
        case ToolSourceKind::Deferred: return "deferred";
    }
    return "builtin";
}

bool ParseToolSourceKind(const std::string& s, ToolSourceKind& out) {
    if (s == "builtin") { out = ToolSourceKind::Builtin; return true; }
    if (s == "mcp") { out = ToolSourceKind::Mcp; return true; }
    if (s == "lsp") { out = ToolSourceKind::Lsp; return true; }
    if (s == "plugin-lua") { out = ToolSourceKind::PluginLua; return true; }
    if (s == "plugin-native") { out = ToolSourceKind::PluginNative; return true; }
    if (s == "agent") { out = ToolSourceKind::Agent; return true; }
    if (s == "ptc") { out = ToolSourceKind::Ptc; return true; }
    if (s == "deferred") { out = ToolSourceKind::Deferred; return true; }
    return false;
}

std::string ToString(EffectClass cls) {
    switch (cls) {
        case EffectClass::ReadOnlyLocal: return "read_only_local";
        case EffectClass::ReadOnlyRemote: return "read_only_remote";
        case EffectClass::LocalReversible: return "local_reversible";
        case EffectClass::LocalProcessUnknown: return "local_process_unknown";
        case EffectClass::RemoteIdempotent: return "remote_idempotent";
        case EffectClass::RemoteCompensatable: return "remote_compensatable";
        case EffectClass::RemoteIrreversible: return "remote_irreversible";
        case EffectClass::InProcessUnknown: return "in_process_unknown";
    }
    return "in_process_unknown";
}

bool ParseEffectClass(const std::string& s, EffectClass& out) {
    if (s == "read_only_local") { out = EffectClass::ReadOnlyLocal; return true; }
    if (s == "read_only_remote") { out = EffectClass::ReadOnlyRemote; return true; }
    if (s == "local_reversible") { out = EffectClass::LocalReversible; return true; }
    if (s == "local_process_unknown") { out = EffectClass::LocalProcessUnknown; return true; }
    if (s == "remote_idempotent") { out = EffectClass::RemoteIdempotent; return true; }
    if (s == "remote_compensatable") { out = EffectClass::RemoteCompensatable; return true; }
    if (s == "remote_irreversible") { out = EffectClass::RemoteIrreversible; return true; }
    if (s == "in_process_unknown") { out = EffectClass::InProcessUnknown; return true; }
    return false;
}

std::string ToString(ToolOutcome outcome) {
    switch (outcome) {
        case ToolOutcome::Succeeded: return "succeeded";
        case ToolOutcome::ToolError: return "tool_error";
        case ToolOutcome::UnknownTool: return "unknown_tool";
        case ToolOutcome::Unavailable: return "unavailable";
        case ToolOutcome::SchemaRejected: return "schema_rejected";
        case ToolOutcome::HookDenied: return "hook_denied";
        case ToolOutcome::PermissionDeclined: return "permission_declined";
        case ToolOutcome::ModeDenied: return "mode_denied";
        case ToolOutcome::ScopeGatePending: return "scope_gate_pending";
        case ToolOutcome::CancelledBeforeStart: return "cancelled_before_start";
        case ToolOutcome::CancelledDuringRun: return "cancelled_during_run";
        case ToolOutcome::SpawnFailed: return "spawn_failed";
        case ToolOutcome::TimedOut: return "timed_out";
        case ToolOutcome::OutputLimit: return "output_limit";
        case ToolOutcome::TransportError: return "transport_error";
        case ToolOutcome::ProtocolError: return "protocol_error";
        case ToolOutcome::ProcessExitNonzero: return "process_exit_nonzero";
        case ToolOutcome::PluginException: return "plugin_exception";
        case ToolOutcome::HostError: return "host_error";
        case ToolOutcome::UnknownAfterStart: return "unknown_after_start";
        case ToolOutcome::ResultStoreFailed: return "result_store_failed";
    }
    return "tool_error";
}

bool ParseToolOutcome(const std::string& s, ToolOutcome& out) {
    static const std::pair<const char*, ToolOutcome> kTable[] = {
        {"succeeded", ToolOutcome::Succeeded},
        {"tool_error", ToolOutcome::ToolError},
        {"unknown_tool", ToolOutcome::UnknownTool},
        {"unavailable", ToolOutcome::Unavailable},
        {"schema_rejected", ToolOutcome::SchemaRejected},
        {"hook_denied", ToolOutcome::HookDenied},
        {"permission_declined", ToolOutcome::PermissionDeclined},
        {"mode_denied", ToolOutcome::ModeDenied},
        {"scope_gate_pending", ToolOutcome::ScopeGatePending},
        {"cancelled_before_start", ToolOutcome::CancelledBeforeStart},
        {"cancelled_during_run", ToolOutcome::CancelledDuringRun},
        {"spawn_failed", ToolOutcome::SpawnFailed},
        {"timed_out", ToolOutcome::TimedOut},
        {"output_limit", ToolOutcome::OutputLimit},
        {"transport_error", ToolOutcome::TransportError},
        {"protocol_error", ToolOutcome::ProtocolError},
        {"process_exit_nonzero", ToolOutcome::ProcessExitNonzero},
        {"plugin_exception", ToolOutcome::PluginException},
        {"host_error", ToolOutcome::HostError},
        {"unknown_after_start", ToolOutcome::UnknownAfterStart},
        {"result_store_failed", ToolOutcome::ResultStoreFailed},
    };
    for (const auto& [name, value] : kTable) {
        if (s == name) {
            out = value;
            return true;
        }
    }
    return false;
}

bool OutcomeNeverStarted(ToolOutcome outcome) {
    switch (outcome) {
        case ToolOutcome::HookDenied:
        case ToolOutcome::PermissionDeclined:
        case ToolOutcome::ModeDenied:
        case ToolOutcome::ScopeGatePending:
        case ToolOutcome::CancelledBeforeStart:
        case ToolOutcome::UnknownTool:
        case ToolOutcome::Unavailable:
        case ToolOutcome::SchemaRejected:
            return true;
        default:
            return false;
    }
}

std::string ToString(ToolTraceEventKind kind) {
    switch (kind) {
        case ToolTraceEventKind::Scheduled: return "scheduled";
        case ToolTraceEventKind::ExecutionStarted: return "execution_started";
        case ToolTraceEventKind::ExecutionFinished: return "execution_finished";
        case ToolTraceEventKind::ResultCommitted: return "result_committed";
        case ToolTraceEventKind::Verification: return "verification";
        case ToolTraceEventKind::RecoveryMarker: return "recovery_marker";
        case ToolTraceEventKind::McpLateResponse: return "mcp_late_response_dropped";
    }
    return "scheduled";
}

bool ParseToolTraceEventKind(const std::string& s, ToolTraceEventKind& out) {
    if (s == "scheduled") { out = ToolTraceEventKind::Scheduled; return true; }
    if (s == "execution_started") { out = ToolTraceEventKind::ExecutionStarted; return true; }
    if (s == "execution_finished") { out = ToolTraceEventKind::ExecutionFinished; return true; }
    if (s == "result_committed") { out = ToolTraceEventKind::ResultCommitted; return true; }
    if (s == "verification") { out = ToolTraceEventKind::Verification; return true; }
    if (s == "recovery_marker") { out = ToolTraceEventKind::RecoveryMarker; return true; }
    // 迟到响应的旧写法(late_response_dropped)也认,读老档不坏。
    if (s == "mcp_late_response_dropped" || s == "late_response_dropped") {
        out = ToolTraceEventKind::McpLateResponse;
        return true;
    }
    return false;
}

std::string ToString(ToolResultRef::Kind kind) {
    switch (kind) {
        case ToolResultRef::Kind::Inline: return "inline";
        case ToolResultRef::Kind::Artifact: return "artifact";
        case ToolResultRef::Kind::Unavailable: return "unavailable";
    }
    return "unavailable";
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

namespace {

// 值不存在/类型不对时不写字段:老版本读新行当坏行跳过,新版本读老行
// 字段缺失按默认收——两头都不硬崩。
void PutStr(nlohmann::json& j, const char* key, const std::string& value) {
    if (!value.empty()) {
        j[key] = value;
    }
}

std::string GetStr(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it != j.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return std::string();
}

}  // namespace

std::string SerializeToolTraceEvent(const ToolTraceEvent& event, const std::string& ts) {
    nlohmann::json j;
    j["type"] = "tool_trace_v1";
    j["event"] = ToString(event.kind);
    PutStr(j, "session", event.thread_id);
    PutStr(j, "turn", event.turn_id);
    PutStr(j, "provider_request", event.provider_request_id);
    PutStr(j, "batch", event.batch_id);
    if (event.sequence_in_batch >= 0) {
        j["sequence_in_batch"] = event.sequence_in_batch;
    }
    j["execution_id"] = event.execution_id;
    PutStr(j, "item_id", event.item_id);
    PutStr(j, "tool_use_id", event.tool_use_id);
    j["tool_name"] = event.tool_name;
    PutStr(j, "source", ToString(event.source_kind));
    PutStr(j, "source_instance", event.source_instance);
    PutStr(j, "parent_execution_id", event.parent_execution_id);
    PutStr(j, "retry_of", event.retry_of);
    PutStr(j, "blocked_by", event.blocked_by);
    PutStr(j, "compensates", event.compensates);
    if (event.seq != 0) {
        j["seq"] = event.seq;
    }
    if (event.timestamp_ms != 0) {
        j["timestamp_ms"] = event.timestamp_ms;
    }

    switch (event.kind) {
        case ToolTraceEventKind::Scheduled:
            break;
        case ToolTraceEventKind::ExecutionStarted:
            PutStr(j, "effective_input_sha256", event.effective_input_sha256);
            PutStr(j, "effect_class", ToString(event.effect_class));
            if (event.effective_arguments.is_object()) {
                j["effective_arguments"] = event.effective_arguments;
            }
            break;
        case ToolTraceEventKind::ExecutionFinished: {
            j["outcome"] = ToString(event.outcome);
            PutStr(j, "error_code", event.error_code);
            PutStr(j, "fallback_message", event.fallback_message);
            if (!event.details.is_null() && !event.details.empty()) {
                j["details"] = event.details;
            }
            if (event.duration_ms > 0) {
                j["duration_ms"] = event.duration_ms;
            }
            // MCP 内层账(单子"外层 execution 要挂内层"):jsonrpc id 与
            // transport generation 随 finished 落盘,迟到响应可关联。
            if (event.jsonrpc_request_id >= 0) {
                j["jsonrpc_request_id"] = event.jsonrpc_request_id;
            }
            if (const auto it = event.details.find("transport_generation");
                it != event.details.end() && it->is_number_unsigned()) {
                j["transport_generation"] = it->get<std::uint64_t>();
            }
            nlohmann::json ref;
            ref["kind"] = ToString(event.result_ref.kind);
            PutStr(ref, "sha256", event.result_ref.sha256);
            if (event.result_ref.bytes > 0) {
                ref["bytes"] = event.result_ref.bytes;
            }
            PutStr(ref, "artifact_id", event.result_ref.artifact_id);
            PutStr(ref, "preview", event.result_ref.preview);
            if (event.result_ref.kind == ToolResultRef::Kind::Inline) {
                ref["content"] = event.result_ref.content;
            }
            j["result_ref"] = ref;
            if (!event.undo.path.empty()) {
                nlohmann::json undo;
                undo["path"] = event.undo.path;
                PutStr(undo, "preimage_sha256", event.undo.preimage_sha256);
                PutStr(undo, "postimage_sha256", event.undo.postimage_sha256);
                if (event.undo.created_new_file) {
                    undo["created_new_file"] = true;
                }
                // preimage 正文只在可用时带(超上限不内联,undo 如实标不可用,
                // 不拿半截原文冒充可恢复)。
                if (event.undo.available()) {
                    undo["preimage"] = event.undo.preimage;
                }
                j["undo"] = undo;
            }
            break;
        }
        case ToolTraceEventKind::ResultCommitted:
            break;
        case ToolTraceEventKind::Verification:
            PutStr(j, "label", event.label);
            j["passed"] = event.passed;
            PutStr(j, "after_execution_id", event.after_execution_id);
            PutStr(j, "detail", event.verify_detail);
            break;
        case ToolTraceEventKind::RecoveryMarker:
            PutStr(j, "note", event.note);
            break;
        case ToolTraceEventKind::McpLateResponse:
            if (event.jsonrpc_request_id >= 0) {
                j["jsonrpc_request_id"] = event.jsonrpc_request_id;
            }
            PutStr(j, "detail", event.note);
            break;
    }
    if (!ts.empty()) {
        j["ts"] = ts;
    }
    return platform::DumpJsonSanitized(j);
}

std::optional<ToolTraceEvent> ParseToolTraceEvent(const std::string& line) {
    const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        return std::nullopt;
    }
    const auto type_it = j.find("type");
    if (type_it == j.end() || !type_it->is_string() || type_it->get<std::string>() != "tool_trace_v1") {
        return std::nullopt;
    }
    const auto event_it = j.find("event");
    if (event_it == j.end() || !event_it->is_string()) {
        return std::nullopt;
    }
    ToolTraceEvent event;
    if (!ParseToolTraceEventKind(event_it->get<std::string>(), event.kind)) {
        return std::nullopt;
    }
    event.thread_id = GetStr(j, "session");
    event.turn_id = GetStr(j, "turn");
    event.provider_request_id = GetStr(j, "provider_request");
    event.batch_id = GetStr(j, "batch");
    if (const auto it = j.find("sequence_in_batch"); it != j.end() && it->is_number_integer()) {
        event.sequence_in_batch = it->get<int>();
    }
    event.execution_id = GetStr(j, "execution_id");
    if (event.execution_id.empty()) {
        return std::nullopt;  // 审计主键缺了,这行不可信
    }
    event.item_id = GetStr(j, "item_id");
    event.tool_use_id = GetStr(j, "tool_use_id");
    event.tool_name = GetStr(j, "tool_name");
    if (const std::string source = GetStr(j, "source"); !source.empty()) {
        if (!ParseToolSourceKind(source, event.source_kind)) {
            event.source_kind = ToolSourceKind::Builtin;
        }
    }
    event.source_instance = GetStr(j, "source_instance");
    event.parent_execution_id = GetStr(j, "parent_execution_id");
    event.retry_of = GetStr(j, "retry_of");
    event.blocked_by = GetStr(j, "blocked_by");
    event.compensates = GetStr(j, "compensates");
    if (const auto it = j.find("seq"); it != j.end() && it->is_number_unsigned()) {
        event.seq = it->get<std::uint64_t>();
    }
    if (const auto it = j.find("timestamp_ms"); it != j.end() && it->is_number_integer()) {
        event.timestamp_ms = it->get<std::int64_t>();
    }

    switch (event.kind) {
        case ToolTraceEventKind::Scheduled:
            break;
        case ToolTraceEventKind::ExecutionStarted:
            event.effective_input_sha256 = GetStr(j, "effective_input_sha256");
            if (const std::string cls = GetStr(j, "effect_class"); !cls.empty()) {
                if (!ParseEffectClass(cls, event.effect_class)) {
                    event.effect_class = EffectClass::InProcessUnknown;
                }
            }
            if (const auto it = j.find("effective_arguments"); it != j.end() && it->is_object()) {
                event.effective_arguments = *it;
            }
            break;
        case ToolTraceEventKind::ExecutionFinished: {
            const std::string outcome = GetStr(j, "outcome");
            if (!outcome.empty() && !ParseToolOutcome(outcome, event.outcome)) {
                event.outcome = ToolOutcome::ToolError;
            }
            event.error_code = GetStr(j, "error_code");
            event.fallback_message = GetStr(j, "fallback_message");
            if (const auto it = j.find("details"); it != j.end() && it->is_object()) {
                event.details = *it;
            }
            if (const auto it = j.find("duration_ms"); it != j.end() && it->is_number_integer()) {
                event.duration_ms = it->get<std::int64_t>();
            }
            if (const auto it = j.find("jsonrpc_request_id"); it != j.end() && it->is_number_integer()) {
                event.jsonrpc_request_id = it->get<std::int64_t>();
            }
            if (const auto it = j.find("result_ref"); it != j.end() && it->is_object()) {
                const nlohmann::json& ref = *it;
                const std::string kind = GetStr(ref, "kind");
                if (kind == "inline") {
                    event.result_ref.kind = ToolResultRef::Kind::Inline;
                } else if (kind == "artifact") {
                    event.result_ref.kind = ToolResultRef::Kind::Artifact;
                } else {
                    event.result_ref.kind = ToolResultRef::Kind::Unavailable;
                }
                event.result_ref.sha256 = GetStr(ref, "sha256");
                if (const auto b = ref.find("bytes"); b != ref.end() && b->is_number_unsigned()) {
                    event.result_ref.bytes = b->get<std::uint64_t>();
                }
                event.result_ref.artifact_id = GetStr(ref, "artifact_id");
                event.result_ref.preview = GetStr(ref, "preview");
                if (event.result_ref.kind == ToolResultRef::Kind::Inline) {
                    event.result_ref.content = GetStr(ref, "content");
                }
            }
            if (const auto it = j.find("undo"); it != j.end() && it->is_object()) {
                const nlohmann::json& undo = *it;
                event.undo.path = GetStr(undo, "path");
                event.undo.preimage_sha256 = GetStr(undo, "preimage_sha256");
                event.undo.postimage_sha256 = GetStr(undo, "postimage_sha256");
                event.undo.created_new_file =
                    undo.contains("created_new_file") && undo["created_new_file"].is_boolean() &&
                    undo["created_new_file"].get<bool>();
                event.undo.preimage = GetStr(undo, "preimage");
            }
            break;
        }
        case ToolTraceEventKind::ResultCommitted:
            break;
        case ToolTraceEventKind::Verification:
            event.label = GetStr(j, "label");
            event.passed = j.contains("passed") && j["passed"].is_boolean() && j["passed"].get<bool>();
            event.after_execution_id = GetStr(j, "after_execution_id");
            event.verify_detail = GetStr(j, "detail");
            break;
        case ToolTraceEventKind::RecoveryMarker:
            event.note = GetStr(j, "note");
            break;
        case ToolTraceEventKind::McpLateResponse:
            if (const auto it = j.find("jsonrpc_request_id"); it != j.end() && it->is_number_integer()) {
                event.jsonrpc_request_id = it->get<std::int64_t>();
            }
            event.note = GetStr(j, "detail");
            break;
    }
    return event;
}

std::string BuildTracePreview(const std::string& content, std::size_t head, std::size_t tail) {
    if (content.size() <= head + tail) {
        return skills::RedactSecrets(content);
    }
    const std::size_t head_end = platform::Utf8PrefixBoundary(content, head);
    const std::size_t tail_begin = platform::Utf8SuffixBoundary(content, content.size() - tail);
    const std::string head_part = content.substr(0, head_end);
    const std::string tail_part = content.substr(tail_begin);
    return skills::RedactSecrets(head_part) + "…" + skills::RedactSecrets(tail_part);
}

// ---------------------------------------------------------------------------
// 折叠
// ---------------------------------------------------------------------------

RecoveryClass ToolExecutionRecord::Classify() const {
    // 账坏不折结论(不挑一枚冒充真相);调用方拿 corrupt 单独处置。
    if (corrupt) {
        return RecoveryClass::UnknownAfterStart;  // 最保守:未知,不自动重跑
    }
    // 闸前终态(hook 拒/权限拒/取消/未挂载):没越过执行边界,算"未执行
    // 的 finished"(单子状态机最后一行)。
    if (has_finished && OutcomeNeverStarted(outcome)) {
        return RecoveryClass::Finished;
    }
    if (has_committed) {
        return RecoveryClass::Finished;
    }
    if (has_finished) {
        return RecoveryClass::ResultRecoverable;
    }
    if (has_started) {
        return RecoveryClass::UnknownAfterStart;
    }
    return RecoveryClass::NotStarted;
}

std::string ToString(RecoveryClass cls) {
    switch (cls) {
        case RecoveryClass::NotStarted: return "not_started";
        case RecoveryClass::Finished: return "finished";
        case RecoveryClass::ResultRecoverable: return "result_recoverable";
        case RecoveryClass::UnknownAfterStart: return "unknown_after_start";
    }
    return "unknown_after_start";
}

ToolExecutionRecord& ToolExecutionLedger::Ensure(const ToolTraceEvent& event) {
    if (const auto it = by_execution_.find(event.execution_id); it != by_execution_.end()) {
        return executions_[it->second];
    }
    ToolExecutionRecord record;
    record.execution_id = event.execution_id;
    record.tool_use_id = event.tool_use_id;
    record.tool_name = event.tool_name;
    record.batch_id = event.batch_id;
    record.turn_id = event.turn_id;
    record.parent_execution_id = event.parent_execution_id;
    record.retry_of = event.retry_of;
    record.blocked_by = event.blocked_by;
    record.compensates = event.compensates;
    record.sequence_in_batch = event.sequence_in_batch;
    record.source_kind = event.source_kind;
    record.source_instance = event.source_instance;
    executions_.push_back(std::move(record));
    const std::size_t index = executions_.size() - 1;
    by_execution_.emplace(event.execution_id, index);
    if (!event.tool_use_id.empty()) {
        by_tool_use_[event.tool_use_id].push_back(index);
    }
    return executions_[index];
}

void ToolExecutionLedger::Fold(const ToolTraceEvent& event) {
    ToolExecutionRecord& record = Ensure(event);
    // 身份字段以首见为准(scheduled 先落,后续栅栏只补缺),不追改旧账。
    if (record.tool_use_id.empty()) {
        record.tool_use_id = event.tool_use_id;
        if (!event.tool_use_id.empty()) {
            by_tool_use_[event.tool_use_id].push_back(by_execution_[event.execution_id]);
        }
    }
    if (record.tool_name.empty()) {
        record.tool_name = event.tool_name;
    }
    if (record.batch_id.empty()) {
        record.batch_id = event.batch_id;
    }
    if (record.turn_id.empty()) {
        record.turn_id = event.turn_id;
    }
    if (record.sequence_in_batch < 0) {
        record.sequence_in_batch = event.sequence_in_batch;
    }
    if (record.source_instance.empty()) {
        record.source_instance = event.source_instance;
    }

    switch (event.kind) {
        case ToolTraceEventKind::Scheduled:
            if (!record.has_scheduled) {
                record.has_scheduled = true;
                record.seq_scheduled = event.seq;
            }
            break;
        case ToolTraceEventKind::ExecutionStarted:
            if (!record.has_started) {
                record.has_started = true;
                record.effective_input_sha256 = event.effective_input_sha256;
                record.effect_class = event.effect_class;
            } else if (record.effective_input_sha256 != event.effective_input_sha256) {
                record.corrupt = true;
                record.corrupt_reason = "conflicting execution_started";
                record.conflict_seqs.push_back(event.seq);
            }
            break;
        case ToolTraceEventKind::ExecutionFinished: {
            if (!record.has_finished) {
                record.has_finished = true;
                record.outcome = event.outcome;
                record.error_code = event.error_code;
                record.fallback_message = event.fallback_message;
                record.details = event.details;
                record.duration_ms = event.duration_ms;
                record.result_ref = event.result_ref;
                record.undo = event.undo;
                break;
            }
            // 同 execution 第二枚 finished:相同 outcome + 相同结果摘要 =
            // 幂等重放(取第一枚);不同 = 冲突,标 corrupt 保留两枚 seq。
            const bool same_outcome = record.outcome == event.outcome;
            const bool same_result = record.result_ref.sha256 == event.result_ref.sha256;
            if (!same_outcome || !same_result) {
                record.corrupt = true;
                record.corrupt_reason = "conflicting execution_finished";
                record.conflict_seqs.push_back(event.seq);
            }
            break;
        }
        case ToolTraceEventKind::ResultCommitted:
            if (!record.has_committed) {
                record.has_committed = true;
            }
            break;
        case ToolTraceEventKind::Verification:
            verifications_.push_back(TraceVerification{event.label, event.passed, event.after_execution_id,
                                                        event.verify_detail, event.seq});
            break;
        case ToolTraceEventKind::RecoveryMarker:
        case ToolTraceEventKind::McpLateResponse:
            break;  // 只留账,不折结论
    }
}

const ToolExecutionRecord* ToolExecutionLedger::FindByExecution(const std::string& execution_id) const {
    const auto it = by_execution_.find(execution_id);
    return it == by_execution_.end() ? nullptr : &executions_[it->second];
}

std::vector<const ToolExecutionRecord*> ToolExecutionLedger::FindByToolUse(const std::string& tool_use_id) const {
    std::vector<const ToolExecutionRecord*> out;
    const auto it = by_tool_use_.find(tool_use_id);
    if (it == by_tool_use_.end()) {
        return out;
    }
    out.reserve(it->second.size());
    for (const std::size_t index : it->second) {
        out.push_back(&executions_[index]);
    }
    return out;
}

std::vector<const ToolExecutionRecord*> ToolExecutionLedger::Batch(const std::string& batch_id) const {
    std::vector<const ToolExecutionRecord*> out;
    for (const auto& record : executions_) {
        if (record.batch_id == batch_id) {
            out.push_back(&record);
        }
    }
    std::sort(out.begin(), out.end(), [](const ToolExecutionRecord* a, const ToolExecutionRecord* b) {
        if (a->sequence_in_batch != b->sequence_in_batch) {
            return a->sequence_in_batch < b->sequence_in_batch;
        }
        return a->seq_scheduled < b->seq_scheduled;
    });
    return out;
}

std::size_t ToolExecutionLedger::corrupt_count() const {
    std::size_t count = 0;
    for (const auto& record : executions_) {
        if (record.corrupt) {
            ++count;
        }
    }
    return count;
}

const ToolExecutionRecord* ToolExecutionLedger::FirstExplicitFailure(const std::string& batch_id) const {
    for (const ToolExecutionRecord* record : Batch(batch_id)) {
        if (record->outcome == ToolOutcome::Succeeded) {
            continue;
        }
        // cancelled_before_start 是被收掉的尾巴,不是"明确失败";unknown
        // 是宿主不知道,也不是模型/工具的明确表态。
        if (record->outcome == ToolOutcome::CancelledBeforeStart ||
            record->outcome == ToolOutcome::UnknownAfterStart) {
            continue;
        }
        return record;
    }
    return nullptr;
}

ToolExecutionLedger::SuspectWindow ToolExecutionLedger::ComputeSuspectWindow() const {
    SuspectWindow window;
    if (verifications_.empty()) {
        return window;  // 没有验证点证据,不编确定答案
    }
    // 验证点按 seq 排(折叠时就是文件序,再保险排一次)。
    std::vector<const TraceVerification*> verifs;
    verifs.reserve(verifications_.size());
    for (const auto& v : verifications_) {
        verifs.push_back(&v);
    }
    std::sort(verifs.begin(), verifs.end(), [](const TraceVerification* a, const TraceVerification* b) {
        return a->seq < b->seq;
    });

    // 最后通过的验证点 → 首个失败验证点:中间的 execution 就是可疑窗口。
    const TraceVerification* last_good = nullptr;
    const TraceVerification* first_bad = nullptr;
    for (const TraceVerification* v : verifs) {
        if (v->passed) {
            if (first_bad == nullptr) {
                last_good = v;
            }
        } else if (first_bad == nullptr) {
            first_bad = v;
        }
    }
    if (last_good == nullptr && first_bad == nullptr) {
        return window;
    }
    window.valid = true;
    if (last_good != nullptr) {
        window.last_verified_good = last_good->after_execution_id;
    }
    if (first_bad != nullptr) {
        window.first_observed_bad = first_bad->after_execution_id;
    }
    // 窗口 = (last_good 的 execution, first_bad 的 execution] 之间的全部
    // execution。两侧都拿不到时给空窗口(valid 仍置,如实说"证据不足")。
    for (const auto& record : executions_) {
        const bool after_good =
            window.last_verified_good.empty() ||
            (record.seq_scheduled != 0 &&
             [&]() {
                 const ToolExecutionRecord* good = FindByExecution(window.last_verified_good);
                 return good != nullptr && record.seq_scheduled > good->seq_scheduled;
             }());
        // 窗口含 first_bad 那枚(单子:suspect_window = [c, d]——首个坏
        // 验证点紧跟的执行也在窗内,它是最后一步可疑动作)。
        const bool before_bad =
            window.first_observed_bad.empty() ||
            (record.seq_scheduled != 0 &&
             [&]() {
                 const ToolExecutionRecord* bad = FindByExecution(window.first_observed_bad);
                 return bad != nullptr && record.seq_scheduled <= bad->seq_scheduled;
             }());
        if (after_good && before_bad) {
            window.window.push_back(&record);
        }
    }
    std::sort(window.window.begin(), window.window.end(),
              [](const ToolExecutionRecord* a, const ToolExecutionRecord* b) {
                  return a->seq_scheduled < b->seq_scheduled;
              });
    return window;
}

// ---------------------------------------------------------------------------
// 修补与摘要
// ---------------------------------------------------------------------------

std::string BuildRecoveredResultText(const ToolExecutionRecord& record) {
    std::string text = "[会话恢复] ";
    // 结论先行:恢复侧的判断(Classify)与账里的 outcome 分开说——崩溃
    // 截断的账 outcome 还是默认值,拿它当结论会冒充"成功"。unknown 档
    // 明写 unknown_after_start,不冒充失败也不冒充成功(单子)。
    if (record.Classify() == RecoveryClass::UnknownAfterStart) {
        text += "unknown_after_start";
    } else if (record.Classify() == RecoveryClass::NotStarted) {
        text += "not_started";
    } else {
        text += ToString(record.outcome);
    }
    if (!record.tool_name.empty()) {
        text += "(" + record.tool_name + ")";
    }
    switch (record.Classify()) {
        case RecoveryClass::ResultRecoverable:
        case RecoveryClass::Finished:
            if (record.result_ref.kind == ToolResultRef::Kind::Inline && !record.result_ref.content.empty()) {
                text += ":";
                text += record.result_ref.content;
            } else if (record.result_ref.kind == ToolResultRef::Kind::Artifact) {
                text += ":原始结果在 artifact " + record.result_ref.artifact_id;
            } else if (record.Classify() == RecoveryClass::Finished &&
                       record.outcome == ToolOutcome::Succeeded && record.result_ref.kind == ToolResultRef::Kind::Unavailable) {
                // 闸前终态/旧档没带结果正文:结论明确,正文没了,如实说。
                text += ":原始结果正文未随账保留";
            }
            break;
        case RecoveryClass::UnknownAfterStart:
            text += ":执行已开始、终态未知(宿主崩溃或账截断),副作用状态未核验,不自动重跑。";
            break;
        case RecoveryClass::NotStarted:
            text += ":崩溃前未执行,无副作用。";
            break;
    }
    return text;
}

std::string FormatExecutionSummaryLine(const ToolExecutionRecord& record, bool first_failure) {
    std::ostringstream out;
    out << "#" << record.sequence_in_batch << " " << record.execution_id << " " << record.tool_name << " "
        << ToString(record.outcome) << " " << record.duration_ms << "ms";
    if (record.corrupt) {
        out << " [trace_corrupt]";
    }
    if (!record.parent_execution_id.empty()) {
        out << " (parent " << record.parent_execution_id << ")";
    }
    if (!record.retry_of.empty()) {
        out << " (retry of " << record.retry_of << ")";
    }
    if (!record.blocked_by.empty()) {
        out << " (blocked by " << record.blocked_by << ")";
    }
    if (!record.compensates.empty()) {
        out << " (compensates " << record.compensates << ")";
    }
    if (first_failure) {
        out << " <- first explicit failure";
    }
    return out.str();
}

}  // namespace lubancode::agent
