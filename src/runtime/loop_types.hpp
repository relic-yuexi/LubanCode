// `/loop` 会话定时循环(loop 单第 0 期):领域类型、状态机转换表、错误码。
//
// 单子的定案在这里落地:
//   - LoopTask/LoopTick/策略枚举全在这一只头(纯数据,零 IO)。
//   - 合法状态转换集中成一张表,LoopScheduler 是唯一写口;CLI、TUI、
//     模型工具都不许直改 LoopTask.state。
//   - 错误码是稳定字符串(线上与 UI 都用它,不拿中文正文当机器判断)。
//   - 与未来 /goal 不互为别名:零间隔不收(烧 token、无终点 evaluator),
//     goal continuation 走 SessionWorkScheduler 的 GoalContinuation 档。
//
// 依赖铁律:只认标准库 + nlohmann/json,不 include cli/app/agent,不被
// 反向依赖——loop 状态机是最底层,谁都可以拿来用,它谁都不认。

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lubancode::runtime::loop {

// ---------------------------------------------------------------------------
// 稳定枚举(线上是字符串,不是数字——数字重排就是存档破坏)
// ---------------------------------------------------------------------------

// 任务生命周期。terminal 状态(后五个)不自动复活;resume 只收 Paused。
enum class LoopTaskState {
    Active,            // 排着拍,到点消费
    Paused,            // 用户暂停:保留定义,不排新拍
    Due,               // 到点的拍在队列里等主泵(single-flight 的可见态)
    Running,           // 一枚 tick 的执行轮在跑
    WaitingPermission, // 当前拍悬在工具审批上;后续拍 coalesce
    BackingOff,        // provider 连败/限流退避中,退避期满回 Active
    Completed,         // 模型经 loop_control 声明完成(正常终态)
    Cancelled,         // 用户 stop:审计账不删
    Expired,           // created_at + 7d 到头(recurring expiry)
    Broken,            // prompt 源没了/恢复时命令已不存在等不可继续的坏
};
std::string ToString(LoopTaskState state);
bool ParseLoopTaskState(const std::string& s, LoopTaskState& out);

bool IsLoopTerminal(LoopTaskState state);
// pause 保留、resume 可续的态。
bool IsLoopResumable(LoopTaskState state);
// 到点可消费的态(主泵只认这两档)。
bool IsLoopConsumable(LoopTaskState state);

// 错过的拍怎么处理。首版只有 CoalesceLatest(合并成一拍,不追债)——
// 枚举先立着,单子明令"missed_tick_policy = coalesce_latest"。
enum class MissedTickPolicy { CoalesceLatest };
std::string ToString(MissedTickPolicy policy);
bool ParseMissedTickPolicy(const std::string& s, MissedTickPolicy& out);

// 同一任务上一拍没跑完时下一拍怎么办。首版只有 SingleFlight。
enum class OverlapPolicy { SingleFlight };
std::string ToString(OverlapPolicy policy);
bool ParseOverlapPolicy(const std::string& s, OverlapPolicy& out);

// prompt 的来源档。
enum class LoopPromptSource {
    Inline,       // /loop 5m <正文> 的正文(永远压过 loop.md)
    ProjectFile,  // <project-root>/.lubancode/loop.md(须过项目 trust)
    UserFile,     // <home>/.lubancode/loop.md
    Builtin,      // 内置 maintenance prompt
};
std::string ToString(LoopPromptSource source);
bool ParseLoopPromptSource(const std::string& s, LoopPromptSource& out);

// 一枚 tick 的结局(稳定 outcome 表,单子"失败与退避"节)。
enum class LoopTickOutcome {
    Pending,      // 还没跑完
    Succeeded,    // 正常收口(工具 is_error 不算失败)
    Declined,     // 权限被拒:本拍拒,task 默认仍 Active
    Cancelled,    // 用户停了本拍
    ProviderError,     // provider 连接/请求失败(有限退避)
    RateLimited,       // 带 retry-after 的限流(BackingOff)
    PermissionWaitTimeout, // 无人可答审批超时
    PromptSourceMissing,   // loop.md 没了/读失败:本拍 Broken
    SessionClosed,     // 会话收场,拍不补
    Expired,           // expiry 到头
    Corrupt,           // 存档账对不上
    UnknownAfterStart, // 崩在 started 与 finished 之间,结局不明
};
std::string ToString(LoopTickOutcome outcome);
bool ParseLoopTickOutcome(const std::string& s, LoopTickOutcome& out);

// ---------------------------------------------------------------------------
// 稳定错误码
// ---------------------------------------------------------------------------
inline constexpr const char* kErrLoopNotFound = "loop.not_found";
inline constexpr const char* kErrLoopTooManyActive = "loop.too_many_active";
inline constexpr const char* kErrLoopIntervalInvalid = "loop.interval_invalid";
inline constexpr const char* kErrLoopIntervalTooSmall = "loop.interval_too_small";
inline constexpr const char* kErrLoopIntervalTooLarge = "loop.interval_too_large";
inline constexpr const char* kErrLoopPromptEmpty = "loop.prompt_empty";
inline constexpr const char* kErrLoopPromptSlash = "loop.prompt_slash";
inline constexpr const char* kErrLoopPromptTooLong = "loop.prompt_too_long";
inline constexpr const char* kErrLoopTerminal = "loop.terminal";
inline constexpr const char* kErrLoopBusy = "loop.busy";
inline constexpr const char* kErrLoopInvalidTransition = "loop.invalid_transition";
inline constexpr const char* kErrLoopNotInteractive = "loop.not_interactive";
inline constexpr const char* kErrLoopStoreUnavailable = "loop.store_unavailable";
inline constexpr const char* kErrLoopScope = "loop.scope";
inline constexpr const char* kErrLoopBroken = "loop.broken";

// ---------------------------------------------------------------------------
// 集中默认值(单子"默认值"节:不散在 parser、scheduler、UI 三处)
// ---------------------------------------------------------------------------
struct LoopDefaults {
    static constexpr std::chrono::seconds kDefaultInterval{600};   // 10m
    static constexpr std::chrono::seconds kMinimumInterval{60};    // 1m
    static constexpr std::chrono::seconds kMaximumInterval{604800}; // 7d
    // recurring_expiry = created_at + 7d。
    static constexpr std::chrono::seconds kExpiryAge{604800};
    // 一场 session 最多 8 只 active task。
    static constexpr int kMaxActivePerSession = 8;
    // loop.md 上限 25,000 bytes:超过拒绝(不截断,免得指令尾巴被截没)。
    static constexpr std::size_t kPromptFileMaxBytes = 25000;
    // 同一 denial 连续三拍自动 Pause(免得每十分钟弹同一张框)。
    static constexpr int kDenialPauseThreshold = 3;
    // provider 连续五拍失败自动 Pause。
    static constexpr int kProviderFailPauseThreshold = 5;
    // 同 tick 内 provider 瞬时错误的有限退避:5s/15s,最多两次。
    static constexpr std::chrono::seconds kRetryBackoffFirst{5};
    static constexpr std::chrono::seconds kRetryBackoffSecond{15};
    static constexpr int kMaxAttemptsPerTick = 3;  // 首发 + 两次退避重试
    // 连续用户输入让 loop 饿死:超过 interval 两倍仍未消费,状态栏提示
    // delayed(delayed 只是展示,不是状态)。
    static constexpr int kDelayedAfterIntervals = 2;
};

// ---------------------------------------------------------------------------
// 领域数据
// ---------------------------------------------------------------------------

// wall time 的一枚点(Unix epoch 毫秒)。存档、事件行、next_due 全用这;
// 进程内等待由 scheduler 用 steady clock 另算,恢复时从 wall 重算剩余。
using WallTimeMs = std::int64_t;

// 一只定时任务(长账)。
struct LoopTask {
    std::string task_id;      // loop-N(宿主单调发号)
    std::string session_id;   // 属于哪场 session(session-scoped)
    std::string prompt;       // inline 正文;文件源时是空串(每拍现读)
    LoopPromptSource prompt_source = LoopPromptSource::Inline;
    std::string prompt_file;  // 文件源的路径(空 = inline/builtin)
    std::string prompt_sha256;  // inline 固定 hash;文件源每拍现算
    std::chrono::seconds interval{600};
    WallTimeMs created_at_ms = 0;
    WallTimeMs expires_at_ms = 0;
    WallTimeMs next_due_at_ms = 0;
    LoopTaskState state = LoopTaskState::Active;
    std::uint64_t tick_seq = 0;        // 发过的 tick 总数(下一枚的编号)
    std::uint64_t run_count = 0;       // 真正开了 turn 的拍数
    std::uint64_t skipped_count = 0;   // coalesce 掉的拍数
    std::uint64_t consecutive_failures = 0;  // provider_error 连败账
    std::uint64_t consecutive_denials = 0;   // declined 连拍账
    std::optional<std::string> active_turn_id;  // Running/Waiting 时的 turn
    std::string cwd_identity;   // 创建时的 cwd/worktree 身份;移房即 Pause
    std::uint64_t creation_seq = 0;  // 同刻到点时的稳定排序键

    nlohmann::json to_json() const;
    static LoopTask from_json(const nlohmann::json& j);  // 坏形状抛异常
};

// 一枚拍子(短账:task 是长账,每拍一次尝试,不拿 last_error 抹旧账)。
struct LoopTick {
    std::string task_id;
    std::string tick_id;       // <task_id>#<n>
    std::uint64_t tick_no = 0; // task 内第几拍(1 起)
    WallTimeMs scheduled_at_ms = 0;  // 原定时间轴上的点
    WallTimeMs dispatched_at_ms = 0; // 主泵真正取走的点(0 = 没取)
    WallTimeMs finished_at_ms = 0;   // 0 = 没收口
    std::string prompt_sha256;       // 本拍现读现算的 hash
    LoopPromptSource source = LoopPromptSource::Inline;
    std::string turn_id;             // 空 = 没开 turn
    LoopTickOutcome outcome = LoopTickOutcome::Pending;
    std::string error_code;          // outcome 非 succeeded 时给稳定码
    WallTimeMs next_due_at_ms = 0;   // 本拍收口后排的下一拍
    std::uint32_t missed_count = 0;  // 本拍合并掉的旧刻度数
    std::uint32_t attempts = 1;      // 同 tick 的尝试次数(退避重试 +1)

    nlohmann::json to_json() const;
    static LoopTick from_json(const nlohmann::json& j);
};

// ---------------------------------------------------------------------------
// 状态机转换表(纯函数,单测钉)
// ---------------------------------------------------------------------------

// from --event--> to 是否合法。event 是稳定字符串:"tick_due"/"tick_started"/
// "tick_finished"/"permission_wait"/"permission_resolved"/"pause"/"resume"/
// "stop"/"complete"/"expire"/"break"/"backoff"/"backoff_done"。
bool IsLoopTransitionAllowed(LoopTaskState from, const std::string& event, LoopTaskState to);

// event 名单(parser/scheduler/存档回放共用;未知 event 回放时跳过)。
std::vector<std::string> LoopTransitionEvents();

// ---------------------------------------------------------------------------
// interval 解析(纯函数)
// ---------------------------------------------------------------------------

// "5m"/"2h"/"1d" -> seconds。规则(单子"interval 语法"节):
//   - 一个正整数 + 单位,单位只认 m/h/d,大小写不敏感;不认秒。
//   - 0、负数、小数、溢出、连写 1h30m、无单位 -> nullopt(调用方报
//     kErrLoopIntervalInvalid)。
//   - 超过 7d -> nullopt(kErrLoopIntervalTooLarge 由调用方按场景给)。
//   - "5migrate" 这类普通词不当 interval(词边界:单位后必须到头)。
std::optional<std::chrono::seconds> ParseLoopInterval(const std::string& text);

// 一个 token 是不是 interval 形状(消歧用:"5migrate" 不是,"5m" 是)。
bool LooksLikeLoopInterval(const std::string& token);

// ---------------------------------------------------------------------------
// 下一拍时间轴(纯函数;missed tick 的"不追债"账)
// ---------------------------------------------------------------------------

// now >= next_due 时造一枚 due tick 的账:
//   missed = floor((now - next_due) / interval)
//   新 next_due = next_due + (missed+1) * interval(原时间轴上第一个
//   future slot;跳出去的刻度不补跑,只记 missed_count)
struct DueComputation {
    std::uint32_t missed = 0;       // 合并掉的旧刻度数
    WallTimeMs next_due_at_ms = 0;  // 推进后的下一拍
};
DueComputation ComputeDueAdvance(WallTimeMs next_due_at_ms, WallTimeMs now_ms,
                                 std::chrono::seconds interval);

// pause 后 resume:从 now + interval 起,不补旧账。
WallTimeMs ComputeResumeNextDue(WallTimeMs now_ms, std::chrono::seconds interval);

// expiry 边界:expires_at 恰等于 now 也算过(单子"expiry 边界恰等于 now")。
bool LoopExpired(WallTimeMs expires_at_ms, WallTimeMs now_ms);

}  // namespace lubancode::runtime::loop
