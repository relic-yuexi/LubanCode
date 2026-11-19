// Private shared diagnostics. No ledger, writer, model or observer borrow.
#pragma once
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lubancode::runtime {
class TrajectoryDiagnostics final {
public:
    void Append(std::string note) {
        std::lock_guard lock(mutex_);
        notes_.push_back(std::move(note));
    }
    // The old guarded 128-note insertion remains atomic with its size check.
    void AppendIfBelow(std::string note, std::size_t limit) {
        std::lock_guard lock(mutex_);
        if (notes_.size() < limit) notes_.push_back(std::move(note));
    }
    std::vector<std::string> Snapshot() const {
        std::lock_guard lock(mutex_);
        return notes_;
    }
private:
    mutable std::mutex mutex_;
    std::vector<std::string> notes_;
};
}  // namespace lubancode::runtime
