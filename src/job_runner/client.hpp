#pragma once

#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace lubancode::job_runner {
// The caller pins runner_id outside the Runner state directory. Never replace
// it automatically after state loss, and never replay an indeterminate start.
// This local mailbox client neither starts nor owns the Runner process.
nlohmann::json Request(const std::filesystem::path& state_root,
                       const std::string& expected_runner_id,
                       const nlohmann::json& body, int timeout_ms = 5000);
}  // namespace lubancode::job_runner
