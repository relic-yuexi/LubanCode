// v3 语义校验实现。
#include "trajectory/v3/schema3.hpp"

#include <algorithm>
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
    if (in({K::CompactRequested, K::CompactPending, K::CompactStarted, K::CompactRangeRetreated,
            K::CompactValidationStarted, K::CompactValidationCompleted, K::CompactApplied,
            K::CompactFailed, K::CompactCancelled, K::CompactRejected})) {
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
            K::ToolResultPersisted, K::ToolResultPersistFailed, K::ToolResultSelected, K::ToolResultSummaryFinished})) {
        return IdRequirement{"actionId", true};
    }
    if (in({K::HookDispatchRequested, K::HookPending, K::HookStarted, K::HookCompleted,
            K::HookFailed, K::HookCancelled, K::HookUnknown, K::HookSkipped,
            K::HookEffectsApplied, K::HookEffectsRejected, K::HookOutputProposed,
            K::HookContinuationConsumed})) {
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
    // subagent.* 挂父工具 Action(§4.31:spawn/wait/send/cancel 各自成调用)。
    if (in({K::SubagentSpawnRequested, K::SubagentLinked, K::SubagentObserved,
            K::SubagentSpawnFailed})) {
        return IdRequirement{"actionId", true};
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
// P1 其余域的工具函数(工具结果/hook/subagent/降档)
// ---------------------------------------------------------------------------

// artifactRef(§3.1 六键):{artifactId,kind,path,sha256,bytes,mediaType};
// kind ∈ result_metadata|stdout|stderr|combined|raw_payload|report|image|blob。
std::optional<Schema3Error> ValidateArtifactRef(std::string_view context,
                                                const nlohmann::json& ref) {
    static const std::vector<std::string> kKinds = {
        "result_metadata", "stdout", "stderr", "combined", "raw_payload", "report", "image", "blob",
    };
    if (!ref.is_object()) {
        return Err("schema3.bad_ref", std::string(context) + " artifactRef 应为 object");
    }
    for (const auto* key : {"artifactId", "kind", "path", "sha256", "bytes", "mediaType"}) {
        if (!ref.contains(key)) {
            return Err("schema3.missing_field",
                       std::string(context) + " artifactRef 缺字段: " + key);
        }
    }
    if (!ref["artifactId"].is_string() || ref["artifactId"].get<std::string>().empty() ||
        !ref["path"].is_string() || ref["path"].get<std::string>().empty() ||
        !ref["mediaType"].is_string()) {
        return Err("schema3.bad_ref", std::string(context) + " artifactRef 字段类型错");
    }
    if (!ref["kind"].is_string() ||
        std::find(kKinds.begin(), kKinds.end(), ref["kind"].get<std::string>()) == kKinds.end()) {
        return Err("schema3.bad_enum", std::string(context) + " artifactRef.kind 未知");
    }
    if (!ref["sha256"].is_string() || !IsHex64(ref["sha256"].get<std::string>())) {
        return Err("schema3.bad_ref", std::string(context) + " artifactRef.sha256 应为 64 位十六进制");
    }
    if (!JsonIsNonNegativeInt(ref["bytes"])) {
        return Err("schema3.bad_ref", std::string(context) + " artifactRef.bytes 应为非负整数");
    }
    return std::nullopt;
}

namespace {

// result_ref 数组(§4.16):固定数组;每项六键 artifactRef;同文件不重复。
std::optional<Schema3Error> CheckArtifactRefArray(std::string_view context,
                                                  const nlohmann::json& payload,
                                                  const char* key, bool allow_empty) {
    auto it = payload.find(key);
    if (it == payload.end()) {
        return Err("schema3.missing_field", std::string(context) + " payload 缺字段: " + key);
    }
    if (!it->is_array()) {
        return Err("schema3.bad_type", std::string(context) + " " + key + " 应为数组(空也写 [])");
    }
    if (it->empty() && !allow_empty) {
        return Err("schema3.empty_result_refs", std::string(context) + " " + key + " 不能为空");
    }
    std::unordered_set<std::string> seen;
    for (const auto& ref : *it) {
        if (auto error = ValidateArtifactRef(context, ref)) {
            return error;
        }
        if (!seen.insert(ref["path"].get<std::string>()).second) {
            return Err("schema3.duplicate_artifact",
                       std::string(context) + " 同一文件在 " + key + " 里只列一次");
        }
    }
    return std::nullopt;
}

// 引用数组(如 sourceResultEventRefs/hookEffectEventRefs):每项须合法引用。
std::optional<Schema3Error> CheckRefArray(std::string_view context,
                                          const nlohmann::json& payload, const char* key,
                                          bool allow_empty) {
    auto it = payload.find(key);
    if (it == payload.end()) {
        return Err("schema3.missing_field", std::string(context) + " payload 缺字段: " + key);
    }
    if (!it->is_array()) {
        return Err("schema3.bad_type", std::string(context) + " " + key + " 应为数组");
    }
    if (it->empty() && !allow_empty) {
        return Err("schema3.empty_refs", std::string(context) + " " + key + " 不能为空");
    }
    for (const auto& ref : *it) {
        if (!IsValidRef(ref)) {
            return Err("schema3.bad_ref", std::string(context) + " " + key + " 内引用格式错");
        }
    }
    return std::nullopt;
}

// 工具族公共:payload.tool_call_id == 信封 actionId(§4.15 映射必校);
// attempt 正整数。
std::optional<Schema3Error> CheckToolPayload(std::string_view context, const EventLine& line,
                                             bool require_attempt) {
    if (!line.action_id.has_value()) {
        return Err("schema3.missing_field", std::string(context) + " 信封缺 actionId");
    }
    if (!line.payload.contains("tool_call_id") || !line.payload["tool_call_id"].is_string() ||
        line.payload["tool_call_id"].get<std::string>() != *line.action_id) {
        return Err("schema3.tool_call_id_mismatch",
                   std::string(context) + " payload.tool_call_id 须等于信封 actionId(§4.15)");
    }
    if (require_attempt) {
        if (!line.payload.contains("attempt") ||
            !JsonIsNonNegativeInt(line.payload["attempt"]) ||
            line.payload["attempt"].get<std::uint64_t>() < 1) {
            return Err("schema3.bad_type",
                       std::string(context) + " payload.attempt 应为从 1 起的正整数(§4.15)");
        }
    }
    return std::nullopt;
}

// payload 取必选非空 string。
std::optional<Schema3Error> CheckStringField(std::string_view context,
                                             const nlohmann::json& payload, const char* key) {
    auto it = payload.find(key);
    if (it == payload.end()) {
        return Err("schema3.missing_field", std::string(context) + " payload 缺字段: " + key);
    }
    if (!it->is_string() || it->get<std::string>().empty()) {
        return Err("schema3.bad_type", std::string(context) + " " + key + " 应为非空 string");
    }
    return std::nullopt;
}

// 子会话引用(§4.31):{sessionId, runId, journalPath} 三键。
std::optional<Schema3Error> CheckChildSessionRef(std::string_view context,
                                                 const nlohmann::json& payload) {
    auto it = payload.find("childSessionRef");
    if (it == payload.end() || !it->is_object()) {
        return Err("schema3.missing_field",
                   std::string(context) + " payload.childSessionRef 应为 object");
    }
    for (const auto* key : {"sessionId", "runId", "journalPath"}) {
        if (!it->contains(key) || !(*it)[key].is_string() || (*it)[key].get<std::string>().empty()) {
            return Err("schema3.bad_ref",
                       std::string(context) + " childSessionRef." + std::string(key) +
                           " 应为非空 string(§4.31)");
        }
    }
    return std::nullopt;
}

// 子账检查点(§4.31):{sessionId, runId, seq, lineHash}——固定子账前缀。
std::optional<Schema3Error> CheckChildCheckpointRef(std::string_view context,
                                                    const nlohmann::json& payload) {
    auto it = payload.find("childCheckpointRef");
    if (it == payload.end() || !it->is_object()) {
        return Err("schema3.missing_field",
                   std::string(context) + " payload.childCheckpointRef 应为 object");
    }
    for (const auto* key : {"sessionId", "runId"}) {
        if (!it->contains(key) || !(*it)[key].is_string() || (*it)[key].get<std::string>().empty()) {
            return Err("schema3.bad_ref",
                       std::string(context) + " childCheckpointRef." + std::string(key) +
                           " 应为非空 string");
        }
    }
    if (!it->contains("seq") || !JsonIsNonNegativeInt((*it)["seq"])) {
        return Err("schema3.bad_ref", std::string(context) + " childCheckpointRef.seq 应为非负整数");
    }
    if (!it->contains("lineHash") || !(*it)["lineHash"].is_string() ||
        !IsHex64((*it)["lineHash"].get<std::string>())) {
        return Err("schema3.bad_ref",
                   std::string(context) + " childCheckpointRef.lineHash 应为 64 位十六进制");
    }
    return std::nullopt;
}

}  // namespace

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
                line.purpose != MessagePurpose::Compact && line.purpose != MessagePurpose::ActionSummary) {
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
            // §1.2.1:tool content 为 string(模型可见的最终预览文本)。
            if (!line.message.contains("content") || !line.message["content"].is_string()) {
                return Err("schema3.bad_type", "tool 消息 content 应为 string(§1.2.1)");
            }
            break;
        }
    }
    // 降档派生消息(§4.38):同执行结果的更短预览版本,origin 固定
    // context_runtime,不冒充新执行、不增加工具次数。
    if (line.source_tool_message_ref.has_value() &&
        line.origin != MessageOrigin::ContextRuntime) {
        return Err("schema3.bad_origin",
                   "带 sourceToolMessageRef 的派生消息 origin 须为 context_runtime(§4.38)");
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
    } else if (line.kind == K::CompactRangeRetreated) {
        // §4.64 回退事件:计划修订号与本次退出的引用必须可追;退出引用
        // 不等于 removedMessageRefs,只有成功 applied 后被摘要替代的前缀
        // 才进 removed。
        for (const auto* key : {"planRevision", "retreatedMessageRefs"}) {
            if (!line.payload.contains(key)) {
                return Err("schema3.missing_field",
                           "compact.range.retreated payload 缺字段: " + std::string(key));
            }
        }
        if (!line.payload["planRevision"].is_number_integer() ||
            !line.payload["retreatedMessageRefs"].is_array()) {
            return Err("schema3.bad_type",
                       "compact.range.retreated 的 planRevision 应为整数、"
                       "retreatedMessageRefs 应为数组");
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
    } else if (line.kind == K::ToolExecutionPending) {
        // §4.14:已接纳待执行,必须带 reason(queued/approval/dependency/backoff)。
        if (auto error = CheckToolPayload(kind_name, line, true)) {
            return error;
        }
        if (auto error = CheckStringField(kind_name, line.payload, "reason")) {
            return error;
        }
    } else if (line.kind == K::ToolExecutionStarted) {
        // §4.15/§4.18:越过准入栅栏时 effective_args 已另存快照。
        if (auto error = CheckToolPayload(kind_name, line, true)) {
            return error;
        }
        if (auto error = CheckRefField(kind_name, line.payload, "effectiveArgsRef", true)) {
            return error;
        }
    } else if (line.kind == K::ToolExecutionWaiting) {
        // §2.2/§4.14:执行中等待外部条件,须带 reason 与可恢复等待引用。
        if (auto error = CheckToolPayload(kind_name, line, true)) {
            return error;
        }
        if (auto error = CheckStringField(kind_name, line.payload, "reason")) {
            return error;
        }
        if (auto error = CheckRefField(kind_name, line.payload, "waitRef", true)) {
            return error;
        }
    } else if (line.kind == K::ToolExecutionResumed) {
        if (auto error = CheckToolPayload(kind_name, line, true)) {
            return error;
        }
    } else if (line.kind == K::ToolExecutionFinished) {
        if (auto error = CheckToolPayload(kind_name, line, true)) {
            return error;
        }
        // exit_code:进程工具为整数;非进程工具缺省;退出未知为 null(§4.16)。
        if (line.payload.contains("exit_code") && !line.payload["exit_code"].is_number_integer() &&
            !line.payload["exit_code"].is_null()) {
            return Err("schema3.bad_type", "tool.execution.finished.exit_code 应为整数或 null");
        }
    } else if (line.kind == K::ToolExecutionFailed) {
        if (auto error = CheckToolPayload(kind_name, line, true)) {
            return error;
        }
        if (auto error = CheckStringField(kind_name, line.payload, "error_code")) {
            return error;
        }
    } else if (line.kind == K::ToolExecutionCancelled) {
        if (auto error = CheckToolPayload(kind_name, line, false)) {
            return error;
        }
        // §4.14:区分执行前取消与执行中取消。
        if (auto error = CheckStringField(kind_name, line.payload, "phase")) {
            return error;
        }
        if (line.payload["phase"] != "before_started" &&
            line.payload["phase"] != "during_execution") {
            return Err("schema3.bad_enum",
                       "cancelled.phase 应为 before_started|during_execution(§4.14)");
        }
    } else if (line.kind == K::ToolExecutionRejected) {
        // 参数/权限/准入拒绝:没有执行,不必带 attempt。
        if (auto error = CheckToolPayload(kind_name, line, false)) {
            return error;
        }
        if (auto error = CheckStringField(kind_name, line.payload, "reason")) {
            return error;
        }
    } else if (line.kind == K::ToolExecutionUnknown) {
        if (auto error = CheckToolPayload(kind_name, line, true)) {
            return error;
        }
        if (auto error = CheckStringField(kind_name, line.payload, "reason")) {
            return error;
        }
    } else if (line.kind == K::ToolResultPersisted) {
        if (auto error = CheckToolPayload(kind_name, line, true)) {
            return error;
        }
        if (auto error = CheckArtifactRefArray(kind_name, line.payload, "result_ref", false)) {
            return error;
        }
        if (auto error = CheckRefField(kind_name, line.payload, "executionEventRef", true)) {
            return error;
        }
    } else if (line.kind == K::ToolResultPersistFailed) {
        if (auto error = CheckToolPayload(kind_name, line, true)) {
            return error;
        }
        if (auto error = CheckStringField(kind_name, line.payload, "reason")) {
            return error;
        }
    } else if (line.kind == K::ToolResultSummaryFinished) {
        if (auto error = CheckToolPayload(kind_name, line, false)) return error;
        if (auto error = CheckRefArray(kind_name, line.payload, "sourceResultEventRefs", false)) return error;
        if (auto error = CheckRefArray(kind_name, line.payload, "candidateMessageRefs", true)) return error;
        if (auto error = CheckStringField(kind_name, line.payload, "state")) return error;
        const auto state = line.payload.at("state").get<std::string>();
        if (state != "accepted" && state != "rejected" && state != "failed" && state != "cancelled") {
            return Err("schema3.bad_enum", "action summary state is invalid");
        }
        for (const char* key : {"sourceContextRevision", "modelCalls", "outputBytes", "budgetBytes"}) {
            if (!line.payload.contains(key) || !JsonIsNonNegativeInt(line.payload.at(key))) {
                return Err("schema3.bad_type", std::string("action summary missing integer: ") + key);
            }
        }
        if (state == "accepted") {
            if (line.payload.at("candidateMessageRefs").empty() ||
                line.payload.at("outputBytes").get<std::uint64_t>() > line.payload.at("budgetBytes").get<std::uint64_t>()) {
                return Err("schema3.invalid_summary_candidate", "accepted summary needs a bounded candidate");
            }
            if (auto error = CheckStringField(kind_name, line.payload, "previewSha256")) return error;
            if (!IsHex64(line.payload.at("previewSha256").get<std::string>())) {
                return Err("schema3.invalid_summary_hash", "accepted summary requires SHA-256 hex");
            }
        }
    } else if (line.kind == K::ToolResultSelected) {
        if (auto error = CheckToolPayload(kind_name, line, false)) {
            return error;
        }
        if (auto error = CheckRefArray(kind_name, line.payload, "sourceResultEventRefs", false)) {
            return error;
        }
        if (auto error = CheckRefArray(kind_name, line.payload, "hookEffectEventRefs", true)) {
            return error;
        }
        if (line.payload.contains("summaryEventRef")) {
            if (auto error = CheckRefField(kind_name, line.payload, "summaryEventRef", false)) return error;
        }
        // §4.23:最终有效结果;hook 替代与宿主配对错误各有名目。
        if (auto error = CheckStringField(kind_name, line.payload, "effectiveOutcome")) {
            return error;
        }
        const std::string outcome = line.payload["effectiveOutcome"].get<std::string>();
        if (outcome != "done" && outcome != "failed" && outcome != "substituted" &&
            outcome != "error") {
            return Err("schema3.bad_enum",
                       "effectiveOutcome 应为 done|failed|substituted|error(§4.23)");
        }
    } else if (line.kind == K::HookDispatchRequested) {
        if (auto error = CheckStringField(kind_name, line.payload, "hookPoint")) {
            return error;
        }
    } else if (line.kind == K::HookStarted) {
        for (const auto* key : {"hookInvocationId", "hookId", "handlerKind"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
    } else if (line.kind == K::HookCompleted) {
        for (const auto* key : {"hookInvocationId", "hookId"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
    } else if (line.kind == K::HookFailed) {
        if (auto error = CheckStringField(kind_name, line.payload, "error_code")) {
            return error;
        }
    } else if (line.kind == K::HookCancelled || line.kind == K::HookUnknown ||
               line.kind == K::HookSkipped) {
        if (auto error = CheckStringField(kind_name, line.payload, "reason")) {
            return error;
        }
    } else if (line.kind == K::HookEffectsApplied) {
        if (auto error = CheckStringField(kind_name, line.payload, "effectType")) {
            return error;
        }
    } else if (line.kind == K::HookEffectsRejected) {
        if (auto error = CheckStringField(kind_name, line.payload, "effectType")) {
            return error;
        }
        if (auto error = CheckStringField(kind_name, line.payload, "reason")) {
            return error;
        }
    } else if (line.kind == K::HookOutputProposed) {
        // §7.1:候选先存(proposed),不冒充 handler 已完成。phase ∈
        // before_next/after_next/short_circuit;candidate 为候选正文(引用)。
        for (const auto* key : {"hookInvocationId", "phase"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
        const std::string phase = line.payload["phase"].get<std::string>();
        if (phase != "before_next" && phase != "after_next" && phase != "short_circuit") {
            return Err("schema3.bad_enum",
                       "hook.output.proposed 的 phase 应为 before_next|after_next|short_circuit(§7.1)");
        }
    } else if (line.kind == K::HookContinuationConsumed) {
        // §7.1:一次性执行权消费;不以消费记录冒充下游真的执行或完成。
        if (auto error = CheckStringField(kind_name, line.payload, "hookInvocationId")) {
            return error;
        }
    } else if (line.kind == K::SubagentSpawnRequested) {
        if (auto error = CheckStringField(kind_name, line.payload, "taskId")) {
            return error;
        }
        if (auto error = CheckChildSessionRef(kind_name, line.payload)) {
            return error;
        }
        if (!line.payload.contains("attempt") ||
            !JsonIsNonNegativeInt(line.payload["attempt"]) ||
            line.payload["attempt"].get<std::uint64_t>() < 1) {
            return Err("schema3.bad_type", "subagent.spawn.requested.attempt 应为从 1 起(§4.32)");
        }
    } else if (line.kind == K::SubagentLinked) {
        if (auto error = CheckStringField(kind_name, line.payload, "taskId")) {
            return error;
        }
        if (auto error = CheckChildCheckpointRef(kind_name, line.payload)) {
            return error;
        }
    } else if (line.kind == K::SubagentObserved) {
        if (auto error = CheckStringField(kind_name, line.payload, "taskId")) {
            return error;
        }
        if (auto error = CheckChildCheckpointRef(kind_name, line.payload)) {
            return error;
        }
    } else if (line.kind == K::SubagentSpawnFailed) {
        if (auto error = CheckStringField(kind_name, line.payload, "taskId")) {
            return error;
        }
        if (auto error = CheckStringField(kind_name, line.payload, "phase")) {
            return error;
        }
        if (auto error = CheckStringField(kind_name, line.payload, "reason")) {
            return error;
        }
    } else if (line.kind == K::ContextToolPreviewsReduced) {
        // §4.38:独立上下文提交事件——源/新版本、旧/新档位、替换消息、
        // 完整新链、输入 hash 与前后估算、配对校验引用。
        for (const auto* key :
             {"contextId", "beforeRevision", "afterRevision", "oldPreviewBudget",
              "newPreviewBudget", "replacementRefs", "contextChain", "inputHash",
              "estimatedTokensBefore", "estimatedTokensAfter", "pairingCheckRefs"}) {
            if (!line.payload.contains(key)) {
                return Err("schema3.missing_field",
                           "context.tool_previews.reduced payload 缺字段: " + std::string(key));
            }
        }
        if (auto error = CheckContextChainField(kind_name, line.payload)) {
            return error;
        }
        if (!line.payload["replacementRefs"].is_array() ||
            line.payload["replacementRefs"].empty()) {
            return Err("schema3.bad_type", "replacementRefs 应为非空数组(§4.38)");
        }
        if (!line.payload["pairingCheckRefs"].is_array()) {
            return Err("schema3.bad_type", "pairingCheckRefs 应为数组");
        }
        if (!JsonIsNonNegativeInt(line.payload["oldPreviewBudget"]) ||
            !JsonIsNonNegativeInt(line.payload["newPreviewBudget"]) ||
            line.payload["newPreviewBudget"].get<std::uint64_t>() >=
                line.payload["oldPreviewBudget"].get<std::uint64_t>()) {
            return Err("schema3.bad_type",
                       "降档须 newPreviewBudget < oldPreviewBudget(§4.38 只降不升)");
        }
    }
    // ---- Workflow 编排族(§四 workflow 条目):按 kind 的载荷合同 ----
    else if (line.kind == K::WorkflowDefinitionLoaded) {
        for (const auto* key : {"workflowId", "definitionHash"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
        if (!IsHex64(line.payload["definitionHash"].get<std::string>())) {
            return Err("schema3.bad_type",
                       "workflow.definition.loaded.definitionHash 应为 64 位十六进制");
        }
    } else if (line.kind == K::WorkflowSegmentOpened) {
        if (auto error = CheckStringField(kind_name, line.payload, "segmentId")) {
            return error;
        }
        // 恢复段必须链接源水位:sourceRef 五键跨段引用(§3.1)。
        if (auto error = CheckRefField(kind_name, line.payload, "sourceRef", true)) {
            return error;
        }
    } else if (line.kind == K::WorkflowInputsCommitted) {
        for (const auto* key : {"inputsRef", "sha256"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
        if (!IsHex64(line.payload["sha256"].get<std::string>())) {
            return Err("schema3.bad_type", "workflow.inputs.committed.sha256 应为 64 位十六进制");
        }
    } else if (line.kind == K::WorkflowNodeReserved || line.kind == K::WorkflowNodeDispatched) {
        for (const auto* key : {"nodeId", "nodeExecutionId"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
        if (!line.payload.contains("attempt") || !JsonIsNonNegativeInt(line.payload["attempt"]) ||
            line.payload["attempt"].get<std::uint64_t>() < 1) {
            return Err("schema3.bad_type",
                       std::string(kind_name) + ".attempt 应为从 1 起的正整数");
        }
        // reserve 独有:输入快照内容寻址(§五 reserve 携带输入)。
        if (line.kind == K::WorkflowNodeReserved) {
            if (auto error = CheckStringField(kind_name, line.payload, "nodeKind")) {
                return error;
            }
            if (!line.payload.contains("inputHash") ||
                !line.payload["inputHash"].is_string() ||
                !IsHex64(line.payload["inputHash"].get<std::string>())) {
                return Err("schema3.bad_type",
                           "workflow.node.reserved.inputHash 应为 64 位十六进制(输入快照)");
            }
        }
    } else if (line.kind == K::WorkflowNodeWaiting) {
        for (const auto* key : {"nodeId", "waitKind"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
    } else if (line.kind == K::WorkflowNodeRetrying) {
        for (const auto* key : {"nodeId", "nodeExecutionId"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
    } else if (line.kind == K::WorkflowNodeCompleted) {
        for (const auto* key : {"nodeId", "nodeExecutionId", "outcome"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
        const std::string outcome = line.payload["outcome"].get<std::string>();
        if (outcome != "success" && outcome != "empty") {
            return Err("schema3.bad_enum",
                       "workflow.node.completed.outcome 应为 success|empty(失败走 "
                       "workflow.node.failed,§五)");
        }
    } else if (line.kind == K::WorkflowNodeFailed || line.kind == K::WorkflowRunFailed) {
        if (auto error = CheckStringField(kind_name, line.payload, "errorCode")) {
            return error;
        }
        if (line.kind == K::WorkflowNodeFailed) {
            if (auto error = CheckStringField(kind_name, line.payload, "nodeExecutionId")) {
                return error;
            }
        }
    } else if (line.kind == K::WorkflowOutputCommitted) {
        for (const auto* key : {"nodeId", "nodeExecutionId", "outputId", "outputHash", "outputRef"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
        if (!IsHex64(line.payload["outputHash"].get<std::string>())) {
            return Err("schema3.bad_type", "workflow.output.committed.outputHash 应为 64 位十六进制");
        }
        if (!line.payload.contains("validation") || !line.payload["validation"].is_object() ||
            !line.payload["validation"].contains("passed") ||
            !line.payload["validation"]["passed"].is_boolean()) {
            return Err("schema3.bad_type",
                       "workflow.output.committed.validation 应为 {passed:bool,...}(§五 产物合同)");
        }
    } else if (line.kind == K::WorkflowCheckpointCommitted) {
        for (const auto* key : {"checkpointId", "checkpointRef", "sha256"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
        if (!IsHex64(line.payload["sha256"].get<std::string>())) {
            return Err("schema3.bad_type", "workflow.checkpoint.committed.sha256 应为 64 位十六进制");
        }
        if (!line.payload.contains("throughSeq") || !JsonIsNonNegativeInt(line.payload["throughSeq"])) {
            return Err("schema3.bad_type",
                       "workflow.checkpoint.committed.throughSeq 应为非负整数(已提交水位)");
        }
    } else if (line.kind == K::WorkflowBranchStarted) {
        if (auto error = CheckStringField(kind_name, line.payload, "nodeId")) {
            return error;
        }
        if (!line.payload.contains("branches") || !line.payload["branches"].is_array() ||
            line.payload["branches"].empty()) {
            return Err("schema3.bad_type", "workflow.branch.started.branches 应为非空数组");
        }
    } else if (line.kind == K::WorkflowJoinCompleted) {
        for (const auto* key : {"nodeId", "join"}) {
            if (auto error = CheckStringField(kind_name, line.payload, key)) {
                return error;
            }
        }
    } else if (line.kind == K::WorkflowLoopIterationStarted ||
               line.kind == K::WorkflowLoopIterationCompleted) {
        if (auto error = CheckStringField(kind_name, line.payload, "nodeId")) {
            return error;
        }
        if (!line.payload.contains("iteration") || !JsonIsNonNegativeInt(line.payload["iteration"]) ||
            line.payload["iteration"].get<std::uint64_t>() < 1) {
            return Err("schema3.bad_type", std::string(kind_name) + ".iteration 应为从 1 起");
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
