#include "trajectory/event.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace lubancode::trajectory {
namespace {

template <typename Enum, std::size_t N>
const char* LookupName(const std::array<std::pair<Enum, const char*>, N>& table, Enum value) {
    for (const auto& [candidate, name] : table) {
        if (candidate == value) {
            return name;
        }
    }
    return nullptr;
}

template <typename Enum, std::size_t N>
std::optional<Enum> LookupValue(const std::array<std::pair<Enum, const char*>, N>& table,
                                std::string_view name) {
    for (const auto& [value, candidate] : table) {
        if (name == candidate) {
            return value;
        }
    }
    return std::nullopt;
}

constexpr std::array<std::pair<RunKind, const char*>, 6> kRunKindNames{{
    {RunKind::MainSession, "main_session"},
    {RunKind::Subagent, "subagent"},
    {RunKind::Workflow, "workflow"},
    {RunKind::WorkflowNode, "workflow_node"},
    {RunKind::Goal, "goal"},
    {RunKind::Loop, "loop"},
}};

constexpr std::array<std::pair<Plane, const char*>, 4> kPlaneNames{{
    {Plane::Conversation, "conversation"},
    {Plane::Execution, "execution"},
    {Plane::Control, "control"},
    {Plane::Evidence, "evidence"},
}};

constexpr std::array<std::pair<Actor, const char*>, 5> kActorNames{{
    {Actor::User, "user"},
    {Actor::Model, "model"},
    {Actor::Tool, "tool"},
    {Actor::Host, "host"},
    {Actor::Verifier, "verifier"},
}};

constexpr std::array<std::pair<Origin, const char*>, 19> kOriginNames{{
    {Origin::ExternalUser, "external_user"},
    {Origin::QueuedUser, "queued_user"},
    {Origin::PeerAgent, "peer_agent"},
    {Origin::ScheduledHost, "scheduled_host"},
    {Origin::GoalContinuation, "goal_continuation"},
    {Origin::ProviderModel, "provider_model"},
    {Origin::BuiltinTool, "builtin_tool"},
    {Origin::McpTool, "mcp_tool"},
    {Origin::PluginTool, "plugin_tool"},
    {Origin::LspTool, "lsp_tool"},
    {Origin::SubagentTool, "subagent_tool"},
    {Origin::Hook, "hook"},
    {Origin::MemoryRecall, "memory_recall"},
    {Origin::BackgroundCompletion, "background_completion"},
    {Origin::AgentRoster, "agent_roster"},
    {Origin::BudgetGuard, "budget_guard"},
    {Origin::CompactRuntime, "compact_runtime"},
    {Origin::RecoveryRuntime, "recovery_runtime"},
    {Origin::VerifierHost, "verifier_host"},
}};

constexpr std::array<std::pair<Visibility, const char*>, 6> kVisibilityNames{{
    {Visibility::ModelInput, "model_input"},
    {Visibility::ModelOutput, "model_output"},
    {Visibility::ToolInput, "tool_input"},
    {Visibility::ToolOutput, "tool_output"},
    {Visibility::HostOnly, "host_only"},
    {Visibility::UserVisible, "user_visible"},
}};

constexpr std::array<std::pair<TrainingPolicy, const char*>, 4> kTrainingPolicyNames{{
    {TrainingPolicy::Include, "include"},
    {TrainingPolicy::Metadata, "metadata"},
    {TrainingPolicy::Exclude, "exclude"},
    {TrainingPolicy::Review, "review"},
}};

constexpr std::array<std::pair<Durability, const char*>, 3> kDurabilityNames{{
    {Durability::Buffered, "buffered"},
    {Durability::ProcessCrash, "process_crash"},
    {Durability::PowerLoss, "power_loss"},
}};

// kind 信息表:名字、固定 plane、三档 id 要求、是否 main stream 专属。
// 顺序与 EventKind 枚举声明一致,两处对不上会在启动断言里炸出来。
// plane 归面照 §4.2:conversation=输入/宿主注入/模型输出/回喂结果;
// execution=provider 请求与工具执行;evidence=验证与终裁;其余 control。
constexpr std::array<EventKindInfo, 67> kKindInfos{{
    {"run.started", Plane::Control, IdNeed::Forbidden, IdNeed::Forbidden, IdNeed::Forbidden, false},
    {"run.environment.captured", Plane::Execution, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"run.completed", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden, false},
    {"run.failed", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden, false},
    {"run.cancelled", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden, false},
    {"session.clear_requested", Plane::Control, IdNeed::Forbidden, IdNeed::Forbidden, IdNeed::Forbidden,
     true},
    {"session.ended", Plane::Control, IdNeed::Forbidden, IdNeed::Forbidden, IdNeed::Forbidden, true},
    {"turn.started", Plane::Control, IdNeed::Required, IdNeed::Forbidden, IdNeed::Forbidden, false},
    {"turn.completed", Plane::Control, IdNeed::Required, IdNeed::Optional, IdNeed::Forbidden, false},
    {"turn.failed", Plane::Control, IdNeed::Required, IdNeed::Optional, IdNeed::Forbidden, false},
    {"turn.cancelled", Plane::Control, IdNeed::Required, IdNeed::Optional, IdNeed::Forbidden, false},
    {"input.received", Plane::Conversation, IdNeed::Required, IdNeed::Forbidden, IdNeed::Forbidden,
     false},
    {"context.attached", Plane::Conversation, IdNeed::Required, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"context.detached", Plane::Conversation, IdNeed::Required, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"model.request.prepared", Plane::Execution, IdNeed::Required, IdNeed::Required, IdNeed::Forbidden,
     false},
    {"model.request.sent", Plane::Execution, IdNeed::Required, IdNeed::Required, IdNeed::Forbidden,
     false},
    {"model.output.completed", Plane::Conversation, IdNeed::Required, IdNeed::Required,
     IdNeed::Forbidden, false},
    {"model.output.failed", Plane::Conversation, IdNeed::Required, IdNeed::Required, IdNeed::Forbidden,
     false},
    {"model.output.cancelled", Plane::Conversation, IdNeed::Required, IdNeed::Required,
     IdNeed::Forbidden, false},
    {"tool.execution.planned", Plane::Execution, IdNeed::Required, IdNeed::Required, IdNeed::Required,
     false},
    {"tool.input.effective", Plane::Execution, IdNeed::Required, IdNeed::Required, IdNeed::Required,
     false},
    {"tool.execution.started", Plane::Execution, IdNeed::Required, IdNeed::Required, IdNeed::Required,
     false},
    {"tool.execution.finished", Plane::Execution, IdNeed::Required, IdNeed::Required, IdNeed::Required,
     false},
    {"tool.execution.failed", Plane::Execution, IdNeed::Required, IdNeed::Required, IdNeed::Required,
     false},
    {"tool.execution.cancelled", Plane::Execution, IdNeed::Required, IdNeed::Required,
     IdNeed::Required, false},
    {"tool.execution.unknown", Plane::Execution, IdNeed::Required, IdNeed::Required,
     IdNeed::Required, false},
    {"tool.result.committed", Plane::Conversation, IdNeed::Required, IdNeed::Required,
     IdNeed::Required, false},
    {"control.command.requested", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.command.completed", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.command.failed", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"control.command.cancelled", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.command.rejected", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.title.changed", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"control.cwd.changed", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"control.mode.changed", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"control.context_window.changed", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.checkpoint.created", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.queue.item.enqueued", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.queue.item.dequeued", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.queue.item.cancelled", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.queue.item.expired", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"control.queue.snapshot", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"control.approval.requested", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Optional, false},
    {"control.approval.resolved", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Optional, false},
    {"control.approval.expired", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Optional, false},
    {"control.cancellation.requested", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Optional, false},
    {"control.cancellation.applied", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Optional, false},
    {"compact.requested", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"compact.request.prepared", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"compact.output.generated", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"compact.validation.completed", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"compact.applied", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden, false},
    {"compact.failed", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden, false},
    {"compact.cancelled", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"compact.rejected", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"record.selection.started", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"record.selection.paused", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"record.selection.resumed", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"record.selection.note_added", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"record.selection.completed", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"record.selection.cancelled", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"record.selection.interrupted", Plane::Control, IdNeed::Optional, IdNeed::Optional,
     IdNeed::Forbidden, false},
    {"resume.source.attached", Plane::Control, IdNeed::Optional, IdNeed::Optional, IdNeed::Forbidden,
     false},
    {"verification.started", Plane::Evidence, IdNeed::Required, IdNeed::Optional, IdNeed::Optional,
     false},
    {"verification.recorded", Plane::Evidence, IdNeed::Required, IdNeed::Optional, IdNeed::Optional,
     false},
    {"verification.invalidated", Plane::Evidence, IdNeed::Required, IdNeed::Optional,
     IdNeed::Optional, false},
    {"outcome.assessed", Plane::Evidence, IdNeed::Required, IdNeed::Optional, IdNeed::Optional,
     false},
}};

static_assert(kKindInfos.size() == 67, "kind 信息表与枚举须同长");
static_assert(static_cast<std::size_t>(EventKind::OutcomeAssessed) + 1 == kKindInfos.size(),
              "kind 信息表顺序须与枚举声明一致");

}  // namespace

const char* RunKindName(RunKind value) { return LookupName(kRunKindNames, value); }
std::optional<RunKind> RunKindFromName(std::string_view name) {
    return LookupValue(kRunKindNames, name);
}
const char* PlaneName(Plane value) { return LookupName(kPlaneNames, value); }
std::optional<Plane> PlaneFromName(std::string_view name) { return LookupValue(kPlaneNames, name); }
const char* ActorName(Actor value) { return LookupName(kActorNames, value); }
std::optional<Actor> ActorFromName(std::string_view name) {
    return LookupValue(kActorNames, name);
}
const char* OriginName(Origin value) { return LookupName(kOriginNames, value); }
std::optional<Origin> OriginFromName(std::string_view name) {
    return LookupValue(kOriginNames, name);
}
const char* VisibilityName(Visibility value) { return LookupName(kVisibilityNames, value); }
std::optional<Visibility> VisibilityFromName(std::string_view name) {
    return LookupValue(kVisibilityNames, name);
}
const char* TrainingPolicyName(TrainingPolicy value) {
    return LookupName(kTrainingPolicyNames, value);
}
std::optional<TrainingPolicy> TrainingPolicyFromName(std::string_view name) {
    return LookupValue(kTrainingPolicyNames, name);
}
const char* DurabilityName(Durability value) { return LookupName(kDurabilityNames, value); }
std::optional<Durability> DurabilityFromName(std::string_view name) {
    return LookupValue(kDurabilityNames, name);
}

const char* EventKindName(EventKind kind) {
    const std::size_t index = static_cast<std::size_t>(kind);
    if (index >= kKindInfos.size()) {
        return "";
    }
    return kKindInfos[index].name;
}

std::optional<EventKind> EventKindFromName(std::string_view name) {
    for (std::size_t i = 0; i < kKindInfos.size(); ++i) {
        if (name == kKindInfos[i].name) {
            return static_cast<EventKind>(i);
        }
    }
    return std::nullopt;
}

const std::vector<EventKind>& AllEventKinds() {
    static const std::vector<EventKind> kinds = [] {
        std::vector<EventKind> result;
        result.reserve(kKindInfos.size());
        for (std::size_t i = 0; i < kKindInfos.size(); ++i) {
            result.push_back(static_cast<EventKind>(i));
        }
        return result;
    }();
    return kinds;
}

const EventKindInfo& EventKindInfoOf(EventKind kind) {
    return kKindInfos[static_cast<std::size_t>(kind)];
}

bool IsValidActorOrigin(Actor actor, Origin origin) {
    switch (actor) {
        case Actor::User:
            // 真人与排队输入才挂 user;peer 消息不冒充真人(§2.7)。
            return origin == Origin::ExternalUser || origin == Origin::QueuedUser;
        case Actor::Model:
            return origin == Origin::ProviderModel;
        case Actor::Tool:
            return origin == Origin::BuiltinTool || origin == Origin::McpTool ||
                   origin == Origin::PluginTool || origin == Origin::LspTool ||
                   origin == Origin::SubagentTool;
        case Actor::Host:
            return origin == Origin::PeerAgent || origin == Origin::ScheduledHost ||
                   origin == Origin::GoalContinuation || origin == Origin::Hook ||
                   origin == Origin::MemoryRecall || origin == Origin::BackgroundCompletion ||
                   origin == Origin::AgentRoster || origin == Origin::BudgetGuard ||
                   origin == Origin::CompactRuntime || origin == Origin::RecoveryRuntime;
        case Actor::Verifier:
            return origin == Origin::VerifierHost;
    }
    return false;
}

namespace {

bool ParseStringField(const nlohmann::json& json, const char* key, std::string* out,
                      std::string* error_code, std::string* message) {
    const auto it = json.find(key);
    if (it == json.end()) {
        *error_code = "schema.missing_field";
        *message = std::string("缺字段: ") + key;
        return false;
    }
    if (!it->is_string()) {
        *error_code = "schema.bad_type";
        *message = std::string("字段须是字符串: ") + key;
        return false;
    }
    *out = it->get<std::string>();
    return true;
}

bool ParseOptionalString(const nlohmann::json& json, const char* key, std::optional<std::string>* out,
                         std::string* error_code, std::string* message) {
    const auto it = json.find(key);
    if (it == json.end() || it->is_null()) {
        *out = std::nullopt;
        return true;
    }
    if (!it->is_string()) {
        *error_code = "schema.bad_type";
        *message = std::string("字段须是字符串: ") + key;
        return false;
    }
    *out = it->get<std::string>();
    return true;
}

}  // namespace

nlohmann::json EventEnvelope::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = std::string(kEventSchema);
    json["schema_version"] = schema_version;
    json["workspace_key"] = workspace_key;
    json["session_id"] = session_id;
    json["run_id"] = run_id;
    json["run_kind"] = RunKindName(run_kind);
    json["event_id"] = event_id;
    json["seq"] = seq;
    if (turn_id.has_value()) {
        json["turn_id"] = *turn_id;
    }
    if (request_id.has_value()) {
        json["request_id"] = *request_id;
    }
    if (call_id.has_value()) {
        json["call_id"] = *call_id;
    }
    json["kind"] = EventKindName(kind);
    json["plane"] = PlaneName(plane);
    json["actor"] = ActorName(actor);
    json["origin"] = OriginName(origin);
    nlohmann::json visibility_json = nlohmann::json::array();
    for (const Visibility value : visibility) {
        visibility_json.push_back(VisibilityName(value));
    }
    json["visibility"] = std::move(visibility_json);
    json["training_policy"] = TrainingPolicyName(training_policy);
    if (causation_id.has_value()) {
        json["causation_id"] = *causation_id;
    }
    if (correlation_id.has_value()) {
        json["correlation_id"] = *correlation_id;
    }
    if (relations.is_object() && !relations.empty()) {
        json["relations"] = relations;
    }
    json["wall_time_ms"] = wall_time_ms;
    json["monotonic_ns"] = monotonic_ns;
    json["payload"] = payload;
    json["prev_hash"] = prev_hash;
    json["event_hash"] = event_hash;
    return json;
}

std::optional<EventEnvelope> EventEnvelope::FromJsonStrict(const nlohmann::json& json,
                                                           std::string* error_code,
                                                           std::string* message) {
    if (!json.is_object()) {
        *error_code = "schema.bad_type";
        *message = "事件信封须是 JSON object";
        return std::nullopt;
    }
    // 未知键拒绝:信封是封闭合同,多出来的键不算"兼容",算读不懂。
    static constexpr std::array<std::string_view, 25> kKnownKeys{{
        "schema",          "schema_version", "workspace_key", "session_id",
        "run_id",          "run_kind",        "event_id",      "seq",
        "turn_id",         "request_id",      "call_id",       "kind",
        "plane",           "actor",           "origin",        "visibility",
        "training_policy", "causation_id",    "correlation_id", "wall_time_ms",
        "monotonic_ns",    "payload",         "prev_hash",     "event_hash",
        "relations",
    }};
    for (auto it = json.begin(); it != json.end(); ++it) {
        const bool known = std::any_of(kKnownKeys.begin(), kKnownKeys.end(),
                                       [&](std::string_view key) { return it.key() == key; });
        if (!known) {
            *error_code = "schema.unknown_field";
            *message = "信封未知字段: " + it.key();
            return std::nullopt;
        }
    }
    // relations(可选):键集封闭,只认 §6.3 的六项。
    EventEnvelope envelope;
    {
        const auto it = json.find("relations");
        if (it != json.end() && !it->is_null()) {
            if (!it->is_object()) {
                *error_code = "schema.bad_type";
                *message = "relations 须是 object";
                return std::nullopt;
            }
            static constexpr std::array<std::string_view, 6> kRelationKeys{{
                "retry_of", "compensates", "parent_call_id",
                "parent_run_id", "child_run_id", "blocked_by",
            }};
            for (auto rel = it->begin(); rel != it->end(); ++rel) {
                const bool known = std::any_of(kRelationKeys.begin(), kRelationKeys.end(),
                                               [&](std::string_view key) { return rel.key() == key; });
                if (!known) {
                    *error_code = "schema.unknown_field";
                    *message = "relations 未知字段: " + rel.key();
                    return std::nullopt;
                }
            }
            envelope.relations = *it;
        }
    }

    // schema 名与版本。
    {
        const auto it = json.find("schema");
        if (it == json.end()) {
            *error_code = "schema.missing_field";
            *message = "缺字段: schema";
            return std::nullopt;
        }
        if (!it->is_string() || it->get<std::string>() != kEventSchema) {
            *error_code = "schema.bad_schema_name";
            *message = "schema 名不是 " + std::string(kEventSchema);
            return std::nullopt;
        }
    }
    {
        const auto it = json.find("schema_version");
        if (it == json.end() || !it->is_number_integer()) {
            *error_code = "schema.missing_field";
            *message = "缺字段或非整数: schema_version";
            return std::nullopt;
        }
        const int version = it->get<int>();
        if (version != kEnvelopeSchemaVersion) {
            // 不支持的版本明拒,不猜着读(§2.7 第 8 条)。
            *error_code = "schema.unsupported_version";
            *message = "不支持的 schema_version: " + std::to_string(version);
            return std::nullopt;
        }
        envelope.schema_version = version;
    }
    if (!ParseStringField(json, "workspace_key", &envelope.workspace_key, error_code, message)) {
        return std::nullopt;
    }
    if (!ParseStringField(json, "session_id", &envelope.session_id, error_code, message)) {
        return std::nullopt;
    }
    if (!ParseStringField(json, "run_id", &envelope.run_id, error_code, message)) {
        return std::nullopt;
    }
    {
        std::string name;
        if (!ParseStringField(json, "run_kind", &name, error_code, message)) {
            return std::nullopt;
        }
        const auto value = RunKindFromName(name);
        if (!value.has_value()) {
            *error_code = "schema.bad_enum";
            *message = "未知 run_kind: " + name;
            return std::nullopt;
        }
        envelope.run_kind = *value;
    }
    if (!ParseStringField(json, "event_id", &envelope.event_id, error_code, message)) {
        return std::nullopt;
    }
    {
        const auto it = json.find("seq");
        if (it == json.end() || !it->is_number_unsigned()) {
            *error_code = "schema.missing_field";
            *message = "缺字段或非无符号整数: seq";
            return std::nullopt;
        }
        envelope.seq = it->get<std::uint64_t>();
    }
    if (!ParseOptionalString(json, "turn_id", &envelope.turn_id, error_code, message) ||
        !ParseOptionalString(json, "request_id", &envelope.request_id, error_code, message) ||
        !ParseOptionalString(json, "call_id", &envelope.call_id, error_code, message) ||
        !ParseOptionalString(json, "causation_id", &envelope.causation_id, error_code, message) ||
        !ParseOptionalString(json, "correlation_id", &envelope.correlation_id, error_code, message)) {
        return std::nullopt;
    }
    {
        std::string name;
        if (!ParseStringField(json, "kind", &name, error_code, message)) {
            return std::nullopt;
        }
        const auto value = EventKindFromName(name);
        if (!value.has_value()) {
            *error_code = "schema.unknown_event_kind";
            *message = "未知事件种类: " + name;
            return std::nullopt;
        }
        envelope.kind = *value;
    }
    {
        std::string name;
        if (!ParseStringField(json, "plane", &name, error_code, message)) {
            return std::nullopt;
        }
        const auto value = PlaneFromName(name);
        if (!value.has_value()) {
            *error_code = "schema.bad_enum";
            *message = "未知 plane: " + name;
            return std::nullopt;
        }
        envelope.plane = *value;
    }
    {
        std::string name;
        if (!ParseStringField(json, "actor", &name, error_code, message)) {
            return std::nullopt;
        }
        const auto value = ActorFromName(name);
        if (!value.has_value()) {
            *error_code = "schema.bad_enum";
            *message = "未知 actor: " + name;
            return std::nullopt;
        }
        envelope.actor = *value;
    }
    {
        std::string name;
        if (!ParseStringField(json, "origin", &name, error_code, message)) {
            return std::nullopt;
        }
        const auto value = OriginFromName(name);
        if (!value.has_value()) {
            *error_code = "schema.bad_enum";
            *message = "未知 origin: " + name;
            return std::nullopt;
        }
        envelope.origin = *value;
    }
    {
        const auto it = json.find("visibility");
        if (it == json.end() || !it->is_array() || it->empty()) {
            *error_code = "schema.missing_field";
            *message = "缺字段或为空数组: visibility";
            return std::nullopt;
        }
        for (const auto& item : *it) {
            if (!item.is_string()) {
                *error_code = "schema.bad_type";
                *message = "visibility 项须是字符串";
                return std::nullopt;
            }
            const auto value = VisibilityFromName(item.get<std::string>());
            if (!value.has_value()) {
                *error_code = "schema.bad_enum";
                *message = "未知 visibility: " + item.get<std::string>();
                return std::nullopt;
            }
            if (std::find(envelope.visibility.begin(), envelope.visibility.end(), *value) !=
                envelope.visibility.end()) {
                *error_code = "schema.duplicate_visibility";
                *message = "visibility 重复: " + item.get<std::string>();
                return std::nullopt;
            }
            envelope.visibility.push_back(*value);
        }
    }
    {
        std::string name;
        if (!ParseStringField(json, "training_policy", &name, error_code, message)) {
            return std::nullopt;
        }
        const auto value = TrainingPolicyFromName(name);
        if (!value.has_value()) {
            *error_code = "schema.bad_enum";
            *message = "未知 training_policy: " + name;
            return std::nullopt;
        }
        envelope.training_policy = *value;
    }
    for (const char* time_key : {"wall_time_ms", "monotonic_ns"}) {
        const auto it = json.find(time_key);
        if (it == json.end() || !it->is_number_integer()) {
            *error_code = "schema.missing_field";
            *message = std::string("缺字段或非整数: ") + time_key;
            return std::nullopt;
        }
        if (std::string_view(time_key) == "wall_time_ms") {
            envelope.wall_time_ms = it->get<std::int64_t>();
        } else {
            envelope.monotonic_ns = it->get<std::int64_t>();
        }
    }
    {
        const auto it = json.find("payload");
        if (it == json.end()) {
            *error_code = "schema.missing_field";
            *message = "缺字段: payload";
            return std::nullopt;
        }
        if (!it->is_object()) {
            *error_code = "schema.bad_type";
            *message = "payload 须是 object";
            return std::nullopt;
        }
        envelope.payload = *it;
    }
    if (!ParseStringField(json, "prev_hash", &envelope.prev_hash, error_code, message)) {
        return std::nullopt;
    }
    if (!ParseStringField(json, "event_hash", &envelope.event_hash, error_code, message)) {
        return std::nullopt;
    }
    return envelope;
}

std::string FormatEventId(std::string_view run_id, std::uint64_t seq) {
    char tail[16];
    std::snprintf(tail, sizeof(tail), "evt-%08llu", static_cast<unsigned long long>(seq));
    return std::string(run_id) + ":" + tail;
}

bool IsHex64(std::string_view value) {
    if (value.size() != 64) {
        return false;
    }
    for (const char c : value) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) {
            return false;
        }
    }
    return true;
}

nlohmann::json MakeTerminalSealPayload(std::string_view first_event_hash,
                                       std::uint64_t event_count_before_terminal, int schema_version,
                                       std::string_view recorder_version) {
    nlohmann::json payload = nlohmann::json::object();
    payload["first_event_hash"] = std::string(first_event_hash);
    payload["event_count_before_terminal"] = event_count_before_terminal;
    // schema_version 落无符号:类型表按 "u" 冻结,int 直塞会成带符号整数被拒。
    payload["schema_version"] = static_cast<std::uint64_t>(schema_version);
    payload["recorder_version"] = std::string(recorder_version);
    return payload;
}

}  // namespace lubancode::trajectory
