#pragma once

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <string_view>
#include <thread>

#include "platform/atomic_write.hpp"

namespace lubancore::detail {

// Internal, lock-held new-session plan writes only. The budget bounds retry
// scheduling and waits; a native write/fsync/close can itself take longer.
template <class Write, class Now, class Wait>
std::expected<lubancode::platform::AtomicWriteReceipt, lubancode::platform::AtomicWriteError>
WriteFrozenPlanWithRetry(const std::filesystem::path& target, std::string_view bytes,
                         Write&& write, Now&& now, Wait&& wait) {
    using Clock = std::chrono::steady_clock;
    using namespace lubancode::platform;
    const auto deadline = now() + std::chrono::seconds(1);
    constexpr unsigned kMaxAttempts = 51;
    const Clock::duration interval = std::chrono::milliseconds(20);
    for (unsigned attempt = 1; ; ++attempt) {
        auto written = write(target, bytes, WriteDurability::ProcessCrashDurability);
        if (written || written.error().failure_kind != WriteFailureKind::TransientReject ||
            written.error().outcome != WriteOutcome::NotCommitted || attempt == kMaxAttempts)
            return written;
        const auto before_wait = now();
        if (before_wait >= deadline) return written;
        wait(std::min(interval, deadline - before_wait));
        // Never start another write after the budget has elapsed, even when the
        // wait overshoots or a native call consumed the remaining retry budget.
        if (now() >= deadline) return written;
    }
}

inline std::expected<lubancode::platform::AtomicWriteReceipt, lubancode::platform::AtomicWriteError>
WriteFrozenPlan(const std::filesystem::path& target, std::string_view bytes) {
    return WriteFrozenPlanWithRetry(target, bytes,
        [](const auto& path, std::string_view content, lubancode::platform::WriteDurability durability) {
            return lubancode::platform::AtomicWriteFile(path, content, durability);
        }, [] { return std::chrono::steady_clock::now(); },
        [](std::chrono::steady_clock::duration duration) { std::this_thread::sleep_for(duration); });
}

inline const char* PlanWriteOutcomeName(lubancode::platform::WriteOutcome outcome) {
    using lubancode::platform::WriteOutcome;
    switch (outcome) {
    case WriteOutcome::NotCommitted: return "NotCommitted";
    case WriteOutcome::CommittedDurabilityNotRequested: return "CommittedDurabilityNotRequested";
    case WriteOutcome::CommittedDurabilityUnconfirmed: return "CommittedDurabilityUnconfirmed";
    case WriteOutcome::CommittedDurable: return "CommittedDurable";
    }
    return "Unknown";
}

} // namespace lubancore::detail
