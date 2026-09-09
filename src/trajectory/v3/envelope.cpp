// session 轨迹 v3 信封实现:枚举名转换、ToJson/FromJsonStrict、行哈希。
#include "trajectory/v3/envelope.hpp"

#include <algorithm>
#include <iterator>

#include "trajectory/journal.hpp"  // ComputeEventHash 复用(SHA256 拼接语义)

namespace lubancode::trajectory::v3 {

namespace {

// 未知键拒收的公共读取辅助:严格按白名单收键,漏键/类型错给稳定码。
template <typename T>
bool GetRequired(const nlohmann::json& json, const char* key, T* out, std::string* error_code,
                 std::string* message, const char* context) {
    auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
        *error_code = "schema3.missing_field";
        *message = std::string(context) + " 缺字段: " + key;
        return false;
    }
    if (const T* parsed = it->get_ptr<const T*>()) {
        *out = *parsed;
        return true;
    }
    *error_code = "schema3.bad_type";
    *message = std::string(context) + " 字段类型错: " + key;
    return false;
}

bool GetOptionalString(const nlohmann::json& json, const char* key,
                       std::optional<std::string>* out) {
    auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
        return true;
    }
    if (!it->is_string()) {
        return false;
    }
    *out = it->get<std::string>();
    return true;
}

// 从 JSON 值解析枚举;null/缺键给 nullopt,非法值给错误。
template <typename E>
bool ParseEnumField(const nlohmann::json& json, const char* key, E (*from_name)(std::string_view),
                    std::optional<E>* out, std::string* error_code, std::string* message,
                    const char* context) {
    auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
        return true;
    }
    if (!it->is_string()) {
        *error_code = "schema3.bad_type";
        *message = std::string(context) + " 字段应为字符串: " + key;
        return false;
    }
    auto value = from_name(it->get<std::string>());
    if (!value.has_value()) {
        *error_code = "schema3.bad_enum";
        *message = std::string(context) + " 未知枚举值: " + key + "=" + it->get<std::string>();
        return false;
    }
    *out = *value;
    return true;
}

void SetIfPresent(nlohmann::json* json, const char* key, const std::optional<std::string>& value) {
    if (value.has_value()) {
        (*json)[key] = *value;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 枚举名
// ---------------------------------------------------------------------------

const char* MessageRoleName(MessageRole value) {
    switch (value) {
        case MessageRole::System: return "system";
        case MessageRole::User: return "user";
        case MessageRole::Assistant: return "assistant";
        case MessageRole::Tool: return "tool";
    }
    return "unknown";
}
std::optional<MessageRole> MessageRoleFromName(std::string_view name) {
    if (name == "system") return MessageRole::System;
    if (name == "user") return MessageRole::User;
    if (name == "assistant") return MessageRole::Assistant;
    if (name == "tool") return MessageRole::Tool;
    return std::nullopt;
}

const char* MessagePurposeName(MessagePurpose value) {
    switch (value) {
        case MessagePurpose::Conversation: return "conversation";
        case MessagePurpose::Compact: return "compact";
        case MessagePurpose::ContextSummary: return "context_summary";
        case MessagePurpose::SessionTitle: return "session_title";
        case MessagePurpose::Capability: return "capability";
    }
    return "unknown";
}
std::optional<MessagePurpose> MessagePurposeFromName(std::string_view name) {
    if (name == "conversation") return MessagePurpose::Conversation;
    if (name == "compact") return MessagePurpose::Compact;
    if (name == "context_summary") return MessagePurpose::ContextSummary;
    if (name == "session_title") return MessagePurpose::SessionTitle;
    if (name == "capability") return MessagePurpose::Capability;
    return std::nullopt;
}

const char* MessageOriginName(MessageOrigin value) {
    switch (value) {
        case MessageOrigin::Human: return "human";
        case MessageOrigin::Soul: return "soul";
        case MessageOrigin::SessionRuntime: return "session_runtime";
        case MessageOrigin::CompactRuntime: return "compact_runtime";
        case MessageOrigin::ContextRuntime: return "context_runtime";
        case MessageOrigin::Hook: return "hook";
        case MessageOrigin::Skill: return "skill";
        case MessageOrigin::Subagent: return "subagent";
        case MessageOrigin::ParentAgent: return "parent_agent";
    }
    return "unknown";
}
std::optional<MessageOrigin> MessageOriginFromName(std::string_view name) {
    if (name == "human") return MessageOrigin::Human;
    if (name == "soul") return MessageOrigin::Soul;
    if (name == "session_runtime") return MessageOrigin::SessionRuntime;
    if (name == "compact_runtime") return MessageOrigin::CompactRuntime;
    if (name == "context_runtime") return MessageOrigin::ContextRuntime;
    if (name == "hook") return MessageOrigin::Hook;
    if (name == "skill") return MessageOrigin::Skill;
    if (name == "subagent") return MessageOrigin::Subagent;
    if (name == "parent_agent") return MessageOrigin::ParentAgent;
    return std::nullopt;
}

const char* DisplayModeName(DisplayMode value) {
    switch (value) {
        case DisplayMode::Visible: return "visible";
        case DisplayMode::Collapsed: return "collapsed";
        case DisplayMode::Hidden: return "hidden";
    }
    return "unknown";
}
std::optional<DisplayMode> DisplayModeFromName(std::string_view name) {
    if (name == "visible") return DisplayMode::Visible;
    if (name == "collapsed") return DisplayMode::Collapsed;
    if (name == "hidden") return DisplayMode::Hidden;
    return std::nullopt;
}

const char* CompletionStatusName(CompletionStatus value) {
    switch (value) {
        case CompletionStatus::Complete: return "complete";
        case CompletionStatus::Interrupted: return "interrupted";
        case CompletionStatus::Truncated: return "truncated";
    }
    return "unknown";
}
std::optional<CompletionStatus> CompletionStatusFromName(std::string_view name) {
    if (name == "complete") return CompletionStatus::Complete;
    if (name == "interrupted") return CompletionStatus::Interrupted;
    if (name == "truncated") return CompletionStatus::Truncated;
    return std::nullopt;
}

const char* OpStatusName(OpStatus value) {
    switch (value) {
        case OpStatus::Pending: return "pending";
        case OpStatus::Running: return "running";
        case OpStatus::Done: return "done";
        case OpStatus::Failed: return "failed";
        case OpStatus::Cancelled: return "cancelled";
        case OpStatus::Rejected: return "rejected";
        case OpStatus::Unknown: return "unknown";
    }
    return "unknown";
}
std::optional<OpStatus> OpStatusFromName(std::string_view name) {
    if (name == "pending") return OpStatus::Pending;
    if (name == "running") return OpStatus::Running;
    if (name == "done") return OpStatus::Done;
    if (name == "failed") return OpStatus::Failed;
    if (name == "cancelled") return OpStatus::Cancelled;
    if (name == "rejected") return OpStatus::Rejected;
    if (name == "unknown") return OpStatus::Unknown;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// EventKindV3 名字
// ---------------------------------------------------------------------------

const char* EventKindV3Name(EventKindV3 kind) {
    switch (kind) {
        case EventKindV3::SessionStarted: return "session.started";
        case EventKindV3::SessionEnded: return "session.ended";
        case EventKindV3::SystemChange: return "system.change";
        case EventKindV3::ContextSystemApplied: return "context.system.applied";
        case EventKindV3::ContextInputApplied: return "context.input.applied";
        case EventKindV3::ContextToolPreviewsReduced: return "context.tool_previews.reduced";
        case EventKindV3::ModelRequestPrepared: return "model.request.prepared";
        case EventKindV3::ModelRequestSent: return "model.request.sent";
        case EventKindV3::ModelRequestFailed: return "model.request.failed";
        case EventKindV3::ModelResponseStarted: return "model.response.started";
        case EventKindV3::ModelResponseDelta: return "model.response.delta";
        case EventKindV3::ModelResponseCompleted: return "model.response.completed";
        case EventKindV3::ModelResponseFailed: return "model.response.failed";
        case EventKindV3::ModelResponseCancelled: return "model.response.cancelled";
        case EventKindV3::ModelUsageAppended: return "model.usage.appended";
        case EventKindV3::CompactRequested: return "compact.requested";
        case EventKindV3::CompactPending: return "compact.pending";
        case EventKindV3::CompactStarted: return "compact.started";
        case EventKindV3::CompactValidationStarted: return "compact.validation.started";
        case EventKindV3::CompactValidationCompleted: return "compact.validation.completed";
        case EventKindV3::CompactApplied: return "compact.applied";
        case EventKindV3::CompactFailed: return "compact.failed";
        case EventKindV3::CompactCancelled: return "compact.cancelled";
        case EventKindV3::CompactRejected: return "compact.rejected";
        case EventKindV3::ToolExecutionPending: return "tool.execution.pending";
        case EventKindV3::ToolExecutionStarted: return "tool.execution.started";
        case EventKindV3::ToolExecutionWaiting: return "tool.execution.waiting";
        case EventKindV3::ToolExecutionResumed: return "tool.execution.resumed";
        case EventKindV3::ToolExecutionFinished: return "tool.execution.finished";
        case EventKindV3::ToolExecutionFailed: return "tool.execution.failed";
        case EventKindV3::ToolExecutionCancelled: return "tool.execution.cancelled";
        case EventKindV3::ToolExecutionRejected: return "tool.execution.rejected";
        case EventKindV3::ToolExecutionUnknown: return "tool.execution.unknown";
        case EventKindV3::ToolResultPersisted: return "tool.result.persisted";
        case EventKindV3::ToolResultPersistFailed: return "tool.result.persist_failed";
        case EventKindV3::ToolResultSelected: return "tool.result.selected";
        case EventKindV3::HookDispatchRequested: return "hook.dispatch.requested";
        case EventKindV3::HookPending: return "hook.pending";
        case EventKindV3::HookStarted: return "hook.started";
        case EventKindV3::HookCompleted: return "hook.completed";
        case EventKindV3::HookFailed: return "hook.failed";
        case EventKindV3::HookCancelled: return "hook.cancelled";
        case EventKindV3::HookUnknown: return "hook.unknown";
        case EventKindV3::HookSkipped: return "hook.skipped";
        case EventKindV3::HookEffectsApplied: return "hook.effects.applied";
        case EventKindV3::HookEffectsRejected: return "hook.effects.rejected";
        case EventKindV3::CommandReceived: return "command.received";
        case EventKindV3::CommandPending: return "command.pending";
        case EventKindV3::CommandStarted: return "command.started";
        case EventKindV3::CommandCompleted: return "command.completed";
        case EventKindV3::CommandFailed: return "command.failed";
        case EventKindV3::CommandRejected: return "command.rejected";
        case EventKindV3::CommandCancelled: return "command.cancelled";
        case EventKindV3::CommandUnknown: return "command.unknown";
        case EventKindV3::InputReceived: return "input.received";
        case EventKindV3::InputEnqueued: return "input.enqueued";
        case EventKindV3::InputAdmitted: return "input.admitted";
        case EventKindV3::InputSuperseded: return "input.superseded";
        case EventKindV3::TitleRequested: return "title.requested";
        case EventKindV3::TitleExtracted: return "title.extracted";
        case EventKindV3::SessionTitleApplied: return "session.title.applied";
        case EventKindV3::ResumeSourceAttached: return "resume.source.attached";
        case EventKindV3::SubagentSpawnRequested: return "subagent.spawn.requested";
        case EventKindV3::SubagentLinked: return "subagent.linked";
        case EventKindV3::SubagentObserved: return "subagent.observed";
        case EventKindV3::SubagentSpawnFailed: return "subagent.spawn.failed";
        case EventKindV3::TaskStarted: return "task.started";
        case EventKindV3::TaskPending: return "task.pending";
        case EventKindV3::TaskCompleted: return "task.completed";
        case EventKindV3::TaskFailed: return "task.failed";
        case EventKindV3::TaskCancelled: return "task.cancelled";
    }
    return "unknown";
}

std::optional<EventKindV3> EventKindV3FromName(std::string_view name) {
    for (EventKindV3 kind : AllEventKindsV3()) {
        if (std::string_view(EventKindV3Name(kind)) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

const std::vector<EventKindV3>& AllEventKindsV3() {
    static const std::vector<EventKindV3> kAll = [] {
        std::vector<EventKindV3> all = {
            EventKindV3::SessionStarted,
            EventKindV3::SessionEnded,
            EventKindV3::SystemChange,
            EventKindV3::ContextSystemApplied,
            EventKindV3::ContextInputApplied,
            EventKindV3::ContextToolPreviewsReduced,
            EventKindV3::ModelRequestPrepared,
            EventKindV3::ModelRequestSent,
            EventKindV3::ModelRequestFailed,
            EventKindV3::ModelResponseStarted,
            EventKindV3::ModelResponseDelta,
            EventKindV3::ModelResponseCompleted,
            EventKindV3::ModelResponseFailed,
            EventKindV3::ModelResponseCancelled,
            EventKindV3::ModelUsageAppended,
            EventKindV3::CompactRequested,
            EventKindV3::CompactPending,
            EventKindV3::CompactStarted,
            EventKindV3::CompactValidationStarted,
            EventKindV3::CompactValidationCompleted,
            EventKindV3::CompactApplied,
            EventKindV3::CompactFailed,
            EventKindV3::CompactCancelled,
            EventKindV3::CompactRejected,
            EventKindV3::ToolExecutionPending,
            EventKindV3::ToolExecutionStarted,
            EventKindV3::ToolExecutionWaiting,
            EventKindV3::ToolExecutionResumed,
            EventKindV3::ToolExecutionFinished,
            EventKindV3::ToolExecutionFailed,
            EventKindV3::ToolExecutionCancelled,
            EventKindV3::ToolExecutionRejected,
            EventKindV3::ToolExecutionUnknown,
            EventKindV3::ToolResultPersisted,
            EventKindV3::ToolResultPersistFailed,
            EventKindV3::ToolResultSelected,
            EventKindV3::HookDispatchRequested,
            EventKindV3::HookPending,
            EventKindV3::HookStarted,
            EventKindV3::HookCompleted,
            EventKindV3::HookFailed,
            EventKindV3::HookCancelled,
            EventKindV3::HookUnknown,
            EventKindV3::HookSkipped,
            EventKindV3::HookEffectsApplied,
            EventKindV3::HookEffectsRejected,
            EventKindV3::CommandReceived,
            EventKindV3::CommandPending,
            EventKindV3::CommandStarted,
            EventKindV3::CommandCompleted,
            EventKindV3::CommandFailed,
            EventKindV3::CommandRejected,
            EventKindV3::CommandCancelled,
            EventKindV3::CommandUnknown,
            EventKindV3::InputReceived,
            EventKindV3::InputEnqueued,
            EventKindV3::InputAdmitted,
            EventKindV3::InputSuperseded,
            EventKindV3::TitleRequested,
            EventKindV3::TitleExtracted,
            EventKindV3::SessionTitleApplied,
            EventKindV3::ResumeSourceAttached,
            EventKindV3::SubagentSpawnRequested,
            EventKindV3::SubagentLinked,
            EventKindV3::SubagentObserved,
            EventKindV3::SubagentSpawnFailed,
            EventKindV3::TaskStarted,
            EventKindV3::TaskPending,
            EventKindV3::TaskCompleted,
            EventKindV3::TaskFailed,
            EventKindV3::TaskCancelled,
        };
        std::sort(all.begin(), all.end(), [](EventKindV3 a, EventKindV3 b) {
            return std::string_view(EventKindV3Name(a)) < std::string_view(EventKindV3Name(b));
        });
        return all;
    }();
    return kAll;
}

std::optional<OpStatus> RequiredStatusForKind(EventKindV3 kind) {
    using K = EventKindV3;
    switch (kind) {
        case K::CompactPending:
        case K::HookPending:
        case K::CommandPending:
        case K::ToolExecutionWaiting:
        case K::TaskPending:
            return OpStatus::Pending;
        case K::CompactStarted:
        case K::HookStarted:
        case K::CommandStarted:
        case K::ToolExecutionStarted:
        case K::ToolExecutionResumed:
        case K::TaskStarted:
            return OpStatus::Running;
        case K::ToolExecutionPending:
            return OpStatus::Pending;
        case K::CompactValidationStarted:
            return OpStatus::Running;
        case K::ModelRequestSent:
        case K::ModelResponseCompleted:
        case K::ToolExecutionFinished:
        case K::HookCompleted:
        case K::CommandCompleted:
        case K::TaskCompleted:
        case K::CompactValidationCompleted:
        case K::CompactApplied:
        case K::SubagentLinked:
            return OpStatus::Done;
        case K::ModelRequestFailed:
        case K::ModelResponseFailed:
        case K::CompactFailed:
        case K::ToolExecutionFailed:
        case K::HookFailed:
        case K::CommandFailed:
        case K::TaskFailed:
        case K::SubagentSpawnFailed:
            return OpStatus::Failed;
        case K::ModelResponseCancelled:
        case K::CompactCancelled:
        case K::ToolExecutionCancelled:
        case K::HookCancelled:
        case K::CommandCancelled:
        case K::TaskCancelled:
            return OpStatus::Cancelled;
        case K::ToolExecutionRejected:
        case K::CompactRejected:
        case K::CommandRejected:
            return OpStatus::Rejected;
        case K::ToolExecutionUnknown:
        case K::HookUnknown:
        case K::CommandUnknown:
            return OpStatus::Unknown;
        default:
            return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// MessageLine JSON
// ---------------------------------------------------------------------------

nlohmann::json MessageLine::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "message";
    json["schemaVersion"] = kSchemaVersion;
    json["sessionId"] = session_id;
    json["runId"] = run_id;
    json["seq"] = seq;
    json["timestamp"] = timestamp;
    json["messageId"] = message_id;
    // turnId 键始终落盘:有值写 string,无值写 null。
    json["turnId"] = turn_id.has_value() ? nlohmann::json(*turn_id) : nlohmann::json(nullptr);
    SetIfPresent(&json, "parentTurnId", parent_turn_id);
    SetIfPresent(&json, "stepId", step_id);
    SetIfPresent(&json, "requestId", request_id);
    SetIfPresent(&json, "actionId", action_id);
    SetIfPresent(&json, "compactId", compact_id);
    json["purpose"] = MessagePurposeName(purpose);
    json["origin"] = MessageOriginName(origin);
    if (display.has_value()) {
        json["display"] = nlohmann::json::object({{"mode", DisplayModeName(*display)}});
    }
    json["message"] = message;
    SetIfPresent(&json, "causedByEventRef", caused_by_event_ref);
    SetIfPresent(&json, "sourceMessageRef", source_message_ref);
    if (system_meta.has_value()) {
        json["systemMeta"] = *system_meta;
    }
    if (completion_status.has_value()) {
        json["completionStatus"] = CompletionStatusName(*completion_status);
    }
    SetIfPresent(&json, "provider", provider);
    SetIfPresent(&json, "wire", wire);
    SetIfPresent(&json, "model", model);
    if (response_model.has_value()) {
        json["responseModel"] = *response_model;
    }
    SetIfPresent(&json, "providerConfigRef", provider_config_ref);
    SetIfPresent(&json, "modelProfileRef", model_profile_ref);
    if (usage.has_value()) {
        json["usage"] = *usage;
    }
    json["prevHash"] = prev_hash;
    json["lineHash"] = line_hash;
    return json;
}

std::optional<MessageLine> MessageLine::FromJsonStrict(const nlohmann::json& json,
                                                       std::string* error_code,
                                                       std::string* message) {
    if (!json.is_object()) {
        *error_code = "schema3.bad_type";
        *message = "message 行应为 JSON object";
        return std::nullopt;
    }
    // 白名单外的键一律拒(未知键策略,schema 文档 §六)。
    static const std::vector<std::string> kAllowedKeys = {
        "type",          "schemaVersion", "sessionId",       "runId",
        "seq",           "timestamp",     "messageId",       "turnId",
        "parentTurnId",  "stepId",        "requestId",       "actionId",
        "compactId",     "purpose",       "origin",          "display",
        "message",       "causedByEventRef", "sourceMessageRef", "systemMeta",
        "completionStatus", "provider",   "wire",            "model",
        "responseModel", "providerConfigRef", "modelProfileRef", "usage",
        "prevHash",      "lineHash",
    };
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (std::find(kAllowedKeys.begin(), kAllowedKeys.end(), it.key()) == kAllowedKeys.end()) {
            *error_code = "schema3.unknown_key";
            *message = "message 行未知键: " + it.key();
            return std::nullopt;
        }
    }
    MessageLine line;
    if (!json.contains("type") || !json.at("type").is_string() ||
        json.at("type").get<std::string>() != "message") {
        *error_code = "schema3.bad_type";
        *message = "type 应为 message";
        return std::nullopt;
    }
    if (!json.contains("schemaVersion") || !json.at("schemaVersion").is_number_integer() ||
        json.at("schemaVersion").get<int>() != kSchemaVersion) {
        *error_code = "schema3.bad_version";
        *message = "schemaVersion 应为 3";
        return std::nullopt;
    }
    if (!GetRequired<std::string>(json, "sessionId", &line.session_id, error_code, message,
                                  "message") ||
        !GetRequired<std::string>(json, "runId", &line.run_id, error_code, message, "message") ||
        !GetRequired<std::uint64_t>(json, "seq", &line.seq, error_code, message, "message") ||
        !GetRequired<std::string>(json, "timestamp", &line.timestamp, error_code, message,
                                  "message") ||
        !GetRequired<std::string>(json, "messageId", &line.message_id, error_code, message,
                                  "message") ||
        !GetRequired<std::string>(json, "prevHash", &line.prev_hash, error_code, message,
                                  "message") ||
        !GetRequired<std::string>(json, "lineHash", &line.line_hash, error_code, message,
                                  "message")) {
        return std::nullopt;
    }
    // turnId:键必须出现(可 null)。
    if (!json.contains("turnId")) {
        *error_code = "schema3.missing_field";
        *message = "message 行缺字段: turnId(键必须出现,可 null)";
        return std::nullopt;
    }
    if (!GetOptionalString(json, "turnId", &line.turn_id)) {
        *error_code = "schema3.bad_type";
        *message = "turnId 应为 string 或 null";
        return std::nullopt;
    }
    std::optional<std::string>* optional_string_fields[] = {
        &line.parent_turn_id, &line.step_id,
        &line.request_id,     &line.action_id,
        &line.compact_id,     &line.caused_by_event_ref,
        &line.source_message_ref, &line.provider,
        &line.wire,           &line.model,
        &line.provider_config_ref, &line.model_profile_ref,
    };
    const char* optional_string_keys[] = {
        "parentTurnId",  "stepId",        "requestId",      "actionId",
        "compactId",     "causedByEventRef", "sourceMessageRef", "provider",
        "wire",          "model",         "providerConfigRef", "modelProfileRef",
    };
    for (std::size_t i = 0; i < std::size(optional_string_keys); ++i) {
        if (!GetOptionalString(json, optional_string_keys[i], optional_string_fields[i])) {
            *error_code = "schema3.bad_type";
            *message = std::string("message 行字段应为 string 或 null: ") + optional_string_keys[i];
            return std::nullopt;
        }
    }
    if (!ParseEnumField<MessagePurpose>(json, "purpose", MessagePurposeFromName, &line.purpose,
                                        error_code, message, "message") ||
        !ParseEnumField<MessageOrigin>(json, "origin", MessageOriginFromName, &line.origin,
                                       error_code, message, "message") ||
        !ParseEnumField<CompletionStatus>(json, "completionStatus", CompletionStatusFromName,
                                          &line.completion_status, error_code, message,
                                          "message")) {
        return std::nullopt;
    }
    if (auto it = json.find("display"); it != json.end()) {
        if (it->is_object() && it->contains("mode") && (*it)["mode"].is_string()) {
            auto mode = DisplayModeFromName((*it)["mode"].get<std::string>());
            if (!mode.has_value()) {
                *error_code = "schema3.bad_enum";
                *message = "display.mode 未知值";
                return std::nullopt;
            }
            line.display = *mode;
        } else {
            *error_code = "schema3.bad_type";
            *message = "display 应为 {mode: ...}";
            return std::nullopt;
        }
    }
    if (auto it = json.find("message"); it != json.end() && it->is_object()) {
        line.message = *it;
    } else {
        *error_code = "schema3.missing_field";
        *message = "message 行缺 message 本体";
        return std::nullopt;
    }
    if (auto it = json.find("systemMeta"); it != json.end()) {
        if (!it->is_object()) {
            *error_code = "schema3.bad_type";
            *message = "systemMeta 应为 object";
            return std::nullopt;
        }
        line.system_meta = *it;
    }
    if (auto it = json.find("responseModel"); it != json.end()) {
        if (!it->is_null() && !it->is_string()) {
            *error_code = "schema3.bad_type";
            *message = "responseModel 应为 string 或 null";
            return std::nullopt;
        }
        line.response_model = *it;
    }
    if (auto it = json.find("usage"); it != json.end()) {
        if (!it->is_null() && !it->is_object()) {
            *error_code = "schema3.bad_type";
            *message = "usage 应为 object 或 null";
            return std::nullopt;
        }
        line.usage = *it;
    }
    return line;
}

// ---------------------------------------------------------------------------
// EventLine JSON
// ---------------------------------------------------------------------------

nlohmann::json EventLine::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["type"] = "event";
    json["schemaVersion"] = kSchemaVersion;
    json["sessionId"] = session_id;
    json["runId"] = run_id;
    json["seq"] = seq;
    json["timestamp"] = timestamp;
    json["eventId"] = event_id;
    json["kind"] = EventKindV3Name(kind);
    if (status.has_value()) {
        json["status"] = OpStatusName(*status);
    }
    SetIfPresent(&json, "turnId", turn_id);
    SetIfPresent(&json, "parentTurnId", parent_turn_id);
    SetIfPresent(&json, "stepId", step_id);
    SetIfPresent(&json, "requestId", request_id);
    SetIfPresent(&json, "actionId", action_id);
    SetIfPresent(&json, "compactId", compact_id);
    SetIfPresent(&json, "commandId", command_id);
    SetIfPresent(&json, "hookDispatchId", hook_dispatch_id);
    SetIfPresent(&json, "taskId", task_id);
    SetIfPresent(&json, "titleGenerationId", title_generation_id);
    json["payload"] = payload;
    if (effects.has_value()) {
        json["effects"] = *effects;
    }
    if (effect_refs.has_value()) {
        json["effectRefs"] = *effect_refs;
    }
    json["prevHash"] = prev_hash;
    json["lineHash"] = line_hash;
    return json;
}

std::optional<EventLine> EventLine::FromJsonStrict(const nlohmann::json& json,
                                                   std::string* error_code,
                                                   std::string* message) {
    if (!json.is_object()) {
        *error_code = "schema3.bad_type";
        *message = "event 行应为 JSON object";
        return std::nullopt;
    }
    static const std::vector<std::string> kAllowedKeys = {
        "type",       "schemaVersion", "sessionId",   "runId",       "seq",
        "timestamp",  "eventId",       "kind",        "status",      "turnId",
        "parentTurnId", "stepId",      "requestId",   "actionId",    "compactId",
        "commandId",  "hookDispatchId", "taskId",     "titleGenerationId",
        "payload",    "effects",       "effectRefs",  "prevHash",    "lineHash",
    };
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (std::find(kAllowedKeys.begin(), kAllowedKeys.end(), it.key()) == kAllowedKeys.end()) {
            *error_code = "schema3.unknown_key";
            *message = "event 行未知键: " + it.key();
            return std::nullopt;
        }
    }
    EventLine line;
    if (!json.is_object() || !json.contains("type") ||
        !json.at("type").is_string() || json.at("type").get<std::string>() != "event") {
        *error_code = "schema3.bad_type";
        *message = "type 应为 event";
        return std::nullopt;
    }
    if (!json.contains("schemaVersion") || !json.at("schemaVersion").is_number_integer() ||
        json.at("schemaVersion").get<int>() != kSchemaVersion) {
        *error_code = "schema3.bad_version";
        *message = "schemaVersion 应为 3";
        return std::nullopt;
    }
    if (!GetRequired<std::string>(json, "sessionId", &line.session_id, error_code, message,
                                  "event") ||
        !GetRequired<std::string>(json, "runId", &line.run_id, error_code, message, "event") ||
        !GetRequired<std::uint64_t>(json, "seq", &line.seq, error_code, message, "event") ||
        !GetRequired<std::string>(json, "timestamp", &line.timestamp, error_code, message,
                                  "event") ||
        !GetRequired<std::string>(json, "eventId", &line.event_id, error_code, message, "event") ||
        !GetRequired<std::string>(json, "prevHash", &line.prev_hash, error_code, message,
                                  "event") ||
        !GetRequired<std::string>(json, "lineHash", &line.line_hash, error_code, message,
                                  "event")) {
        return std::nullopt;
    }
    if (auto it = json.find("kind"); it != json.end() && it->is_string()) {
        auto kind = EventKindV3FromName(it->get<std::string>());
        if (!kind.has_value()) {
            *error_code = "schema3.unknown_kind";
            *message = "未知事件 kind: " + it->get<std::string>();
            return std::nullopt;
        }
        line.kind = *kind;
    } else {
        *error_code = "schema3.missing_field";
        *message = "event 行缺 kind";
        return std::nullopt;
    }
    if (!ParseEnumField<OpStatus>(json, "status", OpStatusFromName, &line.status, error_code,
                                  message, "event")) {
        return std::nullopt;
    }
    std::optional<std::string>* targets[] = {
        &line.turn_id,      &line.parent_turn_id,   &line.step_id,
        &line.request_id,   &line.action_id,        &line.compact_id,
        &line.command_id,   &line.hook_dispatch_id, &line.task_id,
        &line.title_generation_id,
    };
    const char* keys[] = {"turnId", "parentTurnId", "stepId", "requestId", "actionId",
                          "compactId", "commandId", "hookDispatchId", "taskId",
                          "titleGenerationId"};
    for (std::size_t i = 0; i < std::size(keys); ++i) {
        if (!GetOptionalString(json, keys[i], targets[i])) {
            *error_code = "schema3.bad_type";
            *message = std::string("event 行字段应为 string 或 null: ") + keys[i];
            return std::nullopt;
        }
    }
    if (auto it = json.find("payload"); it != json.end() && it->is_object()) {
        line.payload = *it;
    } else {
        *error_code = "schema3.missing_field";
        *message = "event 行缺 payload(可为 {})";
        return std::nullopt;
    }
    if (auto it = json.find("effects"); it != json.end()) {
        if (!it->is_array()) {
            *error_code = "schema3.bad_type";
            *message = "effects 应为数组";
            return std::nullopt;
        }
        line.effects = it->get<std::vector<nlohmann::json>>();
    }
    if (auto it = json.find("effectRefs"); it != json.end()) {
        if (!it->is_array()) {
            *error_code = "schema3.bad_type";
            *message = "effectRefs 应为数组";
            return std::nullopt;
        }
        line.effect_refs = it->get<std::vector<nlohmann::json>>();
    }
    return line;
}

// ---------------------------------------------------------------------------
// 哈希
// ---------------------------------------------------------------------------

std::string ComputeLineHash(std::string_view prev_hash,
                            std::string_view canonical_line_without_hash_keys) {
    return ComputeEventHash(prev_hash, canonical_line_without_hash_keys);
}

bool IsHex64(std::string_view value) {
    if (value.size() != 64) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

}  // namespace lubancode::trajectory::v3
