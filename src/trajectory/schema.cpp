#include "trajectory/schema.hpp"

#include <cstdint>
#include <string_view>

namespace lubancode::trajectory {
namespace {

// ---------------------------------------------------------------------------
// payload 字段表:每 kind 的 required/allowed 键与类型。
//
// 类型码:s=string i=整数(带符号或无符号) u=无符号整数 b=bool o=object
//        a=array any=任意 JSON 值。后缀 '|' 允许 null(如 "o|" 可空对象)。
// allowed = required + 其余可选键;不在表内的键一律拒绝。
// 正文类字段(text 等)允许两种形态:内联 string 或 §8.2 的 BlobRef object
// (recorder 超限 offload 后落盘即是 BlobRef 形态),类型码 "B" 表示
// "string 或 BlobRef object"。
// ---------------------------------------------------------------------------

struct PayloadField {
    EventKind kind;
    const char* name;
    const char* type;
    bool required;
};

// 内容引用形状:string 之外的 "B" 形态。§五样例的简写 {sha256, size} 与
// §8.2 的五键 BlobRef 都算(五键含这两键,天然过检);hash 值本身合不合
// 是 verify 层的事,schema 只钉形状。
bool MatchesBlobRefShape(const nlohmann::json& value) {
    return value.is_object() && value.contains("sha256") && value.contains("size") &&
           value.at("sha256").is_string() && value.at("size").is_number_unsigned();
}

bool FieldMatchesType(const nlohmann::json& value, std::string_view type) {
    bool nullable = false;
    if (!type.empty() && type.back() == '|') {
        nullable = true;
        type.remove_suffix(1);
    }
    if (nullable && value.is_null()) {
        return true;
    }
    if (type == "s") {
        return value.is_string();
    }
    if (type == "i") {
        return value.is_number_integer();
    }
    if (type == "u") {
        return value.is_number_unsigned();
    }
    if (type == "b") {
        return value.is_boolean();
    }
    if (type == "o") {
        return value.is_object();
    }
    if (type == "a") {
        return value.is_array();
    }
    if (type == "B") {
        return value.is_string() || MatchesBlobRefShape(value);
    }
    if (type == "any") {
        return true;
    }
    return false;
}

// 全 kind 的 payload 规则表。逐条对照 §五 payload 样例冻结;没给样例的
// kind 按"事件至少要能自证身份"的原则给最小必填。
constexpr PayloadField kPayloadFields[] = {
    {EventKind::RunStarted, "run_kind", "s", true},
    {EventKind::RunStarted, "agent_run_id", "s", false},
    {EventKind::RunStarted, "owner_run_id", "s", false},
    {EventKind::RunStarted, "parent_agent_run_id", "s|", false},
    {EventKind::RunStarted, "spawn_event_id", "s", false},
    {EventKind::RunStarted, "profile_ref", "s", false},
    {EventKind::RunStarted, "task_ref", "s", false},
    {EventKind::RunStarted, "isolation_ref", "s", false},
    {EventKind::RunStarted, "start_reason", "s", false},
    {EventKind::RunStarted, "writer_version", "s", false},
    {EventKind::RunStarted, "min_reader_version", "u", false},
    {EventKind::RunStarted, "required_capabilities", "a", false},
    {EventKind::RunStarted, "previous_session_id", "s", false},
    // P0-3 resume(§10.4):start_reason=resume 的新场反指 source session。
    {EventKind::RunStarted, "resumed_from_session_id", "s", false},
    {EventKind::RunStarted, "caused_by_event_ref", "o", false},
    {EventKind::RunEnvironmentCaptured, "snapshot_ref", "o", true},
    {EventKind::RunEnvironmentCaptured, "replay_level", "s", false},
    {EventKind::RunEnvironmentCaptured, "gaps", "a", false},
    {EventKind::RunCompleted, "first_event_hash", "s", true},
    {EventKind::RunCompleted, "event_count_before_terminal", "u", true},
    {EventKind::RunCompleted, "schema_version", "u", true},
    {EventKind::RunCompleted, "recorder_version", "s", true},
    {EventKind::RunCompleted, "reason", "s", false},
    {EventKind::RunFailed, "first_event_hash", "s", true},
    {EventKind::RunFailed, "event_count_before_terminal", "u", true},
    {EventKind::RunFailed, "schema_version", "u", true},
    {EventKind::RunFailed, "recorder_version", "s", true},
    {EventKind::RunFailed, "reason", "s", false},
    {EventKind::RunFailed, "error", "s", false},
    {EventKind::RunCancelled, "first_event_hash", "s", true},
    {EventKind::RunCancelled, "event_count_before_terminal", "u", true},
    {EventKind::RunCancelled, "schema_version", "u", true},
    {EventKind::RunCancelled, "recorder_version", "s", true},
    {EventKind::RunCancelled, "reason", "s", false},
    {EventKind::SessionClearRequested, "next_session_id", "s", true},
    {EventKind::SessionClearRequested, "reason", "s", false},
    {EventKind::SessionEnded, "first_event_hash", "s", true},
    {EventKind::SessionEnded, "event_count_before_terminal", "u", true},
    {EventKind::SessionEnded, "schema_version", "u", true},
    {EventKind::SessionEnded, "recorder_version", "s", true},
    {EventKind::SessionEnded, "reason", "s", true},
    {EventKind::SessionEnded, "next_session_id", "s", false},
    {EventKind::SessionEnded, "close_quality", "s", false},
    {EventKind::TurnStarted, "trigger", "s", true},
    {EventKind::TurnStarted, "trigger_ref", "s", false},
    {EventKind::TurnStarted, "queue_item_input_id", "s", false},
    {EventKind::TurnCompleted, "outcome", "s", true},
    {EventKind::TurnCompleted, "summary_ref", "o", false},
    {EventKind::TurnFailed, "reason", "s", true},
    {EventKind::TurnFailed, "error_code", "s", false},
    {EventKind::TurnCancelled, "reason", "s", true},
    {EventKind::TurnCancelled, "error_code", "s", false},
    {EventKind::InputReceived, "input_id", "s", true},
    {EventKind::InputReceived, "content", "a", true},
    {EventKind::InputReceived, "channel", "s", true},
    {EventKind::InputReceived, "sender", "o", true},
    {EventKind::InputReceived, "attachments", "a", false},
    {EventKind::ContextAttached, "context_id", "s", true},
    {EventKind::ContextAttached, "context_kind", "s", true},
    {EventKind::ContextAttached, "scope", "s", true},
    {EventKind::ContextAttached, "content_ref", "o", false},
    {EventKind::ContextAttached, "source_refs", "a", false},
    {EventKind::ContextDetached, "context_id", "s", true},
    {EventKind::ContextDetached, "reason", "s", false},
    // 存储 v2 P0-3(合同 §四):记忆召回快照。snapshot_ref 与 snapshot_inline
    // 二选一——≤512B 小内容允许内联,其余落 session artifacts 后引用。
    {EventKind::ContextInjected, "kind", "s", true},
    {EventKind::ContextInjected, "memory_level", "s", true},
    {EventKind::ContextInjected, "memory_id", "s", true},
    {EventKind::ContextInjected, "memory_schema", "u", true},
    {EventKind::ContextInjected, "memory_updated_at", "s", true},
    {EventKind::ContextInjected, "content_sha256", "s", true},
    {EventKind::ContextInjected, "source_evidence_refs", "a", true},
    {EventKind::ContextInjected, "injected_bytes", "u", true},
    {EventKind::ContextInjected, "snapshot_ref", "s", false},
    {EventKind::ContextInjected, "snapshot_inline", "s", false},
    {EventKind::ModelRequestPrepared, "model", "s", true},
    {EventKind::ModelRequestPrepared, "provider", "s", true},
    {EventKind::ModelRequestPrepared, "wire", "s", true},
    {EventKind::ModelRequestPrepared, "message_refs", "a", true},
    {EventKind::ModelRequestPrepared, "parameters", "o", false},
    {EventKind::ModelRequestPrepared, "system_ref", "B", false},
    {EventKind::ModelRequestPrepared, "toolset_ref", "B", false},
    {EventKind::ModelRequestPrepared, "context_refs", "a", false},
    {EventKind::ModelRequestPrepared, "request_snapshot_ref", "o", false},
    {EventKind::ModelRequestPrepared, "request_snapshot_sha256", "s", false},
    {EventKind::ModelRequestPrepared, "cache_epoch", "u", false},
    {EventKind::ModelRequestPrepared, "purpose", "s", false},
    {EventKind::ModelRequestSent, "prepared_event_id", "s", true},
    {EventKind::ModelRequestSent, "attempt", "u", false},
    // 任务级 turn 账(turn 预算单 §11.1,P1-1):装了任务 turn 门的会话在
    // sent/completed/failed/cancelled 边界带上这些键;旧 stream 缺席合法,
    // 新 stream 不给也合法(主会话/旁路请求没有任务 turn 账)。
    {EventKind::ModelRequestSent, "task_turn_index", "u", false},
    {EventKind::ModelRequestSent, "turn_limit", "u", false},
    {EventKind::ModelRequestSent, "input_round_index", "u", false},
    {EventKind::ModelOutputCompleted, "output_id", "s", true},
    {EventKind::ModelOutputCompleted, "blocks", "a", true},
    {EventKind::ModelOutputCompleted, "stop_reason", "s", true},
    // usage 只在 v1 stream 里随 completed 走(v2 的 canonical owner 是
    // model.usage.recorded;v2 校验在 ValidatePayloadWithVersion 拒这键)。
    {EventKind::ModelOutputCompleted, "usage", "o", false},
    {EventKind::ModelOutputCompleted, "provider_response_id", "s", false},
    {EventKind::ModelOutputCompleted, "task_turn_index", "u", false},
    {EventKind::ModelOutputFailed, "reason", "s", true},
    {EventKind::ModelOutputFailed, "error_code", "s", false},
    {EventKind::ModelOutputFailed, "attempt", "u", false},
    {EventKind::ModelOutputFailed, "task_turn_index", "u", false},
    {EventKind::ModelOutputCancelled, "reason", "s", true},
    {EventKind::ModelOutputCancelled, "error_code", "s", false},
    {EventKind::ModelOutputCancelled, "task_turn_index", "u", false},
    // v2 model.usage.recorded(Token 账本单 §6.1.1):attempt 必须,token
    // 五项与 cache_epoch/prefix_append_only 只在 reported_by_provider=true
    // 时必填、false 时禁现——"没报"不许拿 0 顶上。条件硬约束在
    // ValidatePayloadWithVersion 里逐条裁。
    {EventKind::ModelUsageRecorded, "attempt", "u", true},
    {EventKind::ModelUsageRecorded, "provider_response_id", "s", false},
    {EventKind::ModelUsageRecorded, "reported_by_provider", "b", true},
    {EventKind::ModelUsageRecorded, "input_tokens", "i", false},
    {EventKind::ModelUsageRecorded, "cache_read_tokens", "i", false},
    {EventKind::ModelUsageRecorded, "cache_creation_tokens", "i", false},
    {EventKind::ModelUsageRecorded, "output_tokens", "i", false},
    {EventKind::ModelUsageRecorded, "reasoning_tokens", "i", false},
    {EventKind::ModelUsageRecorded, "cache_epoch", "u", false},
    {EventKind::ModelUsageRecorded, "prefix_append_only", "b", false},
    {EventKind::ToolExecutionPlanned, "call_id", "s", true},
    {EventKind::ToolExecutionPlanned, "tool_name", "s", true},
    {EventKind::ToolExecutionPlanned, "provider_call_id", "s", false},
    {EventKind::ToolExecutionPlanned, "source_output_id", "s", false},
    {EventKind::ToolInputEffective, "call_id", "s", true},
    {EventKind::ToolInputEffective, "tool_name", "s", true},
    {EventKind::ToolInputEffective, "source_kind", "s", true},
    {EventKind::ToolInputEffective, "effect_class", "s", true},
    {EventKind::ToolInputEffective, "effective_arguments", "o", true},
    {EventKind::ToolInputEffective, "effective_arguments_sha256", "s", true},
    {EventKind::ToolInputEffective, "source_instance", "s", false},
    {EventKind::ToolInputEffective, "rewritten_by", "a", false},
    {EventKind::ToolInputEffective, "approval_policy_ref", "o", false},
    {EventKind::ToolExecutionStarted, "call_id", "s", true},
    {EventKind::ToolExecutionStarted, "attempt", "u", false},
    {EventKind::ToolExecutionStarted, "batch_id", "s", false},
    {EventKind::ToolExecutionStarted, "position_in_batch", "u", false},
    {EventKind::ToolExecutionStarted, "parallel_group_id", "s", false},
    {EventKind::ToolExecutionFinished, "outcome", "s", true},
    {EventKind::ToolExecutionFinished, "duration_ms", "i", true},
    {EventKind::ToolExecutionFinished, "exit_code", "i|", false},
    {EventKind::ToolExecutionFinished, "stdout_ref", "o|", false},
    {EventKind::ToolExecutionFinished, "stderr_ref", "o|", false},
    {EventKind::ToolExecutionFinished, "result_ref", "o", false},
    {EventKind::ToolExecutionFinished, "side_effects", "a", false},
    {EventKind::ToolExecutionFinished, "undo_ref", "o|", false},
    {EventKind::ToolExecutionFailed, "reason", "s", true},
    {EventKind::ToolExecutionFailed, "duration_ms", "i", false},
    {EventKind::ToolExecutionFailed, "error_code", "s", false},
    {EventKind::ToolExecutionFailed, "stdout_ref", "o|", false},
    {EventKind::ToolExecutionFailed, "stderr_ref", "o|", false},
    // P0-2(子代理失败路补列):agent 工具的执行终态带 child_run_id 时,
    // result_ref(child_stream:子账终态 hash 对账)与 side_effects 随行——
    // 与 Finished 同一形状,失败也不丢父子边。
    {EventKind::ToolExecutionFailed, "result_ref", "o", false},
    {EventKind::ToolExecutionFailed, "side_effects", "a", false},
    {EventKind::ToolExecutionCancelled, "reason", "s", true},
    {EventKind::ToolExecutionCancelled, "duration_ms", "i", false},
    {EventKind::ToolExecutionUnknown, "reason", "s", true},
    {EventKind::ToolExecutionUnknown, "duration_ms", "i", false},
    {EventKind::ToolExecutionUnknown, "exit_code", "i|", false},
    {EventKind::ToolResultCommitted, "call_id", "s", true},
    {EventKind::ToolResultCommitted, "content", "a", true},
    {EventKind::ToolResultCommitted, "is_error", "b", true},
    {EventKind::ToolResultCommitted, "derived_from_event", "s", false},
    {EventKind::ToolResultCommitted, "truncation", "s", false},
    // P0-2(无损投影):MCP structuredContent 原样随行(nullopt 不落键,
    // 与"server 没给"分得清);图片/音频等富块在 content 数组里以
    // *_ref 形状带 artifact 引用。
    {EventKind::ToolResultCommitted, "structured_content", "o", false},
    {EventKind::ControlCommandRequested, "command_id", "s", true},
    {EventKind::ControlCommandRequested, "command_name", "s", true},
    {EventKind::ControlCommandRequested, "action_name", "s", true},
    {EventKind::ControlCommandRequested, "effect_class", "s", true},
    {EventKind::ControlCommandRequested, "args_ref", "o", false},
    {EventKind::ControlCommandRequested, "raw_ref", "o", false},
    {EventKind::ControlCommandCompleted, "command_id", "s", true},
    {EventKind::ControlCommandCompleted, "status", "s", false},
    {EventKind::ControlCommandCompleted, "effect_refs", "a", false},
    {EventKind::ControlCommandCompleted, "state_hash_before", "s", false},
    {EventKind::ControlCommandCompleted, "state_hash_after", "s", false},
    {EventKind::ControlCommandCompleted, "external_side_effect", "b", false},
    {EventKind::ControlCommandCompleted, "qualified_requested_ref", "o", false},
    {EventKind::ControlCommandCompleted, "boundary_operation_id", "s", false},
    {EventKind::ControlCommandFailed, "command_id", "s", true},
    {EventKind::ControlCommandFailed, "reason", "s", true},
    {EventKind::ControlCommandFailed, "error_code", "s", false},
    {EventKind::ControlCommandCancelled, "command_id", "s", true},
    {EventKind::ControlCommandCancelled, "reason", "s", true},
    {EventKind::ControlCommandCancelled, "error_code", "s", false},
    {EventKind::ControlCommandRejected, "command_id", "s", true},
    {EventKind::ControlCommandRejected, "reason", "s", true},
    {EventKind::ControlCommandRejected, "error_code", "s", false},
    {EventKind::ControlTitleChanged, "title", "s", true},
    {EventKind::ControlTitleChanged, "old_title", "s", false},
    {EventKind::ControlCwdChanged, "cwd", "s", true},
    {EventKind::ControlCwdChanged, "worktree_id", "s", false},
    {EventKind::ControlModeChanged, "mode", "s", true},
    {EventKind::ControlModeChanged, "old_mode", "s", false},
    {EventKind::ControlModeChanged, "reason", "s", false},
    {EventKind::ControlContextWindowChanged, "context_window", "s", true},
    {EventKind::ControlContextWindowChanged, "old_context_window", "s", false},
    {EventKind::ControlCheckpointCreated, "checkpoint_id", "s", true},
    {EventKind::ControlCheckpointCreated, "source_seq", "u", true},
    {EventKind::ControlCheckpointCreated, "source_event_hash", "s", true},
    {EventKind::ControlCheckpointCreated, "state_hash", "s", true},
    {EventKind::ControlCheckpointCreated, "payload_ref", "o", false},
    {EventKind::ControlQueueItemEnqueued, "item_id", "s", true},
    {EventKind::ControlQueueItemEnqueued, "input_id", "s", true},
    {EventKind::ControlQueueItemEnqueued, "enqueue_reason", "s", false},
    {EventKind::ControlQueueItemDequeued, "item_id", "s", true},
    {EventKind::ControlQueueItemDequeued, "input_id", "s", true},
    {EventKind::ControlQueueItemDequeued, "reason", "s", false},
    {EventKind::ControlQueueItemCancelled, "item_id", "s", true},
    {EventKind::ControlQueueItemCancelled, "reason", "s", true},
    {EventKind::ControlQueueItemCancelled, "error_code", "s", false},
    {EventKind::ControlQueueItemExpired, "item_id", "s", true},
    {EventKind::ControlQueueItemExpired, "reason", "s", false},
    {EventKind::ControlQueueSnapshot, "items", "a", true},
    {EventKind::ControlQueueSnapshot, "reason", "s", false},
    {EventKind::ControlApprovalRequested, "approval_id", "s", true},
    {EventKind::ControlApprovalRequested, "call_id", "s", true},
    {EventKind::ControlApprovalRequested, "policy_hash", "s", false},
    {EventKind::ControlApprovalRequested, "prompt_ref", "o", false},
    {EventKind::ControlApprovalResolved, "approval_id", "s", true},
    {EventKind::ControlApprovalResolved, "decision", "s", true},
    {EventKind::ControlApprovalResolved, "policy_hash", "s", false},
    {EventKind::ControlApprovalExpired, "approval_id", "s", true},
    {EventKind::ControlApprovalExpired, "reason", "s", false},
    {EventKind::ControlCancellationRequested, "target", "s", true},
    {EventKind::ControlCancellationRequested, "call_id", "s", false},
    {EventKind::ControlCancellationRequested, "turn_id", "s", false},
    {EventKind::ControlCancellationRequested, "reason", "s", false},
    {EventKind::ControlCancellationApplied, "target", "s", true},
    {EventKind::ControlCancellationApplied, "call_id", "s", false},
    {EventKind::ControlCancellationApplied, "turn_id", "s", false},
    {EventKind::CompactRequested, "trigger", "s", true},
    {EventKind::CompactRequested, "source_range", "a", false},
    {EventKind::CompactRequested, "threshold", "s", false},
    {EventKind::CompactRequested, "user_note_ref", "o", false},
    {EventKind::CompactRequested, "old_epoch", "u", false},
    {EventKind::CompactRequested, "input_state_hash", "s", false},
    {EventKind::CompactRequestPrepared, "request_ref", "o", true},
    {EventKind::CompactRequestPrepared, "route", "s", false},
    {EventKind::CompactRequestPrepared, "prompt_ref", "o", false},
    {EventKind::CompactRequestPrepared, "tool_config_ref", "o", false},
    {EventKind::CompactOutputGenerated, "summary_ref", "o", true},
    {EventKind::CompactOutputGenerated, "contract_ref", "o", false},
    {EventKind::CompactOutputGenerated, "work_state_ref", "o", false},
    {EventKind::CompactOutputGenerated, "hot_zone_refs", "a", false},
    {EventKind::CompactValidationCompleted, "passed", "b", true},
    {EventKind::CompactValidationCompleted, "validator_version", "s", false},
    {EventKind::CompactValidationCompleted, "checks", "a", false},
    {EventKind::CompactApplied, "old_state_hash", "s", true},
    {EventKind::CompactApplied, "new_state_hash", "s", true},
    {EventKind::CompactApplied, "source_event_span", "a", true},
    {EventKind::CompactApplied, "pre_tokens", "u", false},
    {EventKind::CompactApplied, "post_tokens", "u", false},
    {EventKind::CompactApplied, "summary_ref", "o", false},
    {EventKind::CompactApplied, "protected_refs", "a", false},
    {EventKind::CompactApplied, "validator_version", "s", false},
    {EventKind::CompactApplied, "epoch", "u", false},
    {EventKind::CompactFailed, "reason", "s", true},
    {EventKind::CompactFailed, "error_code", "s", false},
    {EventKind::CompactCancelled, "reason", "s", true},
    {EventKind::CompactCancelled, "error_code", "s", false},
    {EventKind::CompactRejected, "reason", "s", true},
    {EventKind::CompactRejected, "error_code", "s", false},
    {EventKind::RecordSelectionStarted, "record_id", "s", true},
    {EventKind::RecordSelectionStarted, "goal", "s", false},
    {EventKind::RecordSelectionStarted, "variables", "a", false},
    {EventKind::RecordSelectionStarted, "acceptance", "s", false},
    {EventKind::RecordSelectionStarted, "start_event_ref", "o", false},
    {EventKind::RecordSelectionStarted, "scope", "s", false},
    {EventKind::RecordSelectionPaused, "record_id", "s", true},
    {EventKind::RecordSelectionPaused, "reason", "s", false},
    {EventKind::RecordSelectionResumed, "record_id", "s", true},
    {EventKind::RecordSelectionResumed, "reason", "s", false},
    {EventKind::RecordSelectionNoteAdded, "record_id", "s", true},
    {EventKind::RecordSelectionNoteAdded, "note_ref", "o", true},
    {EventKind::RecordSelectionNoteAdded, "annotation", "s", false},
    {EventKind::RecordSelectionCompleted, "record_id", "s", true},
    {EventKind::RecordSelectionCompleted, "included_spans", "a", false},
    {EventKind::RecordSelectionCompleted, "excluded_spans", "a", false},
    {EventKind::RecordSelectionCompleted, "source_terminal_hashes", "a", false},
    {EventKind::RecordSelectionCancelled, "record_id", "s", true},
    {EventKind::RecordSelectionCancelled, "reason", "s", true},
    {EventKind::RecordSelectionCancelled, "error_code", "s", false},
    {EventKind::RecordSelectionInterrupted, "record_id", "s", true},
    {EventKind::RecordSelectionInterrupted, "reason", "s", true},
    {EventKind::RecordSelectionInterrupted, "error_code", "s", false},
    {EventKind::ResumeSourceAttached, "source_session_id", "s", true},
    {EventKind::ResumeSourceAttached, "source_terminal_event_hash", "s", true},
    {EventKind::ResumeSourceAttached, "replay_version", "s", true},
    {EventKind::ResumeSourceAttached, "imported_state_hash", "s", true},
    {EventKind::ResumeSourceAttached, "checkpoint_ref", "o", false},
    {EventKind::ResumeSourceAttached, "qualified_event_refs", "a", false},
    {EventKind::VerificationStarted, "verification_id", "s", true},
    {EventKind::VerificationStarted, "kind", "s", true},
    {EventKind::VerificationStarted, "subject", "s", false},
    {EventKind::VerificationRecorded, "verification_id", "s", true},
    {EventKind::VerificationRecorded, "kind", "s", true},
    {EventKind::VerificationRecorded, "passed", "b", true},
    {EventKind::VerificationRecorded, "producer", "s", true},
    {EventKind::VerificationRecorded, "subject", "s", false},
    {EventKind::VerificationRecorded, "command_ref", "o", false},
    {EventKind::VerificationRecorded, "facts", "o", false},
    {EventKind::VerificationRecorded, "artifact_refs", "a", false},
    {EventKind::VerificationRecorded, "observed_after_seq", "u", false},
    {EventKind::VerificationRecorded, "fresh", "b", false},
    {EventKind::VerificationInvalidated, "verification_id", "s", true},
    {EventKind::VerificationInvalidated, "reason", "s", true},
    {EventKind::VerificationInvalidated, "invalidated_by_event", "s", false},
    {EventKind::OutcomeAssessed, "outcome", "s", true},
    {EventKind::OutcomeAssessed, "evidence_refs", "a", false},
    {EventKind::OutcomeAssessed, "criteria", "a", false},
    // 存储 v2 P0-3:Memory 写入因果边(合同 §四)。request 是嵌套摘要
    //(operation/layer/memory_id/title);committed/failed 本批只入合同,
    // worker 回执先落 workspace lifecycle。
    {EventKind::MemorySaveRequested, "request", "o", true},
    {EventKind::MemorySaveRequested, "source_session", "s", false},
    {EventKind::MemorySaveRequested, "source_run", "s", false},
    {EventKind::MemorySaveRequested, "source_turn", "s", false},
    {EventKind::MemorySaveRequested, "source_event_ref", "s", false},
    {EventKind::MemorySaveCommitted, "memory_id", "s", true},
    {EventKind::MemorySaveCommitted, "memory_version", "s", false},
    {EventKind::MemorySaveCommitted, "content_sha256", "s", false},
    {EventKind::MemorySaveCommitted, "memory_path", "s", false},
    {EventKind::MemorySaveCommitted, "committed_at", "s", false},
    {EventKind::MemorySaveFailed, "stable_error_code", "s", true},
    {EventKind::MemorySaveFailed, "retryable", "b", false},
};

}  // namespace

std::optional<SchemaError> ValidatePayloadWithVersion(int schema_version, EventKind kind,
                                                      const nlohmann::json& payload) {
    // 版本无关的表裁(必填/类型/未知键)先过,再叠版本差异。
    if (auto error = ValidatePayload(kind, payload)) {
        return error;
    }
    if (schema_version < kEnvelopeSchemaVersion || schema_version > kMaxEnvelopeSchemaVersion) {
        return SchemaError{"schema.unsupported_version",
                           "不支持的 schema_version: " + std::to_string(schema_version)};
    }
    if (schema_version == kEnvelopeSchemaVersion) {
        // v1:usage 正文只认 completed.payload.usage(legacy);v2 新事件
        // 不得混进 v1 stream。
        if (kind == EventKind::ModelUsageRecorded) {
            return SchemaError{"schema.kind_not_in_version",
                               "model.usage.recorded 只在 schema v2 出现;v1 stream 拒收"};
        }
        return std::nullopt;
    }
    // v2。
    if (kind == EventKind::ModelOutputCompleted && payload.contains("usage")) {
        return SchemaError{"schema.payload_unknown_field",
                           "v2 completed 不带 usage 正文;canonical owner 是 model.usage.recorded"};
    }
    if (kind == EventKind::ModelUsageRecorded) {
        const bool reported = payload.at("reported_by_provider").get<bool>();
        static constexpr const char* kTokenFields[] = {"input_tokens", "cache_read_tokens",
                                                       "cache_creation_tokens", "output_tokens",
                                                       "reasoning_tokens"};
        for (const char* name : kTokenFields) {
            if (reported && !payload.contains(name)) {
                return SchemaError{"schema.payload_missing_field",
                                   std::string("provider 明报时 token 字段必填: ") + name};
            }
            if (!reported && payload.contains(name)) {
                return SchemaError{"schema.payload_forbidden_field",
                                   std::string("provider 没报时不得拿 token 字段顶上: ") + name};
            }
        }
        if (reported) {
            std::int64_t values[5] = {0, 0, 0, 0, 0};
            for (std::size_t i = 0; i < 5; ++i) {
                values[i] = payload.at(kTokenFields[i]).get<std::int64_t>();
                if (values[i] < 0) {
                    return SchemaError{"schema.usage_negative_tokens",
                                       std::string("token 字段不得为负: ") + kTokenFields[i]};
                }
            }
            // reasoning 是 output 的子集(Token 账本单 §6.1/§15.1)。
            if (values[4] > values[3]) {
                return SchemaError{"schema.usage_reasoning_exceeds_output",
                                   "reasoning_tokens 不得大于 output_tokens"};
            }
        }
    }
    return std::nullopt;
}

std::optional<SchemaError> ValidateEnvelope(const EventEnvelope& envelope) {
    const EventKindInfo& info = EventKindInfoOf(envelope.kind);

    if (envelope.workspace_key.empty() || envelope.session_id.empty() || envelope.run_id.empty()) {
        return SchemaError{"schema.missing_field", "workspace_key/session_id/run_id 不得为空"};
    }
    if (envelope.seq == 0) {
        return SchemaError{"schema.bad_seq", "seq 从 1 起,不回收"};
    }
    if (envelope.event_id != FormatEventId(envelope.run_id, envelope.seq)) {
        return SchemaError{"schema.bad_event_id", "event_id 须是 run_id:evt-%08llu 形状"};
    }
    if (envelope.plane != info.plane) {
        return SchemaError{"schema.plane_mismatch",
                           std::string("kind ") + info.name + " 固定 plane 为 " + PlaneName(info.plane)};
    }
    if (!IsValidActorOrigin(envelope.actor, envelope.origin)) {
        return SchemaError{"schema.bad_actor_origin",
                           std::string("actor ") + ActorName(envelope.actor) + " 不许配 origin " +
                               OriginName(envelope.origin)};
    }
    if (envelope.visibility.empty()) {
        return SchemaError{"schema.missing_field", "visibility 不得为空"};
    }
    // id 三档要求(§4.1"该有而没有,提交器拒绝")。
    const auto check_id = [&](IdNeed need, const std::optional<std::string>& value,
                              const char* name) -> std::optional<SchemaError> {
        if (need == IdNeed::Required && !value.has_value()) {
            return SchemaError{"schema.missing_field", std::string("kind ") + info.name + " 须带 " + name};
        }
        if (need == IdNeed::Forbidden && value.has_value()) {
            return SchemaError{"schema.forbidden_field",
                               std::string("kind ") + info.name + " 不许带 " + name};
        }
        return std::nullopt;
    };
    if (auto error = check_id(info.turn, envelope.turn_id, "turn_id")) {
        return error;
    }
    if (auto error = check_id(info.request, envelope.request_id, "request_id")) {
        return error;
    }
    if (auto error = check_id(info.call, envelope.call_id, "call_id")) {
        return error;
    }
    if (!IsHex64(envelope.prev_hash) || !IsHex64(envelope.event_hash)) {
        return SchemaError{"schema.bad_hash", "prev_hash/event_hash 须是 64 位十六进制小写"};
    }
    if (!envelope.payload.is_object()) {
        return SchemaError{"schema.bad_type", "payload 须是 object"};
    }
    return std::nullopt;
}

std::optional<SchemaError> ValidatePayload(EventKind kind, const nlohmann::json& payload) {
    if (!payload.is_object()) {
        return SchemaError{"schema.bad_type", "payload 须是 object"};
    }
    bool kind_registered = false;
    for (const auto& field : kPayloadFields) {
        if (field.kind != kind) {
            continue;
        }
        kind_registered = true;
        if (field.required && !payload.contains(field.name)) {
            return SchemaError{"schema.payload_missing_field",
                               std::string("payload 缺必填字段: ") + field.name};
        }
        if (payload.contains(field.name) && !FieldMatchesType(payload.at(field.name), field.type)) {
            return SchemaError{"schema.payload_bad_type",
                               std::string("payload 字段类型不合: ") + field.name};
        }
    }
    if (!kind_registered) {
        // 表没登记的 kind 一律拒绝:合同封闭,不静默放行。
        return SchemaError{"schema.payload_rule_missing",
                           std::string("kind ") + EventKindName(kind) + " 无 payload 规则"};
    }
    // 未知键拒绝。
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        bool known = false;
        for (const auto& field : kPayloadFields) {
            if (field.kind == kind && it.key() == field.name) {
                known = true;
                break;
            }
        }
        if (!known) {
            return SchemaError{"schema.payload_unknown_field", "payload 未知字段: " + it.key()};
        }
    }
    return std::nullopt;
}

std::optional<SchemaError> ParseAndValidateEventLine(const nlohmann::json& line,
                                                     EventEnvelope* out) {
    std::string error_code;
    std::string message;
    const auto envelope = EventEnvelope::FromJsonStrict(line, &error_code, &message);
    if (!envelope.has_value()) {
        return SchemaError{error_code, message};
    }
    if (auto error = ValidateEnvelope(*envelope)) {
        return error;
    }
    if (auto error = ValidatePayloadWithVersion(envelope->schema_version, envelope->kind,
                                                envelope->payload)) {
        return error;
    }
    if (out != nullptr) {
        *out = *envelope;
    }
    return std::nullopt;
}

}  // namespace lubancode::trajectory
