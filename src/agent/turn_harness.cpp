// turn_harness.hpp 的实现:取消链、收场分型、续投外环、Stop 续跑环四件,
// 全是纯装配逻辑,零终端依赖(engine 层,主回合与子代理共用)。

#include "agent/turn_harness.hpp"

#include "agent/agent.hpp"  // Agent(harness 只在头里前向声明,实现要完整类型)

#include <algorithm>
#include <chrono>
#include <utility>

namespace lubancode::agent {

// ---------------------------------------------------------------------------
// CancelChain
// ---------------------------------------------------------------------------

CancelChain::~CancelChain() {
    Stop();
}

void CancelChain::Add(const std::atomic<bool>* signal) {
    if (signal != nullptr) {
        signals_.push_back(signal);
    }
}

const std::atomic<bool>* CancelChain::Start() {
    if (result_ != nullptr || (result_ == nullptr && merger_.has_value())) {
        return result_;
    }
    if (signals_.empty()) {
        result_ = nullptr;
        return result_;
    }
    if (signals_.size() == 1) {
        // 单信号直通:不起线程,行为与从前的单指针透传一字不差。
        result_ = signals_.front();
        return result_;
    }
    merged_ptr_ = std::make_unique<std::atomic<bool>>(false);
    std::atomic<bool>* merged = merged_ptr_.get();
    merger_.emplace([merged, signals = signals_]() mutable {
        while (!merged->load(std::memory_order_acquire)) {
            for (const std::atomic<bool>* signal : signals) {
                if (signal != nullptr && signal->load(std::memory_order_acquire)) {
                    merged->store(true, std::memory_order_release);
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    result_ = merged;
    return result_;
}

void CancelChain::Stop() {
    if (merged_ptr_ != nullptr) {
        merged_ptr_->store(true, std::memory_order_release);  // 唤醒合并线程好 join
    }
    if (merger_.has_value() && merger_->joinable()) {
        merger_->join();
    }
    merger_.reset();
}

// ---------------------------------------------------------------------------
// 收场分型
// ---------------------------------------------------------------------------

const char* TurnVerdict::StatusTag(Status status) {
    switch (status) {
        case Status::Completed:
            return "completed";
        case Status::Stopped:
            return "stopped";
        case Status::BudgetExhausted:
            return "budget_exhausted";
        case Status::Failed:
            return "failed";
    }
    return "failed";
}

TurnVerdict ClassifyTurnEnd(const TurnEndgame& end) {
    TurnVerdict verdict;
    if (end.wall_clock) {
        // 墙钟超时:接口超时全失效的最后一道闸,压过"用户中止"(看门狗置的
        // 停止信号不是用户的手)。
        verdict.status = TurnVerdict::Status::Failed;
        verdict.reason = TurnVerdict::Reason::WallClockTimeout;
        return verdict;
    }
    if (end.cancelled) {
        verdict.status = TurnVerdict::Status::Stopped;
        verdict.reason = TurnVerdict::Reason::UserStop;
        return verdict;
    }
    if (end.hit_step_limit) {
        verdict.status = TurnVerdict::Status::BudgetExhausted;
        verdict.reason = TurnVerdict::Reason::StepLimit;
        return verdict;
    }
    if (end.time_budget_exhausted) {
        // 时间成本闸(P2-6):预算断线不是失败,是 budget_exhausted——部分
        // 结果照常带走,缘由写明是时间线断的。
        verdict.status = TurnVerdict::Status::BudgetExhausted;
        verdict.reason = TurnVerdict::Reason::TimeBudget;
        return verdict;
    }
    if (end.token_budget_exhausted) {
        verdict.status = TurnVerdict::Status::BudgetExhausted;
        verdict.reason = TurnVerdict::Reason::TokenBudget;
        return verdict;
    }
    if (!end.error.empty()) {
        verdict.status = TurnVerdict::Status::Failed;
        verdict.reason = end.error.find("上下文") != std::string::npos
                             ? TurnVerdict::Reason::MaxContext
                             : TurnVerdict::Reason::ApiError;
        return verdict;
    }
    if (end.history_empty) {
        verdict.status = TurnVerdict::Status::Failed;
        verdict.reason = TurnVerdict::Reason::ProtocolError;
        return verdict;
    }
    if (end.output_budget_exhausted) {
        verdict.status = TurnVerdict::Status::BudgetExhausted;
        verdict.reason = TurnVerdict::Reason::OutputBudget;
        return verdict;
    }
    if (end.require_final_text && end.final_text.empty()) {
        verdict.status = TurnVerdict::Status::Failed;
        verdict.reason = TurnVerdict::Reason::NoFinalText;
        return verdict;
    }
    verdict.status = TurnVerdict::Status::Completed;
    verdict.reason = TurnVerdict::Reason::None;
    return verdict;
}

// ---------------------------------------------------------------------------
// 续投外环
// ---------------------------------------------------------------------------

DriveReport DriveTurn(Agent& agent, const TurnWiring& wiring, api::Message input,
                      const DriveOptions& options) {
    DriveReport report;
    // 首轮吃完整 Message(可带图像附件);续投轮的输入是字符串(拼好的
    // inbox 增量),走 Run 的字符串重载。
    bool first_round = true;
    std::string run_input;
    // 已领出、尚未真正随一次模型请求发出的续投批:领了批的那轮若失败/被
    // 打断/撞限,按批退回未送("取走了不等于送到了")。
    std::optional<ContinuationBatch> inflight;
    const auto restore_inflight = [&inflight]() {
        if (inflight.has_value() && inflight->restore) {
            inflight->restore();
        }
        inflight.reset();
    };

    for (;;) {
        const auto outcome = first_round ? agent.Run(std::move(input), wiring, options.cancel)
                                         : agent.Run(run_input, wiring, options.cancel);
        first_round = false;
        if (!outcome.has_value()) {
            restore_inflight();
            report.ok = false;
            report.error = outcome.error();
            break;
        }
        report.steps_used += outcome->steps_used;
        report.output_budget = outcome->output_budget;
        report.stop_reason = outcome->stop_reason;
        report.final_round = *outcome;
        if (options.on_round_settled) {
            options.on_round_settled(*outcome);
        }
        if (outcome->cancelled) {
            // 打断不是错误:半截文本照常交,退批收场。
            restore_inflight();
            report.cancelled = true;
            break;
        }
        if (options.wall_clock_fired && options.wall_clock_fired()) {
            // 墙钟到点(看门狗置的停止信号把流掐断,loop 按打断收场):不是
            // 用户中止,是超时兜底——按 WallClockTimeout 分型。
            restore_inflight();
            report.wall_clock = true;
            break;
        }
        if (outcome->hit_step_limit) {
            restore_inflight();
            report.hit_step_limit = true;
            break;
        }
        if (outcome->hit_time_budget || outcome->hit_token_budget) {
            // 成本硬线(时间/token):与步数闸同款——退批收场,分型写明线别。
            restore_inflight();
            report.time_budget_exhausted = report.time_budget_exhausted || outcome->hit_time_budget;
            report.token_budget_exhausted = report.token_budget_exhausted || outcome->hit_token_budget;
            break;
        }
        inflight.reset();  // 上一批已随本轮请求真正送达,提交
        if (!options.continuation) {
            break;  // 没有续投源(主回合):单轮即收
        }
        std::optional<ContinuationBatch> drained = options.continuation();
        if (!drained.has_value()) {
            break;  // 封账,可进终态
        }
        // 有未送项:cancel 已置位就不必再起一轮(起了也立刻被打断),退回
        // 未送,让收尾账注列明。
        if (options.cancel != nullptr && options.cancel->load(std::memory_order_acquire)) {
            if (drained->restore) {
                drained->restore();
            }
            report.cancelled = true;
            break;
        }
        run_input = std::move(drained->input);
        inflight = std::move(drained);
    }
    return report;
}

// ---------------------------------------------------------------------------
// Stop 续跑环
// ---------------------------------------------------------------------------

void RunStopContinuation(Agent& agent, const TurnWiring& wiring, const StopOptions& options,
                         DriveReport& report) {
    if (!options.emit) {
        return;  // 没配 stop 钩子,整环跳过
    }
    bool stop_hook_active = false;
    for (int round = 0; round < 2; ++round) {
        const std::string last_text = options.final_text ? options.final_text() : std::string();
        const hooks::HookEventResult merged = options.emit(stop_hook_active, last_text);
        if (!merged.blocked || stop_hook_active) {
            break;  // 没人拉闸,或已经续过一次(不许无限续)
        }
        if (options.on_continue_request) {
            options.on_continue_request(merged.block_reason);
        }
        const auto continuation = agent.Run(options.label + merged.block_reason, wiring, options.cancel);
        if (!continuation.has_value() || continuation->cancelled || continuation->hit_step_limit) {
            break;  // 续跑轮报错/被打断/撞预算:如实停,不带病硬续
        }
        if (continuation->hit_time_budget || continuation->hit_token_budget) {
            // 成本硬线:续跑轮同样停——成本闸不因钩子续跑而豁免(P2-6)。
            report.time_budget_exhausted = report.time_budget_exhausted || continuation->hit_time_budget;
            report.token_budget_exhausted = report.token_budget_exhausted || continuation->hit_token_budget;
            break;
        }
        report.cancelled = report.cancelled || continuation->cancelled;
        report.steps_used += continuation->steps_used;
        report.output_budget = continuation->output_budget;
        if (options.on_round) {
            options.on_round(*continuation);
        }
        stop_hook_active = true;
    }
}

}  // namespace lubancode::agent
