// 无人值守护栏(one-shot 墙钟预算与收工前催验证单):Terminal-Bench 一类
// 评测场每题有墙钟死线,超时宿主进程被直接杀掉,--output 导出也来不及做。
// 这里收两件事,全是 engine 层的纯逻辑,零终端依赖:
//   - WallClockBudget:整场墙钟预算(秒)。工具结果回喂时按档追一行宿主
//     附注(剩余 < 25% 一档、< 120 秒一档,每档一次);步顶查"该不该再发
//     请求"(剩余不足保留余量即优雅收工,按 hit_time_budget 交账);工具
//     执行上下文拿它的 deadline 给 run_command 封顶单次超时。
//   - 收工前催验证:模型头一回不带工具调用就收口时,若写过文件之后没再
//     跑过命令、或 todo 尚有未完项,宿主经 DriveTurn 的续投源再递一条
//     user 消息,要求实测。整场至多两回,预算不足 60 秒不催。
// 装配归 one_shot(只在单发生效);交互会话不填,一处不调。
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "agent/turn_harness.hpp"  // ContinuationSource:催验证接 DriveTurn 的续投源
#include "api/types.hpp"

namespace lubancode::tools {
struct TodoListState;
}  // namespace lubancode::tools

namespace lubancode::agent {

// ---- 阈值(毫秒;单测钉值) ----
inline constexpr std::int64_t kWallBudgetStopRequestsReserveMs = 20000;  // 剩这么多就不再发新请求
inline constexpr std::int64_t kWallBudgetHardCancelReserveMs = 10000;    // 看门狗掐在飞请求的点
inline constexpr std::int64_t kWallBudgetLowAbsoluteMs = 120000;         // 绝对低线:剩 2 分钟
inline constexpr int kWallBudgetLowPercent = 25;                          // 比例低线:剩 25%
inline constexpr std::int64_t kVerifyNudgeMinRemainingMs = 60000;        // 剩不足 1 分钟不催验证
inline constexpr int kVerifyNudgeMaxCount = 2;                            // 整场至多催两回

// 语言档:zh 前缀或空(未定稿)走中文,其余英文(与无人值守段同口径)。
bool UnattendedPrefersChinese(const std::string& language);

// 附注档位账:两档各发一次。
struct WallBudgetNoteState {
    bool quarter_sent = false;  // 剩余 < 25% 那档
    bool low_sent = false;      // 剩余 < 120 秒那档
};

// 纯函数:此刻该不该在工具结果尾巴追附注。两档谁先跨谁先发;同一刻
// 跨过两档只发一条、两档一并记账(不连刷两行)。total_ms <= 0 = 没预算,
// 恒不发。命中即改 state,返回附注正文(带 "[宿主附注]"/"[host note]" 头)。
std::optional<std::string> NextWallBudgetNote(std::int64_t total_ms, std::int64_t remaining_ms,
                                              WallBudgetNoteState& state, bool chinese);

// 纯函数:剩余毫秒是否已到"不再发新请求"的保留线(<= 20 秒)。
bool WallBudgetShouldStopRequests(std::int64_t remaining_ms);

// 整场墙钟预算。起算点由装配层给(进程起跑那刻),剩余按 steady 钟现算。
// 附注账有锁:工具收口在主线程,但看门狗/续投源可能在别处读剩余——读
// 是无锁的(只读常量 + 现取钟),写附注账才进锁。
class WallClockBudget {
public:
    using Clock = std::chrono::steady_clock;

    WallClockBudget(Clock::time_point started, std::int64_t total_ms, bool chinese);

    std::int64_t total_ms() const { return total_ms_; }
    bool chinese() const { return chinese_; }
    Clock::time_point deadline() const { return deadline_; }
    // 剩余毫秒(可为负:已过死线)。
    std::int64_t RemainingMs() const { return RemainingMsAt(Clock::now()); }
    std::int64_t RemainingMsAt(Clock::time_point now) const;
    // 步顶闸:剩余不足保留余量即真。
    bool ShouldStopRequests() const { return WallBudgetShouldStopRequests(RemainingMs()); }
    // 工具结果回喂口:按档取附注(每档一次)。
    std::optional<std::string> TakeToolResultNote();

private:
    Clock::time_point deadline_;
    std::int64_t total_ms_ = 0;
    bool chinese_ = true;
    std::mutex mutex_;
    WallBudgetNoteState note_state_;
};

// 无人值守段里告诉模型总预算的那一句(中英两份)。
std::string WallBudgetPromptLine(int total_secs, bool chinese);

// ---- 收工前催验证 ----

// 纯函数:history 里按序扫工具调用——最后一次 write_file/edit_file 之后
// 没再跑过 run_command 即真(写了没验)。没写过 = 假。
bool HasUnverifiedWrites(const std::vector<api::Message>& history);

// 纯函数:todo 板上还有未完成项(pending/in_progress)。
bool HasOpenTodos(const tools::TodoListState& state);

// 催验证的判据材料。
struct VerifyNudgeFacts {
    bool unverified_writes = false;
    bool open_todos = false;
    int nudges_sent = 0;                       // 本场已催几回
    std::optional<std::int64_t> remaining_ms;  // 没预算 = nullopt(不按时间拦)
};

// 纯函数:催不催。已催满两回不催;有预算且剩余 < 60 秒不催;否则写了
// 没验或 todo 未完即催。
bool ShouldNudgeVerification(const VerifyNudgeFacts& facts);

// 续跑消息正文(中英两份)。
std::string VerificationNudgeText(bool chinese);

// 催验证续投源的装配材料(one_shot 填)。
struct VerifyNudgeOptions {
    const Agent* agent = nullptr;                       // 读 history 判"写了没验"
    std::shared_ptr<tools::TodoListState> todo_state;   // 空 = 没 todo 板
    const WallClockBudget* wall_budget = nullptr;       // 空 = 没预算
    bool chinese = true;
    // 报信口:每催一回调一次(nudge_index 从 1 起),宿主打一行 stderr。可空。
    std::function<void(int nudge_index)> on_nudge;
};

// 造一只续投源:DriveTurn 每轮正常收口(模型不带工具调用就停了)后问它,
// 判据过了就交一批续跑消息再跑一轮,没过就封账。催的次数记在闭包里,
// 整场至多两回。
ContinuationSource BuildVerifyNudgeContinuation(VerifyNudgeOptions options);

}  // namespace lubancode::agent
