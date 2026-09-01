// 任务级 Model Turn 预算(Agent 任务级 Turn 预算与旧 Step 限制迁移设计)。
// 这只头文件住三样东西:术语词表、预算的纯数据形状(Snapshot/Permit)、
// 准入状态机(TurnBudgetAccount)。
//
// ---------------------------------------------------------------------------
// 术语词表(设计单 §3.1;全库"轮"字辈的口径以这份为准)
// ---------------------------------------------------------------------------
//   task          一只 Agent 从任务注册到终态的完整寿命。任务级 turn 预算
//                 (max_turns)的作用域就是它——不是一次 Run,不是一个输入轮。
//   input round   初始任务或一批 inbox/hook 增量触发的一次 Agent::Run()。
//                 旧名 max_steps_per_turn 的"turn"指的就是这一层(兼容窗内
//                 保留旧义;对外文档写它必须写全 input round)。
//   model turn    一次宿主准入的逻辑模型请求,对应至多一条 assistant
//                 message。新合同 runtime.max_turns / turn_limit 里的"turn"
//                 一律指这层。同一 turn 里并行一枚或十枚工具调用都只算一
//                 turn;provider 肚里的网络重试不另算。
//   step          AgentLoop 一次 Run() 里的循环坐标(step_index),内部概念,
//                 不是用户合同。OnModelStepStarted/steps_used 这些名字照旧
//                 服务旧路,不得再拿它替任务预算命名。
//
// 记账口径(设计单 §2.5/§3.2):
//   turns_attempted  宿主准入并发出过的逻辑模型请求(API 错、流断也保留,
//                    失败请求可能花钱,不能当没发生);
//   turns_completed  完整收回 assistant message 的请求;
//   turns_reserved   已占名额、尚未发出的逻辑请求(只在运行态诊断露出,
//                    正常收场必须归零)。
// 硬帽判断认 attempted + reserved,防两条线程同时占到最后一枚。
//
// 依赖方向:engine 层纯数据 + 纯逻辑,不认 tools/(TaskLedger 在 tools 侧
// 持一只 TurnBudgetAccount,用 Locked 系列在台账锁内驱动;workflow 节点与
// 单测直接用带锁系列)。
#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace lubancode::agent {

// 任务级 turn 预算的账面投影(设计单 §6.1)。limit=0 = 不设硬上限。
struct ModelTurnBudgetSnapshot {
    int limit = 0;
    int reserved = 0;
    int attempted = 0;
    int completed = 0;

    // 剩余额度:limit>0 时 = limit - attempted - reserved(负数不该出现,
    // 出现即账错,照实报);limit=0 = 0(不限,没有"剩余"这回事)。
    int Remaining() const { return limit > 0 ? limit - attempted - reserved : 0; }

    bool Exhausted() const { return limit > 0 && attempted + reserved >= limit; }
};

// 一次准入的凭据。一枚 permit 对应至多一次逻辑 backend 调用;permit_id 由
// 账户发号,全任务唯一,幂等口(commit/abort/mark_completed)按它对账。
struct ModelTurnPermit {
    std::uint64_t permit_id = 0;
    int limit = 0;
};

// 预算值从哪一级来(设计单 §4.2/§10.2;doctor/inspect 展示用)。
enum class TurnBudgetSource {
    Default,       // 链上没人给:0(不限)
    Config,        // subagent.default_max_turns
    Definition,    // Agent Definition runtime.max_turns
    HostOverride,  // 宿主/Workflow/测试夹具的 typed override(只可收窄)
};

// 派发档的任务 turn 预算(AgentTurnBudgetProfile,设计单 §10.2):Resolver
// 合并出来的那份,与 legacy per-input step 档案分家。
struct AgentTurnBudgetProfile {
    int max_turns = 0;
    TurnBudgetSource source = TurnBudgetSource::Default;
    // 旧字段(兼容窗)按原语义另记一笔:每个 input round 的模型请求数上限。
    // 只作投影与诊断,不与新账混写。
    std::optional<int> legacy_max_steps_per_input;
};

// 拒绝/出错的人话稳定码(引擎层不认台账,文案自持)。
inline constexpr const char* kTurnBudgetDeniedExhausted = "turn_budget.exhausted";
inline constexpr const char* kTurnBudgetDeniedNotActive = "turn_budget.task_not_active";
inline constexpr const char* kTurnBudgetErrorStalePermit = "turn_budget.stale_permit";

// ---------------------------------------------------------------------------
// TurnBudgetAccount:一枚任务(或一枚 workflow agent 节点)的准入状态机。
// 生命周期 = 任务寿命:RunTask 开跑前冻结 limit,跨任意多次 Agent::Run()
// 共用同一只账——这正是旧"每 Run 重置"漏洞的治法(设计单 §16.8)。
//
// 两套口:
//   带 Lock 后缀 —— 自持 mutex,独立锁域(workflow 节点/本地 gate/单测);
//   Unlocked 系列 —— 调用方自备外部锁(TaskLedger 在台账锁内调,保证
//   "验活态 -> 验额度 -> 占额"在同一锁域,设计单 §6.3)。
//
// 状态机(设计单 §3.2/§6.3/§6.4):
//   TryReserve      验活态 -> 验额度 -> reserved+1 -> 发 permit;
//   CommitSent      reserved-1, attempted+1, 返回从 1 起的 task_turn_index;
//   AbortBeforeSend reserved-1(请求未发,不碰 attempted);
//   MarkCompleted   attempted 的那枚收完整 assistant 时 completed+1,幂等。
// 同一 permit 不可 commit 两次、commit 后不可 abort;重复 MarkCompleted 不
// 加二次。任务串行发模型请求是现状前提,但所有口都在锁内对账,两线程误
// 争时恰一枚成功、另一枚稳定拒绝(设计单 §12)。
// ---------------------------------------------------------------------------
class TurnBudgetAccount {
public:
    explicit TurnBudgetAccount(int limit = 0) : limit_(limit < 0 ? 0 : limit) {}

    TurnBudgetAccount(const TurnBudgetAccount&) = delete;
    TurnBudgetAccount& operator=(const TurnBudgetAccount&) = delete;

    // ---- 预算冻结(注册时一次;此后只读)----
    void FreezeLimit(int limit) {
        const std::lock_guard<std::mutex> lock(mutex_);
        FreezeLimitLocked(limit);
    }
    void FreezeLimitLocked(int limit) { limit_ = limit < 0 ? 0 : limit; }

    // ---- 带锁系列(独立锁域)----------------------------------------------
    std::expected<ModelTurnPermit, std::string> TryReserveLock(bool task_active) {
        const std::lock_guard<std::mutex> lock(mutex_);
        return TryReserveLocked(task_active);
    }
    std::expected<int, std::string> CommitSentLock(const ModelTurnPermit& permit) {
        const std::lock_guard<std::mutex> lock(mutex_);
        return CommitSentLocked(permit);
    }
    bool AbortBeforeSendLock(const ModelTurnPermit& permit) {
        const std::lock_guard<std::mutex> lock(mutex_);
        return AbortBeforeSendLocked(permit);
    }
    bool MarkCompletedLock(const ModelTurnPermit& permit) {
        const std::lock_guard<std::mutex> lock(mutex_);
        return MarkCompletedLocked(permit);
    }
    ModelTurnBudgetSnapshot SnapshotLock() const {
        const std::lock_guard<std::mutex> lock(mutex_);
        return SnapshotLocked();
    }
    // 任务级"将尽提示"的认领(设计单 §9.2):remaining 第一次降到
    // min(limit, threshold)(含)时认领成功,一任务恰一次;limit<=0 恒 false。
    bool ShouldNudgeOnceLock(int threshold) {
        const std::lock_guard<std::mutex> lock(mutex_);
        return ShouldNudgeOnceLocked(threshold);
    }

    // ---- Unlocked 系列(调用方自备外部锁)----------------------------------
    std::expected<ModelTurnPermit, std::string> TryReserveLocked(bool task_active) {
        if (!task_active) {
            return std::unexpected(std::string(kTurnBudgetDeniedNotActive));
        }
        if (limit_ > 0 && attempted_ + reserved_ >= limit_) {
            return std::unexpected(std::string(kTurnBudgetDeniedExhausted));
        }
        ++reserved_;
        const std::uint64_t id = next_permit_id_++;
        last_reserved_permit_ = id;
        return ModelTurnPermit{id, limit_};
    }

    std::expected<int, std::string> CommitSentLocked(const ModelTurnPermit& permit) {
        if (permit.permit_id == 0 || permit.permit_id != last_reserved_permit_) {
            // 同一 permit 不可 commit 两次;已 commit/未知 permit 稳定拒绝。
            return std::unexpected(std::string(kTurnBudgetErrorStalePermit));
        }
        last_reserved_permit_ = 0;
        --reserved_;
        ++attempted_;
        last_attempted_permit_ = permit.permit_id;
        return attempted_;  // task_turn_index,从 1 起
    }

    bool AbortBeforeSendLocked(const ModelTurnPermit& permit) {
        if (permit.permit_id == 0 || permit.permit_id != last_reserved_permit_) {
            return false;  // 已 commit 的 abort 稳定拒绝;未知 permit 忽略
        }
        last_reserved_permit_ = 0;
        --reserved_;
        return true;
    }

    bool MarkCompletedLocked(const ModelTurnPermit& permit) {
        if (permit.permit_id == 0 || permit.permit_id != last_attempted_permit_ ||
            permit.permit_id == last_completed_permit_) {
            // 幂等:同一 permit 重复完成不加第二次;认不得的照实拒绝。
            return false;
        }
        ++completed_;
        last_completed_permit_ = permit.permit_id;
        return true;
    }

    ModelTurnBudgetSnapshot SnapshotLocked() const {
        ModelTurnBudgetSnapshot snapshot;
        snapshot.limit = limit_;
        snapshot.reserved = reserved_;
        snapshot.attempted = attempted_;
        snapshot.completed = completed_;
        return snapshot;
    }

    bool ShouldNudgeOnceLocked(int threshold) {
        if (limit_ <= 0 || nudge_claimed_) {
            return false;
        }
        const int remaining = limit_ - attempted_ - reserved_;
        if (remaining != std::min(limit_, threshold)) {
            return false;
        }
        nudge_claimed_ = true;
        return true;
    }

private:
    mutable std::mutex mutex_;
    int limit_ = 0;
    int reserved_ = 0;
    int attempted_ = 0;
    int completed_ = 0;
    std::uint64_t next_permit_id_ = 1;
    // 单飞 permit 轨迹:任务串行发请求,同一时刻至多一枚在飞。三枚 id 支撑
    // 幂等判定(commit 两次/commit 后 abort/重复完成)。
    std::uint64_t last_reserved_permit_ = 0;
    std::uint64_t last_attempted_permit_ = 0;
    std::uint64_t last_completed_permit_ = 0;
    bool nudge_claimed_ = false;
};

// 引擎侧的窄预算门(设计单 §6.2):AgentLoop 只认这枚接口,不认 TaskLedger。
// 装配层(tools 的 task-scoped adapter / workflow 的本地账户)把闭包灌进来。
// 空函数 = 没装门(主会话/旧调用方),一处不调,行为与从前一字不差。
struct ModelTurnBudgetGate {
    std::function<std::expected<ModelTurnPermit, std::string>()> try_reserve;
    std::function<std::expected<int, std::string>(const ModelTurnPermit&)> commit_sent;
    std::function<void(const ModelTurnPermit&)> abort_before_send;
    std::function<void(const ModelTurnPermit&)> mark_completed;
    std::function<ModelTurnBudgetSnapshot()> snapshot;
    // 任务级"将尽提示"认领口(§9.2):真 = 该注入且本任务仅此一次。
    std::function<bool()> claim_turn_nudge;

    bool armed() const { return try_reserve != nullptr; }
};

// 将尽阈值(设计单 §9.2 定死为 3,与旧 kStepLimitNudgeThreshold 同数)。
inline constexpr int kTurnNudgeThreshold = 3;

// 从一只本地账户造一枚门(workflow 节点/单测用):闭包拷裸指针,账户寿命
// 须盖过用门的那次 Run。
inline ModelTurnBudgetGate MakeLocalTurnBudgetGate(TurnBudgetAccount* account, bool* task_active = nullptr) {
    ModelTurnBudgetGate gate;
    gate.try_reserve = [account, task_active]() {
        return account->TryReserveLock(task_active == nullptr || *task_active);
    };
    gate.commit_sent = [account](const ModelTurnPermit& permit) {
        return account->CommitSentLock(permit);
    };
    gate.abort_before_send = [account](const ModelTurnPermit& permit) {
        account->AbortBeforeSendLock(permit);
    };
    gate.mark_completed = [account](const ModelTurnPermit& permit) {
        account->MarkCompletedLock(permit);
    };
    gate.snapshot = [account]() { return account->SnapshotLock(); };
    gate.claim_turn_nudge = [account]() { return account->ShouldNudgeOnceLock(kTurnNudgeThreshold); };
    return gate;
}

// 纯函数,可单测:limit>0 且 remaining == min(limit, threshold) 时为真
//(remaining = limit - attempted - reserved)。与 TurnBudgetAccount::
// ShouldNudgeOnceLocked 同一条判定,拆出来给不持账户的调用方对账用。
inline bool ShouldNudgeTurnLimit(int limit, int attempted_plus_reserved, int threshold) {
    if (limit <= 0) {
        return false;
    }
    return (limit - attempted_plus_reserved) == std::min(limit, threshold);
}

}  // namespace lubancode::agent
