// Private, standard-library-only process fixture. Never installed or linked
// into a library. It emits exactly the requested ASCII capture bytes.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <iostream>

namespace fs = std::filesystem;

namespace {
bool Write(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) return false;
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    return stream.good();
}
}

int main(int argc, char** argv) {
    if (argc != 8) return 20;
    const fs::path started = fs::u8path(argv[1]);
    const fs::path done = fs::u8path(argv[2]);
    std::uint64_t bytes = 0, delay_ms = 0;
    try {
        bytes = std::stoull(argv[3]);
        delay_ms = std::stoull(argv[4]);
    } catch (...) { return 21; }
    if (bytes < 2 || bytes > 2097153 || delay_ms > 60000) return 22;
    const std::string tag = argv[5];
    const char fill = argv[6][0];
    if (tag.empty() || argv[6][0] == '\0' || argv[6][1] != '\0') return 23;
    const auto cwd = fs::current_path().generic_u8string();
    if (!Write(started, tag + "\n" + std::string(reinterpret_cast<const char*>(cwd.data()), cwd.size()))) return 24;
    if (std::string(argv[7]) != "-") {
        const fs::path release = fs::u8path(argv[7]);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!fs::is_regular_file(release)) {
            if (std::chrono::steady_clock::now() >= deadline) return 27;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    // A complete line survives cmd, PowerShell and POSIX shell forwarding.
    // The CRLF is part of the requested capture size, never an extra record.
    const std::string output(static_cast<std::size_t>(bytes - 2), fill);
    std::cout.write(output.data(), static_cast<std::streamsize>(output.size()));
#ifdef _WIN32
    // The Windows CRT's default text mode expands LF to CRLF. Supplying a
    // literal CRLF here would emit CRCRLF and invalidate the exact byte cap.
    std::cout.put('\n');
#else
    std::cout.write("\r\n", 2);
#endif
    std::cout.flush();
    if (!std::cout.good()) return 25;
    if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    // Overflow/timeout/cancel tests must stop the process before this point.
    return Write(done, tag) ? 0 : 26;
}
