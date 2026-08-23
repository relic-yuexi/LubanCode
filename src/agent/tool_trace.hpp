// 工具调用逐枚追踪(逐枚追踪单):canonical 领域事件、持久恢复栅栏、
// 崩溃恢复账本。
//
// 单子的定案在这里落地:
//   - 回溯/归因/恢复/重试/回滚/补偿/重放不是一回事;本头只管把"一枚
//     工具调用走过哪些阶段、在哪儿停"记成 append-only 事件,判结论的
//     活儿交给账本折叠。
//   - 身份三枚:execution_id(宿主发号,审计主键)/ tool_use_id(Provider
//     wire 配对)/ item_id(Runtime UI 条目)。execution_id 直接复用
//     Runtime item id 的发号局(IdAuthority),不自造第二只计数器。
//   - 持久账只落四类关键栅栏:scheduled / execution_started /
//     execution_finished / result_committed。运行时细相位(ToolPhase)
//     走 Runtime 事件,不 fsync 每一拍。
//   - 恢复只落四档:not_started / finished / result_recoverable /
//     unknown_after_start。能重放的才重放,能补偿的才补偿,unknown 绝不
//     自动重跑。
//
// 分层:本头是纯数据 + 纯函数(序列化/折叠/分类/修补),不碰磁盘、不碰
// 终端;真正落盘的薄壳在 SessionStore::AppendToolTraceEvent,装配在
// runtime/ 的 trace hub。依赖只认标准库 + nlohmann + hooks/hash(SHA-256
// 锚),零 cli/app/runtime 依赖。

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::agent {

// ---------------------------------------------------------------------------
// 稳定枚举(线上是字符串,不是数字——数字重排就是账本破坏)
// ---------------------------------------------------------------------------

// 工具来源。注册时明写,不靠 RTTI 猜(单子"工具来源不能靠 RTTI 猜")。
enum class ToolSourceKind { Builtin, Mcp, Lsp, PluginLua, PluginNative, Agent, Ptc, Deferred };
std::string ToString(ToolSourceKind kind);
bool ParseToolSourceKind(const std::string& s, ToolSourceKind& out);

// 副作用等级(单子"Effect class 与恢复策略"表)。未声明按最危险档
// (InProcessUnknown),声明不能放宽安全:manifest/metadata 声称 read-only
// 只影响恢复建议,不越过权限确认。
enum class EffectClass {
    ReadOnlyLocal,       // read_file/search:可建议重试
    ReadOnlyRemote,      // web_fetch/只读 MCP:不自动重试(费用/限流)
    LocalReversible,     // write_file/edit_file:查 undo token 再询问
    LocalProcessUnknown, // run_command:unknown,先核验
    RemoteIdempotent,    // 带 idempotency key:按 key 查,不直接重发
    RemoteCompensatable, // 支持 delete/cancel:可提补偿(另一枚可见调用)
    RemoteIrreversible,  // 发信/付款/发布:只告警与人工核验
    InProcessUnknown,    // 未声明的 native/Lua:按未知副作用处理
};
std::string ToString(EffectClass cls);
bool ParseEffectClass(const std::string& s, EffectClass& out);

// 稳定终态(is_error 留作 Provider/UI 的粗投影,诊断用这张表)。
enum class ToolOutcome {
    Succeeded,
    ToolError,
    UnknownTool,
    Unavailable,          // 注册表里有、运行时没就绪(延迟未挂载/传输未起)
    SchemaRejected,
    HookDenied,
    PermissionDeclined,
    ModeDenied,           // Plan 模式硬闸拒绝(只读研究单):不冒充用户拒绝
    CancelledBeforeStart, // 未轮到(ESC)或闸前被收掉:没越过执行边界
    CancelledDuringRun,
    SpawnFailed,
    TimedOut,
    OutputLimit,
    TransportError,
    ProtocolError,
    ProcessExitNonzero,
    PluginException,
    HostError,
    UnknownAfterStart,    // 崩溃恢复合成:见过 started 没见 finished
    ResultStoreFailed,    // 工具跑完了,结果没落住
};
std::string ToString(ToolOutcome outcome);
bool ParseToolOutcome(const std::string& s, ToolOutcome& out);
// 这枚 outcome 是否"压根没越过执行边界"(恢复语义:不是副作用未知,
// 是确定没执行)。blocked/declined/cancelled_before_start 三种。
bool OutcomeNeverStarted(ToolOutcome outcome);

// 分层错误码(不用中文正文作机器判断)。人话放 fallback_message,结构
// 详情放 details;测试只钉 code。这里只列宿主自产的码,工具/插件自报的
// 细码(plugin_error_code、jsonrpc error code)进 details。
inline constexpr const char* kErrRegistryUnknownTool = "registry.unknown_tool";
inline constexpr const char* kErrRegistryNotMounted = "registry.not_mounted";
inline constexpr const char* kErrHookPreDenied = "hook.pre.denied";
inline constexpr const char* kErrHookUpdatedInputInvalid = "hook.updated_input.invalid";
inline constexpr const char* kErrPermissionDeclined = "permission.declined";
inline constexpr const char* kErrProcessSpawnFailed = "process.spawn_failed";
inline constexpr const char* kErrProcessTimeout = "process.timeout";
inline constexpr const char* kErrProcessExitNonzero = "process.exit_nonzero";
inline constexpr const char* kErrMcpTransportClosed = "mcp.transport_closed";
inline constexpr const char* kErrMcpTimeout = "mcp.timeout";
inline constexpr const char* kErrMcpJsonrpcError = "mcp.jsonrpc_error";
inline constexpr const char* kErrMcpResponseSchemaInvalid = "mcp.response_schema_invalid";
inline constexpr const char* kErrPluginLuaError = "plugin.lua_error";
inline constexpr const char* kErrPluginNativeBoundary = "plugin.native_exception_boundary";
inline constexpr const char* kErrSessionTraceAppendFailed = "session.trace_append_failed";
inline constexpr const char* kErrSessionResultCommitFailed = "session.result_commit_failed";

// ---------------------------------------------------------------------------
// canonical 领域事件(一份事件,两路消费:Runtime EventSink 吃投影,
// SessionTraceSink 吃持久栅栏)
// ---------------------------------------------------------------------------

enum class ToolTraceEventKind {
    Scheduled,          // 批次排程:assistant tool-use 消息落盘之后写
    ExecutionStarted,   // Tool::execute 前,副作用边界之前(写不成拦副作用工具)
    ExecutionFinished,  // 拿到原始结果并完成 UTF-8 规范化后(PostToolUse 之前)
    ResultCommitted,    // 对应 ToolResultBlock 确实落 session 之后
    Verification,       // 显式验证点(postcondition):可疑窗口的两侧证据
    RecoveryMarker,     // 恢复侧补的注记(不改旧行,append-only)
    McpLateResponse,    // MCP 超时后迟到的响应:丢弃但留账,关联原请求
};
std::string ToString(ToolTraceEventKind kind);
bool ParseToolTraceEventKind(const std::string& s, ToolTraceEventKind& out);

// execution_finished 的结果引用:小结果内联(过单行与字节上限),大结果
// 指 artifact(blob 缺失/哈希不合 → result 不可恢复,但工具可能已完成,
// 绝不是 not_started)。
struct ToolResultRef {
    enum class Kind { Inline, Artifact, Unavailable };
    Kind kind = Kind::Unavailable;
    std::string sha256;      // 结果规范化后的正文摘要
    std::uint64_t bytes = 0; // 字节数
    std::string content;     // Inline:恢复用的正文(有上限,见 kInlineResultCap)
    std::string artifact_id; // Artifact:仓里的稳定 id
    std::string preview;     // 头尾短预览(脱敏;诊断用,不承担恢复)
};
std::string ToString(ToolResultRef::Kind kind);

// 本地文件条件式撤销的 token(write_file/edit_file 产):撤销前重读目标,
// 当前 sha 等于 postimage 才可恢复 preimage;新建文件只在内容仍等于
// postimage 时可移走。preimage 超上限不内联(undo 不可用,如实标注)。
struct ToolUndoToken {
    std::string path;
    std::string preimage_sha256;
    std::string postimage_sha256;
    std::string preimage;    // 条件式恢复用的原文(上限内才带)
    bool created_new_file = false;
    bool available() const { return !path.empty() && !postimage_sha256.empty() && !preimage.empty(); }
};

// 内联结果/撤销 preimage 的字节上限。结果原文本就逐字进 session 的
// tool_result 块,这里同量级;超限走 artifact 或标不可恢复,不翻倍敏感面。
inline constexpr std::uint64_t kInlineResultCap = 64 * 1024;
inline constexpr std::uint64_t kUndoPreimageCap = 256 * 1024;

struct ToolTraceEvent {
    ToolTraceEventKind kind = ToolTraceEventKind::Scheduled;

    // ---- 身份(单子"每枚工具事件至少带"的字段集;按 kind 取用) ----
    std::string thread_id;             // 哪场会话(session 文件本身就是边界,冗余带齐好对账)
    std::string turn_id;
    std::string provider_request_id;   // 若有(MessageStart 的 request id)
    std::string batch_id;              // 同一 assistant message 的五枚共用
    int sequence_in_batch = -1;        // 0..N-1
    std::string execution_id;          // 审计主键(Runtime item id 同源)
    std::string item_id;               // 与 execution_id 同源,单独带好对账
    std::string tool_use_id;           // Provider wire 配对
    std::string tool_name;
    ToolSourceKind source_kind = ToolSourceKind::Builtin;
    std::string source_instance;       // MCP server / plugin id / lsp server;可空
    std::string parent_execution_id;   // 子代理/PTC 内层归属;可空
    std::string retry_of;              // 显式重试关系;可空
    std::string blocked_by;            // 宿主因前置失败明确跳过;可空
    std::string compensates;           // 补偿哪枚调用;可空

    // ---- ExecutionStarted 载荷 ----
    std::string effective_input_sha256; // 钩子改写后实际执行的入参摘要
    EffectClass effect_class = EffectClass::InProcessUnknown;

    // ---- ExecutionFinished 载荷 ----
    ToolOutcome outcome = ToolOutcome::Succeeded;
    std::string error_code;
    std::string fallback_message;       // 人话兜底(UI 可翻,测试只钉 code)
    nlohmann::json details = nlohmann::json::object();
    std::int64_t duration_ms = 0;
    ToolResultRef result_ref;
    ToolUndoToken undo;

    // ---- Verification 载荷 ----
    std::string label;
    bool passed = false;
    std::string after_execution_id;     // 验证点跟在哪枚执行之后
    std::string verify_detail;

    // ---- McpLateResponse 载荷 ----
    std::int64_t jsonrpc_request_id = -1;

    // ---- RecoveryMarker 载荷 ----
    std::string note;

    // ---- 记账 ----
    std::uint64_t seq = 0;              // Runtime 发号,thread 内单调
    std::int64_t timestamp_ms = 0;      // Unix epoch 毫秒
};

// 事件 -> 一行 JSON(不带换行符)。ts 是落盘时刻("yyyy-mm-dd HH:MM:SS"),
// 与其余事件行同款。
std::string SerializeToolTraceEvent(const ToolTraceEvent& event, const std::string& ts);

// 一行 JSON -> 事件。不是合法 JSON、type 不是 "tool_trace_v1"、event 认
// 不得、缺 execution_id,给 nullopt——坏行调用方跳过,不废整场(事件行
// 的通用约定,老版本读到也当坏行跳过,消息账无损)。
std::optional<ToolTraceEvent> ParseToolTraceEvent(const std::string& line);

// 诊断预览:正文过 RedactSecrets(workflow_recorder 的打码器)再截头尾,
// 供 /trace 与 export 用;不承担恢复,只给人看。
std::string BuildTracePreview(const std::string& content, std::size_t head, std::size_t tail);

// ---------------------------------------------------------------------------
// 恢复账本:对每个 scheduled execution 折叠事件(纯)
// ---------------------------------------------------------------------------

// 四种恢复结论(单子"四种恢复结论",只准落这四档;corrupt 是账坏了,
// 不是第五种结论,单独标注)。
enum class RecoveryClass {
    NotStarted,         // 尚未越过执行边界:可确定没调 Tool::execute
    Finished,           // 已有稳定 outcome 与可校验结果(含闸前终态的"未执行")
    ResultRecoverable,  // 执行已结束,结果在 trace/artifact,尚未写回消息
    UnknownAfterStart,  // 见过 durable started,没见 durable finished:副作用未知
};

std::string ToString(RecoveryClass cls);

// 一枚 execution 的折叠账。
struct ToolExecutionRecord {
    std::string execution_id;
    std::string tool_use_id;
    std::string tool_name;
    std::string batch_id;
    std::string turn_id;
    std::string parent_execution_id;
    std::string retry_of;
    std::string blocked_by;
    std::string compensates;
    int sequence_in_batch = -1;
    ToolSourceKind source_kind = ToolSourceKind::Builtin;
    std::string source_instance;
    EffectClass effect_class = EffectClass::InProcessUnknown;
    std::string effective_input_sha256;

    bool has_scheduled = false;
    bool has_started = false;
    bool has_finished = false;
    bool has_committed = false;

    // 终态与结果(取第一枚合法栅栏;冲突标 corrupt,不取"最后一条赢")。
    ToolOutcome outcome = ToolOutcome::Succeeded;
    std::string error_code;
    std::string fallback_message;
    nlohmann::json details = nlohmann::json::object();
    std::int64_t duration_ms = 0;
    ToolResultRef result_ref;
    ToolUndoToken undo;
    std::uint64_t seq_scheduled = 0;

    // 账坏证据:冲突行号与因由,保留不抹。
    bool corrupt = false;
    std::string corrupt_reason;
    std::vector<std::uint64_t> conflict_seqs;

    // 四档结论(纯折叠;不猜副作用,unknown 不冒充失败也不冒充成功)。
    RecoveryClass Classify() const;
};

// 验证点记录(可疑窗口的两侧证据)。
struct TraceVerification {
    std::string label;
    bool passed = false;
    std::string after_execution_id;
    std::string detail;
    std::uint64_t seq = 0;
};

class ToolExecutionLedger {
public:
    // 折一枚事件。规矩:
    //   - 同 execution 的相同 event 幂等(重复行取第一枚合法栅栏);
    //   - 同 execution 两枚冲突 finished(outcome/结果摘要不同)→ corrupt,
    //     保留两枚 seq,不挑一枚冒充真相;
    //   - started/finished/committed 认得没见过 scheduled 的 execution
    //     (PTC 内层调用自起一摊),按首见建账。
    void Fold(const ToolTraceEvent& event);

    // 按 execution_id 查(nullptr = 没这本账)。
    const ToolExecutionRecord* FindByExecution(const std::string& execution_id) const;
    // 按 tool_use_id 查(Provider 重号时全列出来,不串账)。
    std::vector<const ToolExecutionRecord*> FindByToolUse(const std::string& tool_use_id) const;
    // 同一批(batch_id)按 sequence_in_batch 排好的列表。
    std::vector<const ToolExecutionRecord*> Batch(const std::string& batch_id) const;

    const std::vector<ToolExecutionRecord>& executions() const { return executions_; }
    const std::vector<TraceVerification>& verifications() const { return verifications_; }
    std::size_t corrupt_count() const;

    // ---- 归因(保守;不冒充因果证明) ----
    // 最早明确失败:按 batch 序扫,第一枚 outcome 不是 Succeeded 的执行
    // (cancelled_before_start 不算"明确失败",它是被收掉的尾巴)。
    // 没有给空。
    const ToolExecutionRecord* FirstExplicitFailure(const std::string& batch_id) const;
    // 可疑窗口:最后一个通过的验证点之后、第一个失败验证点之前的那段
    // execution 序列。没有验证点证据给空表——不编确定答案。
    struct SuspectWindow {
        bool valid = false;
        std::string last_verified_good;    // 通过验证点时最近的 execution id
        std::string first_observed_bad;    // 失败验证点紧跟的 execution id(可空)
        std::vector<const ToolExecutionRecord*> window;
    };
    SuspectWindow ComputeSuspectWindow() const;

private:
    ToolExecutionRecord& Ensure(const ToolTraceEvent& event);

    std::vector<ToolExecutionRecord> executions_;
    std::map<std::string, std::size_t> by_execution_;
    std::map<std::string, std::vector<std::size_t>> by_tool_use_;
    std::vector<TraceVerification> verifications_;
};

// ---------------------------------------------------------------------------
// trace-aware 修补(RepairToolPairs 的升级;旧 session 无 trace 走老逻辑)
// ---------------------------------------------------------------------------

// 恢复侧给一枚 execution 合成的 tool_result 正文:标 [会话恢复],保留
// 稳定 outcome;inline 结果可得时带原文,不可得如实说不可恢复。
std::string BuildRecoveredResultText(const ToolExecutionRecord& record);

// 五工具批次摘要行(/trace 用):"#2 item-17 edit_file schema_rejected 1ms"。
struct TraceBatchLine {
    std::string text;
};
std::string FormatExecutionSummaryLine(const ToolExecutionRecord& record, bool first_failure);

}  // namespace lubancode::agent
