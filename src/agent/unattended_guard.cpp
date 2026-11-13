// unattended_guard.hpp 的实现:墙钟预算的档位账与收工前催验证的判据。

#include "agent/unattended_guard.hpp"

#include <utility>
#include <variant>

#include "agent/agent.hpp"
#include "tools/todo_tool.hpp"

namespace lubancode::agent {

bool UnattendedPrefersChinese(const std::string& language) {
    return language.empty() || language.rfind("zh", 0) == 0;
}

namespace {

// 剩余秒数取整给模型看(负数按 0 说)。
std::int64_t RemainingSecsForText(std::int64_t remaining_ms) {
    return remaining_ms <= 0 ? 0 : (remaining_ms + 999) / 1000;
}

}  // namespace

std::optional<std::string> NextWallBudgetNote(std::int64_t total_ms, std::int64_t remaining_ms,
                                              WallBudgetNoteState& state, bool chinese) {
    if (total_ms <= 0) {
        return std::nullopt;
    }
    // 比例线用整数比:remaining/total < 25% 等价 remaining*100 < total*25。
    const bool crossed_quarter = remaining_ms * 100 < total_ms * kWallBudgetLowPercent;
    const bool crossed_low = remaining_ms < kWallBudgetLowAbsoluteMs;
    const bool fire = (crossed_quarter && !state.quarter_sent) || (crossed_low && !state.low_sent);
    if (!fire) {
        return std::nullopt;
    }
    // 同一刻跨过的档一并记账:两档挨得近时只念一回。
    state.quarter_sent = state.quarter_sent || crossed_quarter;
    state.low_sent = state.low_sent || crossed_low;
    const std::string secs = std::to_string(RemainingSecsForText(remaining_ms));
    if (chinese) {
        return "[宿主附注] 墙钟预算只剩约 " + secs +
               " 秒。别再开新方向,赶紧收尾:用一条快命令实测结果(测试/产物文件/服务响应),然后简短交代。";
    }
    return "[host note] About " + secs +
           " seconds of wall-clock time remain. Stop exploring and wrap up now: run one quick command to "
           "verify the result (tests, output files, service response), then give a brief summary.";
}

bool WallBudgetShouldStopRequests(std::int64_t remaining_ms) {
    return remaining_ms <= kWallBudgetStopRequestsReserveMs;
}

WallClockBudget::WallClockBudget(Clock::time_point started, std::int64_t total_ms, bool chinese)
    : deadline_(started + std::chrono::milliseconds(total_ms)), total_ms_(total_ms), chinese_(chinese) {}

std::int64_t WallClockBudget::RemainingMsAt(Clock::time_point now) const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now).count();
}

std::optional<std::string> WallClockBudget::TakeToolResultNote() {
    const std::int64_t remaining = RemainingMs();
    std::lock_guard<std::mutex> lock(mutex_);
    return NextWallBudgetNote(total_ms_, remaining, note_state_, chinese_);
}

std::string WallBudgetPromptLine(int total_secs, bool chinese) {
    const std::string secs = std::to_string(total_secs);
    if (chinese) {
        return "- 这一场总共约有 " + secs +
               " 秒墙钟时间,到点进程会被直接杀掉。照这个预算排活:先做最要紧的,检查挑快的,长命令设好超时或放后台。";
    }
    return "- You have about " + secs +
           " seconds of wall-clock time in total; the process is killed when it runs out. Plan accordingly: do "
           "the essential work first, prefer fast checks, and give long commands a timeout or run them in the "
           "background.";
}

bool HasUnverifiedWrites(const std::vector<api::Message>& history) {
    bool pending_write = false;
    for (const api::Message& message : history) {
        if (message.role != api::Role::Assistant) {
            continue;
        }
        for (const api::ContentBlock& block : message.content) {
            const auto* use = std::get_if<api::ToolUseBlock>(&block);
            if (use == nullptr) {
                continue;
            }
            if (use->name == "write_file" || use->name == "edit_file") {
                pending_write = true;
            } else if (use->name == "run_command") {
                pending_write = false;
            }
        }
    }
    return pending_write;
}

bool HasOpenTodos(const tools::TodoListState& state) {
    for (const tools::TodoItem& item : state.items) {
        if (item.status != tools::TodoStatus::Completed) {
            return true;
        }
    }
    return false;
}

bool ShouldNudgeVerification(const VerifyNudgeFacts& facts) {
    if (facts.nudges_sent >= kVerifyNudgeMaxCount) {
        return false;
    }
    if (facts.remaining_ms.has_value() && *facts.remaining_ms < kVerifyNudgeMinRemainingMs) {
        return false;
    }
    return facts.unverified_writes || facts.open_todos;
}

std::string VerificationNudgeText(bool chinese) {
    if (chinese) {
        return "收工之前:跑一条命令实测结果真能用(测试、文件在不在、服务能不能响应)。有没做完的就接着做。"
               "都验过了,就简短交代最终结果。";
    }
    return "Before you finish: run a command to verify the result actually works (tests, file exists, service "
           "responds). If something is unfinished, continue. If everything is verified, reply with a brief final "
           "summary.";
}

ContinuationSource BuildVerifyNudgeContinuation(VerifyNudgeOptions options) {
    auto nudges_sent = std::make_shared<int>(0);
    return [options = std::move(options), nudges_sent]() -> std::optional<ContinuationBatch> {
        VerifyNudgeFacts facts;
        facts.unverified_writes = options.agent != nullptr && HasUnverifiedWrites(options.agent->history());
        facts.open_todos = options.todo_state != nullptr && HasOpenTodos(*options.todo_state);
        facts.nudges_sent = *nudges_sent;
        if (options.wall_budget != nullptr) {
            facts.remaining_ms = options.wall_budget->RemainingMs();
        }
        if (!ShouldNudgeVerification(facts)) {
            return std::nullopt;  // 封账:验过了/没写过/催满了/时间不够
        }
        ++*nudges_sent;
        if (options.on_nudge) {
            options.on_nudge(*nudges_sent);
        }
        ContinuationBatch batch;
        batch.input = VerificationNudgeText(options.chinese);
        // 领了批的那轮失败/被打断:这一回不算催过(没送到),退回计数。
        batch.restore = [nudges_sent]() {
            if (*nudges_sent > 0) {
                --*nudges_sent;
            }
        };
        return batch;
    };
}

}  // namespace lubancode::agent
