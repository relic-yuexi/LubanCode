#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace lubancode::job_runner {
// Owns a new process before it may execute user code. The caller persists its
// starting identity before Release(). No method ever adopts a persisted PID.
class Process {
public:
    static std::unique_ptr<Process> Create(const nlohmann::json& spec,
                                         const std::filesystem::path& log_dir,
                                         const std::vector<std::string>& environment);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    std::uint64_t pid() const;
    void Release();
    void Cancel();
    std::optional<std::int64_t> Poll();

private:
    struct Impl;
    explicit Process(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}  // namespace lubancode::job_runner
