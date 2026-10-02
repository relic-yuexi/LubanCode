#pragma once
#include <csignal>
#include <cstddef>
#include <filesystem>

namespace lubancode::job_runner {
int Serve(const std::filesystem::path& state_root, std::size_t max_running,
          std::size_t max_records, volatile std::sig_atomic_t& stop);
}
