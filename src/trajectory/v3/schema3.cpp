// v3 语义校验实现。
#include "trajectory/v3/schema3.hpp"

#include <unordered_map>
#include <unordered_set>

namespace lubancode::trajectory::v3 {

namespace {

Schema3Error Err(std::string code, std::string message) {
    return Schema3Error{std::move(code), std::move(message)};
}

bool JsonIsNonNegativeInt(const nlohmann::json& value) {
    return value.is_number_unsigned() ||
           (value.is_number_integer() && value.get<std::int64_t>() >= 0);
}

// 按 kind 要求的身份字段(§2.2/§四):kind 前缀 → (信封字段, 是否必选)。
struct IdRequirement {
    const char* envelope_field;
    bool required;
};
std::optional<IdRequirement> IdRequirementForKind(EventKindV3 kind) {
    using K = EventKindV3;
    const auto in = [kind](std::initializer_list<K> ks) {
        for (K k : ks) {
            if (k == kind) return true;
        }
        return false;
    };
    if (in({K::CompactRequested, K::CompactPending, K::CompactStarted, K::CompactValidationStarted,
            K::CompactValidationCompleted, K::CompactApplied, K::CompactFailed,
            K::CompactCancelled, K::CompactRejected})) {
        return IdRequirement{"compactId", true};
    }
    if (in({K::ModelResponseStarted, K::ModelResponseDelta, K::ModelResponseCompleted,
            K::ModelResponseFailed, K::ModelResponseCancelled, K::ModelRequestSent,
            K::ModelRequestFailed})) {
        return IdRequirement{"requestId", true};
    }
    if (kind == K::ModelRequestPrepared || kind == K::ModelUsageAppended) {
        return IdRequirement{"requestId", true};
    }
    if (in({K::ToolExecutionPending, K::ToolExecutionStarted, K::ToolExecutionWaiting,
            K::ToolExecutionResumed, K::ToolExecutionFinished, K::ToolExecutionFailed,
            K::ToolExecutionCancelled, K::ToolExecutionRejected, K::ToolExecutionUnknown,
            K::ToolResultPersisted, K::ToolResultPersistFailed, K::ToolResultSelected})) {
        return IdRequirement{"actionId", true};
    }
    if (in({K::HookDispatchRequested, K::HookPending, K::HookStarted, K::HookCompleted,
            K::HookFailed, K::HookCancelled, K::HookUnknown, K::HookSkipped,
            K::HookEffectsApplied, K::HookEffectsRejected})) {
        return IdRequirement{"hookDispatchId", true};
    }
    if (in({K::CommandReceived, K::CommandPending, K::CommandStarted, K::CommandCompleted,
            K::CommandFailed, K::CommandRejected, K::CommandCancelled, K::CommandUnknown})) {
        return IdRequirement{"commandId", true};
    }
    if (in({K::TaskStarted, K::TaskPending, K::TaskCompleted, K::TaskFailed,
            K::TaskCancelled})) {
        return IdRequirement{"taskId", true};
    }
    if (in({K::TitleRequested, K::TitleExtracted, K::SessionTitleApplied})) {
        return IdRequirement{"titleGenerationId", true};
    }
    return std::nullopt;
}

}  // namespace

// ---------------------------------------------------------------------------
// 引用与链
// ---------------------------------------------------------------------------

bool IsValidRef(const nlohmann::json& ref) {
    if (ref.is_string()) {
        return !ref.get<std::string>().empty();
    }
    if (!ref.is_object()) {
        return false;
    }
    // 跨会话五键(§3.1):sessionId/runId/id 为 string,seq 为整数,hash 为
    // 64 位十六进制。
    return ref.contains("sessionId") && ref.contains("runId") && ref.contains("seq") &&
           ref.contains("id") && ref.contains("hash") && ref["sessionId"].is_string() &&
           ref["runId"].is_string() && ref["seq"].is_number_integer() && ref["id"].is_string() &&
           ref["hash"].is_string() && IsHex64(ref["hash"].get<std::string>());
}

std::optional<Schema3Error> CheckRefField(std::string_view context, const nlohmann::json& payload,
                                          const char* key, bool required) {
    auto it = payload.find(key);
    if (it == payload.end()) {
        if (required) {
            return Err("schema3.missing_field",
                       std::string(context) + " payload 缺引用字段: " + key);
        }
        return std::nullopt;
    }
    if (!IsValidRef(*it)) {
        return Err("schema3.bad_ref",
                   std::string(context) + " 引用格式错: " + key);
    }
    return std::nullopt;
}

std::optional<Schema3Error> ValidateContextChain(std::string_view context,
                                                 const std::vector<nlohmann::json>& chain) {
    if (chain.empty()) {
        return Err("schema3.empty_chain", std::string(context) + " 链不能为空");
    }
    std::unordered_set<std::string> refs;
    std::unordered_map<std::string, std::string> prev_of;  // ref -> prevMessageRef
    for (const auto& node : chain) {
        if (!node.is_object() || !node.contains("messageRef") ||
            !node["messageRef"].is_string() || !node.contains("prevMessageRef")) {
            return Err("schema3.bad_chain_node",
                       std::string(context) + " 链节点须为 {messageRef, prevMessageRef}");
        }
        const std::string ref = node["messageRef"].get<std::string>();
        if (ref.empty()) {
            return Err("schema3.bad_chain_node", std::string(context) + " messageRef 非空");
        }
        if (!node["prevMessageRef"].is_null() && !node["prevMessageRef"].is_string()) {
            return Err("schema3.bad_chain_node",
                       std::string(context) + " prevMessageRef 应为 string 或 null");
        }
        if (!refs.insert(ref).second) {
            return Err("schema3.duplicate_chain_node",
                       std::string(context) + " 链内 messageRef 重复: " + ref);
        }
        prev_of[ref] =
            node["prevMessageRef"].is_null() ? std::string() : node["prevMessageRef"].get<std::string>();
    }
    // 根唯一、根前驱 null、非根前驱在链内、邻接一致、无环、连通。
    std::string root;
    int root_count = 0;
    for (const auto& [ref, prev] : prev_of) {
        if (prev.empty()) {
            ++root_count;
            root = ref;
        } else if (prev_of.find(prev) == prev_of.end()) {
            return Err("schema3.dangling_prev",
                       std::string(context) + " 前驱不在链内: " + ref + " -> " + prev);
        }
    }
    if (root_count != 1) {
        return Err("schema3.bad_root",
                   std::string(context) + " 根节点应恰有一个,实得 " + std::to_string(root_count));
    }
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        // 数组按根到尾序列化,邻接项必须与 prevMessageRef 一致(§4.30)。
        const std::string expect_prev =
            chain[i + 1]["prevMessageRef"].is_null()
                ? std::string()
                : chain[i + 1]["prevMessageRef"].get<std::string>();
        if (expect_prev != chain[i]["messageRef"].get<std::string>()) {
            return Err("schema3.chain_order_mismatch",
                       std::string(context) + " 链数组邻接与 prevMessageRef 不一致");
        }
    }
    if (!chain.empty() && !chain[0]["prevMessageRef"].is_null()) {
        return Err("schema3.bad_root", std::string(context) + " 首节点前驱应为 null");
    }
    // 环检查:从根走一遍,须遍历全部节点。
    std::unordered_set<std::string> visited;
    std::string cursor = root;
    std::unordered_map<std::string, std::string> next_of;
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        next_of[chain[i]["messageRef"].get<std::string>()] =
            chain[i + 1]["messageRef"].get<std::string>();
    }
    while (visited.insert(cursor).second) {
        auto it = next_of.find(cursor);
        if (it == next_of.end()) {
            break;
        }
        cursor = it->second;
    }
    if (visited.size() != chain.size()) {
        return Err("schema3.chain_not_connected",
                   std::string(context) + " 链存在环或分叉,未全连通");
    }
    return std::nullopt;
}

std::optional<Schema3Error> ValidateAppendedChain(std::string_view context,
                                                  const std::vector<nlohmann::json>& chain) {
    if (chain.empty()) {
        return Err("schema3.empty_chain", std::string(context) + " 追加链不能为空");
    }
    std::unordered_set<std::string> refs;
    for (const auto& node : chain) {
        if (!node.is_object() || !node.contains("messageRef") ||
            !node["messageRef"].is_string() || !node.contains("prevMessageRef")) {
            return Err("schema3.bad_chain_node",
                       std::string(context) + " 追加链节点须为 {messageRef, prevMessageRef}");
        }
        if (!refs.insert(node["messageRef"].get<std::string>()).second) {
            return Err("schema3.duplicate_chain_node",
                       std::string(context) + " 追加链内 messageRef 重复");
        }
    }
    // 首节点前驱非 null:追加必接旧尾(§4.30"首节点接旧尾")。
    if (chain[0]["prevMessageRef"].is_null()) {
        return Err("schema3.bad_append_root",
                   std::string(context) + " 追加链首节点前驱应为旧尾,不为 null");
    }
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        const auto& expect = chain[i + 1]["prevMessageRef"];
        if (expect.is_null() ||
            chain[i]["messageRef"].get<std::string>() != expect.get<std::string>()) {
            return Err("schema3.chain_order_mismatch",
                       std::string(context) + " 追加链邻接与 prevMessageRef 不一致");
        }
    }
    return std::nullopt;
}

std::optional<Schema3Error> CheckContextChainField(std::string_view context,
                                                   const nlohmann::json& payload,
                                                   const char* key) {
    auto it = payload.find(key);
    if (it == payload.end()) {
        return Err("schema3.missing_field", std::string(context) + " payload 缺链字段: " + key);
    }
    if (!it->is_array()) {
        return Err("schema3.bad_type", std::string(context) + " " + key + " 应为数组");
    }
    return ValidateContextChain(context, it->get<std::vector<nlohmann::json>>());
}

std::optional<Schema3Error> ValidateUsage(const nlohmann::json& usage) {
    if (usage.is_null()) {
        return std::nullopt;
    }
    if (!usage.is_object()) {
        return Err("schema3.bad_type", "usage 应为 object 或 null");
    }
    static const std::vector<std::string> kKnownKeys = {
        "inputTokens", "outputTokens", "reasoningTokens", "cacheReadTokens", "cacheWriteTokens",
    };
    for (const auto& key : kKnownKeys) {
        auto it = usage.find(key);
        if (it == usage.end()) {
            continue;
        }
        if (!JsonIsNonNegativeInt(*it)) {
            return Err("schema3.bad_usage", "usage." + key + " 应为非负整数");
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// message 行
// ---------------------------------------------------------------------------

std::optional<Schema3Error> ValidateMessageLine(const MessageLine& line) {
    auto role = MessageRoleFromName(line.message.value("role", ""));
    if (!role.has_value()) {
        return Err("schema3.bad_role", "message.role 须为 system/user/assistant/tool");
    }
    switch (*role) {
        case MessageRole::System: {
            if (line.turn_id.has_value()) {
                return Err("schema3.system_turn_not_null", "system 消息 turnId 恒为 null");
            }
            if (!line.system_meta.has_value() || !line.system_meta->is_object()) {
                return Err("schema3.missing_field", "system 消息必带 systemMeta");
            }
            if (line.purpose != MessagePurpose::Conversation &&
                line.purpose != MessagePurpose::Compact) {
                return Err("schema3.bad_purpose",
                           "system 消息 purpose 只能是 conversation 或 compact");
            }
            break;
        }
        case MessageRole::User: {
            if (line.purpose == MessagePurpose::ContextSummary) {
                if (line.turn_id.has_value()) {
                    return Err("schema3.summary_turn_not_null",
                               "注入摘要 turnId=null(§4.6),不冒充真人回合");
                }
                if (!line.source_message_ref.has_value()) {
                    return Err("schema3.missing_field",
                               "context_summary 摘要必带 sourceMessageRef 指回候选产物");
                }
            } else if (!line.turn_id.has_value()) {
                // compact prompt(user)归内部回合,turnId 必有值;session_title
                // prompt 归首 query 的 turn(§4.34)。
                return Err("schema3.missing_field", "user 消息 turnId 必填(摘要除外)");
            }
            break;
        }
        case MessageRole::Assistant: {
            if (!line.turn_id.has_value()) {
                return Err("schema3.missing_field", "assistant 消息 turnId 必填");
            }
            if (!line.request_id.has_value()) {
                return Err("schema3.missing_field", "assistant 必带 requestId(§4.44)");
            }
            if (!line.provider.has_value() || !line.wire.has_value() ||
                !line.model.has_value()) {
                return Err("schema3.missing_source",
                           "assistant 必带 provider/wire/model(§4.44)");
            }
            if (!line.response_model.has_value()) {
                return Err("schema3.missing_field",
                           "assistant 必带 responseModel 键(缺失实报为 null)");
            }
            if (!line.usage.has_value()) {
                return Err("schema3.missing_field",
                           "assistant 必带 usage 键(缺实报为 null,不补 0)");
            }
            if (auto error = ValidateUsage(*line.usage)) {
                return error;
            }
            break;
        }
        case MessageRole::Tool: {
            if (!line.turn_id.has_value() || !line.action_id.has_value()) {
                return Err("schema3.missing_field", "tool 消息必带 turnId 与 actionId");
            }
            if (!line.message.contains("tool_call_id") ||
                !line.message["tool_call_id"].is_string() ||
                line.message["tool_call_id"].get<std::string>() != *line.action_id) {
                return Err("schema3.tool_call_id_mismatch",
                           "message.tool_call_id 须等于信封 actionId(§4.15)");
            }
            break;
        }
    }
    // 内部回合(compact)消息必带 compactId(§4.5)。
    if (line.purpose == MessagePurpose::Compact && !line.compact_id.has_value()) {
        return Err("schema3.missing_field", "purpose=compact 的消息必带 compactId");
    }
    if (line.purpose == MessagePurpose::ContextSummary && !line.compact_id.has_value()) {
        return Err("schema3.missing_field", "context_summary 摘要必带 compactId");
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// event 行
// ---------------------------------------------------------------------------

std::optional<Schema3Error> ValidateEventLine(const EventLine& line) {
    // kind ↔ status 固定映射(§2.2)。
    auto required_status = RequiredStatusForKind(line.kind);
    if (required_status.has_value()) {
        if (!line.status.has_value()) {
            return Err("schema3.missing_field",
                       std::string("kind ") + EventKindV3Name(line.kind) +
                           " 必带 status=" + OpStatusName(*required_status));
        }
        if (*line.status != *required_status) {
            return Err("schema3.status_kind_mismatch",
                       std::string("kind ") + EventKindV3Name(line.kind) + " 的 status 须为 " +
                           OpStatusName(*required_status) + ",实得 " +
                           OpStatusName(*line.status));
        }
    } else if (line.status.has_value()) {
        return Err("schema3.status_kind_mismatch",
                   std::string("kind ") + EventKindV3Name(line.kind) + " 不携带 status 字段");
    }
    // 按 kind 的必选身份字段。
    if (auto requirement = IdRequirementForKind(line.kind)) {
        const std::optional<std::string>* value = nullptr;
        std::string field = requirement->envelope_field;
        if (field == "compactId") value = &line.compact_id;
        else if (field == "requestId") value = &line.request_id;
        else if (field == "actionId") value = &line.action_id;
        else if (field == "hookDispatchId") value = &line.hook_dispatch_id;
        else if (field == "commandId") value = &line.command_id;
        else if (field == "taskId") value = &line.task_id;
        else if (field == "titleGenerationId") value = &line.title_generation_id;
        if (value != nullptr && requirement->required && !value->has_value()) {
            return Err("schema3.missing_field",
                       std::string("kind ") + EventKindV3Name(line.kind) + " 必带 " + field);
        }
    }
    using K = EventKindV3;
    const std::string kind_name = EventKindV3Name(line.kind);
    // 关键 payload 子字段(§四;完整载荷表归领域层)。
    if (line.kind == K::ModelRequestPrepared) {
        for (const auto* key : {"contextId", "contextRevision", "systemMessageRef",
                                "inputMessageRefs", "readThroughSeq", "readThroughHash"}) {
            if (!line.payload.contains(key)) {
                return Err("schema3.missing_field",
                           "model.request.prepared payload 缺字段: " + std::string(key));
            }
        }
        if (!line.payload["inputMessageRefs"].is_array() ||
            !line.payload["contextRevision"].is_number_integer() ||
            !line.payload["readThroughSeq"].is_number_integer()) {
            return Err("schema3.bad_type",
                       "model.request.prepared 的 inputMessageRefs/contextRevision/"
                       "readThroughSeq 类型错");
        }
        for (const auto& ref : line.payload["inputMessageRefs"]) {
            if (!IsValidRef(ref)) {
                return Err("schema3.bad_ref", "inputMessageRefs 引用格式错");
            }
        }
        if (!line.turn_id.has_value() || !line.step_id.has_value()) {
            return Err("schema3.missing_field",
                       "model.request.prepared 必带 turnId 与 stepId(§4.4)");
        }
    } else if (line.kind == K::CompactApplied) {
        for (const auto* key :
             {"sourceContextRevision", "newContextRevision", "oldStateHash", "newStateHash",
              "summaryMessageRef", "validationEventRef", "removedMessageRefs",
              "retainedMessageRefs", "protectedTurnIds", "contextId", "contextChain",
              "contextTokensBefore", "contextTokensAfter", "tokenMetric", "trigger"}) {
            if (!line.payload.contains(key)) {
                return Err("schema3.missing_field",
                           "compact.applied payload 缺字段: " + std::string(key));
            }
        }
        if (auto error = CheckContextChainField("compact.applied", line.payload)) {
            return error;
        }
        if (!line.payload["removedMessageRefs"].is_array() ||
            !line.payload["retainedMessageRefs"].is_array() ||
            !line.payload["protectedTurnIds"].is_array()) {
            return Err("schema3.bad_type", "compact.applied 清单字段应为数组");
        }
    } else if (line.kind == K::ContextInputApplied) {
        for (const auto* key :
             {"contextId", "beforeRevision", "afterRevision", "appendedChain",
              "addedMessageRefs"}) {
            if (!line.payload.contains(key)) {
                return Err("schema3.missing_field",
                           "context.input.applied payload 缺字段: " + std::string(key));
            }
        }
        // appendedChain 是追加片段(首节点接旧尾),不适用"根唯一"的全链
        // 校验;单独验非空/无重复/邻接一致。
        const auto& appended = line.payload.at("appendedChain");
        if (!appended.is_array()) {
            return Err("schema3.bad_type", "appendedChain 应为数组");
        }
        if (auto error = ValidateAppendedChain("context.input.applied",
                                               appended.get<std::vector<nlohmann::json>>())) {
            return error;
        }
        if (!line.payload["addedMessageRefs"].is_array()) {
            return Err("schema3.bad_type", "addedMessageRefs 应为数组");
        }
    } else if (line.kind == K::ContextSystemApplied) {
        for (const auto* key :
             {"contextId", "beforeRevision", "afterRevision", "rootMessageRef", "contextChain"}) {
            if (!line.payload.contains(key)) {
                return Err("schema3.missing_field",
                           "context.system.applied payload 缺字段: " + std::string(key));
            }
        }
        if (auto error = CheckContextChainField("context.system.applied", line.payload)) {
            return error;
        }
    } else if (line.kind == K::SessionStarted) {
        if (!line.payload.contains("context") || !line.payload["context"].is_object()) {
            return Err("schema3.missing_field", "session.started payload 缺 context(修订 1 链)");
        }
        const auto& context = line.payload["context"];
        for (const auto* key : {"contextId", "revision", "contextChain"}) {
            if (!context.contains(key)) {
                return Err("schema3.missing_field",
                           "session.started.context 缺字段: " + std::string(key));
            }
        }
        if (auto error = CheckContextChainField("session.started", context)) {
            return error;
        }
    } else if (line.kind == K::SystemChange) {
        for (const auto* key : {"cause", "oldSystemMessageRef", "settingsVersion",
                                "systemChanged"}) {
            if (!line.payload.contains(key)) {
                return Err("schema3.missing_field",
                           "system.change payload 缺字段: " + std::string(key));
            }
        }
        if (!line.payload["systemChanged"].is_boolean()) {
            return Err("schema3.bad_type", "system.change.systemChanged 应为布尔");
        }
    } else if (line.kind == K::ModelResponseDelta) {
        for (const auto* key : {"requestId", "streamId", "messageId", "sequence", "deltaType"}) {
            if (!line.payload.contains(key)) {
                return Err("schema3.missing_field",
                           "model.response.delta payload 缺字段: " + std::string(key));
            }
        }
        if (!line.payload["sequence"].is_number_integer()) {
            return Err("schema3.bad_type", "model.response.delta.sequence 应为整数");
        }
    } else if (line.kind == K::ModelResponseStarted) {
        for (const auto* key : {"requestId", "streamId", "messageId"}) {
            if (!line.payload.contains(key)) {
                return Err("schema3.missing_field",
                           "model.response.started payload 缺字段: " + std::string(key));
            }
        }
    }
    // pending 类必须带 reason(§4.14)。
    if (line.status == OpStatus::Pending && !line.payload.contains("reason")) {
        return Err("schema3.missing_field",
                   std::string(kind_name) + " pending 必须带 payload.reason(§4.14)");
    }
    return std::nullopt;
}

}  // namespace lubancode::trajectory::v3
