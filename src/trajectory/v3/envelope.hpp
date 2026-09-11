// session 轨迹 v3 信封合同(docs/architecture/trajectory-v3-schema.md §一)。
//
// 两类行:message(模型交互消息)/ event(发生过什么)。公共信封字段
// camelCase,seq 两类行共用、单写者发号,哈希链承继 v2 算法:
//   lineHash = SHA256(prevHash || canonicalJson(本行去 prevHash/lineHash))
// 首行 prevHash 为 64 个 '0'。落盘行 = canonical dump(全字段)。
//
// 本件只认类型与转换;强校验(kind↔status 映射、按 kind/purpose 的必选
// 字段)在 schema3.hpp;写盘与发号在 writer.hpp。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory::v3 {

// ---------------------------------------------------------------------------
// 固定合同值
// ---------------------------------------------------------------------------

inline constexpr int kSchemaVersion = 3;
// 链锚承继 v2 journal.hpp kGenesisHash:64 个 '0'。
inline constexpr std::string_view kGenesisHash =
    "0000000000000000000000000000000000000000000000000000000000000000";
// schema 名:防把别的 JSONL 当 v3 轨迹(与 v2 kEventSchema 区分)。
inline constexpr std::string_view kV3SchemaName = "lubancode.trajectory.v3";

// ---------------------------------------------------------------------------
// 枚举(schema 文档 §1.2/§2.2;线上名是 schema 的一部分,改了便是改 schema)
// ---------------------------------------------------------------------------

enum class MessageRole { System, User, Assistant, Tool };
enum class MessagePurpose { Conversation, Compact, ContextSummary, SessionTitle, Capability };
enum class MessageOrigin {
    Human,
    Soul,
    SessionRuntime,
    CompactRuntime,
    ContextRuntime,
    Hook,
    Skill,
    Subagent,
    ParentAgent,
};
enum class DisplayMode { Visible, Collapsed, Hidden };
enum class CompletionStatus { Complete, Interrupted, Truncated };
enum class OpStatus { Pending, Running, Done, Failed, Cancelled, Rejected, Unknown };

const char* MessageRoleName(MessageRole value);
std::optional<MessageRole> MessageRoleFromName(std::string_view name);
const char* MessagePurposeName(MessagePurpose value);
std::optional<MessagePurpose> MessagePurposeFromName(std::string_view name);
const char* MessageOriginName(MessageOrigin value);
std::optional<MessageOrigin> MessageOriginFromName(std::string_view name);
const char* DisplayModeName(DisplayMode value);
std::optional<DisplayMode> DisplayModeFromName(std::string_view name);
const char* CompletionStatusName(CompletionStatus value);
std::optional<CompletionStatus> CompletionStatusFromName(std::string_view name);
const char* OpStatusName(OpStatus value);
std::optional<OpStatus> OpStatusFromName(std::string_view name);

// ---------------------------------------------------------------------------
// 事件 kind(schema 文档 §2.1 全表;P0 首批冻结)
// ---------------------------------------------------------------------------

enum class EventKindV3 {
    SessionStarted,
    SessionEnded,
    SystemChange,
    ContextSystemApplied,
    ContextInputApplied,
    ContextToolPreviewsReduced,
    ModelRequestPrepared,
    ModelRequestSent,
    ModelRequestFailed,
    ModelResponseStarted,
    ModelResponseDelta,
    ModelResponseCompleted,
    ModelResponseFailed,
    ModelResponseCancelled,
    ModelUsageAppended,
    CompactRequested,
    CompactPending,
    CompactStarted,
    CompactRangeRetreated,
    CompactValidationStarted,
    CompactValidationCompleted,
    CompactApplied,
    CompactFailed,
    CompactCancelled,
    CompactRejected,
    ToolExecutionPending,
    ToolExecutionStarted,
    ToolExecutionWaiting,
    ToolExecutionResumed,
    ToolExecutionFinished,
    ToolExecutionFailed,
    ToolExecutionCancelled,
    ToolExecutionRejected,
    ToolExecutionUnknown,
    ToolResultPersisted,
    ToolResultPersistFailed,
    ToolResultSelected,
    HookDispatchRequested,
    HookPending,
    HookStarted,
    HookCompleted,
    HookFailed,
    HookCancelled,
    HookUnknown,
    HookSkipped,
    HookEffectsApplied,
    HookEffectsRejected,
    // LuaHook 单 P0-B(§7.1 洋葱前置两项语义):候选先存(proposed)与
    // 一次性执行权消费(continuation.consumed)。两者都是事实记录,不携带
    // status;proposed 不冒充 handler 已完成,consumed 不冒充下游已执行。
    HookOutputProposed,
    HookContinuationConsumed,
    CommandReceived,
    CommandPending,
    CommandStarted,
    CommandCompleted,
    CommandFailed,
    CommandRejected,
    CommandCancelled,
    CommandUnknown,
    InputReceived,
    InputEnqueued,
    InputAdmitted,
    InputSuperseded,
    TitleRequested,
    TitleExtracted,
    SessionTitleApplied,
    ResumeSourceAttached,
    SubagentSpawnRequested,
    SubagentLinked,
    SubagentObserved,
    SubagentSpawnFailed,
    TaskStarted,
    TaskPending,
    TaskCompleted,
    TaskFailed,
    TaskCancelled,
    // Workflow 编排账(Workflow 接入 v3 第一棒,schema 文档 §四 workflow 域):
    // 编排事实的专用事件族。事件账 profile(V3EventLedger)只写 event 行、
    // 只认这些 kind——不造 system 首行、不写 message 行,不偷填假 session
    // 字段。payload 合同见 schema3 与 schema 文档 §四 workflow 条目。
    WorkflowDefinitionLoaded,
    WorkflowSegmentOpened,
    WorkflowInputsCommitted,
    WorkflowNodeReserved,
    WorkflowNodeDispatched,
    WorkflowNodeWaiting,
    WorkflowNodeRetrying,
    WorkflowNodeCompleted,
    WorkflowNodeFailed,
    WorkflowNodeCancelled,
    WorkflowNodeSkipped,
    WorkflowOutputCommitted,
    WorkflowCheckpointCommitted,
    WorkflowBranchStarted,
    WorkflowJoinCompleted,
    WorkflowLoopIterationStarted,
    WorkflowLoopIterationCompleted,
    WorkflowRunCompleted,
    WorkflowRunFailed,
    WorkflowRunCancelled,
};

const char* EventKindV3Name(EventKindV3 kind);
std::optional<EventKindV3> EventKindV3FromName(std::string_view name);
const std::vector<EventKindV3>& AllEventKindsV3();

// 生命周期事件(kind 后缀)要求的 status(schema 文档 §2.2 固定映射)。
// 返回 nullopt = 该 kind 不携带 status 字段。
std::optional<OpStatus> RequiredStatusForKind(EventKindV3 kind);

// ---------------------------------------------------------------------------
// message 行信封(schema 文档 §1.2)
// ---------------------------------------------------------------------------

struct MessageLine {
    // 公共信封
    std::string session_id;
    std::string run_id;
    std::uint64_t seq = 0;  // writer 发号;结构体阶段可为 0
    std::string timestamp;  // ISO-8601 UTC 毫秒,writer 生成
    std::string prev_hash;
    std::string line_hash;

    // message 特有
    std::string message_id;
    // turnId 键始终落盘(null 或 string);system 与 context_summary 摘要
    // 必须为 null(§1.2/§4.6)。
    std::optional<std::string> turn_id;
    std::optional<std::string> parent_turn_id;
    std::optional<std::string> step_id;
    std::optional<std::string> request_id;
    std::optional<std::string> action_id;
    std::optional<std::string> compact_id;
    MessagePurpose purpose = MessagePurpose::Conversation;
    MessageOrigin origin = MessageOrigin::Human;
    std::optional<DisplayMode> display;  // 缺省 visible,落盘省键
    nlohmann::json message = nlohmann::json::object();  // {"role": ..., ...}
    std::optional<std::string> caused_by_event_ref;
    std::optional<std::string> source_message_ref;
    // tool 消息的选用回执(§4.18/§4.19):指向 tool.result.selected 事件,
    // 声明"正文来自该次选定的结果版本"。
    std::optional<std::string> result_selection_ref;
    // 降档派生消息(§4.38)指回原 tool 消息:同一执行结果的更短预览版本。
    std::optional<std::string> source_tool_message_ref;
    std::optional<nlohmann::json> system_meta;  // system 消息必填
    std::optional<CompletionStatus> completion_status;  // 缺省 complete
    // assistant 来源(§4.44):provider/wire/model 必填;responseModel 键
    // 必须出现(可为 null)——用 json 直存(optional 有值即落键,null 合法)。
    std::optional<std::string> provider;
    std::optional<std::string> wire;
    std::optional<std::string> model;
    std::optional<nlohmann::json> response_model;  // json: string 或 null
    std::optional<std::string> provider_config_ref;
    std::optional<std::string> model_profile_ref;
    // usage 唯一 owner(§五):assistant 必带键;null = 缺实报,不补 0。
    std::optional<nlohmann::json> usage;  // json: object 或 null

    // 组 JSON(含 hash 键;发号/哈希归 writer)。可选字段空则不写键;
    // turnId/responseModel/usage 按"assistant 必现键"规则落 null。
    nlohmann::json ToJson() const;
    // 严格解析:未知键拒、类型与枚举错拒。语义校验在 schema3。
    static std::optional<MessageLine> FromJsonStrict(const nlohmann::json& json,
                                                     std::string* error_code,
                                                     std::string* message);
};

// ---------------------------------------------------------------------------
// event 行信封(schema 文档 §1.3)
// ---------------------------------------------------------------------------

struct EventLine {
    // 公共信封
    std::string session_id;
    std::string run_id;
    std::uint64_t seq = 0;
    std::string timestamp;
    std::string event_id;
    EventKindV3 kind = EventKindV3::SessionStarted;
    std::optional<OpStatus> status;  // 按 §2.2 映射,非生命周期不落键
    std::optional<std::string> turn_id;
    std::optional<std::string> parent_turn_id;
    std::optional<std::string> step_id;
    std::optional<std::string> request_id;
    std::optional<std::string> action_id;
    std::optional<std::string> compact_id;
    std::optional<std::string> command_id;
    std::optional<std::string> hook_dispatch_id;
    std::optional<std::string> task_id;
    std::optional<std::string> title_generation_id;
    nlohmann::json payload = nlohmann::json::object();
    std::optional<std::vector<nlohmann::json>> effects;     // §4.27
    std::optional<std::vector<nlohmann::json>> effect_refs;  // §4.27
    std::string prev_hash;
    std::string line_hash;

    nlohmann::json ToJson() const;
    static std::optional<EventLine> FromJsonStrict(const nlohmann::json& json,
                                                   std::string* error_code,
                                                   std::string* message);
};

// lineHash = SHA256(prevHash || canonical(line 去 prevHash/lineHash))。
// 承继 v2 ComputeEventHash,只是字段名 camelCase。
std::string ComputeLineHash(std::string_view prev_hash,
                            std::string_view canonical_line_without_hash_keys);

// 是否 64 位十六进制小写。
bool IsHex64(std::string_view value);

}  // namespace lubancode::trajectory::v3
