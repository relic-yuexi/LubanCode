// 重试退避实现(骨架拆解批五先行半批)。

#include "runtime/retry_backoff.hpp"

#include <algorithm>
#include <mutex>
#include <random>
#include <thread>

namespace lubancode::runtime {

namespace {

// 抖动:±25% 均匀抖。默认关(文件头说明:现状三家都没开,接线另立一批)。
std::int64_t ApplyJitter(std::int64_t wait_ms, bool enabled) {
    if (!enabled || wait_ms <= 0) {
        return wait_ms;
    }
    static std::mutex jitter_mutex;
    static std::mt19937_64 engine{std::random_device{}()};
    std::lock_guard<std::mutex> lock(jitter_mutex);
    const std::int64_t span = std::max<std::int64_t>(1, wait_ms / 4);
    std::uniform_int_distribution<std::int64_t> dist(-span, span);
    return std::max<std::int64_t>(0, wait_ms + dist(engine));
}

}  // namespace

std::optional<std::chrono::milliseconds> BackoffWaitMs(const BackoffPolicy& policy,
                                                       std::uint32_t attempt) {
    std::int64_t wait_ms = 0;
    switch (policy.kind) {
        case BackoffPolicy::Kind::Fixed:
            wait_ms = policy.initial_ms;
            break;
        case BackoffPolicy::Kind::Exponential: {
            std::int64_t factor = 1;
            // 2^(attempt-1) 按拍翻倍;溢出前先撞 max_ms 帽的场合常见,
            // 翻倍路上防一手指头(负数/溢出)。
            for (std::uint32_t i = 1; i < attempt; ++i) {
                if (factor > (std::int64_t{1} << 40)) break;  // ~1e12,帽必先到
                factor *= 2;
            }
            wait_ms = policy.initial_ms * factor;
            break;
        }
        case BackoffPolicy::Kind::Ladder:
            if (attempt < 1 || static_cast<std::size_t>(attempt) > policy.ladder_ms.size()) {
                return std::nullopt;  // 阶梯用完,没得再等
            }
            wait_ms = policy.ladder_ms[attempt - 1];
            break;
    }
    if (policy.max_ms > 0) {
        wait_ms = std::min(wait_ms, policy.max_ms);
    }
    return std::chrono::milliseconds(ApplyJitter(wait_ms, policy.jitter));
}

bool WaitBackoffCancellable(std::chrono::milliseconds wait, const std::atomic<bool>* cancel) {
    if (wait <= std::chrono::milliseconds::zero()) {
        return cancel == nullptr || !cancel->load();
    }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancel != nullptr && cancel->load()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cancel == nullptr || !cancel->load();
}

}  // namespace lubancode::runtime
