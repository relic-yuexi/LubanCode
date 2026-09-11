// v3 读取侧(P2):一份原始账,三项读取结果(单子 §4.1)。
//
//   历史时间线   全部消息/事件/system 变更/压缩标记可滚动;原文保留,
//                标明哪些内容已由摘要替代——不因 compact 折叠丢滚动史。
//   当前模型上下文 当前选中 system + 摘要 + 保留消息 + 后续输入,从
//                contextChain 投影,排除 compact 内部问答(它们从未入链)。
//   单次请求输入  model.request.prepared 的 inputMessageRefs 固定不变;
//                本件按 revision 历史链回放,与 prepared 对表(§4.30
//                "验收需比较链遍历结果与请求 inputMessageRefs")。
//
// 其余读取投影:按 actionId 折叠工具快照(§4.19)、result_preview 展开
// (§4.18"读取投影",缺 blob 标缺口)、跨会话五键引用验 hash(§3.1)、
// 父子账递归遍历(§4.31)、带来源链的 resume(§4.10/§4.59)。
//
// 纯读:不开写柄、不调模型、不重跑工具、不发外部消息(§5.1"只读
// replay 零调用零重跑");验卷复用 VerifyV3File(哈希链+语义)。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/hooks.hpp"
#include "trajectory/v3/writer.hpp"

namespace lubancode::trajectory::v3 {

// ---------------------------------------------------------------------------
// 读账:验卷 + 逐行解析 + 索引 + 每版链历史
// ---------------------------------------------------------------------------

struct V3Ledger {
    struct Entry {
        std::uint64_t seq = 0;
        bool is_message = false;
        std::size_t index = 0;  // messages[] 或 events[] 下标
    };

    std::filesystem::path path;
    std::string session_id;
    std::string run_id;
    std::uint64_t lines = 0;
    std::vector<MessageLine> messages;  // 落盘序(seq 升序)
    std::vector<EventLine> events;      // 落盘序(seq 升序)
    std::vector<Entry> timeline;        // 两类行合流,seq 升序

    std::map<std::string, std::size_t> message_index;  // messageId → messages[]
    std::map<std::string, std::size_t> event_index;    // eventId → events[]

    ContextView context;  // 重放终态(同 writer::Continue)

    // 每版链的 (system,有序输入引用):单次请求输入回放对表用。
    // key = contextRevision;value.first = 根 system messageRef,
    // value.second = 链上根之后的有序 messageRef。
    std::map<std::uint64_t, std::pair<std::string, std::vector<std::string>>> revision_chains;

    const MessageLine* FindMessage(std::string_view id) const;
    const EventLine* FindEvent(std::string_view id) const;
    // 末行身份(跨会话五键引用的源侧)。
    std::optional<Entry> LastEntry() const;
};

// 验卷不过(坏行/断链/截断尾)→ unexpected,错误码前缀 v3writer.*。
std::expected<V3Ledger, std::string> ReadV3Ledger(const std::filesystem::path& jsonl);

// ---------------------------------------------------------------------------
// 压缩标记与 system 切换视图(时间线专用)
// ---------------------------------------------------------------------------

struct CompactMarkerView {
    std::string event_id;  // compact.applied
    std::string compact_id;
    std::uint64_t seq = 0;
    std::string timestamp;
    // token 标记读 applied 持久字段,resume 不拿今天的 tokenizer 重算(§4.11)。
    std::uint64_t context_tokens_before = 0;
    std::uint64_t context_tokens_after = 0;
    std::string token_metric;  // 估算口径(json 序列化;缺省空)
    std::string trigger;
    std::string summary_message_ref;
    std::string validation_event_ref;
    std::vector<std::string> removed_message_refs;   // 只退出模型上下文,原档不删
    std::vector<std::string> retained_message_refs;
    std::vector<std::string> protected_turn_ids;
    std::uint64_t source_revision = 0;
    std::uint64_t new_revision = 0;
};

struct SystemSwitchView {
    std::string event_id;  // system.change
    std::uint64_t seq = 0;
    std::string timestamp;
    std::string cause;
    bool system_changed = false;
    std::string old_system_ref;
    std::string new_system_ref;  // 随后的 context.system.applied 根(未完成时空)
    bool completed = false;      // applied 落稳才真切换(§4.3)
};

// ---------------------------------------------------------------------------
// 投影一:历史时间线(§4.1 行 1)
// ---------------------------------------------------------------------------

struct TimelineMessageView {
    std::uint64_t seq = 0;
    std::string message_id;
    std::string timestamp;
    MessageRole role = MessageRole::User;
    MessagePurpose purpose = MessagePurpose::Conversation;
    MessageOrigin origin = MessageOrigin::Human;
    DisplayMode display = DisplayMode::Visible;
    std::optional<CompletionStatus> completion_status;
    std::optional<nlohmann::json> usage;  // assistant 实报;缺实报为 nullopt,
                                          // 读取不补 0(§五)
    std::optional<std::string> provider;
    std::optional<std::string> model;
    // 上下文状态(多次压缩不能靠永久布尔,§4.10):
    bool in_current_context = false;      // 在当前链上
    bool replaced_by_derivation = false;  // 降档退链的原版(§4.38)
    std::string derived_from;             // 派生消息指回的原消息(sourceToolMessageRef)
    std::vector<std::string> removed_by_compacts;  // 历次把它移出上下文的 compactId
};

struct HistoryTimeline {
    struct Item {
        enum class Kind { Message, SystemSwitch, CompactMarker, PreviewReduction, Event };
        Kind kind = Kind::Event;
        std::uint64_t seq = 0;
        std::string timestamp;
        std::string id;  // messageId 或 eventId
        TimelineMessageView message;    // kind=Message
        CompactMarkerView compact;      // kind=CompactMarker
        SystemSwitchView system_switch;  // kind=SystemSwitch
        std::string event_kind_name;    // kind=Event/PreviewReduction 的 kind 名
    };
    std::vector<Item> items;  // seq 升序,两类行与标记合流
    // messageId → items[] 下标(滚动定位)。
    std::map<std::string, std::size_t> message_items;
    // compactId → 压缩标记 items[] 下标(展开详情用)。
    std::map<std::string, std::size_t> compact_items;
};

// 历史时间线投影:全部消息(含 hidden/compact 内部问答)+ system 切换 +
// 压缩标记 + 降档标记,按 seq 滚动;逐消息标注上下文状态。
HistoryTimeline ProjectHistoryTimeline(const V3Ledger& ledger);

// ---------------------------------------------------------------------------
// 投影二:当前模型上下文(§4.1 行 2)
// ---------------------------------------------------------------------------

struct ModelContextMessage {
    std::string message_id;
    MessageRole role = MessageRole::User;
    MessagePurpose purpose = MessagePurpose::Conversation;
    nlohmann::json message;  // message 本体(role/content/...),原样
    bool derived_preview = false;  // 降档派生版本(§4.38)
};

struct ModelContext {
    std::string context_id;
    std::uint64_t revision = 0;
    std::string system_message_id;
    std::string system_content;  // 完整拼装结果,读取不重拼(§4.3)
    std::vector<ModelContextMessage> inputs;  // 链序(根之后)
    std::uint64_t preview_budget_bytes = 32768;
    // 校验:同一 actionId 在链上只许一个预览版本(§4.38"任何一次请求
    // 只选一个版本")。非空 = 坏链信号。
    std::vector<std::string> duplicate_action_versions;
    // 链引用在账上找不到(验过卷后不应发生;防住仍报,§4.4"空缺引用
    // 不能从历史猜一份填补")。
    std::vector<std::string> missing_refs;
    // 未收口 compact(无终态):按源上下文恢复,内部回合不冒充主链(§4.8)。
    std::vector<std::string> open_compact_ids;
};

// 当前模型上下文投影:从 ledger.context.chain 取选中 system 与有序输入;
// compact 内部问答/prompt/候选回复从未入链,天然排除(§4.9)。
ModelContext ProjectModelContext(const V3Ledger& ledger);

// 单次请求输入回放(§4.1 行 3):prepared 的 systemMessageRef 与
// inputMessageRefs 必须等于该 contextRevision 的链投影。返回空串 = 一致。
std::string CheckPreparedAgainstChain(const V3Ledger& ledger, std::string_view prepared_event_id);

// ---------------------------------------------------------------------------
// 按 actionId 折叠的工具快照(§4.19)
// ---------------------------------------------------------------------------

struct ToolAttemptView {
    std::uint64_t attempt = 1;
    std::string status;  // 折叠后:pending/running/done/failed/cancelled/
                         // rejected/unknown(§4.14"不能只取最后一行 status")
    bool started = false;
    bool waiting = false;
    std::string pending_reason;
    std::string waiting_reason;
    std::optional<std::int64_t> exit_code;      // null = 未知,不默认 0
    std::optional<std::uint64_t> execution_duration_ms;
    std::optional<std::string> effective_args_ref;
    std::optional<std::string> idempotency_key;
    std::vector<std::string> event_ids;  // 本 attempt 的 tool.execution.* 事件
    // started 后无终态:恢复投影标 unknown,只读重放不合成假终态(§4.20)。
    bool unknown_recovery = false;
};

struct ToolActionSnapshot {
    std::string tool_call_id;  // actionId
    std::string turn_id;
    std::string step_id;
    std::optional<std::string> assistant_message_ref;  // 声明消息
    std::optional<std::string> provider_tool_call_id;
    std::optional<std::string> tool_name;     // 声明块 function.name(取得到时)
    std::optional<nlohmann::json> declared_args;  // 声明块参数(原始 args 的
                                                  // owner 是 assistant 调用块)
    std::vector<ToolAttemptView> attempts;

    // 结果链(§4.18):persisted(可多次/attempt 多版本)→ selected。
    std::vector<std::string> persisted_event_refs;
    std::vector<std::vector<nlohmann::json>> result_refs;  // 与上对齐
    std::optional<std::string> persist_failed_reason;
    std::optional<std::string> selected_event_ref;
    std::string effective_outcome;  // selected 载荷;无选用为空

    // 消息版本:原版 + 降档派生,seq 序;当前链上只一支(§4.38)。
    struct MessageVersion {
        std::string message_id;
        bool on_current_chain = false;
        std::string source_tool_message_ref;  // 派生版指回原版
    };
    std::vector<MessageVersion> message_versions;

    // 整体折叠状态(§4.14:由尝试链、重试决策、结果持久状态和消息提交
    // 状态共同折叠):done/failed/cancelled/rejected/unknown/pending/
    // running;另有三档缺口态(失败与恢复单 P1-A):selected_no_message =
    // 已选用结果、tool 消息未提交(§4.59 恢复补消息不重跑);
    // result_missing = 执行已有终态、结果链没立起来(补保存/补接纳,不
    // 重跑);message_not_admitted = tool 消息已写、接纳未成(不冒充有效
    // 上下文,按提交链补接纳)。
    std::string folded_status;
};

// 折叠全部工具调用;actionId 升序(tool 序号即声明顺序)。
std::vector<ToolActionSnapshot> FoldToolActions(const V3Ledger& ledger);
const ToolActionSnapshot* FindActionSnapshot(const std::vector<ToolActionSnapshot>& snapshots,
                                             std::string_view action_id);

// ---------------------------------------------------------------------------
// hook dispatch 折叠(LuaHook 单 P0-B,§4.22/§7.1/§7.3):把 hook.* 事件
// 折成"每 dispatch、每 invocation 干到哪一步"的恢复视图。纯读:不执行
// 脚本、不连 MCP(§5.1 只读 replay 零调用零重跑)。
// ---------------------------------------------------------------------------

struct HookEffectView {
    std::string effect_type;
    bool applied = false;
    std::string reason;              // rejected 的原因
    nlohmann::json applied_value;    // applied 时随行的采用值(input.rewrite
                                     // 的候选即工作版本;缺省 null)
    std::string event_id;
};

struct HookInvocationView {
    std::string invocation_id;
    std::string hook_id;
    std::string handler_kind;
    std::string definition_hash;
    int definition_order = 0;
    // 折叠后:running/completed/failed/cancelled/unknown;started 事件缺
    // 失的 matched 条目不出现在这里(skipped 由 dispatch 层汇总)。
    std::string status;
    std::optional<std::string> decision;
    // 洋葱语义(§7.1):proposed 候选与 continuation 消费。
    bool continuation_consumed = false;
    std::vector<std::pair<std::string, nlohmann::json>> outputs_proposed;  // phase -> candidate
    std::vector<HookEffectView> effects;  // 事件序
    std::string terminal_event_id;        // completed/failed/cancelled/unknown 事件
};

struct HookDispatchView {
    std::string dispatch_id;
    std::string hook_point;
    std::optional<std::string> turn_id, step_id, action_id, request_id;
    bool requested = false;   // hook.dispatch.requested 在账
    bool skipped = false;
    std::string skip_reason;
    std::vector<HookHandlerSpec> matched_handlers;  // requested 快照(§4.22:
                                                    // resume 不改读今天的脚本)
    std::vector<HookInvocationView> invocations;    // started 序
    // 折叠状态:requested/skipped/completed/denied/failed/cancelled/unknown/
    // running(dispatch 开着未收口)。
    std::string folded_status;
    // 恢复用:链上最近一次采用的 input.rewrite 候选(工作版本;无 = 原输入)。
    bool has_adopted_working_input = false;
    nlohmann::json adopted_working_input;
    // 已采用的 context.append 文本(事件序;恢复时防重复注入的对账底)。
    std::vector<std::string> adopted_context_appends;
};

// 折叠全部 hook dispatch(落盘序)。事件流是良嵌套洋葱序,折叠只看每枚
// invocation 的 started/终态与效果事件,不依赖嵌套形状。
std::vector<HookDispatchView> FoldHookDispatches(const V3Ledger& ledger);
const HookDispatchView* FindHookDispatch(const std::vector<HookDispatchView>& dispatches,
                                         std::string_view dispatch_id);

// ---------------------------------------------------------------------------
// result_preview 读取投影(§4.18)与 artifact 缺口
// ---------------------------------------------------------------------------

struct ArtifactProbe {
    std::string artifact_id;
    std::string path;       // 相对 session 根
    bool exists = false;    // 文件在
    bool hash_ok = false;   // sha256 对上 ref
    std::uint64_t bytes = 0;      // 实际文件字节数
    std::string gap_reason;       // missing_blob / hash_mismatch / unreadable
};

struct ResultPreviewProjection {
    std::string tool_message_id;
    std::string result_preview;  // 读取投影 = 该 tool 消息的 content 正文
                                 //(§4.18:原始 event 不复制预览文本)
    std::string result_selection_ref;
    std::string summary_event_ref;
    std::vector<std::string> summary_candidate_refs;
    std::vector<std::string> source_result_event_refs;  // selected → persisted
    std::vector<nlohmann::json> result_refs;            // 选用链上全部 artifactRef
    std::vector<ArtifactProbe> artifacts;               // 对 session_dir 实探
    // 无缺口才可宣称完整(§4.10"明确显示缺件,不能声称原文齐全")。
    bool complete = true;
};

// 展开:tool 消息 → resultSelectionRef → tool.result.selected →
// sourceResultEventRefs → tool.result.persisted → result_ref[];
// 逐枚 artifact 对 session_dir 实探(存在性 + sha256)。
// session_dir 为空 → 只做引用链展开,artifacts 全部标 missing_blob。
ResultPreviewProjection ExpandResultPreview(const V3Ledger& ledger,
                                            const std::filesystem::path& session_dir,
                                            std::string_view tool_message_id);

// ---------------------------------------------------------------------------
// 跨会话五键引用(§3.1/§4.2)
// ---------------------------------------------------------------------------

struct CrossSessionRef {
    std::string session_id;
    std::string run_id;
    std::string id;
    std::uint64_t seq = 0;
    std::string hash;
};

// 五键对象 → 结构;形状不对返回 nullopt。
std::optional<CrossSessionRef> ParseCrossSessionRef(const nlohmann::json& ref);

struct CrossSessionRefCheck {
    bool ok = false;
    std::string reason;  // ledger_unreadable / session_mismatch / run_mismatch /
                         // seq_out_of_range / id_mismatch / hash_mismatch
};

// 对目标账验五键:sessionId/runId 对得上、seq 在界内、该行 id 与 lineHash
// 都对得上(§4.2"跨旧会话的引用至少含 sessionId/runId/消息或事件 ID,
// 并校验源 hash,不能只存裸 seq")。
CrossSessionRefCheck VerifyCrossSessionRef(const CrossSessionRef& ref, const V3Ledger& target);

// ---------------------------------------------------------------------------
// 父子账递归遍历(§4.31)
// ---------------------------------------------------------------------------

struct SubagentSessionNode {
    std::string session_id;
    std::string run_id;
    std::filesystem::path jsonl_path;  // 由 childSessionRef.journalPath 解析
    std::string parent_session_id;
    std::string parent_action_id;
    std::string task_id;
    std::string spawn_event_id;
    // linked / not_linked / spawn_failed / child_missing / unreadable /
    // cycle / spawned(子账在、父侧未见 linked)
    std::string link_status;
    // 子账首行 systemMeta.spawnEventRef 五键对父账 spawn 事件的验hash结果。
    CrossSessionRefCheck source_check;
    std::optional<V3Ledger> ledger;  // 可读则载入(含自身链视图)
    std::vector<SubagentSessionNode> children;
};

// 从 root_jsonl 出发递归遍历 subagents/:父账 spawn.requested →
// childSessionRef → 子账 → 子账自己的 subagents/……环与重复(sessionId
// 已见过)标 cycle 不再下钻;子账缺失标 child_missing,不宣称完整恢复。
SubagentSessionNode WalkSessionTree(const std::filesystem::path& root_jsonl, int max_depth = 8);

// ---------------------------------------------------------------------------
// 带来源链的 resume(§4.10/§4.59)
// ---------------------------------------------------------------------------

// resume.source.attached 的载荷合同(P2 读取侧定,随 schema 增补登记):
//   payload.sourceRef = {sessionId, runId, seq, id, hash}  五键指源末行
//   payload.contextRevision / payload.systemMessageRef / payload.branch
struct ResumeSourceStep {
    std::string session_id;
    std::string run_id;
    std::filesystem::path jsonl_path;
    std::optional<CrossSessionRef> ref;    // 本步 attached 里的五键
    CrossSessionRefCheck check;            // 验 hash 结果
    bool duplicate = false;                // 链上重复显示,不重复计入(§4.10)
    std::optional<V3Ledger> ledger;        // 祖先账(历史分页用;缺失为空)
};

struct ResumeExecutionState {
    // 未收口工具(§4.59:tool1/2 已配齐不重跑,tool3/4 从原地续):
    // 含无终态、started 无终态(unknown_recovery)、已选用无消息
    //(selected_no_message,补消息不重跑)。
    std::vector<ToolActionSnapshot> open_actions;
    // 无终态 compact(ContextView.open_compact_ids)。
    std::vector<std::string> open_compact_ids;
    // 有未收口工作的 turn(恢复决策:先续工具再请求模型)。
    std::vector<std::string> turns_with_open_work;
};

struct ResumeProjection {
    // ③ 历史索引:本账时间线 + 祖先账(各自 seq,不跨文件混排)。
    HistoryTimeline timeline;
    std::vector<ResumeSourceStep> source_chain;  // 直接源在前,祖先在后
    bool source_chain_ok = true;                 // 全验过、无环、无重复
    // ② 模型输入:只取本账链,请求不重携祖先全史(§4.10)。
    ModelContext model_context;
    // ① 执行状态。
    ResumeExecutionState execution;
    // token 标记仍在(§4.11:读 applied 持久字段,不重算)。
    std::vector<CompactMarkerView> compact_markers;
};

// 祖先账路径解析:默认 sessions/<id>/<id>.jsonl(以 jsonl.parent.parent
// 为 sessions 根);测试可注入自定义解析。
using SourceLedgerResolver = std::function<std::filesystem::path(const std::string& session_id)>;

// resume 三恢复(§4.59 表):历史索引/模型输入/执行状态各归各;
// 沿 resume.source.attached 回溯来源链,逐级验 hash、去重、检环;
// 祖先缺失只报缺口——本账链自足,精确上下文恢复不受影响。
//
// 链护栏两道(§4.10"检查环和引用错误"):sessionId 已见过标 duplicate 不
// 再下钻(环);max_source_depth 封顶回溯级数,超深标
// check.reason="chain_depth_exceeded" 停走(超长链不无限读账)。
std::expected<ResumeProjection, std::string> ProjectResume(
    const std::filesystem::path& jsonl, SourceLedgerResolver resolver = nullptr,
    int max_source_depth = 64);

}  // namespace lubancode::trajectory::v3
