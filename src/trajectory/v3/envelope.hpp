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
// goal_evaluation(§4.67.6):验收内部回合的实际 system/user/assistant,
// 不进 main 输入链;goal_continuation 的续跑 user 正文走 Conversation
//(它真进 main),宿主来源在 origin 区分,不另立 purpose。
// action_summary(B2):整批结果压缩的摘要模型内部回合,同样不进 main 链。
// memory_extract(记忆抽取取消误报 ESC 单 Bug 2):回合收尾记忆抽取的
// 旁路内部回合(system/转写 user/assistant),不进 main 链——旁路桥
// TrajectoryBypassBridge 的 v3 写模式用它;schema 纯追加,旧账零出现。
enum class MessagePurpose {
    Conversation,
    Compact,
    ContextSummary,
    SessionTitle,
    Capability,
    GoalEvaluation,
    ActionSummary,
    MemoryExtract,
};
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
    ToolResultSummaryFinished,
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
    // 上下文预算单(P1):会话窗口预算的事实提交(与 session.title.applied
    // 同族的控制状态行,不带 status)。payload 合同:contextWindow(正整
    // 数,token 数)必填;oldContextWindow/provider/model/source(写账来路
    // manual|initial|resumed)可选——旧档与最小写入不带,读取侧按缺省读。
    SessionContextWindowApplied,
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

    // Goal 模式(§4.67 G0):goal 控制状态的唯一生效点。事实提交,不带
    // status(与 context.*.applied 同族:控制状态提交,不是操作生命周期)。
    // payload 合同:goalId/fromStateRevision/toStateRevision/contractRevision/
    // snapshotRef/snapshotSha256/lifecycle(+可选 causeRef);完整 goal 状态
    // 在不可变快照 sessions/<id>/state/goals/<goalId>/rev-*.json,本行只记
    // 提交锚(§4.55)。
    StateGoalApplied,
    // Goal 模式 G2(§4.67.6 表):checkpoint/evidence 的事实记录(不改活动
    // head)与验收三段。全部 statusless 事实行——requested 是"发起验收"
    // (材料版本冻结),completed 是"判词到手"(不等于目标已完成),rejected
    // 是"候选被拒"(校验不过/二次失败,带原因);goalId/evaluationId 走
    // payload,与 state.goal.applied 同口径(控制状态族不占信封身份字段)。
    GoalCheckpointRecorded,
    GoalEvidenceRecorded,
    GoalEvaluationRequested,
    GoalEvaluationCompleted,
    GoalEvaluationRejected,
    // Goal 模式 G3(§4.67.6/§4.67.7):后台等待与预算归属的事实行。
    // goal.wait.registered:goal 登记后台等待(taskRefs、通知去重键、巡检
    // 计划);等待计划是否生效仍看 state.goal.applied,本行只是登记事实。
    // goal.wait.resolved:等待解除(交付去重键 + 原因);迟到解除不改账。
    // goal.usage.recorded:逐 requestId 的 usage 归属与计量来源;投影累计
    // 值、(sessionId,requestId) 去重,不重复计费。三者全部 statusless。
    GoalWaitRegistered,
    GoalWaitResolved,
    GoalUsageRecorded,

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

    // 异步工具族(异步工具单 P0,合同与 fixture 已验、生产未接):任务执行、
    // 协议配对与结果投递的事实行。全部 statusless——registered/dispatched/
    // acknowledged 只表示事件已发生,不等于业务 job 已完成;执行/投递状态
    // 由 payload(observedStatus 等)与读取侧投影表达,unknown 是执行投影
    // 状态,不硬塞本信封的 status(§二 2.2 豁免同 state.goal.applied 族)。
    // 执行终态、结果持久化与选用复用已有 tool.execution/tool.result 事件,
    // 不另造重复身份。身份映射(单 §5 逻辑名 → 落点):actionId/attempt=
    // 信封 actionId+payload attempt;jobId/deliveryId/ownerEpoch/wireCallRef/
    // resultRef+resultVersion=payload;targetRequestId=信封 requestId;
    // originRef=信封 turnId/stepId+payload assistantMessageRef。
    ToolJobRegistered,
    ToolJobDispatched,
    ToolJobObserved,
    ToolJobCancelRequested,
    ToolDeliveryPrepared,
    ToolDeliveryAcknowledged,
    ToolDeliveryUncertain,
    // 能力快照(§4 能力三态):provider/endpoint/wire/model/工具声明/运行
    // 配置合成的能力判定及其依据的留档。纯合同+fixture,不接真探针;
    // unknown 默认不用 native_deferred 的闸门在读取侧 DecideAsyncModes。
    ToolCapabilityRecorded,

    // Gateway 常驻总装 V0(总装单 §五/§六/§九):work↔turn 绑定事实与回复
    // 选择提交。两者都是 statusless 事实行,合同与 fixture 先行、生产装配
    // 归 V1(与 tool.job 族同规矩,schemaVersion 纯追加不改旧义)。
    // gateway.work.bound:预留 turn 身份绑定 workId 的关联事实——恢复器
    // 扫 V3 流凭 workId 反查原轮(§六"V3 已开轮,work 还没记 turnRef"
    // 窗口),不另派新轮;ownerEpoch/attempt 辨新旧认领。
    // reply.selection.committed:对外回复的选定事实(§九:引用最终正文
    // artifact 与来源 message;原件先落稳、选择事实后提交,resume 后
    // selectionId 不变,不重新散列投递身份)。
    GatewayWorkBound,
    ReplySelectionCommitted,

    // 提示组合事实(应用Worker接入单 §五 134):部署档组合系统提示的
    // 可追溯记录——组合次序、各段渲染正文 hash、来源层与最终快照 ID。
    // statusless 事实提交(同 state.goal.applied 族):只记"本场用了哪份
    // 组合",不改控制状态;业务正文伪装不了宿主权限(来源逐段在账)。
    PromptCompositionApplied,

    // 记忆账(记忆抽取取消误报 ESC 单 Bug 2,schema 纯追加;旧账零出现):
    //   memory.extraction.assessed —— 回合收尾的抽取门控与结果评估(v2
    //     同名事件的 v3 对应;statusless 事实,turnId=触发它的主回合)。
    //   memory.write.receipted —— 四路写路(save/forget/accept)的排队/
    //     被拒回执(v2 同名事件的 v3 对应;statusless 事实)。
    MemoryExtractionAssessed,
    MemoryWriteReceipted,
    // 记忆账续(T08/V3-GAP-03,Session v3 旧设计清理单;schema 纯追加,
    // 旧账零出现):
    //   memory.recall.injected —— 一条记忆真正注入模型的完整事实(v2
    //     context.injected 的 v3 对应;statusless)。主会话注入:正文快照
    //     落 display=hidden 的正式 user 消息(origin=context_runtime,不冒
    //     充人类输入)、AdmitMessages 接纳进链,事件载荷带 memoryId/
    //     revision/contentSha256 与 messageRef——恢复拿快照解释旧请求,不
    //     再用当下 topic 正文倒推。派工冻结(targetRunId 非空):正文内联
    //     或 snapshot_ref 落事件载荷,父账不接纳进链(父模型没见过这段)。
    //   memory.save.requested —— 写入因果边(v2 同名事件的 v3 对应;
    //     statusless)。requested 只记"谁发起了一笔写",排队成败看
    //     memory.write.receipted,落盘回执在 workspace lifecycle(memory.
    //     save.committed)——三态按真实回执分账,不互相冒充。
    MemoryRecallInjected,
    MemorySaveRequested,
    // 渠道远端审批(QQ 接入单 Q6 §12.2):渠道会话里须确认工具的审批
    // 请求与裁决,全 statusless 事实行。requested 记宿主发的审批卡
    //(token hash、工具名、规范参数 hash、身份摘要、期限——正文参数
    // 不入账,脱敏由宿主摘要层保证);resolved 记裁决(approved/
    // declined/timeout/cancelled/card_failed,by=操作者或收口原因)。
    // 关联锚:信封 turnId + payload toolUseId(与 gateway.work.bound 同款
    // 反查路)。账不裁决:决议生效在审批 broker,这里只留审计事实。
    ChannelApprovalRequested,
    ChannelApprovalResolved,

    // ---- T11 / V3-GAP-06(Session v3 旧设计清理单):五域遗漏事实,schema
    // 纯追加,全部 statusless 事实行,旧账零出现。----
    // 标题来源(T11-A):session.title.applied 的 payload.source 分 manual/
    // local/generated/inherited;generated 行带真 titleGenerationId(与
    // title.requested/extracted 同号),manual/local 不伪造生成身份。
    // 环境快照(T11-C):session.environment.captured——本场 run 的取材事实
    //(OS/git/provider/能力配置快照经 blob 引用 + 重现等级 + 缺口清单)。
    // 捕获时间即信封 timestamp;没采集的场,读取侧按缺件处理,不拿今天
    // 环境补昨天事实。
    SessionEnvironmentCaptured,
    // 审批档位(T11-B):approval.mode.applied——档位事实与单次审批(channel
    // .approval.* / 确认门)分家。payload 带 mode/source/policyVersion
    //(source ∈ launch/user_toggle/resume_recomputed/inherited);恢复时
    // 有效档 = min(源场档,当前策略)重算,不静默提权,本行只记事实。
    ApprovalModeApplied,
    // 验证/迟到/恢复注记(T11-D):verification 关联工具(actionId)与产物
    // 版本;失效原因(verification.invalidated)、迟到响应(tool
    // .observation.late)与恢复注记(recovery.note.recorded)只记观察,
    // 不覆盖已提交终态,也不触发工具重做。
    ToolVerificationRecorded,
    ToolVerificationInvalidated,
    ToolObservationLate,
    RecoveryNoteRecorded,
    // 容量/预算(T11-E):context.pressure.recorded——发送前容量压力与预算
    // 裁决(verdict ∈ reserve_clamped/exceeded_denied/max_tokens_degraded)。
    // 不复制累计用量:用量唯一可累计事实仍是 assistant message 的 usage
    // owner(§五),本行只带当次判定数字与剩余量。
    ContextPressureRecorded,
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
