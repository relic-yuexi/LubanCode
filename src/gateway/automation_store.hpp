// AutomationStore(常驻总装 V1 第二件事):最小持久任务账——once 与手动
// run-now。V2 才加 interval/cron/时区/misfire,这里先把"账落得住、重启
// 重建得起、身份不随恢复洗掉"钉死。
//
// 账规矩(contracts.md §2/§11):
//   - 写者唯一(Gateway 持锁实例);事件行追加制,append 走
//     JournalWriter::AppendLine(PowerLoss)——写失败即 broken,受理面停住,
//     不拿"队列里还有"冒充已落账(§11.5 写盘失败停止受理/执行)。
//   - 索引/投影是派生物:重开从 jobs.jsonl 逐行重放,坏行跳过并记数,
//     不崩宿主;已提交行不回写不改义。
//   - 身份定式(§11.1):jobId 由调用方给(带域,不收会话局部号);
//     occurrenceId = hash(jobId + revision + slot),run-now 的 slot 用
//     请求时刻——同键重发命中同一 occurrence,不另造一拍。
//   - 幂等:创建/触发带 idempotencyKey,同键同载荷回原回执,同键异载荷
//     conflict(automation.revision_conflict 口径)。
//   - 不存模型输出正文(prompt 是任务规格不是产出;产出去 V3 与
//     delivery 账,这里只存 run/delivery 的引用)。
//
// 装配边界:纯文件账,零 runtime/agent 依赖(engine 层)。执行器的接线
// 在 runtime 层的 automation pump;持久任务的创建入口是本地控制命令
// (work_pump.hpp),不许用户直接改这个文件。
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trajectory/journal.hpp"  // JournalWriter:PowerLoss 档账行

namespace lubancode::gateway {

// 一枚持久任务(V1 最小面:once)。V2 的 specRevision/时区/预算字段
// 到时纯追加,不改旧义。
struct AutomationJob {
    std::string job_id;
    std::string prompt;
    std::int64_t due_at_ms = 0;   // once 的计划 slot(wall clock)
    std::uint64_t revision = 1;   // spec revision(创建即 1)
    std::int64_t created_at_ms = 0;
    std::string idempotency_key;  // 创建幂等键
    bool deleted = false;         // 暂无删除面;字段留给 V2 的 cancel
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
    // claim 账(occurrence.claimed 行):
    std::string owner_epoch;      // fencing 代号(= 持锁 boot_id)
    std::uint64_t attempt = 1;    // 从 1 起(V1 恒 1:重启不重派,重派归 V2)
    std::int64_t claimed_at_ms = 0;
    // 绑定账(occurrence.bound 行):V3 场与轮的预留身份——恢复器凭它
    // 找到原场原轮,不扫全 workspace。
    std::string session_id;
    std::string turn_id;
    // 结算账(occurrence.settled 行):
    std::string outcome;          // succeeded | failed | needs_review
    std::string detail;
    std::int64_t settled_at_ms = 0;
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
        std::string job_id;      // 原值或首发值
        std::string occurrence_id;  // run-now 建出的 occurrence
    };
    // once 任务:建 job 的同笔落首枚 occurrence(slot = due_at_ms,
    // 计划内时间),due_at_ms <= now 立即到点。job_id 空则发号(job-<n>)。
    JobReceipt CreateOnceJob(const std::string& job_id, const std::string& prompt,
                             std::int64_t due_at_ms, std::int64_t now_ms,
                             const std::string& idempotency_key);
    // 手动触发一次(不论 schedule):新 occurrence,slot = 请求时刻。
    JobReceipt RequestRunNow(const std::string& job_id, std::int64_t now_ms,
                             const std::string& idempotency_key);

    // ---- 派发面(泵用) ------------------------------------------------------
    // 认领:due 的 occurrence 落 occurrence.claimed(PowerLoss)后交出。
    // append 失败回空 + broken(调用方停泵),不把"想认领"当"已认领"。
    std::optional<AutomationOccurrence> ClaimDue(const std::string& owner_epoch,
                                                 std::int64_t now_ms);
    // 绑定预留 session/turn 身份(occurrence.bound 行)。绑定是恢复反查的
    // 锚:claim 后 bound 前崩溃,V1 保守 needs_review(Settle 处理),不猜。
    bool BindOccurrence(const std::string& occurrence_id, const std::string& session_id,
                        const std::string& turn_id, std::int64_t now_ms);
    // 结算:outcome ∈ succeeded|failed|needs_review。重复结算回 false
    //(幂等拒绝,不改首笔)。
    bool SettleOccurrence(const std::string& occurrence_id, const std::string& outcome,
                          const std::string& detail, std::int64_t now_ms);

    // ---- 只读投影(status/测试用) ------------------------------------------
    std::vector<AutomationJob> ListJobs() const;
    std::optional<AutomationJob> FindJob(const std::string& job_id) const;
    std::optional<AutomationOccurrence> FindOccurrence(const std::string& occurrence_id) const;
    std::vector<AutomationOccurrence> ListOccurrences() const;
    // 到点未 claim 的数(status work 栏)。
    std::size_t DueCount(std::int64_t now_ms) const;
    // 未结算(claimed 未 settled)的清单——重启恢复裁决的输入。
    std::vector<AutomationOccurrence> OpenOccurrences() const;

    bool broken() const { return broken_; }
    const std::filesystem::path& log_path() const { return log_path_; }

private:
    bool AppendLinePowerLoss(const nlohmann::json& line);

    std::filesystem::path log_path_;
    std::optional<trajectory::JournalWriter> writer_;
    bool broken_ = false;
    std::map<std::string, AutomationJob> jobs_;
    std::map<std::string, AutomationOccurrence> occurrences_;
    std::map<std::string, std::string> create_keys_;   // idempotencyKey -> jobId
    std::map<std::string, std::string> runnow_keys_;   // idempotencyKey -> occurrenceId
    std::uint64_t job_counter_ = 0;
};

// occurrenceId 定式(§11.1):hash(jobId + revision + slot)。"dl-"/"occ-"
// 前缀只作可读标记;同一 (jobId, revision, slot) 恒同一 id——重启、重试、
// 多次 resume 都不洗。
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
    std::uint64_t job_counter = 0;
};
AutomationProjection ReadAutomationProjection(const std::filesystem::path& log_file);

}  // namespace lubancode::gateway
