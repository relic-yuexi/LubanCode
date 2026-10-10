#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "fake_http_server.hpp"
#include "lubancore/core.hpp"
#include "sdk/opening_test_hooks.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "runtime/session_service.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"
#include "workspace/index.hpp"

namespace {
using namespace std::chrono_literals;
namespace sdk = lubancore;
namespace fs = std::filesystem;
namespace platform = lubancode::platform;
using Json = nlohmann::json;

struct LifecycleFixture {
    fs::path root;
    LifecycleFixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("lubancore-lifecycle-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
    }
    ~LifecycleFixture() { std::error_code ec; fs::remove_all(root, ec); }
    sdk::RuntimeOptions Roots() const {
        return {platform::PathToUtf8(root / "data"), platform::PathToUtf8(root / "resources")};
    }
    fs::path SessionDir(const std::string& id) const {
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(root / "cwd", root / "data");
        REQUIRE(identity.has_value());
        auto workspace_dir = lubancode::workspace::index::ResolveDirByWorkspaceKey(
            root / "data" / "workspaces", identity->workspace_key);
        REQUIRE(workspace_dir.has_value());
        return *workspace_dir / "sessions" / id;
    }
    lubancode::runtime::SessionLaunchRequest RawLaunch() const {
        lubancode::runtime::SessionLaunchRequest launch;
        launch.cwd_utf8 = platform::PathToUtf8(root / "cwd");
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(root / "cwd", root / "data");
        REQUIRE(identity.has_value());
        launch.workspace_identity = *identity;
        launch.workspaces_root = root / "data" / "workspaces";
        launch.lubancode_version = "sdk-lifecycle-test";
        launch.v3_system_content = "SDK lifecycle fixture";
        return launch;
    }
};

using GenerateFunction = std::function<sdk::Result<sdk::ModelReply>(const sdk::ModelRequest&, sdk::Cancellation)>;
class CallbackBackend final : public sdk::Backend {
public:
    explicit CallbackBackend(GenerateFunction generate) : generate_(std::move(generate)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        return generate_(request, cancel);
    }
private:
    GenerateFunction generate_;
};
sdk::SessionOptions Options(const LifecycleFixture& fixture, GenerateFunction generate) {
    sdk::SessionOptions options;
    options.cwd = platform::PathToUtf8(fixture.root / "cwd");
    options.model = "fixture";
    options.system_prompt = "SDK lifecycle fixture";
    options.max_steps_per_turn = 4;
    options.backend = std::make_unique<CallbackBackend>(std::move(generate));
    return options;
}

struct LifetimeGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    std::atomic<bool> entered{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> returned{false};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> timed_out{false};

    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
};
class LifetimeBackend final : public sdk::Backend {
public:
    explicit LifetimeBackend(std::shared_ptr<LifetimeGate> gate) : gate_(std::move(gate)) {}
    ~LifetimeBackend() override { gate_->destroyed.store(true); }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation cancel) override {
        gate_->entered.store(true);
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        std::unique_lock lock(gate_->mutex);
        while (!cancel.requested() && !gate_->released && std::chrono::steady_clock::now() < deadline) {
            gate_->cv.wait_for(lock, 2ms);
        }
        gate_->cancelled.store(cancel.requested());
        // Keep the callback alive after cancellation, so a close that merely
        // signals its worker cannot pass the lifetime assertions.
        if (!gate_->cv.wait_until(lock, deadline, [&] { return gate_->released; })) {
            gate_->timed_out.store(true);
        }
        gate_->returned.store(true);
        return std::unexpected(sdk::Error{"fixture.cancelled", "released cooperative backend"});
    }
private:
    std::shared_ptr<LifetimeGate> gate_;
};
struct ReleaseLifetimeGate {
    std::shared_ptr<LifetimeGate> gate;
    ~ReleaseLifetimeGate() { gate->Release(); }
};
bool WaitForFlag(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
    return flag.load();
}

struct ScopedOpeningHook {
    sdk::detail::testing::OpeningStartHookHandle previous;
    explicit ScopedOpeningHook(sdk::detail::testing::OpeningStartHook hook)
        : previous(sdk::detail::testing::ReplaceOpeningStartHook(
              std::make_shared<const sdk::detail::testing::OpeningStartHook>(std::move(hook)))) {}
    ~ScopedOpeningHook() { sdk::detail::testing::ReplaceOpeningStartHook(std::move(previous)); }
};
struct OpeningOwners {
    LifetimeGate backend;
    bool hold_destructor = false;
    std::atomic<unsigned> model_calls{0};
    std::atomic<bool> capture_destroyed{false}, provider_destroyed{false};
};
class OpeningBackend final : public sdk::Backend {
public:
    explicit OpeningBackend(std::shared_ptr<OpeningOwners> owners) : owners_(std::move(owners)) {}
    ~OpeningBackend() override {
        owners_->backend.entered.store(true);
        if (owners_->hold_destructor) {
            std::unique_lock lock(owners_->backend.mutex);
            if (!owners_->backend.cv.wait_for(lock, 20s, [&] { return owners_->backend.released; }))
                owners_->backend.timed_out.store(true);
        }
        owners_->backend.destroyed.store(true);
    }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
        ++owners_->model_calls;
        return sdk::ModelReply{"worker started"};
    }
private:
    std::shared_ptr<OpeningOwners> owners_;
};
class OpeningSink final : public sdk::events::v1::EventSink {
public:
    explicit OpeningSink(std::shared_ptr<OpeningOwners> owners) : owners_(std::move(owners)) {}
    ~OpeningSink() override { owners_->provider_destroyed.store(true); }
    sdk::Result<std::unique_ptr<sdk::events::v1::EventQueue>> CreateQueue(std::size_t) override {
        return std::unexpected(sdk::Error{"fixture.unused", "no subscription before startup"});
    }
private:
    std::shared_ptr<OpeningOwners> owners_;
};
struct OpeningCapture {
    std::shared_ptr<OpeningOwners> owners;
    ~OpeningCapture() { owners->capture_destroyed.store(true); }
};
sdk::SessionOptions OpeningOptions(const LifecycleFixture& fixture, const std::shared_ptr<OpeningOwners>& owners) {
    auto options = Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{};
    });
    options.backend = std::make_unique<OpeningBackend>(owners);
    options.event_sink = std::make_unique<OpeningSink>(owners);
    auto capture = std::make_shared<OpeningCapture>();
    capture->owners = owners;
    sdk::Tool tool;
    tool.name = "startup_owner_probe";
    tool.execute = [capture](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        return sdk::ToolResult{"unused"};
    };
    options.custom_tools.push_back(std::move(tool));
    return options;
}
void CheckOpeningRetired(const std::shared_ptr<OpeningOwners>& owners) {
    CHECK(owners->model_calls.load() == 0);
    CHECK(owners->backend.destroyed.load());
    CHECK_FALSE(owners->backend.timed_out.load());
    CHECK(owners->capture_destroyed.load());
    CHECK(owners->provider_destroyed.load());
}

lubancode::test_support::FakeHttpResponse Sse(std::vector<std::string> frames) {
    lubancode::test_support::FakeHttpResponse response;
    response.headers.emplace_back("Content-Type", "text/event-stream");
    for (const auto& frame : frames) response.body += "data: " + frame + "\n\n";
    return response;
}
} // namespace

TEST_CASE("SDK lifecycle: thread startup failure retires owners and permits same ID recovery") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto seed = (*runtime)->OpenSession(Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{};
    }));
    REQUIRE(seed.has_value());
    const auto id = (*seed)->id();
    REQUIRE((*seed)->Close().has_value());
    seed->reset();
    bool nonstandard = false;
    SUBCASE("standard thread allocation failure") {}
    SUBCASE("nonstandard startup exception") { nonstandard = true; }
    auto owners = std::make_shared<OpeningOwners>();
    unsigned hook_calls = 0;
    {
        ScopedOpeningHook hook([&] {
            ++hook_calls;
            if (nonstandard) throw 17;
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        });
        auto options = OpeningOptions(fixture, owners);
        options.resume_session_id = id;
        const auto failed = (*runtime)->OpenSession(std::move(options));
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().code == "sdk.session.open_failed");
    }
    CHECK(hook_calls == 1);
    CheckOpeningRetired(owners);
    // Moving the real session directory also detects a retained Windows writer.
    const auto original = fixture.SessionDir(id);
    auto moved = original;
    moved += ".startup-retired";
    fs::rename(original, moved);
    fs::rename(moved, original);
    auto healthy_owners = std::make_shared<OpeningOwners>();
    auto options = OpeningOptions(fixture, healthy_owners);
    options.resume_session_id = id;
    auto healthy = (*runtime)->OpenSession(std::move(options));
    REQUIRE(healthy.has_value());
    CHECK((*healthy)->id() == id);
    const auto submitted = (*healthy)->Submit("after-start-failure", "continue");
    REQUIRE(submitted.has_value());
    const auto result = (*healthy)->WaitResult(submitted->operation_id, 15s);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(healthy_owners->model_calls.load() == 1);
    REQUIRE((*healthy)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    std::cout << "[sdk-opening-start-path] " << (nonstandard ? "nonstandard" : "standard") << '\n';
}

TEST_CASE("SDK lifecycle: repeated thread startup failures do not poison runtime admission") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    unsigned hook_calls = 0;
    {
        ScopedOpeningHook hook([&] { ++hook_calls; throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again)); });
        for (unsigned i = 0; i != 4; ++i) {
            auto owners = std::make_shared<OpeningOwners>();
            auto failed = (*runtime)->OpenSession(OpeningOptions(fixture, owners));
            REQUIRE_FALSE(failed.has_value());
            CHECK(failed.error().code == "sdk.session.open_failed");
            CheckOpeningRetired(owners);
        }
    }
    CHECK(hook_calls == 4);
    auto healthy = (*runtime)->OpenSession(OpeningOptions(fixture, std::make_shared<OpeningOwners>()));
    REQUIRE(healthy.has_value());
    REQUIRE((*healthy)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    fs::rename(fixture.root / "data", fixture.root / "retired-data");
    std::cout << "[sdk-opening-start-path] repeated\n";
}

TEST_CASE("SDK lifecycle: thread startup fault is isolated to its calling thread") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    std::atomic<unsigned> hook_calls{0};
    ScopedOpeningHook hook([&] { ++hook_calls; throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again)); });
    auto other = std::async(std::launch::async, [&] {
        return (*runtime)->OpenSession(OpeningOptions(fixture, std::make_shared<OpeningOwners>()));
    });
    REQUIRE(other.wait_for(10s) == std::future_status::ready);
    auto healthy = other.get();
    REQUIRE(healthy.has_value());
    CHECK(hook_calls == 0);
    REQUIRE((*healthy)->Close().has_value());
    auto owners = std::make_shared<OpeningOwners>();
    auto failed = (*runtime)->OpenSession(OpeningOptions(fixture, owners));
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == "sdk.session.open_failed");
    CHECK(hook_calls == 1);
    CheckOpeningRetired(owners);
    REQUIRE((*runtime)->Shutdown().has_value());
    std::cout << "[sdk-opening-start-path] isolation\n";
}

TEST_CASE("SDK lifecycle: shutdown waits for failed thread startup owner retirement") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto owners = std::make_shared<OpeningOwners>();
    owners->hold_destructor = true;
    std::atomic<unsigned> hook_calls{0};
    std::future<sdk::Result<std::shared_ptr<sdk::Session>>> opening;
    std::future<sdk::Result<void>> shutdown;
    // Release before either owned future joins, even on an assertion failure.
    struct Release { std::shared_ptr<OpeningOwners> owners; ~Release() { owners->backend.Release(); } } release{owners};
    opening = std::async(std::launch::async, [&] {
        ScopedOpeningHook hook([&] { ++hook_calls; throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again)); });
        return (*runtime)->OpenSession(OpeningOptions(fixture, owners));
    });
    REQUIRE(WaitForFlag(owners->backend.entered));
    std::promise<void> entering_shutdown;
    auto entered = entering_shutdown.get_future();
    shutdown = std::async(std::launch::async, [&] {
        entering_shutdown.set_value();
        return (*runtime)->Shutdown();
    });
    REQUIRE(entered.wait_for(10s) == std::future_status::ready);
    CHECK(shutdown.wait_for(100ms) == std::future_status::timeout);
    CHECK_FALSE(owners->backend.destroyed.load());
    owners->backend.Release();
    REQUIRE(opening.wait_for(10s) == std::future_status::ready);
    auto failed = opening.get();
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == "sdk.session.open_failed");
    REQUIRE(shutdown.wait_for(10s) == std::future_status::ready);
    CHECK(shutdown.get().has_value());
    CHECK(hook_calls.load() == 1);
    CheckOpeningRetired(owners);
    auto closed = (*runtime)->OpenSession(OpeningOptions(fixture, std::make_shared<OpeningOwners>()));
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error().code == "sdk.runtime.closed");
    std::cout << "[sdk-opening-start-path] shutdown\n";
}

TEST_CASE("SDK lifecycle: dropping the last session joins its backend and leaves runtime usable") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto gate = std::make_shared<LifetimeGate>();
    auto options = Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{};
    });
    options.backend = std::make_unique<LifetimeBackend>(gate);
    auto opened = (*runtime)->OpenSession(std::move(options));
    REQUIRE(opened.has_value());
    auto session = std::move(*opened);
    std::weak_ptr<sdk::Session> handle = session;
    std::future<void> dropping;
    // On every REQUIRE failure, release the backend before a future or Session
    // destructor waits for it. The async task is always joined, never detached.
    ReleaseLifetimeGate release{gate};
    REQUIRE(session->Submit("drop-owner", "wait for cancellation").has_value());
    REQUIRE(WaitForFlag(gate->entered));
    dropping = std::async(std::launch::async, [last = std::move(session)]() mutable { last.reset(); });
    REQUIRE(WaitForFlag(gate->cancelled));
    CHECK(dropping.wait_for(100ms) == std::future_status::timeout);
    CHECK_FALSE(gate->returned.load());
    CHECK_FALSE(gate->destroyed.load());
    gate->Release();
    REQUIRE(dropping.wait_for(10s) == std::future_status::ready);
    dropping.get();
    CHECK(handle.expired());
    CHECK(gate->returned.load());
    CHECK(gate->destroyed.load());
    CHECK_FALSE(gate->timed_out.load());

    auto next = (*runtime)->OpenSession(Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{std::string(12000, 'r')};
    }));
    REQUIRE(next.has_value());
    const auto id = (*next)->id();
    const auto receipt = (*next)->Submit("next-owner", "runtime still admits sessions");
    REQUIRE(receipt.has_value());
    const auto completed = (*next)->WaitResult(receipt->operation_id, 15s);
    REQUIRE(completed.has_value());
    REQUIRE(completed->state == sdk::OperationState::Succeeded);
    REQUIRE((*next)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    const auto saved = (*next)->ReadOperation(receipt->operation_id);
    REQUIRE(saved.has_value());
    CHECK((*next)->id() == id);
    CHECK(saved->state == sdk::OperationState::Succeeded);
    CHECK(saved->result_persisted);
    CHECK(saved->final_text == std::string(12000, 'r'));
}

TEST_CASE("SDK lifecycle: concurrent shutdown calls both wait for the same live backend") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto gate = std::make_shared<LifetimeGate>();
    auto options = Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{};
    });
    options.backend = std::make_unique<LifetimeBackend>(gate);
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    std::future<sdk::Result<void>> first;
    std::future<sdk::Result<void>> second;
    ReleaseLifetimeGate release{gate};
    REQUIRE((*session)->Submit("two-shutdowns", "wait for cancellation").has_value());
    REQUIRE(WaitForFlag(gate->entered));
    first = std::async(std::launch::async, [owner = runtime->get()] { return owner->Shutdown(); });
    REQUIRE(WaitForFlag(gate->cancelled));
    std::promise<void> started;
    auto second_started = started.get_future();
    second = std::async(std::launch::async, [owner = runtime->get(), start = std::move(started)]() mutable {
        start.set_value();
        return owner->Shutdown();
    });
    REQUIRE(second_started.wait_for(10s) == std::future_status::ready);
    second_started.get();
    CHECK(first.wait_for(100ms) == std::future_status::timeout);
    CHECK(second.wait_for(100ms) == std::future_status::timeout);
    CHECK_FALSE(gate->returned.load());
    CHECK_FALSE(gate->destroyed.load());
    gate->Release();
    REQUIRE(first.wait_for(10s) == std::future_status::ready);
    REQUIRE(second.wait_for(10s) == std::future_status::ready);
    CHECK(first.get().has_value());
    CHECK(second.get().has_value());
    CHECK(gate->returned.load());
    CHECK(gate->destroyed.load());
    CHECK_FALSE(gate->timed_out.load());
    REQUIRE((*runtime)->Shutdown().has_value());
    const auto rejected = (*session)->Submit("after-shutdown", "closed");
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == "sdk.session.closed");
}

TEST_CASE("SDK lifecycle: unrecoverable accepted input fails resume instead of waiting forever") {
    LifecycleFixture fixture;
    std::string session_id, operation_id;
    {
        lubancode::runtime::SessionService source(fixture.RawLaunch());
        REQUIRE(source.runtime() != nullptr);
        session_id = source.trajectory()->session_id();
        auto receipt = source.SubmitInput({"accepted-key", "must not disappear", {}});
        REQUIRE(receipt.accepted);
        operation_id = receipt.operation_id;
        REQUIRE(source.Close("fixture_checkpoint").error_code.empty());
    }
    const auto input = fixture.SessionDir(session_id) / "operations-inputs" / (operation_id + ".json");
    SUBCASE("missing original") { REQUIRE(fs::remove(input)); }
    SUBCASE("malformed original") { std::ofstream(input, std::ios::trunc) << "{broken"; }
    SUBCASE("valid JSON with changed payload") {
        Json original;
        { std::ifstream file(input); file >> original; }
        original["text"] = "changed after durable acceptance";
        std::ofstream(input, std::ios::trunc) << original.dump();
    }

    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(fixture, [calls](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*calls;
        return sdk::ModelReply{"should not execute"};
    });
    options.resume_session_id = session_id;
    auto resumed = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error().code == "sdk.resume.input_unavailable");
    CHECK(calls->load() == 0);
    REQUIRE((*runtime)->Shutdown().has_value());
    // Failed opening must release its source lock and file handles as well.
    fs::rename(fixture.root / "data", fixture.root / "closed-data");
}

TEST_CASE("SDK lifecycle: corrupt operation ledger cannot discard a completed operation identity") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto first = (*runtime)->OpenSession(Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{"durable completed answer"};
    }));
    REQUIRE(first.has_value());
    const auto session_id = (*first)->id();
    const auto submitted = (*first)->Submit("durable-original-key", "completed input");
    REQUIRE(submitted.has_value());
    const auto completed = (*first)->WaitResult(submitted->operation_id, 15s);
    REQUIRE(completed.has_value());
    REQUIRE(completed->state == sdk::OperationState::Succeeded);
    REQUIRE(completed->result_persisted);
    REQUIRE((*first)->Close().has_value());

    const auto source_dir = fixture.SessionDir(session_id);
    const auto operations_file = source_dir / "operations.jsonl";
    std::vector<std::string> lines;
    {
        std::ifstream input(operations_file, std::ios::binary);
        for (std::string line; std::getline(input, line);) lines.push_back(std::move(line));
    }
    REQUIRE(lines.size() == 3);
    REQUIRE(Json::parse(lines[0]).value("kind", "") == "operation.accepted");
    REQUIRE(Json::parse(lines[1]).value("kind", "") == "operation.dispatched");
    REQUIRE(Json::parse(lines[2]).value("kind", "") == "operation.final");
    bool rewrite_ledger = true;
    SUBCASE("malformed accepted line") { lines[0] = "{broken accepted fact"; }
    SUBCASE("accepted line missing but following facts remain valid JSON") { lines.erase(lines.begin()); }
    SUBCASE("conflicting second acceptance for the same operation") {
        auto conflict = Json::parse(lines[0]);
        conflict["clientOperationId"] = "different-client-key";
        lines.insert(lines.begin() + 1, conflict.dump());
    }
    SUBCASE("whole ledger missing while completed result remains") {
        REQUIRE(fs::remove(operations_file));
        rewrite_ledger = false;
    }
    SUBCASE("whole ledger empty while completed result remains") { lines.clear(); }
    if (rewrite_ledger) {
        std::ofstream output(operations_file, std::ios::binary | std::ios::trunc);
        for (const auto& line : lines) output << line << '\n';
        output.close();
        REQUIRE_FALSE(output.fail());
    }
    // Only the operation ledger was changed. A valid V3 stream alone cannot
    // prove that operation keys and counters remain safe to reuse.
    REQUIRE(lubancode::trajectory::v3::ReadV3Ledger(source_dir / (session_id + ".jsonl")).has_value());
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(fixture, [calls](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        ++*calls;
        return sdk::ModelReply{"must not execute"};
    });
    options.resume_session_id = session_id;
    const auto resumed = (*runtime)->OpenSession(std::move(options));
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error().code == "sdk.resume.operation_ledger_invalid");
    CHECK(calls->load() == 0);
    REQUIRE((*runtime)->Shutdown().has_value());
    fs::rename(fixture.root / "data", fixture.root / "closed-data");
}

TEST_CASE("SDK lifecycle: custom tool cannot join another session or shut down its runtime") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto idle = (*runtime)->OpenSession(Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{"idle answer"};
    }));
    REQUIRE(idle.has_value());
    auto steps = std::make_shared<std::atomic<int>>(0);
    auto checked = std::make_shared<std::atomic<bool>>(false);
    auto options = Options(fixture, [steps](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        if (steps->fetch_add(1) == 0) return sdk::ModelReply{"", {{"reentrant-call", "probe", "{}"}}};
        return sdk::ModelReply{"callback returned"};
    });
    sdk::Tool probe;
    probe.name = "probe";
    probe.requires_approval = false;
    probe.execute = [other = *idle, owner = runtime->get(), checked](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
        const auto close = other->Close();
        const auto wait = other->WaitResult("unused", 1ms);
        const auto shutdown = owner->Shutdown();
        checked->store(!close && close.error().code == "sdk.lifecycle.reentrant" &&
                       !wait && wait.error().code == "sdk.lifecycle.reentrant" &&
                       !shutdown && shutdown.error().code == "sdk.lifecycle.reentrant");
        return sdk::ToolResult{"checked"};
    };
    options.custom_tools.push_back(std::move(probe));
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    auto submitted = (*session)->Submit("tool-key", "exercise tool callback");
    REQUIRE(submitted.has_value());
    auto result = (*session)->WaitResult(submitted->operation_id, 15s);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(checked->load());
    // Rejected lifecycle calls must not close admission in either handle.
    auto still_open = (*idle)->Submit("after-callback", "still open");
    REQUIRE(still_open.has_value());
    REQUIRE((*idle)->WaitResult(still_open->operation_id, 15s).has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
}

TEST_CASE("SDK lifecycle: resumed session rejects an approval token from its previous turn") {
    LifecycleFixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto executions = std::make_shared<std::atomic<int>>(0);
    const auto make_options = [&] {
        auto calls = std::make_shared<std::atomic<int>>(0);
        auto options = Options(fixture, [calls](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
            if (calls->fetch_add(1) == 0) return sdk::ModelReply{"", {{"guarded-call", "guarded", "{}"}}};
            return sdk::ModelReply{"approved current turn"};
        });
        sdk::Tool tool;
        tool.name = "guarded";
        tool.execute = [executions](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
            ++*executions;
            return sdk::ToolResult{"executed"};
        };
        options.custom_tools.push_back(std::move(tool));
        return options;
    };
    const auto await_approval = [](const std::shared_ptr<sdk::EventStream>& events) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            auto event = events->Next(100ms);
            REQUIRE(event.has_value());
            if (event->has_value() && (*event)->approval) return *(*event)->approval;
        }
        FAIL("expected approval event did not arrive");
        return sdk::Approval{};
    };

    auto first = (*runtime)->OpenSession(make_options());
    REQUIRE(first.has_value());
    const auto session_id = (*first)->id();
    auto first_events = (*first)->Subscribe();
    REQUIRE(first_events.has_value());
    const auto first_receipt = (*first)->Submit("old-turn", "await old approval");
    REQUIRE(first_receipt.has_value());
    const auto old_approval = await_approval(*first_events);
    REQUIRE_FALSE(old_approval.request_id.empty());
    REQUIRE((*first)->Close().has_value());
    const auto old_result = (*first)->ReadOperation(first_receipt->operation_id);
    REQUIRE(old_result.has_value());
    CHECK(old_result->state == sdk::OperationState::Cancelled);
    CHECK(executions->load() == 0);

    auto resume_options = make_options();
    resume_options.resume_session_id = session_id;
    auto resumed = (*runtime)->OpenSession(std::move(resume_options));
    REQUIRE(resumed.has_value());
    CHECK((*resumed)->id() == session_id);
    auto new_events = (*resumed)->Subscribe();
    REQUIRE(new_events.has_value());
    const auto new_receipt = (*resumed)->Submit("new-turn", "await current approval");
    REQUIRE(new_receipt.has_value());
    const auto current_approval = await_approval(*new_events);
    CHECK(current_approval.request_id != old_approval.request_id);
    const auto stale = (*resumed)->ResolveApproval(old_approval.request_id, sdk::ApprovalDecision::Accept);
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error().code == "stale_request_id");
    const auto still_pending = (*resumed)->PendingApprovals();
    REQUIRE(still_pending.size() == 1);
    CHECK(still_pending.front().request_id == current_approval.request_id);
    CHECK(executions->load() == 0);

    REQUIRE((*resumed)->ResolveApproval(current_approval.request_id, sdk::ApprovalDecision::Accept).has_value());
    const auto result = (*resumed)->WaitResult(new_receipt->operation_id, 15s);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(result->result_persisted);
    CHECK(result->turn_id != old_result->turn_id);
    CHECK(executions->load() == 1);
    REQUIRE((*runtime)->Shutdown().has_value());
}

namespace {
void CheckMcpResultBoundary(bool image) {
    LifecycleFixture fixture;
#ifdef _WIN32
    const char* python = "python";
#else
    const char* python = "python3";
#endif
    const auto located = platform::RunProcess({python, "-c", "import json,sys; print(json.dumps(sys.executable))"}, 10000);
    REQUIRE_FALSE(located.spawn_failed);
    REQUIRE_FALSE(located.timed_out);
    REQUIRE(located.exit_code == 0);
    const auto executable = Json::parse(located.output);
    REQUIRE(executable.is_string());
    REQUIRE(platform::Utf8ToPath(executable.get<std::string>()).is_absolute());

    lubancode::test_support::FakeHttpServer server;
    const auto arguments = Json{{"kind", image ? "image" : "text"}}.dump();
    const auto argument_delta = Json{{"type", "content_block_delta"}, {"index", 0},
        {"delta", {{"type", "input_json_delta"}, {"partial_json", arguments}}}}.dump();
    server.Enqueue(Sse({
        R"({"type":"message_start","message":{"id":"m1","model":"fixture","usage":{"input_tokens":1,"output_tokens":0}}})",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"rich-call","name":"mcp__fixture__rich","input":{}}})",
        argument_delta,
        R"({"type":"content_block_stop","index":0})",
        R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":1}})",
        R"({"type":"message_stop"})"}));
    server.Enqueue(Sse({
        R"({"type":"message_start","message":{"id":"m2","model":"fixture","usage":{"input_tokens":1,"output_tokens":0}}})",
        R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})",
        R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"text received"}})",
        R"({"type":"content_block_stop","index":0})",
        R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}})",
        R"({"type":"message_stop"})"}));

    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    sdk::SessionOptions options;
    options.cwd = platform::PathToUtf8(fixture.root / "cwd");
    options.model = "fixture";
    options.connection = sdk::Connection{sdk::Wire::Anthropic,
        "http://127.0.0.1:" + std::to_string(server.port()), "FAKE_SDK_FIXTURE"};
    options.approval_mode = sdk::ApprovalMode::Yolo;
    options.max_steps_per_turn = 4;
    sdk::McpServer mcp;
    mcp.name = "fixture";
    mcp.command = executable.get<std::string>();
    mcp.arguments = {std::string(LUBANCODE_TEST_FIXTURES_DIR) + "/mcp_test_server.py"};
    for (const char* name : {"SystemRoot", "PATH", "TEMP", "TMP"}) {
        if (const auto value = platform::GetEnvVar(name)) mcp.environment.emplace_back(name, *value);
    }
    mcp.tools = {"rich"};
    options.mcp_servers.push_back(std::move(mcp));
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    const auto session_id = (*session)->id();
    auto events = (*session)->Subscribe();
    REQUIRE(events.has_value());
    const auto submitted = (*session)->Submit("mcp-key", "inspect fixture result");
    REQUIRE(submitted.has_value());
    const auto result = (*session)->WaitResult(submitted->operation_id, 20s);
    REQUIRE(result.has_value());
    std::string observed_events;
    for (;;) {
        const auto event = (*events)->Next(0ms);
        if (!event) { observed_events += event.error().code; break; }
        if (!event->has_value()) break;
        observed_events += (*event)->kind + ";";
    }
    INFO("MCP operation error: " << result->error);
    INFO("MCP event sequence (kinds only): " << observed_events);
    const auto session_dir = fixture.SessionDir(session_id);
    INFO("MCP session directory: " << platform::PathToUtf8(session_dir));
    INFO("MCP session path characters: " << session_dir.native().size());
    INFO("MCP PNG path characters before atomic temporary suffix: " <<
         (session_dir / "artifacts" / "sha256" / (std::string(64, 'a') + ".png")).native().size());
    std::string unexpected_tool_result;
    if (image) {
        const auto captured_requests = server.requests();
        if (captured_requests.size() > 1) {
            const auto request = Json::parse(captured_requests[1].body);
            for (const auto& message : request.at("messages")) {
                for (const auto& block : message.at("content")) {
                    if (block.value("type", "") != "tool_result" ||
                        block.value("tool_use_id", "") != "rich-call") continue;
                    // Only this fixture's tool reply is shown, never request
                    // headers, connection settings or the whole model request.
                    unexpected_tool_result += "is_error=" + std::string(block.value("is_error", false) ? "true" : "false") +
                        " content=" + block.at("content").dump().substr(0, 1200);
                }
            }
        }
    }
    INFO("Unexpected image follow-up tool reply: " << unexpected_tool_result);
    CHECK(result->result_persisted);
    if (image) {
        // The current production media-budget contract rejects unestimated
        // image/audio/blob results after capture. It must not invent a token
        // price, rerun the tool, or send the next request.
        CHECK(result->state == sdk::OperationState::Failed);
        CHECK(result->error.find("tool_batch.unestimated_media_or_reasoning") != std::string::npos);
        CHECK(result->final_text.empty());
    } else {
        CHECK(result->state == sdk::OperationState::Succeeded);
        CHECK(result->error.empty());
        CHECK(result->final_text == "text received");
    }
    const auto duplicate = (*session)->Submit("mcp-key", "inspect fixture result");
    REQUIRE(duplicate.has_value());
    CHECK(duplicate->duplicate);
    CHECK(duplicate->operation_id == submitted->operation_id);
    REQUIRE((*session)->Close().has_value());

    const auto requests = server.requests();
    REQUIRE(requests.size() == (image ? 1 : 2));
    CHECK(requests[0].target == "/v1/messages");
    if (!image) {
        CHECK(requests[1].target == "/v1/messages");
        const auto request = Json::parse(requests[1].body);
        bool model_received_text = false;
        for (const auto& message : request.at("messages")) {
            for (const auto& block : message.at("content")) {
                if (block.value("type", "") != "tool_result" ||
                    block.value("tool_use_id", "") != "rich-call") continue;
                CHECK_FALSE(block.value("is_error", false));
                REQUIRE(block.at("content").is_string());
                model_received_text = block.at("content").get<std::string>().find("只有文本") != std::string::npos;
            }
        }
        CHECK(model_received_text);
    }

    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(session_dir / (session_id + ".jsonl"));
    REQUIRE(ledger.has_value());
    using Kind = lubancode::trajectory::v3::EventKindV3;
    std::string action_id;
    for (const auto& event : ledger->events) {
        if (event.kind == Kind::ToolExecutionPending &&
            event.payload.value("provider_tool_call_id", "") == "rich-call") {
            REQUIRE(action_id.empty());
            REQUIRE(event.action_id.has_value());
            action_id = *event.action_id;
        }
    }
    REQUIRE_FALSE(action_id.empty());
    int starts = 0, finishes = 0, persisted = 0, image_captures = 0;
    for (const auto& event : ledger->events) {
        if (event.kind == Kind::ToolExecutionStarted) {
            ++starts;
            CHECK(event.action_id == action_id);
        }
        if (event.kind == Kind::ToolExecutionFinished) {
            ++finishes;
            CHECK(event.action_id == action_id);
        }
        if (event.kind != Kind::ToolResultPersisted) continue;
        ++persisted;
        CHECK(event.action_id == action_id);
        CHECK(event.payload.value("tool_call_id", "") == action_id);
        if (!image) continue;
        for (const auto& ref : event.payload.at("result_ref")) {
            const auto relative_path = ref.value("path", "");
            if (ref.value("kind", "") != "raw_payload" || relative_path.find("capture-") == std::string::npos) continue;
            ++image_captures;
            std::ifstream captured(session_dir / platform::Utf8ToPath(relative_path), std::ios::binary);
            REQUIRE(captured.is_open());
            const auto blocks = Json::parse(captured);
            REQUIRE(blocks.size() == 2);
            CHECK(blocks[0].value("type", "") == "text");
            CHECK(blocks[1].value("type", "") == "image");
            CHECK(blocks[1].value("mime_type", "") == "image/png");
            const auto& artifact = blocks[1].at("artifact");
            CHECK(artifact.value("stored", false));
            const auto recorded_path = artifact.at("path").get<std::string>();
            CHECK_FALSE(recorded_path.starts_with("\\\\?\\"));
            CHECK_FALSE(recorded_path.starts_with("//?/"));
            auto png = platform::Utf8ToPath(recorded_path);
            if (png.is_relative()) png = session_dir / png;
            const auto native_png = platform::FileIoPath(png);
            REQUIRE(fs::is_regular_file(native_png));
            CHECK(fs::equivalent(platform::FileIoPath(png.parent_path()),
                                 platform::FileIoPath(session_dir / "artifacts" / "sha256")));
            CHECK(png.extension() == ".png");
            CHECK(fs::file_size(native_png) == artifact.at("bytes").get<std::uintmax_t>());
            std::ifstream png_file(native_png, std::ios::binary);
            REQUIRE(png_file.is_open());
            std::string magic(8, '\0');
            png_file.read(magic.data(), 8);
            REQUIRE(png_file.gcount() == 8);
            CHECK(magic == std::string("\x89PNG\r\n\x1a\n", 8));
        }
    }
    CHECK(starts == 1);
    CHECK(finishes == 1);
    CHECK(persisted >= 1);
    if (image) CHECK(image_captures == 1);
}
} // namespace

TEST_CASE("SDK lifecycle: real MCP text reaches the next model request and completes once") {
    CheckMcpResultBoundary(false);
}

TEST_CASE("SDK lifecycle: real MCP image is captured once and rejected by the media budget") {
    CheckMcpResultBoundary(true);
}


#include "sdk/backend_owner_test_hooks.hpp"
#include <exception>
#include <stdexcept>

namespace {
namespace backend_owner_port = lubancore::detail::testing;
struct ObservedBackendOwner {
    std::weak_ptr<sdk::Backend> observed;
    backend_owner_port::BackendOwnerObserverHandle previous;
    ObservedBackendOwner() : previous(backend_owner_port::ReplaceBackendOwnerObserver(
        std::make_shared<const backend_owner_port::BackendOwnerObserver>(
            [this](const std::shared_ptr<sdk::Backend>& owner) { observed = owner; }))) {}
    ~ObservedBackendOwner() { backend_owner_port::ReplaceBackendOwnerObserver(std::move(previous)); }
    std::shared_ptr<sdk::Backend> Lock() const { return observed.lock(); }
};
struct MutualOwnerGate {
    std::mutex mutex; std::condition_variable cv; unsigned entered = 0;
    bool Meet() {
        std::unique_lock lock(mutex); ++entered; cv.notify_all();
        return cv.wait_for(lock, 5s, [&] { return entered == 2; });
    }
};
struct BackendOwnerProbe {
    std::atomic<unsigned> generated{0}, destroyed{0};
    std::atomic<bool> generate_checked{false}, destroy_checked{false}, probe_threw{false};
    std::function<sdk::Result<sdk::ModelReply>()> generate;
    std::function<void()> destroy;
};
class ObservedPublicBackend final : public sdk::Backend {
public:
    explicit ObservedPublicBackend(std::shared_ptr<BackendOwnerProbe> value) : probe_(std::move(value)) {}
    ~ObservedPublicBackend() override {
        ++probe_->destroyed;
        try { if (probe_->destroy) probe_->destroy(); }
        catch (...) { probe_->probe_threw.store(true); }
    }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
        ++probe_->generated;
        if (probe_->generate) return probe_->generate();
        return sdk::ModelReply{"owner probe"};
    }
private:
    std::shared_ptr<BackendOwnerProbe> probe_;
};
sdk::SessionOptions BackendOwnerOptions(const LifecycleFixture& fixture,
                                      const std::shared_ptr<BackendOwnerProbe>& probe) {
    auto options = Options(fixture, [](const auto&, sdk::Cancellation) -> sdk::Result<sdk::ModelReply> {
        return sdk::ModelReply{};
    });
    options.backend = std::make_unique<ObservedPublicBackend>(probe);
    return options;
}
bool ReentryRejected(sdk::Runtime& runtime, sdk::Session& session) {
    const auto id = session.id();
    const auto close = session.Close();
    const auto wait = session.WaitResult("owner-missing-operation", 1ms);
    const auto shutdown = runtime.Shutdown();
    const auto cancel = session.Cancel("owner-missing-operation");
    (void)session.PendingApprovals();
    return !id.empty() && !close && close.error().code == "sdk.lifecycle.reentrant" &&
        !wait && wait.error().code == "sdk.lifecycle.reentrant" &&
        !shutdown && shutdown.error().code == "sdk.lifecycle.reentrant" &&
        !cancel && cancel.error().code == "sdk.operation.not_found";
}
auto InvokeObserved(lubancode::api::Backend& adapter) {
    lubancode::api::Request request;
    request.model = "owner-probe";
    request.system = "owner probe";
    return adapter.send_stream(request, [](const lubancode::api::StreamEvent&) {}, nullptr);
}
}

TEST_CASE("SDK Backend owner: background adapter rejects lifecycle reentry and permits query/cancel") {
    std::cout << "[sdk-backend-owner-path] background\n";
    LifecycleFixture fixture; auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto probe = std::make_shared<BackendOwnerProbe>();
    ObservedBackendOwner observation;
    auto session = (*runtime)->OpenSession(BackendOwnerOptions(fixture,probe)); REQUIRE(session);
    auto owner = observation.Lock(); REQUIRE(owner);
    auto adapter = backend_owner_port::AdaptObservedBackend(owner); REQUIRE(adapter);
    probe->generate = [&] { probe->generate_checked.store(ReentryRejected(**runtime,**session));
        return sdk::Result<sdk::ModelReply>{sdk::ModelReply{"background"}}; };
    std::exception_ptr failure; bool sent = false;
    std::jthread worker([&] { try { sent = InvokeObserved(*adapter).has_value(); } catch (...) { failure = std::current_exception(); } });
    worker.join(); REQUIRE(failure == nullptr); CHECK(sent); CHECK(probe->generate_checked.load());
    CHECK(probe->generated.load() == 1);
    adapter.reset(); owner.reset(); REQUIRE((*session)->Close()); CHECK(probe->destroyed.load() == 1);
    REQUIRE((*runtime)->Shutdown());
}

TEST_CASE("SDK Backend owner: two background callbacks reject mutual Session close before locking") {
    std::cout << "[sdk-backend-owner-path] mutual\n";
    LifecycleFixture fixture; auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto left = std::make_shared<BackendOwnerProbe>(), right = std::make_shared<BackendOwnerProbe>();
    ObservedBackendOwner observation;
    auto a = (*runtime)->OpenSession(BackendOwnerOptions(fixture,left)); REQUIRE(a);
    auto aa = backend_owner_port::AdaptObservedBackend(observation.Lock()); REQUIRE(aa);
    auto b = (*runtime)->OpenSession(BackendOwnerOptions(fixture,right)); REQUIRE(b);
    auto bb = backend_owner_port::AdaptObservedBackend(observation.Lock()); REQUIRE(bb);
    MutualOwnerGate entered;
    left->generate = [&] { if (!entered.Meet()) return sdk::Result<sdk::ModelReply>{std::unexpected(sdk::Error{"fixture.timeout","mutual owner gate"})}; left->generate_checked.store(ReentryRejected(**runtime,**b));
        return sdk::Result<sdk::ModelReply>{sdk::ModelReply{"left"}}; };
    right->generate = [&] { if (!entered.Meet()) return sdk::Result<sdk::ModelReply>{std::unexpected(sdk::Error{"fixture.timeout","mutual owner gate"})}; right->generate_checked.store(ReentryRejected(**runtime,**a));
        return sdk::Result<sdk::ModelReply>{sdk::ModelReply{"right"}}; };
    std::exception_ptr fa,fb; bool sa=false,sb=false;
    std::jthread wa([&] { try { sa=InvokeObserved(*aa).has_value(); } catch (...) { fa=std::current_exception(); } });
    std::jthread wb([&] { try { sb=InvokeObserved(*bb).has_value(); } catch (...) { fb=std::current_exception(); } });
    wa.join(); wb.join(); REQUIRE(fa==nullptr); REQUIRE(fb==nullptr); CHECK(sa); CHECK(sb);
    CHECK(left->generate_checked.load()); CHECK(right->generate_checked.load());
    aa.reset(); bb.reset(); REQUIRE((*runtime)->Shutdown());
    CHECK(left->destroyed.load()==1); CHECK(right->destroyed.load()==1);
}

TEST_CASE("SDK Backend owner: actual Session closes before the final borrowed owner is destroyed on another thread") {
    std::cout << "[sdk-backend-owner-path] late-deleter\n";
    LifecycleFixture fixture; auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto probe=std::make_shared<BackendOwnerProbe>(); ObservedBackendOwner observation;
    auto session=(*runtime)->OpenSession(BackendOwnerOptions(fixture,probe)); REQUIRE(session);
    auto owner=observation.Lock(); REQUIRE(owner);
    auto adapter=backend_owner_port::AdaptObservedBackend(owner); REQUIRE(adapter);
    probe->destroy=[&] { probe->destroy_checked.store(ReentryRejected(**runtime,**session)); };
    REQUIRE((*session)->Close()); CHECK(probe->destroyed.load()==0);
    owner.reset(); CHECK_FALSE(observation.observed.expired());
    std::jthread retiring([owned=std::move(adapter)]() mutable { owned.reset(); }); retiring.join();
    CHECK(observation.observed.expired()); CHECK(probe->destroyed.load()==1);
    CHECK(probe->destroy_checked.load()); CHECK_FALSE(probe->probe_threw.load());
    REQUIRE((*session)->Close()); REQUIRE((*runtime)->Shutdown());
}

TEST_CASE("SDK Backend owner: final real Session cleanup rejects destructor lifecycle reentry") {
    std::cout << "[sdk-backend-owner-path] session-deleter\n";
    LifecycleFixture fixture; auto runtime=sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto probe=std::make_shared<BackendOwnerProbe>(); ObservedBackendOwner observation;
    auto session=(*runtime)->OpenSession(BackendOwnerOptions(fixture,probe)); REQUIRE(session);
    REQUIRE_FALSE(observation.observed.expired());
    probe->destroy=[&] { probe->destroy_checked.store(ReentryRejected(**runtime,**session)); };
    REQUIRE((*session)->Close()); CHECK(probe->destroyed.load()==1);
    CHECK(probe->destroy_checked.load()); CHECK_FALSE(probe->probe_threw.load());
    CHECK(observation.observed.expired()); REQUIRE((*runtime)->Shutdown());
}

TEST_CASE("SDK Backend owner: standard and unknown Generate exceptions restore the SDK TLS guard") {
    std::cout << "[sdk-backend-owner-path] exceptions\n";
    for (bool unknown : {false,true}) {
    LifecycleFixture fixture; auto runtime=sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto probe=std::make_shared<BackendOwnerProbe>(); ObservedBackendOwner observation;
    auto session=(*runtime)->OpenSession(BackendOwnerOptions(fixture,probe)); REQUIRE(session);
    auto adapter=backend_owner_port::AdaptObservedBackend(observation.Lock()); REQUIRE(adapter);
    probe->generate=[&]() -> sdk::Result<sdk::ModelReply> {
        probe->generate_checked.store(ReentryRejected(**runtime,**session));
        if (unknown) throw 71; throw std::runtime_error("owner exception");
    };
    bool failed=false, restored=false; std::exception_ptr failure;
    std::jthread worker([&] { try { failed=!InvokeObserved(*adapter); restored=(*session)->Close().has_value(); }
        catch (...) { failure=std::current_exception(); } }); worker.join();
    REQUIRE(failure==nullptr); CHECK(failed); CHECK(restored); CHECK(probe->generate_checked.load());
    CHECK(probe->destroyed.load()==0); adapter.reset(); CHECK(probe->destroyed.load()==1);
    REQUIRE((*runtime)->Shutdown());
    }
}

TEST_CASE("SDK Backend owner: nested adapter exception restores the outer SDK guard then releases it") {
    std::cout << "[sdk-backend-owner-path] nested\n";
    for (bool unknown : {false,true}) {
    LifecycleFixture fixture; auto runtime=sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto outer=std::make_shared<BackendOwnerProbe>(), inner=std::make_shared<BackendOwnerProbe>();
    ObservedBackendOwner observation;
    auto a=(*runtime)->OpenSession(BackendOwnerOptions(fixture,outer)); REQUIRE(a);
    auto aa=backend_owner_port::AdaptObservedBackend(observation.Lock()); REQUIRE(aa);
    auto b=(*runtime)->OpenSession(BackendOwnerOptions(fixture,inner)); REQUIRE(b);
    auto bb=backend_owner_port::AdaptObservedBackend(observation.Lock()); REQUIRE(bb);
    inner->generate=[&]() -> sdk::Result<sdk::ModelReply> {
        inner->generate_checked.store(ReentryRejected(**runtime,**a));
        if (unknown) throw 72; throw std::runtime_error("nested owner exception");
    };
    bool inner_failed=false;
    outer->generate=[&] { inner_failed=!InvokeObserved(*bb); outer->generate_checked.store(ReentryRejected(**runtime,**b));
        return sdk::Result<sdk::ModelReply>{sdk::ModelReply{"nested returned"}}; };
    bool sent=false,restored=false; std::exception_ptr failure;
    std::jthread worker([&] { try { sent=InvokeObserved(*aa).has_value(); restored=(*a)->Close().has_value() && (*b)->Close().has_value(); }
        catch (...) { failure=std::current_exception(); } }); worker.join();
    REQUIRE(failure==nullptr); CHECK(sent); CHECK(restored); CHECK(inner_failed);
    CHECK(inner->generate_checked.load()); CHECK(outer->generate_checked.load());
    aa.reset(); bb.reset(); CHECK(outer->destroyed.load()==1); CHECK(inner->destroyed.load()==1);
    REQUIRE((*runtime)->Shutdown());
    }
}

TEST_CASE("SDK Backend owner: allocation and observer failure retire captures before Backend") {
    std::cout << "[sdk-backend-owner-path] allocation-rollback\n";
    for (const bool allocation_failure : {true, false}) {
        LifecycleFixture fixture;
        auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
        auto probe = std::make_shared<BackendOwnerProbe>();
        auto retired = std::make_shared<std::atomic<bool>>(false);
        auto retired_before_backend = std::make_shared<std::atomic<bool>>(false);
        struct Capture {
            std::shared_ptr<BackendOwnerProbe> probe;
            std::shared_ptr<std::atomic<bool>> retired, retired_before_backend;
            ~Capture() {
                retired_before_backend->store(probe->destroyed.load() == 0);
                retired->store(true);
            }
        };
        auto options = BackendOwnerOptions(fixture, probe);
        auto capture = std::make_shared<Capture>();
        capture->probe = probe; capture->retired = retired;
        capture->retired_before_backend = retired_before_backend;
        sdk::Tool tool;
        tool.name = "backend_allocation_capture";
        tool.execute = [capture](const std::string&, const sdk::ToolContext&) -> sdk::Result<sdk::ToolResult> {
            return sdk::ToolResult{"unused"};
        };
        options.custom_tools.push_back(std::move(tool));
        // A moved-from std::function may retain its SBO capture. Retire the
        // caller's copy before asking the SDK to retire its own source.
        tool.execute = {};
        capture.reset();
        probe->destroy = [retired, probe_raw = probe.get(), owner = runtime->get()] {
            const auto shutdown = owner->Shutdown();
            probe_raw->destroy_checked.store(retired->load() && !shutdown &&
                shutdown.error().code == "sdk.lifecycle.reentrant");
        };
        std::atomic<unsigned> observer_calls{0};
        struct FaultScope {
            bool previous;
            backend_owner_port::BackendOwnerObserverHandle observer;
            explicit FaultScope(bool fail, std::atomic<unsigned>& calls) : previous(
                backend_owner_port::ReplaceBackendOwnerAllocationFailure(fail)),
                observer(backend_owner_port::ReplaceBackendOwnerObserver(
                    std::make_shared<const backend_owner_port::BackendOwnerObserver>(
                        [&calls](const std::shared_ptr<sdk::Backend>&) { ++calls; throw std::runtime_error("owner observer rollback"); }))) {}
            ~FaultScope() {
                backend_owner_port::ReplaceBackendOwnerAllocationFailure(previous);
                backend_owner_port::ReplaceBackendOwnerObserver(std::move(observer));
            }
        } fault(allocation_failure, observer_calls);
        auto session = (*runtime)->OpenSession(std::move(options));
        REQUIRE_FALSE(session);
        CHECK(session.error().code == "sdk.session.open_failed");
        CHECK_FALSE(backend_owner_port::ReplaceBackendOwnerAllocationFailure(false));
        CHECK(observer_calls.load() == (allocation_failure ? 0u : 1u));
        CHECK(retired->load()); CHECK(retired_before_backend->load());
        CHECK(probe->destroy_checked.load()); CHECK(probe->destroyed.load() == 1);
        CHECK(probe->generated.load() == 0); CHECK_FALSE(probe->probe_threw.load());
        CHECK((*runtime)->Shutdown());
    }
}
