// Private remote-CI fixture. Never installed and never used by the consumer.
// Beside this executable, probe.mode controls its bounded behavior:
// blocking (default), wrong-version, or version-fail.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
long Pid() {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}
void Write(const fs::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << value;
    output.close();
    if (output.fail()) throw std::runtime_error("cannot publish probe marker");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--mcp-marker") {
            Write(fs::path(argv[2]), "MCP was started\n");
            return 17;
        }
        const auto directory = fs::absolute(fs::path(argv[0])).parent_path();
        std::string mode = "blocking";
        std::ifstream input(directory / "probe.mode", std::ios::binary);
        if (input) std::getline(input, mode);
        if (argc == 2 && std::string(argv[1]) == "--version") {
            if (mode == "version-fail") return 7;
            std::cout << "ripgrep " << (mode == "wrong-version" ? "14.1.0" : "15.2.0") << '\n';
            return 0;
        }
        Write(directory / "search.pid", std::to_string(Pid()) + "\n");
        std::cout << R"({"type":"match","data":{"path":{"text":"slow.txt"},"lines":{"text":"controlled search\n"},"line_number":1,"submatches":[]}})" << '\n' << std::flush;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            if (fs::exists(directory / "release")) return 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Write(directory / "natural-completion", "probe reached its bounded deadline\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 19;
    }
}
