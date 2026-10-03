#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "platform/paths.hpp"
#include "tools/background_tasks.hpp"
#include "tools/run_command.hpp"

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using lubancode::tools::CommandExecutionLimits;
using lubancode::tools::RunCommandTool;
using lubancode::tools::Tool;
using lubancode::tools::ToolExecutionContext;

struct Watchdog {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread thread;
    Watchdog() : thread([this] {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 90s, [this] { return done; })) std::abort();
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex); done = true; }
        cv.notify_all();
        thread.join();
    }
};

struct Directory {
    fs::path path;
    Directory() {
        static std::atomic<std::uint64_t> next{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 128; ++attempt) {
            const auto candidate = fs::temp_directory_path() /
                ("lubancode-command-limits-" + std::to_string(stamp) + "-" +
                 std::to_string(next.fetch_add(1)));
            std::error_code error;
            const bool created = fs::create_directory(candidate, error);
            if (created) { path = candidate; break; }
            REQUIRE_MESSAGE(!error, ("fresh test root: " + error.message()));
        }
        REQUIRE(!path.empty());
    }
    ~Directory() {
        std::error_code error;
        fs::remove_all(path, error);
        try { CHECK_MESSAGE(!error, ("owned test root cleanup: " + error.message())); }
        catch (...) {}
    }
};

std::string Utf8(const fs::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    std::string result{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(stream.bad());
    return result;
}
bool MatchesStarted(const std::string& text, const fs::path& cwd, const std::string& tag) {
    const std::string prefix = tag + "\n";
    if (!text.starts_with(prefix)) return false;
    const std::string actual = text.substr(prefix.size());
    if (actual.empty() || actual.find_first_of("\r\n") != std::string::npos ||
        actual.find('\0') != std::string::npos) return false;
    std::error_code error;
    return fs::equivalent(fs::u8path(actual), cwd, error) && !error;
}
bool WaitStarted(const fs::path& cwd, const std::string& tag,
                 std::chrono::milliseconds timeout = 10000ms) {
    const auto path = cwd / (tag + ".started");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (fs::is_regular_file(path, error) && !error) {
            std::ifstream stream(path, std::ios::binary);
            if (stream.is_open()) {
                const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
                if (!stream.bad() && MatchesStarted(text, cwd, tag)) return true;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}
std::array<std::string, 2> Shells() {
#ifdef _WIN32
    return {"cmd", "powershell"};
#else
    return {"sh", "bash"};
#endif
}
std::string Quote(const std::string& value, const std::string& shell) {
#ifdef _WIN32
    if (shell == "cmd") {
        REQUIRE(value.find_first_of("\"%\r\n") == std::string::npos);
        return "\"" + value + "\"";
    }
#endif
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') quoted += shell == "powershell" ? "''" : "'\\''";
        else quoted.push_back(ch);
    }
    return quoted + "'";
}

struct Probe {
    fs::path executable;
    explicit Probe(const fs::path& root) {
        const fs::path original = fs::u8path(LUBANCORE_TEST_COMMAND_LIMITS_PROBE);
        REQUIRE(fs::is_regular_file(original));
        executable = root / original.filename();
        REQUIRE(fs::copy_file(original, executable));
        fs::permissions(executable, fs::status(original).permissions());
    }
    nlohmann::json Input(const fs::path& cwd, const std::string& shell,
                         const std::string& tag, std::uint64_t bytes,
                         std::uint64_t delay_ms, char fill = 'A',
                         bool wait_for_release = false) const {
        // Arguments stay in the tested synchronous shell. No background shell,
        // pipeline or external launcher can escape the platform process tree.
        std::string command = shell == "powershell" ? "& " : "";
        command += Quote(Utf8(executable), shell);
        const std::array<std::string, 7> arguments = {tag + ".started", tag + ".done",
            std::to_string(bytes), std::to_string(delay_ms), tag, std::string(1, fill),
            wait_for_release ? tag + ".release" : "-"};
        for (const auto& argument : arguments) command += " " + Quote(argument, shell);
        return {{"command", command}, {"shell", shell}, {"cwd", Utf8(cwd)}};
    }
    void CheckReleased() {
        // Windows cannot delete this actual executable while its process still
        // owns the image. POSIX unlink is only cleanup, not a liveness proof.
        REQUIRE(fs::remove(executable));
        CHECK_FALSE(fs::exists(executable));
    }
};

ToolExecutionContext Context(std::uint64_t timeout_ms, std::uint64_t bytes,
                              const std::atomic<bool>* cancel = nullptr) {
    ToolExecutionContext context;
    context.cancel = cancel;
    context.command_limits = CommandExecutionLimits{timeout_ms, bytes};
    return context;
}
void Limits(const Tool::Result& result, std::uint64_t timeout_ms, std::uint64_t bytes) {
    REQUIRE(result.details.contains("timeout_ms"));
    REQUIRE(result.details.contains("max_output_bytes"));
    CHECK(result.details.at("timeout_ms").get<std::uint64_t>() == timeout_ms);
    CHECK(result.details.at("max_output_bytes").get<std::uint64_t>() == bytes);
    CHECK_FALSE(result.details.contains("captured_output_bytes"));
}
void Started(const fs::path& cwd, const std::string& tag) {
    REQUIRE(fs::is_regular_file(cwd / (tag + ".started")));
    const std::string text = Read(cwd / (tag + ".started"));
    INFO("actual started marker: " << text);
    CHECK(MatchesStarted(text, cwd, tag));
}

std::string DiagnosticHex(const std::string& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 15]);
    }
    return hex;
}
nlohmann::json DiagnosticMarker(const fs::path& path) {
    nlohmann::json record = {{"path", Utf8(path)}};
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !fs::exists(status))) {
        record["state"] = "absent";
        return record;
    }
    if (error) {
        record["state"] = "status_failed";
        record["error"] = error.message();
        return record;
    }
    if (!fs::is_regular_file(status)) {
        record["state"] = "not_regular";
        return record;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        record["state"] = "open_failed";
        return record;
    }
    const std::string bytes{std::istreambuf_iterator<char>(stream),
                            std::istreambuf_iterator<char>()};
    record["state"] = stream.bad() ? "read_failed" : "read";
    record["size_bytes"] = bytes.size();
    record["bytes_hex"] = DiagnosticHex(bytes);
    return record;
}
std::mutex& DiagnosticMutex() {
    static std::mutex mutex;
    return mutex;
}
void InvocationDiagnostic(const char* phase, const nlohmann::json& input,
                          const ToolExecutionContext* context, const fs::path& cwd,
                          const std::string& tag, std::chrono::milliseconds elapsed,
                          const Tool::Result* result = nullptr,
                          const char* exception = nullptr) noexcept {
    // The values and marker bytes belong to this actual call. Diagnostic reads
    // never replace Started/WaitStarted or any result assertion below.
    try {
        nlohmann::json record = {{"phase", phase}, {"shell", input.at("shell")},
            {"tag", tag}, {"input", input}, {"cwd", Utf8(cwd)},
            {"entry", context ? "explicit_context" : "legacy_direct"},
            {"elapsed_ms", elapsed.count()}, {"limits", nullptr},
            {"started", DiagnosticMarker(cwd / (tag + ".started"))},
            {"done", DiagnosticMarker(cwd / (tag + ".done"))}};
        if (context && context->command_limits) {
            record["limits"] = {{"timeout_ms", context->command_limits->timeout_ms},
                {"max_output_bytes", context->command_limits->max_output_bytes}};
        }
        if (context) {
            record["cancel_present"] = context->cancel != nullptr;
            if (context->cancel) record["cancel_requested"] = context->cancel->load();
        }
        if (result) {
            record["result"] = {{"is_error", result->is_error},
                {"outcome", result->outcome}, {"error_code", result->error_code},
                {"details", result->details}, {"content", result->content},
                {"content_size_bytes", result->content.size()},
                {"content_hex", DiagnosticHex(result->content)}};
        }
        if (exception) record["exception"] = exception;
        // Hex retains original bytes even if an error contains invalid UTF-8.
        const std::string line = record.dump(-1, ' ', true,
            nlohmann::json::error_handler_t::replace);
        std::lock_guard lock(DiagnosticMutex());
        std::fprintf(stderr, "[command-limits-invocation] %s\n", line.c_str());
        std::fflush(stderr);
    } catch (...) {
        // Diagnostics must not change the tool outcome or unwind its cleanup.
        try {
            std::lock_guard lock(DiagnosticMutex());
            std::fprintf(stderr, "[command-limits-invocation] diagnostic_unavailable phase=%s\n", phase);
            std::fflush(stderr);
        } catch (...) {}
    }
}
template <typename Call>
Tool::Result RecordedInvocation(const nlohmann::json& input,
                                const ToolExecutionContext* context,
                                const fs::path& cwd, const std::string& tag, Call call) {
    InvocationDiagnostic("before", input, context, cwd, tag, 0ms);
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    };
    try {
        auto result = call();
        InvocationDiagnostic("after", input, context, cwd, tag, elapsed(), &result);
        return result;
    } catch (const std::exception& error) {
        InvocationDiagnostic("threw", input, context, cwd, tag, elapsed(), nullptr, error.what());
        throw;
    } catch (...) {
        InvocationDiagnostic("threw", input, context, cwd, tag, elapsed(), nullptr, "non_std_exception");
        throw;
    }
}
Tool::Result ExecuteRecorded(RunCommandTool& tool, const nlohmann::json& input,
                             const ToolExecutionContext& context, const fs::path& cwd,
                             const std::string& tag) {
    return RecordedInvocation(input, &context, cwd, tag,
        [&] { return tool.execute(input, context); });
}
Tool::Result ExecuteRecorded(RunCommandTool& tool, const nlohmann::json& input,
                             const fs::path& cwd, const std::string& tag) {
    return RecordedInvocation(input, nullptr, cwd, tag,
        [&] { return tool.execute(input); });
}
void Mark(const char* path) {
    std::printf("[command-limits-path] %s\n", path);
    std::fflush(stdout);
}
struct FinishCalls {
    std::vector<std::atomic<bool>*> cancels;
    std::vector<std::future<Tool::Result>*> futures;
    std::vector<fs::path> releases;
    ~FinishCalls() {
        for (const auto& path : releases) {
            // Failure cleanup opens every owned gate before joining workers.
            try { std::ofstream stream(path, std::ios::binary); stream.put('1'); }
            catch (...) {}
        }
        for (auto* cancel : cancels) cancel->store(true, std::memory_order_release);
        for (auto* future : futures) if (future->valid()) future->wait();
    }
};
}

TEST_CASE("RunCommand execution limits: exact ASCII cap and one-byte overflow use both shells") {
    Watchdog watchdog;
    Directory directory;
    Probe probe(directory.path);
    RunCommandTool tool;
    const auto schema = tool.input_schema();
    for (const auto& shell : Shells()) {
        const std::string exact = shell + "-exact";
        const auto exact_input = probe.Input(directory.path, shell, exact, 128, 0);
        const auto success = ExecuteRecorded(tool, exact_input, Context(15000, 128), directory.path, exact);
        REQUIRE_FALSE(success.is_error);
        CHECK(success.outcome == "succeeded");
        Limits(success, 15000, 128);
        Started(directory.path, exact);
        CHECK(Read(directory.path / (exact + ".done")) == exact);
        CHECK(success.content.ends_with(std::string(126, 'A') + "\r\n"));
        const std::string excess = shell + "-excess";
        const auto excess_input = probe.Input(directory.path, shell, excess, 129, 60000);
        const auto rejected = ExecuteRecorded(tool, excess_input, Context(15000, 128), directory.path, excess);
        REQUIRE(rejected.is_error);
        CHECK(rejected.outcome == "output_limit");
        CHECK(rejected.error_code == "process.output_limit");
        Limits(rejected, 15000, 128);
        Started(directory.path, excess);
        CHECK_FALSE(fs::exists(directory.path / (excess + ".done")));
        CHECK(rejected.content.find("run_in_background") == std::string::npos);
        const nlohmann::json evidence = {
            {"shell", shell}, {"cwd", exact_input.at("cwd")},
            {"exact_command", exact_input.at("command")}, {"excess_command", excess_input.at("command")},
            {"timeout_ms", success.details.at("timeout_ms")},
            {"max_output_bytes", success.details.at("max_output_bytes")},
            {"exact_request_bytes", 128}, {"excess_request_bytes", 129},
            {"exact_outcome", success.outcome}, {"excess_error_code", rejected.error_code}};
        std::printf("[command-limits-shell] %s\n", evidence.dump().c_str());
        std::fflush(stdout);
    }
    CHECK(tool.input_schema() == schema);
    probe.CheckReleased();
    Mark("boundary");
}

TEST_CASE("RunCommand execution limits: started host timeout and tighter model timeout") {
    Watchdog watchdog;
    Directory directory;
    Probe probe(directory.path);
    RunCommandTool tool;
    for (const auto& shell : Shells()) {
        for (const bool model_tightens : {false, true}) {
            const std::string tag = shell + (model_tightens ? "-model-timeout" : "-host-timeout");
            auto input = probe.Input(directory.path, shell, tag, 128, 60000);
            const std::uint64_t actual = model_tightens ? 5000 : 8000;
            input["timeout_ms"] = model_tightens ? 5000 : 20000;
            const auto result = ExecuteRecorded(tool, input, Context(8000, 1024), directory.path, tag);
            REQUIRE(result.is_error);
            CHECK(result.outcome == "timed_out");
            CHECK(result.error_code == "process.timeout");
            Limits(result, actual, 1024);
            Started(directory.path, tag); // Refuse a pre-start deadline as proof.
            CHECK_FALSE(fs::exists(directory.path / (tag + ".done")));
            CHECK(result.content.find("run_in_background") == std::string::npos);
        }
    }
    probe.CheckReleased();
    Mark("timeout");
}

TEST_CASE("RunCommand execution limits: actual started cancellation returns after process exit") {
    Watchdog watchdog;
    Directory directory;
    Probe probe(directory.path);
    RunCommandTool tool;
    for (const auto& shell : Shells()) {
        std::atomic<bool> cancel{false};
        const std::string tag = shell + "-cancel";
        const auto input = probe.Input(directory.path, shell, tag, 128, 60000);
        const auto context = Context(20000, 1024, &cancel);
        std::future<Tool::Result> future;
        FinishCalls cleanup{{&cancel}, {&future}};
        future = std::async(std::launch::async, [&] {
            return ExecuteRecorded(tool, input, context, directory.path, tag);
        });
        REQUIRE(WaitStarted(directory.path, tag));
        cancel.store(true, std::memory_order_release);
        REQUIRE(future.wait_for(10s) == std::future_status::ready);
        const auto result = future.get();
        REQUIRE(result.is_error);
        CHECK(result.outcome == "cancelled_during_run");
        Limits(result, 20000, 1024);
        Started(directory.path, tag);
        CHECK_FALSE(fs::exists(directory.path / (tag + ".done")));
    }
    probe.CheckReleased();
    Mark("cancel");
}

TEST_CASE("RunCommand execution limits: four concurrent contexts share one tool without image or cwd leakage") {
    Watchdog watchdog;
    Directory directory;
    Probe probe(directory.path);
    const std::array<fs::path, 4> cwds = {directory.path / "shared", directory.path / "shared",
        directory.path / "project-b", directory.path / "project-c"};
    REQUIRE(fs::create_directory(cwds[0]));
    REQUIRE(fs::create_directory(cwds[2]));
    REQUIRE(fs::create_directory(cwds[3]));
    const std::array<std::uint64_t, 4> caps = {128, 256, 512, 1024};
    const std::array<std::uint64_t, 4> timeouts = {12000, 15000, 18000, 21000};
    std::array<std::atomic<bool>, 4> cancels{};
    std::array<nlohmann::json, 4> inputs;
    std::array<ToolExecutionContext, 4> contexts;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const std::string tag = "context-" + std::to_string(i);
        inputs[i] = probe.Input(cwds[i], Shells()[i % 2], tag,
            caps[i] + (i == 0 ? 1 : 0), i == 0 ? 60000 : 150, 'A', true);
        contexts[i] = Context(timeouts[i], caps[i], &cancels[i]);
    }
    std::array<std::future<Tool::Result>, 4> futures;
    RunCommandTool tool;
    const auto schema = tool.input_schema();
    FinishCalls cleanup{{&cancels[0], &cancels[1], &cancels[2], &cancels[3]},
                        {&futures[0], &futures[1], &futures[2], &futures[3]},
                        {cwds[0] / "context-0.release", cwds[1] / "context-1.release",
                         cwds[2] / "context-2.release", cwds[3] / "context-3.release"}};
    for (std::size_t i = 0; i < futures.size(); ++i) {
        futures[i] = std::async(std::launch::async, [&, i] {
            return ExecuteRecorded(tool, inputs[i], contexts[i], cwds[i], "context-" + std::to_string(i));
        });
    }
    for (std::size_t i = 0; i < futures.size(); ++i) {
        REQUIRE(WaitStarted(cwds[i], "context-" + std::to_string(i)));
    }
    // All four actual processes are waiting together. An async launch alone
    // would not prove that the executions overlap.
    for (auto& future : futures) CHECK(future.wait_for(0ms) == std::future_status::timeout);
    for (const auto& path : cleanup.releases) {
        std::ofstream release(path, std::ios::binary);
        REQUIRE(release.is_open());
        release.put('1');
        release.flush();
        REQUIRE(release.good());
    }
    for (std::size_t i = 0; i < futures.size(); ++i) {
        REQUIRE(futures[i].wait_for(25s) == std::future_status::ready);
        const auto result = futures[i].get();
        Limits(result, timeouts[i], caps[i]);
        const std::string tag = "context-" + std::to_string(i);
        Started(cwds[i], tag);
        if (i == 0) {
            CHECK(result.is_error);
            CHECK(result.error_code == "process.output_limit");
            CHECK_FALSE(fs::exists(cwds[i] / (tag + ".done")));
        } else {
            REQUIRE_FALSE(result.is_error);
            CHECK(result.outcome == "succeeded");
            CHECK(Read(cwds[i] / (tag + ".done")) == tag);
            CHECK(result.content.ends_with(std::string(static_cast<std::size_t>(caps[i] - 2), 'A') + "\r\n"));
        }
        for (std::size_t foreign = 0; foreign < cwds.size(); ++foreign) {
            if (cwds[foreign] != cwds[i]) CHECK_FALSE(fs::exists(cwds[foreign] / (tag + ".started")));
        }
    }
    CHECK(tool.input_schema() == schema);
    probe.CheckReleased();
    Mark("four-contexts");
}

TEST_CASE("RunCommand execution limits: invalid images and background escape routes start nothing") {
    Watchdog watchdog;
    Directory directory;
    Probe probe(directory.path);
    RunCommandTool tool;
    const auto schema = tool.input_schema();
    const auto before = lubancode::tools::BackgroundTaskRegistry::Instance().List();
    const std::array<CommandExecutionLimits, 4> invalid = {
        CommandExecutionLimits{0, 128}, {86400001, 128}, {15000, 0}, {15000, 2097153}};
    for (std::size_t i = 0; i < invalid.size(); ++i) {
        const std::string tag = "invalid-" + std::to_string(i);
        auto context = Context(invalid[i].timeout_ms, invalid[i].max_output_bytes);
        const auto result = ExecuteRecorded(tool,
            probe.Input(directory.path, Shells()[i % 2], tag, 128, 0), context, directory.path, tag);
        REQUIRE(result.is_error);
        CHECK(result.error_code == "process.invalid_execution_limits");
        CHECK_FALSE(fs::exists(directory.path / (tag + ".started")));
    }
    for (const auto& shell : Shells()) {
        for (unsigned variant = 0; variant < 4; ++variant) {
            const std::string tag = shell + "-escape-" + std::to_string(variant);
            auto input = probe.Input(directory.path, shell, tag, 128, 0);
            if (variant == 0) input["run_in_background"] = true;
            else input["max_runtime_ms"] = variant == 1 ? nlohmann::json(nullptr) : nlohmann::json(variant - 2);
            const auto result = ExecuteRecorded(tool, input, Context(15000, 128), directory.path, tag);
            REQUIRE(result.is_error);
            CHECK(result.error_code == "process.execution_mode_rejected");
            CHECK_FALSE(fs::exists(directory.path / (tag + ".started")));
        }
    }
    const auto after = lubancode::tools::BackgroundTaskRegistry::Instance().List();
    REQUIRE(after.size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i) CHECK(after[i].task_id == before[i].task_id);
    CHECK(tool.input_schema() == schema);
    probe.CheckReleased();
    Mark("invalid-and-background");
}

TEST_CASE("RunCommand execution limits: absent image preserves legacy fields and both execute entries") {
    Watchdog watchdog;
    Directory directory;
    Probe probe(directory.path);
    RunCommandTool tool;
    for (const auto& shell : Shells()) {
        for (const bool explicit_context : {false, true}) {
            const std::string tag = shell + (explicit_context ? "-legacy-context" : "-legacy-direct");
            auto input = probe.Input(directory.path, shell, tag, 4096, 0);
            input["timeout_ms"] = 15000;
            input["run_in_background"] = false;
            input["max_runtime_ms"] = nullptr; // Legacy ignores this foreground field.
            const auto result = explicit_context
                ? ExecuteRecorded(tool, input, ToolExecutionContext{}, directory.path, tag)
                : ExecuteRecorded(tool, input, directory.path, tag);
            REQUIRE_FALSE(result.is_error);
            CHECK(result.outcome == "succeeded");
            CHECK(result.details.at("timeout_ms").get<int>() == 15000);
            CHECK_FALSE(result.details.contains("max_output_bytes"));
            CHECK_FALSE(result.details.contains("captured_output_bytes"));
            CHECK(result.content.ends_with(std::string(4094, 'A') + "\r\n"));
            Started(directory.path, tag);
            CHECK(Read(directory.path / (tag + ".done")) == tag);
        }
    }
#ifdef _WIN32
    // The new scoped pipeline must retain the old exit/error contract without
    // collecting native output until exit. The boundary case above keeps the
    // overflowing native process alive and requires output_limit before its
    // host deadline; these actual calls check the other wrapper semantics.
    struct ScriptCase { std::string command; int exit_code; std::string output; };
    const std::array<ScriptCase, 4> scripts = {{
        {"Write-Output 'scoped-wrapper-ok'", 0, "scoped-wrapper-ok"},
        {"Write-Error 'scoped-wrapper-error'", 1, "scoped-wrapper-error"},
        {"exit 7", 7, ""},
        {"Write-Error 'before-native'; & " + Quote(Utf8(probe.executable), "powershell") +
            " 'bad-argv'; Write-Output 'after-native'", 20, "after-native"}
    }};
    std::size_t script_index = 0;
    for (const auto& script : scripts) {
        INFO("actual scoped PowerShell command: " << script.command);
        const nlohmann::json input = {{"command", script.command}, {"shell", "powershell"},
            {"cwd", Utf8(directory.path)}};
        const auto result = ExecuteRecorded(tool, input, Context(15000, 4096),
            directory.path, "scoped-wrapper-" + std::to_string(script_index++));
        CHECK(result.is_error == (script.exit_code != 0));
        CHECK(result.outcome == (script.exit_code == 0 ? "succeeded" : "process_exit_nonzero"));
        REQUIRE(result.details.contains("exit_code"));
        CHECK(result.details.at("exit_code").get<int>() == script.exit_code);
        Limits(result, 15000, 4096);
        if (!script.output.empty()) CHECK(result.content.find(script.output) != std::string::npos);
        CHECK(result.content.find("#< CLIXML") == std::string::npos);
    }
#endif
    probe.CheckReleased();
    Mark("legacy");
}
