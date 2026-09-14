// AutomationStore(常驻总装 V1 第二件事 + V2 周期扩展):持久任务账——
// once/interval/cron 三种计划形态、领域操作(update/pause/resume/cancel)、
// 周期拍点生成(misfire 政策/同 slot 合并/并发与队列帽)、恢复重派、
// heartbeat 观察账、/loop 导入 receipt。V1 已把"账落得住、重启重建得起、
// 身份不随恢复洗掉"钉死;V2 纯追加,不改旧义。
//
// 账规矩(contracts.md §2/§11/§13):
//   - 写者唯一(Gateway 持锁实例);事件行追加制,append 走
//     JournalWriter::AppendLine(PowerLoss)——写失败即 broken,受理面停住,
//     不拿"队列里还有"冒充已落账(§11.5 写盘失败停止受理/执行)。
//   - 索引/投影是派生物:重开从 jobs.jsonl 逐行重放,坏行跳过并记数,
//     不崩宿主;已提交行不回写不改义。未知 type 前向兼容跳过。
//   - 身份定式(§11.1):jobId 由调用方给(带域,不收会话局部号);
//     occurrenceId = hash(jobId + revision + slot),slot 用计划内 UTC
//     时间(interval/cron 同式)——重启、重试、多次 resume 都不洗;
//     时钟倒拨不重跑原 slot(游标只前进,不回拨)。
//   - 幂等:创建/触发/领域操作带 idempotencyKey,同键同载荷回原回执,
//     同键异载荷 conflict(automation.revision_conflict 口径);/loop 导入
//     按来源(sessionId+taskId)幂等——无 receipt 不暗搬、不双跑。
//   - 写操作带 expectedRevision(CAS,照 GoalService/SessionCreateLedger
//     先例:0 = 拒,必须显式);排队中的 occurrence 固定建账时的 revision。
//   - 不存模型输出正文(prompt 是任务规格不是产出;产出去 V3 与
//     delivery 账,这里只存 run/delivery 的引用)。heartbeat 的观察账
//     只存结果 hash 与是否已通知,不存正文。
//
// V2 周期语义(contracts.md §13,调度引擎在 automation_schedule.*):
//   - misfire:coalesce(默认,停机跨多周期合并补一拍,occurrence 落最老
//     一拍、missedCount 记覆盖范围)| skip(错过的拍不补,游标前进,
//     下一拍等未来)。
//   - 同 job 不重叠:有 claimed 未结算的活儿不再生成;最多留一份合并
//     待办(已有 scheduled 待办时,新到点的拍并进它,occurrence.merged)。
//   - 队列帽:全局 open(scheduled+claimed)occurrence 数达帽即停生成
//     (游标不动,下轮重试),不无限 catch up。
//   - 重派:claim 后无绑定行(未开轮)可重派同一 occurrence,attempt+1;
//     attempt 帽(默认 3)到顶转 needs_review。已绑定的按 V1 账裁决。
//   - deadline:job 可带 deadlineMs;过线的待执行 occurrence 结算
//     cancelled(deadline_reached),不判 failed、不再执行;生成侧同样
//     停。在飞硬掐沿用 V1 墙钟预算(同步泵在 turn 边界生效,如实分账)。
//   - heartbeat(notifyOnChange):结算前记 occurrence.observed(结果
//     hash/是否变化/是否投递);正文未变不再投递;检查失败永远投递,
//     不记"无变化"。
//
// 装配边界:纯文件账,零 runtime/agent 依赖(engine 层)。执行器的接线
// 在 runtime 层的 automation pump;持久任务的创建/变更入口是本地控制
// 命令(work_pump.hpp),不许用户直接改这个文件。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gateway/automation_schedule.hpp"  // ScheduleSpec/Kind/MisfirePolicy(纯函数引擎)
#include "trajectory/journal.hpp"           // JournalWriter:PowerLoss 档账行

namespace lubancode::gateway {

// 任务生命周期(V2 纯追加;V1 只有 active 语义,旧账重放默认 active)。
enum class AutomationJobState { Active, Paused, Cancelled };
std::string ToString(AutomationJobState state);

// 一枚持久任务。V1 字段(scheduleKind=once)不变;V2 字段全带默认,
// 旧账重放落默认值,行为与 V1 一致。
struct AutomationJob {
    std::string job_id;
    std::string prompt;
    std::int64_t due_at_ms = 0;   // once 的计划 slot(wall clock)
    std::uint64_t revision = 1;   // spec revision(创建即 1;update +1)
    std::int64_t created_at_ms = 0;
    std::string idempotency_key;  // 创建幂等键
    bool deleted = false;         // V1 遗留字段(V2 用 state,不再置位)
    // ---- V2 周期语义 ----
    ScheduleKind schedule_kind = ScheduleKind::Once;
    std::int64_t interval_seconds = 0;  // interval:周期秒
    std::int64_t anchor_ms = 0;         // interval:锚点(slot = anchor + k*interval)
    std::string cron_expr;              // cron:五字段受限子集原文
    std::string timezone = "UTC";       // 显式存储;cron 的运算输入
    MisfirePolicy misfire = MisfirePolicy::Coalesce;
    std::int64_t deadline_ms = 0;       // 0 = 不设;过线不派发、结算 cancelled
    AutomationJobState state = AutomationJobState::Active;
    std::int64_t schedule_cursor_ms = 0;  // 生成游标:此时刻(含)之前的拍已消化
    bool notify_on_change = false;        // heartbeat 口:结果未变不投递
    std::string last_observed_sha;        // heartbeat:上次已投递正文 hash
    std::string session_policy = "fresh";  // fresh|continuation(continuation 归 V3 渠道线,本批明拒)
    std::string imported_from;            // 非空 = /loop 显式导入("loop:<sessionId>:<taskId>")
};

// 一次该跑的事实。
struct AutomationOccurrence {
    enum class State {
        Scheduled,  // 到点等 claim
        Claimed,    // 已认领(ownerEpoch 在账),等绑定/执行/结算
        Settled,    // 终态(outcome 见下)
    };
    std::string occurrence_id;
    std::string job_id;
    std::int64_t slot_ms = 0;     // 计划内时间(§11.1:不用实际启动时间)
    State state = State::Scheduled;
    std::string reason;           // schedule | run_now(生成来源)
    std::uint32_t missed_count = 0;  // coalesce 并掉的拍数(覆盖范围账)
    // claim 账(occurrence.claimed 行):
    std::string owner_epoch;      // fencing 代号(= 持锁 boot_id)
    std::uint64_t attempt = 1;    // 从 1 起;claim 后无绑定重派 +1(V2)
    std::int64_t claimed_at_ms = 0;
    bool cancel_requested = false;  // 取消请求(取消边界:在飞的收完再算)
    // 绑定账(occurrence.bound 行):V3 场与轮的预留身份——恢复器凭它
    // 找到原场原轮,不扫全 workspace。无绑定 = 未开轮,可重派。
    std::string session_id;
    std::string turn_id;
    // 结算账(occurrence.settled 行):
    std::string outcome;          // succeeded | failed | needs_review | cancelled
    std::string detail;
    std::int64_t settled_at_ms = 0;
    // 观察账(occurrence.observed 行;heartbeat 用):
    std::string observed_sha;
    bool observed_changed = false;
    bool observed_delivered = false;
};

// /loop 导入 receipt(§十 V2 第五件事):显式导入的唯一凭据。无 receipt
// 不暗搬、不双跑——账上没有 receipt 就没有"已导入"这回事。
struct LoopImportReceipt {
    std::string receipt_id;      // "imp-" + hash(sessionId + taskId)
    std::string job_id;          // 导入建出的 interval job
    std::string source_session_id;
    std::string source_task_id;
    std::string prompt_sha256;
    std::int64_t interval_seconds = 0;
    std::int64_t imported_at_ms = 0;
    std::string idempotency_key;
};

class AutomationStore {
public:
    struct OpenResult {
        bool ok = false;
        std::string error;             // 人话(带稳定码前缀)
        std::size_t skipped_lines = 0; // 重放跳过的坏行数(诊断,不崩)
    };

    // 打开(或新建)automation 账:目录不存在则建;既有账逐行重放投影。
    // 打不开/读不懂账 = 拒绝开张(caller 停业务面),不悄悄从空账跑。
    static OpenResult Open(AutomationStore* out, const std::filesystem::path& log_file);

    AutomationStore() = default;
    ~AutomationStore();
    AutomationStore(AutomationStore&&) noexcept;
    AutomationStore& operator=(AutomationStore&&) noexcept;
    AutomationStore(const AutomationStore&) = delete;
    AutomationStore& operator=(const AutomationStore&) = delete;

    // ---- 任务创建与手动触发 ----------------------------------------------
    struct JobReceipt {
        bool accepted = false;   // 本次新建/新触发
        bool duplicate = false;  // 同幂等键同载荷:回原回执
        std::string error_code;  // automation.revision_conflict | automation.append_failed
                                 // | automation.schedule_invalid | automation.timezone_invalid
                                 // | automation.job_terminal | automation.import_conflict
        std::string job_id;      // 原值或首发值
        std::string occurrence_id;  // run-now/once 建出的 occurrence
        std::string receipt_id;     // import-loop 的 receipt id
        std::uint64_t revision = 0;  // 领域操作后的 revision
    };

    // 任务规格(创建用;interval 锚点缺省 = now)。
    struct JobSpec {
        std::string job_id;   // 空 = 发号(job-<n>)
        std::string prompt;
        ScheduleKind kind = ScheduleKind::Once;
        std::int64_t due_at_ms = 0;        // once
        std::int64_t interval_seconds = 0; // interval
        std::string cron_expr;             // cron
        std::string timezone = "UTC";
        MisfirePolicy misfire = MisfirePolicy::Coalesce;
        std::int64_t deadline_ms = 0;
        bool notify_on_change = false;
        std::string session_policy = "fresh";  // 只认 fresh(continuation 归 V3,明拒)
    };
    // 通用创建(V2):once 同笔落首枚 occurrence(= V1 语义);interval/
    // cron 不建 occurrence,归 SweepSchedule 按政策生成。坏规格明拒。
    JobReceipt CreateJob(const JobSpec& spec, std::int64_t now_ms,
                         const std::string& idempotency_key);
    // once 任务(V1 面,签名行为不变;内部走 CreateJob)。
    JobReceipt CreateOnceJob(const std::string& job_id, const std::string& prompt,
                             std::int64_t due_at_ms, std::int64_t now_ms,
                             const std::string& idempotency_key);
    // 手动触发一次(不论 schedule):新 occurrence,slot = 请求时刻。
    JobReceipt RequestRunNow(const std::string& job_id, std::int64_t now_ms,
                             const std::string& idempotency_key);

    // ---- /loop 显式导入(V2 第五件事) ------------------------------------
    // 按来源(sessionId+taskId)幂等:同来源再导回原 receipt(duplicate),
    // 不建第二个 job——无 receipt 不暗搬、不双跑。导入建的是 interval job
    // (prompt 固定副本,不逐拍现读原 /loop 状态;原状态只读留档)。
    JobReceipt ImportLoop(const std::string& source_session_id, const std::string& source_task_id,
                          const std::string& prompt, std::int64_t interval_seconds,
                          std::int64_t now_ms, const std::string& idempotency_key);
    std::optional<LoopImportReceipt> FindLoopImport(const std::string& source_session_id,
                                                    const std::string& source_task_id) const;
    std::vector<LoopImportReceipt> ListLoopImports() const;

    // ---- 领域操作(V2 第二件事;CAS + 幂等键) -----------------------------
    struct JobUpdatePatch {
        bool set_prompt = false;
        std::string prompt;
        bool set_due_at = false;           // once
        std::int64_t due_at_ms = 0;
        bool set_interval = false;         // interval(改锚点 = now,时间轴重排)
        std::int64_t interval_seconds = 0;
        bool set_cron = false;             // cron(游标 = now,时间轴重排)
        std::string cron_expr;
        bool set_timezone = false;
        std::string timezone;
        bool set_misfire = false;
        MisfirePolicy misfire = MisfirePolicy::Coalesce;
        bool set_deadline = false;
        std::int64_t deadline_ms = 0;
        bool set_notify_on_change = false;
        bool notify_on_change = false;
    };
    // expected_revision 必须等于当前 revision(CAS;0 = 拒,必须显式)。
    // 排队中的 occurrence 固定建账 revision,不受 update 影响。
    JobReceipt UpdateJob(const std::string& job_id, std::uint64_t expected_revision,
                         const JobUpdatePatch& patch, std::int64_t now_ms,
                         const std::string& idempotency_key);
    // pause:停生成与派发(已在飞的沿取消边界收);已排 occurrence 原地
    // 等待,不结算。resume:游标直进到 now(paused 窗口的拍不补跑)。
    JobReceipt PauseJob(const std::string& job_id, std::uint64_t expected_revision,
                        std::int64_t now_ms, const std::string& idempotency_key);
    JobReceipt ResumeJob(const std::string& job_id, std::uint64_t expected_revision,
                         std::int64_t now_ms, const std::string& idempotency_key);
    // cancel:终态。先停未来派发(scheduled 的 occurrence 就地结算
    // cancelled,历史保留),再标在飞(claimed)的取消边界;不删账。
    JobReceipt CancelJob(const std::string& job_id, std::uint64_t expected_revision,
                         std::int64_t now_ms, const std::string& idempotency_key);

    // ---- 周期生成(V2 第一件事;泵每 tick 调) -----------------------------
    struct SweepResult {
        bool ok = true;                // false = 账 broken(调用方停泵)
        std::size_t generated = 0;     // 新建 occurrence 数
        std::size_t merged = 0;        // 并入既有待办的拍数(occurrence.merged)
        std::size_t skipped_slots = 0; // misfire=skip 丢掉的拍数
        bool stalled = false;          // 队列帽满,生成停(游标不动)
    };
    SweepResult SweepSchedule(std::int64_t now_ms);
    // 队列帽(全局 open occurrence 数;测试可调,生产默认 256)。
    void set_max_open_occurrences(std::size_t cap) { max_open_occurrences_ = cap; }
    std::size_t max_open_occurrences() const { return max_open_occurrences_; }
    // 重派 attempt 帽(claim 后无绑定重派的次数上限;到顶转 needs_review)。
    static constexpr std::uint64_t kMaxAttempts = 3;

    // ---- 派发面(泵用) ------------------------------------------------------
    // 认领:due 的 occurrence 落 occurrence.claimed(PowerLoss)后交出。
    // paused/cancelled 任务的 occurrence 不认领;过 job deadline 的就地
    // 结算 cancelled(deadline_reached)再挑下一枚。append 失败回空 +
    // broken(调用方停泵),不把"想认领"当"已认领"。
    std::optional<AutomationOccurrence> ClaimDue(const std::string& owner_epoch,
                                                 std::int64_t now_ms);
    // 绑定预留 session/turn 身份(occurrence.bound 行)。绑定是恢复反查的
    // 锚;无绑定 = 未开轮(可重派),有绑定 = 已开轮(按 V3 账裁决)。
    bool BindOccurrence(const std::string& occurrence_id, const std::string& session_id,
                        const std::string& turn_id, std::int64_t now_ms);
    // 结算:outcome ∈ succeeded|failed|needs_review|cancelled。重复结算回
    // false(幂等拒绝,不改首笔)。
    bool SettleOccurrence(const std::string& occurrence_id, const std::string& outcome,
                          const std::string& detail, std::int64_t now_ms);
    // 重派(恢复路):claim 后无绑定(未开轮)的 occurrence 回 Scheduled,
    // attempt+1。回 false = attempt 帽到顶/账 broken/已不在 claimed(调用
    // 方按 needs_review 收)。
    bool RedispatchOccurrence(const std::string& occurrence_id, const std::string& reason,
                              std::int64_t now_ms);
    // heartbeat 观察账:occurrence.observed 行(结果 hash/是否变化/是否已
    // 投递);update_last_observed 时同步 job 的 last_observed_sha(重放同
    // 源)。"检查失败不能记无变化"由调用方保证:失败也投递,changed=true。
    bool RecordObservation(const std::string& occurrence_id, const std::string& result_sha,
                           bool changed, bool delivered, bool update_last_observed,
                           std::int64_t now_ms);

    // ---- 只读投影(status/测试用) ------------------------------------------
    std::vector<AutomationJob> ListJobs() const;
    std::optional<AutomationJob> FindJob(const std::string& job_id) const;
    std::optional<AutomationOccurrence> FindOccurrence(const std::string& occurrence_id) const;
    std::vector<AutomationOccurrence> ListOccurrences() const;
    std::vector<AutomationOccurrence> ListJobOccurrences(const std::string& job_id) const;
    // 到点未 claim 的数(status work 栏)。
    std::size_t DueCount(std::int64_t now_ms) const;
    // 未结算(claimed 未 settled)的清单——重启恢复裁决的输入。
    std::vector<AutomationOccurrence> OpenOccurrences() const;
    // open(scheduled+claimed)总数——队列帽的口径。
    std::size_t OpenOccurrenceCount() const;

    bool broken() const { return broken_; }
    const std::filesystem::path& log_path() const { return log_path_; }

private:
    bool AppendLinePowerLoss(const nlohmann::json& line);
    // 幂等键登记(领域操作族):同键同操作回原回执,同键异操作/异任务拒。
    struct OpKeyEntry {
        std::string verb;       // update|pause|resume|cancel
        std::string job_id;
        std::uint64_t revision = 0;
    };
    bool CheckOpKey(const std::string& idempotency_key, const std::string& verb,
                    const std::string& job_id, JobReceipt* receipt);
    void RegisterOpKey(const std::string& idempotency_key, const std::string& verb,
                       const std::string& job_id, std::uint64_t revision);

    std::filesystem::path log_path_;
    std::optional<trajectory::JournalWriter> writer_;
    bool broken_ = false;
    std::map<std::string, AutomationJob> jobs_;
    std::map<std::string, AutomationOccurrence> occurrences_;
    std::map<std::string, std::string> create_keys_;  // idempotencyKey -> jobId
    std::map<std::string, std::string> runnow_keys_;  // idempotencyKey -> occurrenceId
    std::map<std::string, OpKeyEntry> op_keys_;       // idempotencyKey -> 领域操作回执
    std::map<std::string, LoopImportReceipt> loop_imports_;  // "loop:<sid>:<tid>" -> receipt
    std::uint64_t job_counter_ = 0;
    std::size_t max_open_occurrences_ = 256;
};

// occurrenceId 定式(§11.1):hash(jobId + revision + slot)。"occ-" 前缀只
// 作可读标记;同一 (jobId, revision, slot) 恒同一 id——重启、重试、多次
// resume 都不洗。
std::string MakeOccurrenceId(const std::string& job_id, std::uint64_t revision,
                             std::int64_t slot_ms);

// 只读投影(status 分栏/测试用):从账文件逐行重放,坏行跳过留数;文件
// 不存在给空投影——零建目录零写盘(disabled 零副作用合同 §6)。与
// AutomationStore::Open 同一份重放逻辑,写侧也走它(单一真源)。
struct AutomationProjection {
    std::map<std::string, AutomationJob> jobs;                // jobId -> job
    std::map<std::string, AutomationOccurrence> occurrences;  // occurrenceId -> occurrence
    std::size_t skipped_lines = 0;
    std::map<std::string, std::string> create_keys;  // 幂等键 -> jobId
    std::map<std::string, std::string> runnow_keys;  // 幂等键 -> occurrenceId
    std::map<std::string, LoopImportReceipt> loop_imports;  // 来源键 -> receipt
    std::uint64_t job_counter = 0;
};
AutomationProjection ReadAutomationProjection(const std::filesystem::path& log_file);

}  // namespace lubancode::gateway
