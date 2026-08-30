// SessionManager 与 session 生命周期(P0 新轨迹记录单 §3.2/§3.3/§15.3/§7.5)。
//
// 进程级 SessionManager 管 active SessionRuntime 与 `clear` 换账事务:
// 先备新目录,再封旧账,末后原子切 active 指针(§15.3)。八步换账照
// §3.3.1 逐字落死;状态迁移照 §3.3.2 全图,由 SessionManager 独占。
// SessionRuntime 不得原地清零后复用旧 Recorder——clear 后整只换新。
//
// 边界:trajectory 纯库,不 include app/cli/runtime。真正"停活/收口/清
// 内存"归运行时侧,经 ClearParticipant 回调进来(P0-2 接线,合成测试用
// fake);轨迹侧只掌账、只落事实。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/directory.hpp"
#include "trajectory/recorder.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/session_lock.hpp"

namespace lubancode::trajectory {

// ---------------------------------------------------------------------------
// 生命周期状态机(§3.3.2 全图)
// ---------------------------------------------------------------------------

// tombstone 不是 session 内状态,是 workspace 管理态(已删除目录),不进
// 这张枚举表(§3.3.2 明文)。
enum class SessionStatus {
    Preparing,
    Running,
    Closing,
    Closed,
    Incomplete,
    Corrupt,
    Archived,
};

const char* SessionStatusName(SessionStatus status);
std::optional<SessionStatus> SessionStatusFromName(std::string_view name);

// 迁移图逐边:
//   preparing -> running
//   running -> closing | incomplete
//   closing -> closed | incomplete
//   closed <-> archived
//   任意未 corrupt 态 -> corrupt(校验失败,只读隔离)
// 其余一律非法(preparing 不得直闭、closed 不得复活)。
bool CanTransitionSessionStatus(SessionStatus from, SessionStatus to);

// session.json 的 status 原子更新。from 取 manifest 现值,先验图再写;
// recovery_collapse=true 仅供恢复器:session.json 落后于 Journal 可证事实
// 时,允许把被跳过的中间态(closing)折叠掉(如 running->closed),其余
// 非法边照拒(§3.3.2"以 Journal 可证事实为准")。
std::expected<void, std::string> TransitionSessionStatus(const std::filesystem::path& session_dir,
                                                         SessionManifest* manifest,
                                                         SessionStatus to,
                                                         bool recovery_collapse = false);

// ---------------------------------------------------------------------------
// workspace lifecycle 账(§3.2)
// ---------------------------------------------------------------------------

// 只记四类管理操作(§3.2):session 创建、归档、恢复引用、删除。一次
// 管理操作一只目录(lifecycle/<operation_id>/intent.json + result.json),
// 无共享 append writer。clear 换账本体记在两场 session 的 Journal 里,
// 不占这张账;新 session 的创建是 create_session 操作。
enum class LifecycleOperation {
    CreateSession,
    ArchiveSession,
    ResumeReference,
    DeleteSession,
};
const char* LifecycleOperationName(LifecycleOperation operation);
std::optional<LifecycleOperation> LifecycleOperationFromName(std::string_view name);

struct LifecycleIntent {
    int schema_version = 1;
    std::string operation_id;
    std::string operation;  // create_session | archive_session | resume_reference | delete_session
    std::string workspace_key;
    std::string session_id;  // 目标 session(create 的产物、其余的操作对象)
    std::int64_t requested_at_ms = 0;
    nlohmann::json parameters = nlohmann::json::object();

    nlohmann::json ToJson() const;
    static std::optional<LifecycleIntent> FromJson(const nlohmann::json& json);
};

struct LifecycleResult {
    int schema_version = 1;
    std::string operation_id;
    std::string status;  // completed | failed
    std::int64_t completed_at_ms = 0;
    nlohmann::json outcome = nlohmann::json::object();

    nlohmann::json ToJson() const;
    static std::optional<LifecycleResult> FromJson(const nlohmann::json& json);
};

class WorkspaceLifecycle {
public:
    WorkspaceLifecycle() = default;
    explicit WorkspaceLifecycle(std::filesystem::path workspace_dir)
        : workspace_dir_(std::move(workspace_dir)) {}

    // create-new 占位并写 intent.json:operation_id 不得复用,目录已存在即
    // 失败(稳定码 lifecycle.intent_exists)。
    std::expected<std::filesystem::path, std::string> WriteIntent(
        const LifecycleIntent& intent) const;
    // 原子写 result.json:已存在即失败——历史结果不许改写
    // (lifecycle.result_exists)。
    std::expected<void, std::string> WriteResult(const LifecycleResult& result) const;

    static std::optional<LifecycleIntent> ReadIntent(const std::filesystem::path& operation_dir);
    static std::optional<LifecycleResult> ReadResult(const std::filesystem::path& operation_dir);

    const std::filesystem::path& dir() const { return workspace_dir_; }

private:
    std::filesystem::path workspace_dir_;
};

// 删除 session 的 tombstone(§3.2):id、末 hash、删除时间与原因;不留
// 正文,不作重放材料。
struct SessionTombstone {
    int schema_version = 1;
    std::string session_id;
    std::int64_t deleted_at_ms = 0;
    std::string reason;  // user_delete | aborted_before_start
    std::optional<std::string> last_event_hash;  // 空 preparing 无事件 → nullopt
    std::string operation_id;

    nlohmann::json ToJson() const;
    static std::optional<SessionTombstone> FromJson(const nlohmann::json& json);
};

// 写 tombstone(tombstones/<session_id>.json,create-new:一枚 session 只
// 删一次,lifecycle.tombstone_exists)。
std::expected<void, std::string> WriteSessionTombstone(const std::filesystem::path& tombstones_dir,
                                                       const SessionTombstone& tombstone);
std::optional<SessionTombstone> ReadSessionTombstone(const std::filesystem::path& tombstones_dir,
                                                     const std::string& session_id);

// ---------------------------------------------------------------------------
// clear 的运行时回调合同(§3.3.1 第 3/8 步)
// ---------------------------------------------------------------------------

// 轨迹侧 SessionManager 掌账;真正停活、按取消合同收口 child、清内存归
// 运行时侧。回调返回的是"事实申报",SessionManager 落账时仍以 Journal
// 状态机校验兜底。
struct ClearParticipant {
    struct ChildClosure {
        std::string run_id;               // subagent/workflow/node run id
        bool terminal_written = false;    // 已在旧目录落 terminal
        bool unknown = false;             // 副作用不明——不许装成功
    };

    virtual ~ClearParticipant() = default;
    // 活动 main turn 的 turn_id;空串 = 没有。clear/exit 都先收它,否则
    // run terminal 写不进(状态机硬约束)。
    virtual std::string CancelActiveTurn() = 0;
    // 已在跑 child 的收口申报(各自旧目录写 terminal)。
    virtual std::vector<ChildClosure> CancelActiveChildren() = 0;
    // 未送达 queue item 的 item_id 清单(逐枚落 cancelled(reason=clear))。
    virtual std::vector<std::string> CancelQueuedItems() = 0;
    // 活动 /record 选段的 record_id;空串 = 没有(先封 interrupted,不跨
    // session 暗续)。
    virtual std::string ActiveRecordSelectionId() = 0;
    // 第 8 步:清 main history、turn state、临时队列与本 session 缓存。
    virtual void ResetInMemoryState() = 0;
};

// 空实现(纯合成路径/无活动执行时用)。
struct NullClearParticipant : ClearParticipant {
    std::string CancelActiveTurn() override { return {}; }
    std::vector<ChildClosure> CancelActiveChildren() override { return {}; }
    std::vector<std::string> CancelQueuedItems() override { return {}; }
    std::string ActiveRecordSelectionId() override { return {}; }
    void ResetInMemoryState() override {}
};

// ---------------------------------------------------------------------------
// 时间/随机/锁身份注入(单测喂固定钟与假进程身份)
// ---------------------------------------------------------------------------

struct SessionManagerClock : RecorderClock {
    // session_id/operation_id 尾部 6 位(大写字母数字)。
    virtual std::string Random6() const;
    // 锁持有者身份;默认当前真进程。测试注入死 PID 身份以模拟"上一只
    // 进程崩了留下的陈旧锁"。
    virtual SessionLockOwner LockOwner() const;
};

// ---------------------------------------------------------------------------
// 句柄与结果
// ---------------------------------------------------------------------------

// 跨 session 的事件引用(qualified ref / caused_by ref;§6.3 只靠稳定 id
// + event hash,不存可搬绝对路径)。
struct EventRef {
    std::string session_id;
    std::string event_id;
    std::string event_hash;

    nlohmann::json ToJson() const;
    static std::optional<EventRef> FromJson(const nlohmann::json& json);
};

// 一场活动 session 的轨迹侧句柄:目录 + main recorder + manifest + 独占
// 锁。SessionRuntime(P0-2)从这里领 scoped sink;clear 换账整只换新,
// 不原地清零复用旧 Recorder(§15.3)。
struct ActiveSession {
    TrajectoryDirectory directory;
    std::optional<TrajectoryRecorder> main;  // 关柄后仍在,只是拒写
    SessionManifest manifest;
    SessionLock lock;
    SessionStatus status = SessionStatus::Preparing;

    std::filesystem::path session_dir() const { return directory.session_dir(); }
    const std::string& session_id() const { return manifest.session_id; }
};

struct ClearRequest {
    std::string command_id = "cmd-clear-0001";  // command lifecycle id
    std::string reason = "user_clear";
    // 真人敲 /clear → requested 的 actor=user/origin=external_user(§5.5);
    // 宿主自发清账走 false → host/scheduled_host。
    bool user_initiated = true;
};

struct ClearOutcome {
    // 空 = 成功。稳定码:
    //   clear.busy               同一时刻只许一场 clear(§3.3.1)
    //   clear.no_active_session  没有可换的 active running session
    //   clear.step<N>_failed     第 N 步落盘失败(旧账不撤,恢复器续办)
    std::string error_code;
    std::string message;

    std::string boundary_operation_id;
    std::string old_session_id;
    std::string new_session_id;
    std::string old_main_run_id;
    std::string new_main_run_id;

    // ---- 八步各自的落盘证据(事件 id/引用;失败止于断点前) ----
    // 第 1 步:新目录 session.json(status=preparing)已原子落。
    bool new_session_prepared = false;
    // 第 2 步:旧 main 两枚(qualified requested 在前,clear_requested 在后)。
    std::string requested_event_id;         // control.command.requested(旧 main)
    std::string clear_requested_event_id;   // session.clear_requested(旧 main)
    // 第 3 步:活动收口证据。
    std::string turn_cancelled_event_id;              // 可空
    std::vector<std::string> queue_cancelled_event_ids;
    std::string selection_interrupted_event_id;       // 可空
    // 第 4 步:旧 run terminal + session.ended 封链。
    std::string old_run_terminal_event_id;
    std::string old_run_terminal_kind;  // run.completed | run.failed
    std::string old_session_ended_event_id;
    std::string old_close_quality;      // clean | incomplete
    EventRef old_session_ended_ref;     // 新 run.started 反指的旧终态事件
    // 第 5 步:旧 session.json 已转 closed/incomplete。
    bool old_session_json_finalized = false;
    // 第 6 步:新 main 首两条。
    std::string new_run_started_event_id;
    std::string new_command_completed_event_id;
    // 第 7 步:新 session.json 转 running,active 指针已切。
    bool new_session_running = false;
    bool active_switched = false;
    // 关柄后旧账整本 hash(§8.3)。
    std::string old_journal_sha256;
};

struct CloseRequest {
    std::string reason = "exit";  // exit | eof | shutdown | switch_to_resume
};

struct CloseOutcome {
    // 空 = 成功。close.busy / close.no_active_session / close.step<N>_failed
    std::string error_code;
    std::string message;
    std::string session_id;
    std::string run_terminal_kind;
    std::string close_quality;  // clean | incomplete
    std::string journal_sha256;
};

// ---------------------------------------------------------------------------
// resume-as-new(§10.4 七步)
// ---------------------------------------------------------------------------

// 交互 /resume 的跨 session command 生命周期素材(§14.1:旧 main 写
// requested 与 session terminal,新 main 在 run.started 之后写 command
// terminal,qualified ref 指回旧 requested,两边同带 boundary_operation_id)。
struct ResumeBoundaryCommand {
    std::string command_id;
    std::string requested_session_id;  // requested 所在的(刚封口的)session
    std::string requested_event_id;
    std::string boundary_operation_id;
};

struct ResumeRequest {
    std::string source_session_id;  // 空 = 本 workspace 最近一场可恢复的
    // 交互 /resume:第 6 步在新 main 补跨 session control.command.completed。
    // --continue 启动路不写(没有旧 requested 可指)。
    bool interactive = false;
    bool user_initiated = true;
    // 直接前驱 session(交互 /resume 封口的那场);--continue 无前驱,
    // previous_session_id 落 source。空串 = 让 manager 自取。
    std::string previous_session_id;
    ResumeBoundaryCommand boundary_command;  // interactive 时必填
};

struct ResumeOutcome {
    // 空 = 成功。稳定码:
    //   resume.busy                换账掌管中
    //   resume.source_not_found    指认的 source 不在(或没有可恢复场)
    //   resume.source_locked       source 仍被别的进程持写锁(§10.4 末段)
    //   resume.source_corrupt      hash chain/schema/父子边验不过(非截断)
    //   resume.source_unsupported  折叠 fail-closed(未知关键事件/超前版本)
    //   resume.step<N>_failed      第 N 步落盘失败
    std::string error_code;
    std::string message;

    std::string source_session_id;
    std::string new_session_id;
    std::string new_main_run_id;
    std::string source_main_last_event_hash;
    std::uint64_t source_event_count = 0;
    bool source_truncated_tail = false;  // incomplete 前缀恢复(§3.3.2)

    // ---- 七步各自的落盘证据 ----
    // 第 1 步:source 验账过(链+父子边)。
    bool source_verified = false;
    // 第 2 步:checkpoint 高水位;false = 从头折叠。
    bool from_checkpoint = false;
    std::uint64_t checkpoint_seq = 0;
    std::string checkpoint_event_hash;
    // 第 3 步:折叠出的有效对话与控制态 + imported state hash。
    std::string replay_version;  // std::to_string(kReplayProjectionVersion)
    std::string imported_state_hash;
    std::vector<ReplayMessage> effective_conversation;  // 引用,不复制 child 正文
    ReplayControlState control;
    // 第 4 步:悬空工具三道账;unknown 副作用不重跑。
    std::vector<ReplayDanglingTool> dangling_tools;
    // 第 5 步:新 session 开张(run.started(start_reason=resume))。
    std::string new_run_started_event_id;
    // 第 6 步:resume.source.attached(+ 交互路跨 session command.completed)。
    std::string resume_attached_event_id;
    std::string command_completed_event_id;
    // 第 7 步:session.json running,active 指针已切;新 turn/request/call/
    // seq 全从新命名空间起号(recorder 新开,天然新号)。
    bool new_session_running = false;
    bool active_switched = false;
};

// ---------------------------------------------------------------------------
// 恢复器(§3.3.1 末段/§3.3.2)
// ---------------------------------------------------------------------------

// 一份 main.jsonl 的可证事实清单(恢复器与硬门共用;单遍扫描,不重放)。
struct MainJournalFacts {
    bool journal_exists = false;
    bool verify_ok = false;
    bool truncated_tail = false;
    std::string verify_error_code;

    bool has_run_started = false;
    std::string start_reason;
    std::optional<std::string> previous_session_id;
    std::string first_event_id;
    std::string first_event_hash;

    bool clear_requested = false;
    std::string clear_requested_next_session_id;
    // 换账操作 id(clear_requested/command.requested 的 correlation_id)。
    std::string boundary_operation_id;

    bool command_requested = false;
    std::string command_requested_event_id;
    std::string command_id;
    bool command_completed = false;

    bool run_terminal = false;
    std::string run_terminal_kind;
    bool session_ended = false;
    std::string close_quality;
    std::optional<std::string> next_session_id;
    std::string session_ended_event_id;
    std::string session_ended_event_hash;

    std::uint64_t event_count = 0;
    std::string last_event_id;
    std::string last_event_hash;

    // 悬空账(turn/queue item/record selection 开了没收口)。
    std::vector<std::string> dangling_turn_ids;
    std::vector<std::string> dangling_queue_item_ids;
    std::vector<std::string> dangling_selection_ids;
};

// 扫一份 stream(main/subagent/workflow 通用:只认状态机骨架事实)。
MainJournalFacts ScanStreamFacts(const std::filesystem::path& stream_path);

// 以 Journal 可证事实推导 session 状态(§3.3.2"session.json 更新失败时,
// 以 Journal 可证事实为准")。
SessionStatus DeriveSessionStatusFromFacts(const MainJournalFacts& facts);

struct SessionRecoveryEntry {
    std::string session_id;
    SessionStatus status = SessionStatus::Preparing;
    bool session_json_corrected = false;   // session.json 与事实不符,已重建
    bool clear_continued = false;          // 续办了换账(封旧账/开新账)
    bool aborted_before_start = false;     // 空 preparing 清账 + tombstone
    bool owned_by_live_process = false;    // 活锁在外进程,只读不动
    std::vector<std::string> notes;        // 证据摘要(事件 id 等)
};

struct WorkspaceRecoveryReport {
    std::vector<SessionRecoveryEntry> sessions;
    // 恢复后本进程接续的 active session(空 = 没有,调用方另开新场)。
    std::string adopted_session_id;
};

// 换账崩溃后续办策略:默认把空 preparing 的新账开张(续办到底);
// AbortEmptyPreparing 把空 preparing 清账(tombstone 记 aborted_before_start),
// 旧账按事实收口,不替用户开新场。
enum class ClearRecoveryPolicy { CompleteSwitch, AbortEmptyPreparing };

// ---------------------------------------------------------------------------
// SessionManager
// ---------------------------------------------------------------------------

struct SessionManagerOptions {
    std::filesystem::path trajectories_root;  // ~/.lubancode/trajectories
    std::filesystem::path workspace_root;     // 仓库根或启动 cwd(§3.2)
    std::string readable_workspace_name;
    std::string launch_cwd;  // UTF-8 文本,进 manifest
    std::string lubancode_version;
    RecorderOptions recorder;
};

class SessionManager {
public:
    SessionManager(SessionManagerOptions options, SessionManagerClock* clock = nullptr);
    ~SessionManager();  // 析构不封账:正常退出走 Close,崩溃即崩(锁留陈旧)

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // 进程启动开一场(§3.1:一次启动一间):
    //   workspace 建/认 → lifecycle create intent → session 目录(preparing)
    //   → 独占锁 → main recorder → run.started(start_reason=process_launch)
    //   → session.json(running) → lifecycle result。
    std::expected<ActiveSession*, std::string> LaunchSession();

    // clear 八步换账(§3.3.1 逐字)。串行掌管;重复请求回 clear.busy。
    ClearOutcome Clear(const ClearRequest& request, ClearParticipant* participant);

    // 正常封口(/exit 与 EOF,§14.5):cancel + bounded join 收齐活动流,
    // run terminal、session.ended、session.json closed;收不回的执行记
    // unknown,标 incomplete,不写 clean closed(§3.3.2 closed 硬门)。
    // reason=switch_to_resume 是交互 /resume 封当前场用——封口后由
    // ResumeAsNew 开新场,本柄不再接活。
    CloseOutcome Close(const CloseRequest& request, ClearParticipant* participant);

    // resume-as-new(§10.4 七步):只读 source(验账→checkpoint/从头折叠
    // →悬空分档),再开一间新 session(start_reason=resume,resumed_from=
    // source)。source Journal 永不 reopen append;已完成的 child 只核
    // terminal hash,不把正文灌进新 main。active session 存在时先由调用方
    // Close(switch_to_resume)——本口不管封旧场。
    ResumeOutcome ResumeAsNew(const ResumeRequest& request);

    // 本 workspace 最近一场可恢复的 session(closed/archived/incomplete,
    // 按创建时间取新);跳过本进程 active 的那场。空串 = 没有。
    std::string LatestResumableSessionId();

    // 启动恢复器:扫 workspace 全部 session,以 Journal 可证事实为准重建
    // session.json;clear 崩在半路的按 next_session_id 与两边终态续办
    // (不合并两本 JSONL,不复用旧 session_id);可接续的新账收作 active。
    WorkspaceRecoveryReport RecoverWorkspace(ClearRecoveryPolicy policy =
                                                 ClearRecoveryPolicy::CompleteSwitch);

    // ---- workspace 管理操作(§3.2;只动非 active 的 session) ----
    // closed -> archived;lifecycle 记 intent/result。正文与 hash 不变。
    std::expected<void, std::string> ArchiveSession(const std::string& session_id);
    // 真删目录:durable intent → tombstone → 删目录 → result。活锁或
    // active session 拒绝。
    std::expected<void, std::string> DeleteSession(const std::string& session_id,
                                                   const std::string& reason);
    // 记一次恢复引用(resume 的 source 指认;不另开 session)。
    std::expected<void, std::string> RecordResumeReference(const std::string& source_session_id,
                                                           const std::string& note);

    // ---- 观测 ----
    ActiveSession* active() { return active_.has_value() ? &*active_ : nullptr; }
    const ActiveSession* active() const { return active_.has_value() ? &*active_ : nullptr; }
    const std::string& workspace_key() const { return workspace_key_; }
    const std::filesystem::path& workspace_dir() const { return workspace_dir_; }
    std::filesystem::path SessionDirOf(const std::string& session_id) const;
    WorkspaceLifecycle lifecycle() const { return WorkspaceLifecycle(workspace_dir_); }

    // session 内未收口的 stream 数(closed 硬门,§3.3.2):main +
    // subagents + workflow/node/goal/loop 各 JSONL 里没有 run terminal 的。
    static std::vector<std::filesystem::path> UnterminatedStreamsInSession(
        const std::filesystem::path& session_dir);

private:
    bool EnsureWorkspace(std::string* error);
    std::string NextMainRunId() const;
    std::string NewStampId() const;  // session_id/operation_id 同形状
    EventScope MainBaseScope(const SessionManifest& manifest) const;
    // LatestResumableSessionId 的持锁内版本(ResumeAsNew 七步内用)。
    std::string LatestResumableSessionIdLocked();

    // ---- 恢复器内部(RecoverWorkspace 持锁调用) ----
    // 换账新侧续办:空 preparing 开张(Start)或半开的续写(Continue),
    // 补 command.completed、session.json 转 running,可接续则收作 active。
    void ContinueNewSide(const std::filesystem::path& next_dir, const std::string& next_id,
                         const MainJournalFacts& next_facts, const MainJournalFacts& old_facts,
                         SessionRecoveryEntry* old_entry, WorkspaceRecoveryReport* report);
    // 空 preparing 清账:delete lifecycle op + tombstone(aborted_before_start)。
    SessionRecoveryEntry AbortEmptyPreparing(const std::filesystem::path& session_dir,
                                             const std::string& session_id);
    // 管理操作共用的 lifecycle intent+result 落账。
    std::expected<std::string, std::string> RunLifecycleOp(
        LifecycleOperation operation, const std::string& session_id,
        const nlohmann::json& parameters, const nlohmann::json& outcome);

    // 第 3 步共用的"活动收口"落账(clear 与 close 同一合同)。
    struct ClosureEvidence {
        std::string turn_cancelled_event_id;
        std::vector<std::string> queue_cancelled_event_ids;
        std::string selection_interrupted_event_id;
        bool unknown_present = false;  // child/turn 有 unknown 或没收口
    };
    ClosureEvidence CloseActiveWork(ActiveSession* session, ClearParticipant* participant,
                                    const std::string& cancel_reason,
                                    const std::string& selection_interrupt_reason);

    SessionManagerOptions options_;
    SessionManagerClock default_clock_;
    SessionManagerClock* clock_ = &default_clock_;
    std::string workspace_key_;
    std::filesystem::path workspace_dir_;
    std::optional<ActiveSession> active_;
    // clear/close 同一把串行闸:换账掌管期间,并发请求回 clear/close.busy
    // (§3.3.1"重复请求要排队或回 clear_in_progress",这里选回忙)。
    std::atomic<bool> boundary_in_progress_{false};
    std::mutex mutex_;
};

}  // namespace lubancode::trajectory
