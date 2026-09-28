#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "fake_http_server.hpp"
#include "lubancore/core.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <csignal>
#include <pthread.h>
#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;
namespace sdk = lubancore;
namespace fs = std::filesystem;
namespace platform = lubancode::platform;

struct HostFixture {
    fs::path root = fs::temp_directory_path() / ("sdk-host-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    HostFixture() { fs::create_directories(root / "cwd"); fs::create_directories(root / "resources"); }
    ~HostFixture() { std::error_code ec; fs::remove_all(root, ec); }
};

int Descriptor(FILE* stream) {
#ifdef _WIN32
    return _fileno(stream);
#else
    return fileno(stream);
#endif
}
int Duplicate(int fd) {
#ifdef _WIN32
    return _dup(fd);
#else
    return dup(fd);
#endif
}
int ReplaceDescriptor(int source, int target) {
#ifdef _WIN32
    return _dup2(source, target);
#else
    return dup2(source, target);
#endif
}
void CloseDescriptor(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

// Capture at the C descriptor boundary: iostream-only redirection would miss
// the former fprintf(stderr) fallback inside the installed SDK library.
class CaptureStream {
public:
    CaptureStream(FILE* stream, const fs::path& path) : stream_(stream) {
#ifdef _WIN32
        captured_ = _wfopen(path.c_str(), L"w+b");
#else
        captured_ = std::fopen(path.c_str(), "w+b");
#endif
        if (!captured_) throw std::runtime_error("cannot create stream capture");
        std::fflush(stream_);
        saved_ = Duplicate(Descriptor(stream_));
        if (saved_ < 0 || ReplaceDescriptor(Descriptor(captured_), Descriptor(stream_)) < 0) {
            if (saved_ >= 0) CloseDescriptor(saved_);
            std::fclose(captured_);
            throw std::runtime_error("cannot redirect stream capture");
        }
    }
    ~CaptureStream() { Restore(); std::fclose(captured_); }
    std::string Finish() {
        Restore();
        std::rewind(captured_);
        std::string result;
        char bytes[1024];
        while (const auto count = std::fread(bytes, 1, sizeof(bytes), captured_)) result.append(bytes, count);
        return result;
    }
private:
    void Restore() {
        if (saved_ < 0) return;
        std::fflush(stream_);
        (void)ReplaceDescriptor(saved_, Descriptor(stream_));
        CloseDescriptor(saved_);
        saved_ = -1;
    }
    FILE* stream_;
    FILE* captured_ = nullptr;
    int saved_ = -1;
};
} // namespace

TEST_CASE("SDK host boundary: provider warning preserves results without writing host streams") {
    HostFixture fixture;
    const auto original_cwd = fs::current_path();
    const auto* locale = std::setlocale(LC_ALL, nullptr);
    const std::string original_locale = locale ? locale : "";
    lubancode::test_support::FakeHttpServer server;
    lubancode::test_support::FakeHttpResponse response;
    response.headers = {{"Content-Type", "text/event-stream"}};
    // The inconsistent provider cache counters deliberately exercise the real
    // Chat backend's LogSink::Warn path, not a fake backend or test-only log call.
    response.body =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"quiet answer\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2,"
        "\"prompt_cache_hit_tokens\":7,\"prompt_cache_miss_tokens\":8}}\n\n"
        "data: [DONE]\n\n";
    server.Enqueue(std::move(response));
    CaptureStream captured_out(stdout, fixture.root / "stdout.log");
    CaptureStream captured_err(stderr, fixture.root / "stderr.log");
    auto runtime = sdk::Runtime::Create({platform::PathToUtf8(fixture.root / "data"),
                                         platform::PathToUtf8(fixture.root / "resources")});
    REQUIRE(runtime.has_value());
    sdk::SessionOptions options;
    options.cwd = platform::PathToUtf8(fixture.root / "cwd");
    options.model = "fixture";
    sdk::Connection connection;
    connection.wire = sdk::Wire::ChatCompletions;
    connection.base_url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1";
    connection.api_key = "FAKE_HOST_BOUNDARY_KEY";
    options.connection = std::move(connection);
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    auto receipt = (*session)->Submit("warning-key", "question");
    REQUIRE(receipt.has_value());
    auto result = (*session)->WaitResult(receipt->operation_id, 15s);
    REQUIRE(result.has_value());
    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    const auto diagnostic_bytes = captured_err.Finish();
    const auto output_bytes = captured_out.Finish();
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(result->final_text == "quiet answer");
    CHECK(result->result_persisted);
    CHECK(server.requests().size() == 1);
    CHECK(diagnostic_bytes.empty());
    CHECK(output_bytes.empty());
    CHECK(fs::current_path() == original_cwd);
    const auto* current_locale = std::setlocale(LC_ALL, nullptr);
    CHECK(std::string(current_locale ? current_locale : "") == original_locale);
}

#ifndef _WIN32
namespace {
volatile std::sig_atomic_t host_sigpipe_count = 0;
void HostSigpipeHandler(int) { host_sigpipe_count = 1; }
class HostSignalState {
public:
    HostSignalState() {
        sigemptyset(&pipe_);
        sigaddset(&pipe_, SIGPIPE);
        REQUIRE(sigaction(SIGPIPE, nullptr, &previous_action_) == 0);
        REQUIRE(pthread_sigmask(SIG_SETMASK, nullptr, &previous_mask_) == 0);
        struct sigaction action {};
        action.sa_handler = HostSigpipeHandler;
        sigemptyset(&action.sa_mask);
        REQUIRE(sigaction(SIGPIPE, &action, nullptr) == 0);
        REQUIRE(pthread_sigmask(SIG_UNBLOCK, &pipe_, nullptr) == 0);
        host_sigpipe_count = 0;
    }
    ~HostSignalState() {
        (void)pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
        (void)sigaction(SIGPIPE, &previous_action_, nullptr);
    }
    const sigset_t& pipe() const { return pipe_; }
    void CheckHandler() const {
        struct sigaction current {};
        REQUIRE(sigaction(SIGPIPE, nullptr, &current) == 0);
        CHECK(current.sa_handler == HostSigpipeHandler);
        CHECK(host_sigpipe_count == 0);
    }
private:
    struct sigaction previous_action_ {};
    sigset_t previous_mask_ {};
    sigset_t pipe_ {};
};
} // namespace

TEST_CASE("SDK host boundary: child pipe failure preserves host SIGPIPE state") {
    HostSignalState host;
    // Darwin's old process-directed SIGPIPE can arrive on this other unblocked
    // thread. A current-thread-only mask cannot satisfy this regression.
    std::jthread unrelated([](std::stop_token stop) {
        while (!stop.stop_requested()) std::this_thread::sleep_for(1ms);
    });
    std::mutex mutex;
    std::condition_variable ready;
    std::string child_output;
    platform::ChildProcess child;
    const auto started = child.Start("/bin/sh", {"-c", "exec 0<&-; printf ready; sleep 10"}, {},
        [&](std::string_view bytes) {
            std::lock_guard lock(mutex);
            child_output.append(bytes);
            ready.notify_all();
            return true;
        }, [](std::string_view) {});
    REQUIRE(started.success);
    {
        std::unique_lock lock(mutex);
        REQUIRE(ready.wait_for(lock, 5s, [&] { return child_output == "ready"; }));
    }
    bool preexisting = false;
    SUBCASE("unblocked host") {}
    SUBCASE("blocked host with preexisting pending signal") {
        REQUIRE(pthread_sigmask(SIG_BLOCK, &host.pipe(), nullptr) == 0);
        REQUIRE(pthread_kill(pthread_self(), SIGPIPE) == 0);
        preexisting = true;
    }
    CHECK_FALSE(child.Write("broken pipe"));
    sigset_t pending;
    sigset_t mask;
    REQUIRE(sigpending(&pending) == 0);
    REQUIRE(pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0);
    CHECK((sigismember(&mask, SIGPIPE) == 1) == preexisting);
    CHECK((sigismember(&pending, SIGPIPE) == 1) == preexisting);
    if (preexisting && sigismember(&pending, SIGPIPE) == 1) {
        int caught = 0;
        REQUIRE(sigwait(&host.pipe(), &caught) == 0);
        CHECK(caught == SIGPIPE);
    }
    child.Shutdown(0);
    host.CheckHandler();
}

TEST_CASE("SDK host boundary: stdin pump and spawn preserve host and child signal policy") {
    HostSignalState host;
    const auto pumped = platform::RunProcessWithStdin(
        {"/bin/sh", "-c", "exec 0<&-; printf closed"}, std::string(1024 * 1024, 'x'), 5000);
    CHECK_FALSE(pumped.spawn_failed);
    CHECK_FALSE(pumped.timed_out);
    CHECK(pumped.stdout_bytes == "closed");
    host.CheckHandler();
    // exec resets a caught handler to default. An inherited global SIG_IGN or
    // leaked blocked mask would wrongly let this child print the marker.
    const auto child = platform::RunProcess({"/bin/sh", "-c", "kill -PIPE $$; printf leaked"}, 5000);
    CHECK_FALSE(child.spawn_failed);
    CHECK_FALSE(child.timed_out);
    CHECK(child.output.empty());
    CHECK(child.exit_code != 0);
    host.CheckHandler();
}
#endif
