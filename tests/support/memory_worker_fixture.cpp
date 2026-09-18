#include <chrono>
#include <thread>
#include <string_view>

#include "memory/project_memory.hpp"
#include "platform/paths.hpp"

// Delay real queue consumption until LaunchWorker has returned. Dropping the
// Windows Job handle on return kills us before any entry can reach disk.
int main(int argc, char** argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--memory-worker") return 2;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    return lubancode::memory::RunPendingMemoryJobs(
        lubancode::platform::Utf8ToPath(argv[2])).has_value() ? 0 : 1;
}
