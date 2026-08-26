// 重试退避(骨架拆解批五先行半批:外壳横切件抽公共件)。
//
// 三外壳各养各的"败了等多久再试"(总单病三):
//   - loop_scheduler:同 tick 两级阶梯(5s/15s,首发+两次重试用完);
//   - workflow/runtime:节点级 initial_ms * 2^(attempt-1),帽 max_ms
//     (RetryPolicy 里还声明了 jitter,运行时一直没启用);
//   - goal evaluator:初判+一次 schema 修复重问,无等待——那是对话式
//     修复,不是退避,不在这件里(批五回报有说明)。
// 机制件收前两种的公共形状:
//   - 阶梯种类:Fixed(每拍 initial_ms)/ Exponential(initial_ms *
//     2^(attempt-1),帽 max_ms)/ Ladder(逐拍显式表,越表即没得再等);
//   - 抖动档:装上了(±25% 均匀抖动),默认关。现状三家都没开抖动,
//     行为不变铁律下谁也不许顺手打开——workflow 的 RetryPolicy.jitter
//     继续只是"声明了未启用",接线开抖动另立一批。
//   - 等待:可被取消打断的 10ms 轮询等(workflow 现形状;loop 的等待在
//     装配层,只拿走查表结果)。
//
// 依赖铁律:只认标准库,零实现依赖;不 include loop/goal/workflow 头。

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace lubancode::runtime {

struct BackoffPolicy {
    enum class Kind {
        Fixed,        // 每拍都等 initial_ms
        Exponential,  // initial_ms * 2^(attempt-1),帽 max_ms
        Ladder,       // 逐拍的显式等待表(越表 = 没得再等)
    };

    Kind kind = Kind::Exponential;
    std::int64_t initial_ms = 0;  // Fixed/Exponential 的底
    std::int64_t max_ms = 0;      // 上限;0 = 不设帽
    std::vector<std::int64_t> ladder_ms;  // Ladder 的逐拍表(attempt 1 起对表)
    bool jitter = false;          // 抖动档(默认关;开了改节奏,见文件头)
};

// attempt(从 1 起数,指已经失败的那次)之后,下次重试该等多久。
// Ladder 越表给 nullopt——阶梯用完,没有再试的份(调用方按失败收口)。
// Fixed/Exponential 不设 attempt 上限(上限是重试额度的事,各家自带:
// loop 的 attempts>=3、workflow 的 attempt<max_attempts)。
std::optional<std::chrono::milliseconds> BackoffWaitMs(const BackoffPolicy& policy,
                                                       std::uint32_t attempt);

// 等完 wait(10ms 轮询,cancel 可打断,fake 短等待不赌时序)。返回 true =
// 等满;false = cancel 打断。wait <= 0 直接过。
bool WaitBackoffCancellable(std::chrono::milliseconds wait, const std::atomic<bool>* cancel);

}  // namespace lubancode::runtime
