// Trajectory 事件总合同(P0 新轨迹记录单 §四/§五)。
//
// 每枚事实事件一只信封(EventEnvelope),所有 JSONL 共用同一形状。字段与
// 枚举照 §4.1-4.5 逐字段钉死,不另造。本件只认类型与转换;强校验在
// schema.hpp,状态机硬约束在 recorder.hpp。
//
// 依赖铁律:trajectory 是纯库,不 include app/cli/runtime;hash 用 hooks 的
// 自含 SHA-256,路径转换用 platform 的 UTF-8 通道。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::trajectory {

// ---------------------------------------------------------------------------
// 固定合同值(§4.1/§8.4)
// ---------------------------------------------------------------------------

// 信封 schema 名:防把别的 JSONL 当轨迹。
inline constexpr std::string_view kEventSchema = "lubancode.trajectory.event";
// 本件实现的 envelope schema 版本;只认已实现版本(§4.1)。
inline constexpr int kEnvelopeSchemaVersion = 1;
// v2(Token 账本单 §6.1.1):usage 事实从 model.output.completed.payload.usage
// 迁出,自立 model.usage.recorded 事件为 canonical owner。v1 completed 的
// usage 只作 legacy read。一条 stream 不混 v1/v2;session manifest 钉
// event schema major。v2 与 v1 的差异只在:
//   - 新增 kind model.usage.recorded(v1 stream 拒收);
//   - model.output.completed 的 payload 不再带 usage 键(v2 拒收该键)。
inline constexpr int kMaxEnvelopeSchemaVersion = 2;

// ---------------------------------------------------------------------------
// 枚举(§4.2-4.5)
// ---------------------------------------------------------------------------

// OneShot(单发轨迹断档单):one_shot 一场的 main run——单发不是"轻问答"
// 了,实战派活全走它;单列 run_kind 让账本/投影/管理面认得出这场是单发。
// 加值是纯增量:旧 stream 不含此名,旧读者读新档按 schema.bad_enum 拒
//(与超前版本同待遇);session.* 事件照许(单发一场也是进程的 main
// stream,recorder 状态机的白名单同步放开)。
enum class RunKind { MainSession, Subagent, Workflow, WorkflowNode, Goal, Loop, OneShot };

enum class Plane { Conversation, Execution, Control, Evidence };

enum class Actor { User, Model, Tool, Host, Verifier };

enum class Origin {
    ExternalUser,
    QueuedUser,
    PeerAgent,
    ScheduledHost,
    GoalContinuation,
    ProviderModel,
    BuiltinTool,
    McpTool,
    PluginTool,
    LspTool,
    SubagentTool,
    Hook,
    MemoryRecall,
    BackgroundCompletion,
    AgentRoster,
    BudgetGuard,
    CompactRuntime,
    RecoveryRuntime,
    VerifierHost,
};

enum class Visibility { ModelInput, ModelOutput, ToolInput, ToolOutput, HostOnly, UserVisible };

enum class TrainingPolicy { Include, Metadata, Exclude, Review };

// 落盘耐久档(§7.4)。
enum class Durability { Buffered, ProcessCrash, PowerLoss };

// 枚举 <-> 线上名。枚举名稳定,线上名是 v1 合同的一部分,改了便是改 schema。
const char* RunKindName(RunKind value);
std::optional<RunKind> RunKindFromName(std::string_view name);
const char* PlaneName(Plane value);
std::optional<Plane> PlaneFromName(std::string_view name);
const char* ActorName(Actor value);
std::optional<Actor> ActorFromName(std::string_view name);
const char* OriginName(Origin value);
std::optional<Origin> OriginFromName(std::string_view name);
const char* VisibilityName(Visibility value);
std::optional<Visibility> VisibilityFromName(std::string_view name);
const char* TrainingPolicyName(TrainingPolicy value);
std::optional<TrainingPolicy> TrainingPolicyFromName(std::string_view name);
const char* DurabilityName(Durability value);
std::optional<Durability> DurabilityFromName(std::string_view name);

// ---------------------------------------------------------------------------
// 事件种类(§五全列 67 种 + v2 的 model.usage.recorded,再加存储 v2 的
// context.injected 与 memory.save.* 三枚、子代理空轨迹单 P0-B 一枚、
// workflow 编排单十二枚,共 86 种)
// ---------------------------------------------------------------------------

enum class EventKind {
    // 5.1 生命周期
    RunStarted,
    RunEnvironmentCaptured,
    RunCompleted,
    RunFailed,
    RunCancelled,
    SessionClearRequested,
    SessionEnded,
    TurnStarted,
    TurnCompleted,
    TurnFailed,
    TurnCancelled,
    // 5.2 输入与上下文
    InputReceived,
    ContextAttached,
    ContextDetached,
    // P0-3(存储 v2 合同 §四):记忆召回快照——每次真正注入模型的记忆落
    // 一枚,payload 带 memory_id/content_sha256/snapshot_ref,Replay 凭它
    // 重建"当时模型看见哪一版",不读今天的 Memory。
    ContextInjected,
    // 派工长任务预检三项账:最终 token 窗口判定当场落，包含应急收窄
    // 放行与稳定拒绝两支；不冒充上下文内容附着/移除事件。
    ContextPressureRecorded,
    // 5.3 模型请求与输出
    ModelRequestPrepared,
    ModelRequestSent,
    ModelOutputCompleted,
    ModelOutputFailed,
    ModelOutputCancelled,
    // v2(Token 账本单 §6.1.1):一次 request attempt 的 usage canonical
    // owner。completed/failed/cancelled 都引用它,不复制 usage 正文。
    ModelUsageRecorded,
    // 5.4 工具调用与结果
    ToolExecutionPlanned,
    ToolInputEffective,
    ToolExecutionStarted,
    ToolExecutionFinished,
    ToolExecutionFailed,
    ToolExecutionCancelled,
    ToolExecutionUnknown,
    ToolResultCommitted,
    // 5.5 控制与证据
    ControlCommandRequested,
    ControlCommandCompleted,
    ControlCommandFailed,
    ControlCommandCancelled,
    ControlCommandRejected,
    ControlTitleChanged,
    ControlCwdChanged,
    ControlModeChanged,
    ControlContextWindowChanged,
    ControlCheckpointCreated,
    ControlQueueItemEnqueued,
    ControlQueueItemDequeued,
    ControlQueueItemCancelled,
    ControlQueueItemExpired,
    ControlQueueSnapshot,
    ControlApprovalRequested,
    ControlApprovalResolved,
    ControlApprovalExpired,
    ControlCancellationRequested,
    ControlCancellationApplied,
    CompactRequested,
    CompactRequestPrepared,
    CompactOutputGenerated,
    CompactValidationCompleted,
    CompactApplied,
    CompactFailed,
    CompactCancelled,
    CompactRejected,
    RecordSelectionStarted,
    RecordSelectionPaused,
    RecordSelectionResumed,
    RecordSelectionNoteAdded,
    RecordSelectionCompleted,
    RecordSelectionCancelled,
    RecordSelectionInterrupted,
    ResumeSourceAttached,
    VerificationStarted,
    VerificationRecorded,
    VerificationInvalidated,
    OutcomeAssessed,
    // 5.6 Memory 写入因果边(存储 v2 合同 §四,P0-3 接线):requested 落在
    // 请求写入的那场 session;committed/failed 由异步 worker 写 workspace
    // lifecycle 回执(单子 §6.3),事件种类先入合同,回执补记归后续波次。
    MemorySaveRequested,
    MemorySaveCommitted,
    MemorySaveFailed,
    // 子代理空轨迹单 P0-B:子 run 开张失败的父侧 typed 事实。由父 run 持有
    //(main stream),payload 只记阶段与稳定码、引用边(reserved_run_id/
    // stream_ref),不抄子 prompt 正文与敏感绝对路径——诊断要能跨进程留证,
    // 不靠终端滚屏。
    SubagentRunStartFailed,
    // workflow 编排账(workflow 会话归属统一单,§3.6):编排 Journal 只记
    // 编排事实——定义装载、node 派发/终态、branch/join、loop 轮次、
    // checkpoint。node attempt 的模型请求/工具正文各落 nodes/<run>.jsonl,
    // 不进这族;纯控制节点只记编排,不另开 node 文件。
    WorkflowDefinitionLoaded,
    WorkflowNodeDispatched,
    WorkflowNodeRetrying,
    WorkflowNodeWaiting,
    WorkflowNodeCompleted,
    WorkflowNodeFailed,
    WorkflowNodeSkipped,
    WorkflowBranchStarted,
    WorkflowJoinCompleted,
    WorkflowLoopIterationStarted,
    WorkflowLoopIterationCompleted,
    WorkflowCheckpointSaved,
};

const char* EventKindName(EventKind kind);
std::optional<EventKind> EventKindFromName(std::string_view name);
// 全部合法 kind 名(排序稳定;测试与 schema 遍历用)。
const std::vector<EventKind>& AllEventKinds();

// turn_id/request_id/call_id 三档要求(§4.1"该有而没有,提交器拒绝")。
enum class IdNeed { Required, Optional, Forbidden };

struct EventKindInfo {
    const char* name;
    Plane plane;      // v1 固定面(§4.2 的分类落死在合同里)
    IdNeed turn;      // envelope.turn_id 要求
    IdNeed request;   // envelope.request_id 要求
    IdNeed call;      // envelope.call_id 要求
    bool main_stream_only;  // 只许出现在 main stream(session.* 等,§5.1)
};
const EventKindInfo& EventKindInfoOf(EventKind kind);

// actor/origin 合法组合(§4.3 表)。peer/宿主注入不得挂 user 名下。
bool IsValidActorOrigin(Actor actor, Origin origin);

// ---------------------------------------------------------------------------
// EventEnvelope(§4.1 全字段)
// ---------------------------------------------------------------------------

struct EventEnvelope {
    std::string workspace_key;
    std::string session_id;
    std::string run_id;
    RunKind run_kind = RunKind::MainSession;
    std::uint64_t seq = 0;  // run 内连续递增,不回收;发号归 recorder
    std::string event_id;   // run_id + ":evt-" + 8 位零填 seq
    EventKind kind = EventKind::RunStarted;
    Plane plane = Plane::Control;
    Actor actor = Actor::Host;
    Origin origin = Origin::RecoveryRuntime;
    std::vector<Visibility> visibility;
    TrainingPolicy training_policy = TrainingPolicy::Exclude;
    std::optional<std::string> turn_id;
    std::optional<std::string> request_id;
    std::optional<std::string> call_id;
    std::optional<std::string> causation_id;
    std::optional<std::string> correlation_id;
    // §6.3 通用关系字段(retry_of/compensates/parent_call_id/parent_run_id/
    // child_run_id/blocked_by),整包落信封可选键 "relations";键集封闭。
    nlohmann::json relations = nlohmann::json::object();
    std::int64_t wall_time_ms = 0;
    std::int64_t monotonic_ns = 0;
    nlohmann::json payload = nlohmann::json::object();
    std::string prev_hash;   // 上一枚 event_hash;首枚为 kGenesisHash
    std::string event_hash;  // SHA256(prev_hash || canonical(无 event_hash))
    int schema_version = kEnvelopeSchemaVersion;

    // 组 JSON(canonical dump 直接可用)。可选字段空则不写键;schema 与
    // canonical dump 两边对"空=缺省"同一口径。
    nlohmann::json ToJson() const;
    // 严格解析:未知键拒绝,类型与枚举错拒绝。结构之外的语义校验在
    // schema.hpp。
    static std::optional<EventEnvelope> FromJsonStrict(const nlohmann::json& json,
                                                       std::string* error_code,
                                                       std::string* message);
};

// event_id 固定形状:run_id + ":evt-" + %08llu(§4.1 样例)。
std::string FormatEventId(std::string_view run_id, std::uint64_t seq);

// 是否 64 位十六进制小写(hash 字段形状)。
bool IsHex64(std::string_view value);

// run 终态与 session 终态的封口 payload 四件套(§8.3)。
nlohmann::json MakeTerminalSealPayload(std::string_view first_event_hash,
                                       std::uint64_t event_count_before_terminal, int schema_version,
                                       std::string_view recorder_version);

}  // namespace lubancode::trajectory
