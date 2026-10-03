#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "job_runner/common.hpp"
#include "job_runner/process.hpp"
#include "platform/paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace {
namespace fs = std::filesystem;
using lubancode::job_runner::Process;

struct OwnedDirectory {
    fs::path path = fs::temp_directory_path() /
        ("luban-runner-release-" + lubancode::job_runner::RandomHex());
    OwnedDirectory() { fs::create_directories(path); }
    ~OwnedDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};

std::optional<std::int64_t> WaitOwned(Process& process) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto result = process.Poll()) return result;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    return std::nullopt;
}

#ifndef _WIN32
struct ScopedSigpipe {
    using Handler = void (*)(int);
    Handler previous = std::signal(SIGPIPE, SIG_IGN);
    ~ScopedSigpipe() { if (previous != SIG_ERR) std::signal(SIGPIPE, previous); }
};
struct ScopedDiagnostic {
    std::ostringstream bytes;
    std::streambuf* previous = std::cerr.rdbuf(bytes.rdbuf());
    ~ScopedDiagnostic() { std::cerr.rdbuf(previous); }
};
#endif
}  // namespace

TEST_CASE("Runner Release retains a real native result without changing its launch error") {
    OwnedDirectory directory;
    const auto cwd = lubancode::platform::PathToUtf8(directory.path);
#ifdef _WIN32
    wchar_t system[MAX_PATH + 1]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH + 1);
    REQUIRE(length > 0);
    REQUIRE(length < MAX_PATH + 1);
    const auto executable = lubancode::platform::PathToUtf8(fs::path(system) / "cmd.exe");
    nlohmann::json spec = {{"argv", {executable, "/c", "exit", "0"}}, {"cwd", cwd}};
    auto process = Process::Create(spec, directory.path, {});
    REQUIRE(process->pid() > 0);
    REQUIRE_NOTHROW(process->Release());
    const auto result = WaitOwned(*process);
    REQUIRE(result.has_value());
    CHECK(*result == 0);
    CHECK(process->Poll() == result);
    process.reset();  // Close the owned process and Job Object before its files.
    std::cout << "[runner-release-path] windows-normal-release\n";
#else
    // serve ignores SIGPIPE before any Release; this direct production Process
    // test follows that same signal disposition and restores it on every exit.
    ScopedSigpipe sigpipe;
    REQUIRE(sigpipe.previous != SIG_ERR);
    nlohmann::json spec = {{"argv", {"/usr/bin/true"}}, {"cwd", cwd}};
    auto process = Process::Create(spec, directory.path, {});
    REQUIRE(process->pid() > 0);
    REQUIRE_NOTHROW(process->Cancel());
    const auto result = WaitOwned(*process);
    REQUIRE(result.has_value());
    REQUIRE(*result == 128 + SIGKILL);
    CHECK(process->pid() == 0);  // Poll actually reaped this child; no pipe reader remains.
    std::string code;
    std::string diagnostic;
    {
        ScopedDiagnostic capture;
        try { process->Release(); }
        catch (const lubancode::job_runner::Error& error) { code = error.what(); }
        diagnostic = capture.bytes.str();
    }
    REQUIRE(code == "runner.launch_failed");
    REQUIRE(diagnostic == "runner.release_failed stage=release syscall=write count=-1 errno=" +
                          std::to_string(EPIPE) + "\n");
    CHECK(process->Poll() == result);
    process.reset();
    std::cout << "[runner-release-path] posix-owned-gate-rejection\n";
#endif
}
