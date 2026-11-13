#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "tools/tool_job_coordinator.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
using lubancode::tools::JobExecutionContext;
using lubancode::tools::JobExecutor;
using lubancode::tools::JobStartRequest;
using lubancode::tools::Tool;
using lubancode::tools::ToolJobCoordinator;

struct Watchdog {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread thread;
    Watchdog() : thread([this] {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 15s, [this] { return done; })) std::abort();
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex); done = true; }
        cv.notify_all();
        thread.join();
    }
};
struct Gate {
    std::promise<void> entered, release;
    std::shared_future<void> entered_future = entered.get_future().share();
    std::shared_future<void> release_future = release.get_future().share();
    std::atomic<bool> signalled{false}, opened{false};
    void Enter() {
        if (!signalled.exchange(true)) entered.set_value();
        release_future.wait();
    }
    void Open() { if (!opened.exchange(true)) release.set_value(); }
};
struct JoinOnExit {
    std::vector<std::shared_ptr<Gate>> gates;
    std::vector<std::future<bool>*> booleans;
    std::vector<std::future<void>*> voids;
    ~JoinOnExit() {
        for (const auto& gate : gates) gate->Open();
        for (auto* future : booleans) if (future->valid()) future->wait();
        for (auto* future : voids) if (future->valid()) future->wait();
    }
};
struct OwnedDirectory {
    fs::path path;
    explicit OwnedDirectory(const std::string& tag) {
        static std::atomic<std::uint64_t> number{0};
        const auto root = fs::temp_directory_path();
        for (unsigned attempt = 0; attempt != 64; ++attempt) {
            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            auto candidate = root / ("lubancode-job-start-" + tag + "-" + std::to_string(tick) +
                "-" + std::to_string(number.fetch_add(1)));
            std::error_code error;
            const bool created = fs::create_directory(candidate, error);
            if (error == std::errc::file_exists) continue;
            REQUIRE_MESSAGE(!error, error.message());
            if (created) { path = std::move(candidate); break; }
        }
        REQUIRE_FALSE(path.empty());
    }
    ~OwnedDirectory() {
        std::error_code error;
        fs::remove_all(path, error);  // Only this fixture's create-new directory.
        // A reporter exception during stack unwinding must not terminate the
        // fixture after its actual cleanup has already run.
        try { CHECK_MESSAGE(!error, error.message()); } catch (...) {}
    }
};
struct Harness {
    // Declared first: constructor unwinding also closes the actual writer and
    // coordinator before removing this fixture's exclusively created root.
    OwnedDirectory directory;
    fs::path journal;
    std::optional<v3::V3Writer> writer;
    std::unique_ptr<ToolJobCoordinator> coordinator;
    std::vector<std::shared_ptr<Gate>> gates;
    explicit Harness(const std::string& tag, ToolJobCoordinator::Options options,
                     JobExecutor executor, v3::V3WriterOptions writer_options = {}) : directory(tag) {
        journal = directory.path / "s1.jsonl";
        auto started = v3::V3Writer::Start(journal, "20261003-120000-JSTART", "run-000001",
                                          "job startup fixture", nlohmann::json::object(), std::move(writer_options));
        REQUIRE_MESSAGE(started.has_value(), (started ? std::string() : started.error()));
        writer = std::move(*started);
        coordinator = std::make_unique<ToolJobCoordinator>(*writer,
            [](const std::string&, const nlohmann::json&) {
                return lubancode::tools::JobAuthDecision{true, false, ""};
            }, std::move(executor), std::move(options));
    }
    ~Harness() {
        for (const auto& gate : gates) gate->Open();
        coordinator.reset();
        writer.reset();
    }
    JobStartRequest Request(const std::string& call) {
        v3::MessageDraft draft;
        draft.turn_id = "turn-000001";
        draft.step_id = "step-000001";
        draft.request_id = "request-000001";
        draft.origin = v3::MessageOrigin::SessionRuntime;
        draft.provider = "fixture";
        draft.wire = "responses";
        draft.model = "fixture";
        draft.response_model = "fixture";
        draft.usage = {{"inputTokens", 1}, {"outputTokens", 1}};
        draft.message = {{"role", "assistant"}, {"content", "start"},
            {"tool_calls", nlohmann::json::array({{{"id", call}, {"type", "function"},
              {"function", {{"name", "project_write"}, {"arguments", "{}"}}}}})}};
        const auto message = writer->AppendMessage(draft, v3::Durability::PowerLoss);
        REQUIRE(message.status == v3::WriteReceipt::Status::Committed);
        REQUIRE(writer->AdmitMessages({message.id}).status == v3::WriteReceipt::Status::Committed);
        JobStartRequest request;
        request.tool_name = "project_write";
        request.tool_input = nlohmann::json::object();
        request.turn_id = "turn-000001";
        request.step_id = "step-000001";
        request.assistant_message_ref = message.id;
        request.policy.side_effect_class = "local_write";
        request.policy.resource_keys = {"project:same"};
        return request;
    }
    v3::V3Ledger Ledger() const {
        auto ledger = v3::ReadV3Ledger(journal);
        REQUIRE_MESSAGE(ledger.has_value(), (ledger ? std::string() : ledger.error()));
        return std::move(*ledger);
    }
    std::string Bytes() const {
        std::ifstream input(journal, std::ios::binary);
        REQUIRE(input.is_open());
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        REQUIRE_FALSE(input.bad());
        return bytes;
    }
};
std::size_t Count(const v3::V3Ledger& ledger, const std::string& action, v3::EventKindV3 kind) {
    std::size_t count = 0;
    for (const auto& event : ledger.events)
        if (event.action_id == action && event.kind == kind && event.payload.value("attempt", 0) == 2) ++count;
    return count;
}
std::size_t Observed(const v3::V3Ledger& ledger, const std::string& action, const std::string& state) {
    std::size_t count = 0;
    for (const auto& event : ledger.events)
        if (event.action_id == action && event.kind == v3::EventKindV3::ToolJobObserved &&
            event.payload.value("observedStatus", "") == state) ++count;
    return count;
}
void FailedOnce(Harness& harness, const lubancode::tools::JobStartResult& job) {
    REQUIRE(job.ok);
    const auto status = harness.coordinator->GetJob(job.job_id);
    CHECK(status.state == "failed");
    CHECK(status.failure == "tool.job.worker_start_failed");
    CHECK(harness.coordinator->running_count() == 0);
    CHECK(harness.coordinator->queued_count() == 0);
    const auto ledger = harness.Ledger();
    CHECK(Count(ledger, job.action_id, v3::EventKindV3::ToolExecutionStarted) == 1);
    CHECK(Count(ledger, job.action_id, v3::EventKindV3::ToolExecutionFailed) == 1);
    CHECK(Observed(ledger, job.action_id, "failed") == 1);
    const auto bytes = harness.Bytes();
    harness.coordinator->PumpCompletions();
    CHECK(harness.Bytes() == bytes);
}
struct CopyState {
    std::atomic<bool> reject{false};
    std::atomic<unsigned> calls{0}, rejected{0};
    std::shared_ptr<Gate> first_gate;
};
struct ThrowingCopy {
    std::shared_ptr<CopyState> state;
    explicit ThrowingCopy(std::shared_ptr<CopyState> value) : state(std::move(value)) {}
    ThrowingCopy(const ThrowingCopy& other) : state(other.state) {
        if (state->reject.load()) { ++state->rejected; throw std::runtime_error("executor copy rejected"); }
    }
    ThrowingCopy(ThrowingCopy&&) = default;
    Tool::Result operator()(const JobExecutionContext&) const {
        const auto call = ++state->calls;
        if (call == 1 && state->first_gate) state->first_gate->Enter();
        return Tool::Result{"actual-result", false};
    }
};
void Mark(const char* path) { std::printf("[job-start-path] %s\n", path); }
}

TEST_CASE("Job start transaction keeps a throwing executor copy out of running state") {
    Watchdog watchdog;
    auto state = std::make_shared<CopyState>();
    auto quota = std::make_shared<lubancode::tools::GlobalRunningQuota>();
    quota->limit = 1;
    ToolJobCoordinator::Options options;
    options.global = quota;
    options.limits.session_running = 1;
    options.limits.per_tool = 1;
    Harness harness("copy", options, JobExecutor{ThrowingCopy{state}});
    state->reject = true;
    const auto failed = harness.coordinator->StartJob(harness.Request("copy-fail"));
    FailedOnce(harness, failed);
    CHECK(state->calls == 0);
    CHECK(state->rejected == 1);
    CHECK(quota->running == 0);
    state->reject = false;
    const auto actual = harness.coordinator->StartJob(harness.Request("copy-next"));
    REQUIRE(actual.ok);
    const auto waited = harness.coordinator->WaitJobs({actual.job_id}, 2000, true);
    REQUIRE(waited.satisfied);
    REQUIRE(waited.statuses.size() == 1);
    CHECK(waited.statuses.front().state == "succeeded");
    CHECK(state->calls == 1);
    CHECK(quota->running == 0);
    REQUIRE(harness.coordinator->Shutdown());
    Mark("executor-copy");
}

TEST_CASE("Job start transaction catches standard nonstandard and empty thread starts") {
    Watchdog watchdog;
    for (int variant = 0; variant != 3; ++variant) {
        auto calls = std::make_shared<std::atomic<unsigned>>(0);
        auto quota = std::make_shared<lubancode::tools::GlobalRunningQuota>();
        ToolJobCoordinator::Options options;
        options.global = quota;
        options.thread_starter = [variant](std::thread&, std::function<void()>) {
            if (variant == 0) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            if (variant == 1) throw 17;
        };
        Harness harness("reject-" + std::to_string(variant), options,
            [calls](const JobExecutionContext&) { ++*calls; return Tool::Result{"not-run", false}; });
        FailedOnce(harness, harness.coordinator->StartJob(harness.Request("reject")));
        CHECK(calls->load() == 0);
        CHECK(quota->running == 0);
        REQUIRE(harness.coordinator->Shutdown());
    }
    Mark("before-thread");
}

TEST_CASE("Job start transaction retains a real thread when its starter throws afterwards") {
    Watchdog watchdog;
    auto gate = std::make_shared<Gate>();
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto quota = std::make_shared<lubancode::tools::GlobalRunningQuota>();
    ToolJobCoordinator::Options options;
    options.global = quota;
    options.thread_starter = [](std::thread& thread, std::function<void()> entry) {
        thread = std::thread(std::move(entry));
        throw std::runtime_error("after the real thread was published");
    };
    std::future<bool> closing;
    Harness harness("after", options, [gate, calls](const JobExecutionContext& context) {
        REQUIRE(context.cancel != nullptr);
        ++*calls; gate->Enter();
        return Tool::Result{"real-success-after-starter-error", false};
    });
    harness.gates.push_back(gate);
    JoinOnExit cleanup{{gate}, {&closing}, {}};
    const auto job = harness.coordinator->StartJob(harness.Request("after"));
    REQUIRE(job.ok);
    REQUIRE(gate->entered_future.wait_for(2s) == std::future_status::ready);
    CHECK(harness.coordinator->running_count() == 1);
    CHECK(quota->running == 1);
    closing = std::async(std::launch::async, [&] { return harness.coordinator->Shutdown(); });
    CHECK(closing.wait_for(50ms) == std::future_status::timeout);
    gate->Open();
    REQUIRE(closing.wait_for(2s) == std::future_status::ready);
    CHECK(closing.get());
    CHECK(harness.coordinator->shutdown_complete());
    CHECK(calls->load() == 1);
    CHECK(quota->running == 0);
    const auto ledger = harness.Ledger();
    CHECK(Count(ledger, job.action_id, v3::EventKindV3::ToolExecutionFinished) == 1);
    CHECK(Count(ledger, job.action_id, v3::EventKindV3::ToolExecutionFailed) == 0);
    CHECK(Observed(ledger, job.action_id, "succeeded") == 1);
    Mark("after-thread");
}

TEST_CASE("Job start transaction releases a queued job failure reservation for its successor") {
    Watchdog watchdog;
    auto state = std::make_shared<CopyState>();
    state->first_gate = std::make_shared<Gate>();
    auto quota = std::make_shared<lubancode::tools::GlobalRunningQuota>();
    quota->limit = 1;
    ToolJobCoordinator::Options options;
    options.global = quota;
    options.limits.session_running = 1;
    options.limits.per_tool = 1;
    Harness harness("queued", options, JobExecutor{ThrowingCopy{state}});
    harness.gates.push_back(state->first_gate);
    const auto first = harness.coordinator->StartJob(harness.Request("first"));
    REQUIRE(first.ok);
    REQUIRE(state->first_gate->entered_future.wait_for(2s) == std::future_status::ready);
    const auto second = harness.coordinator->StartJob(harness.Request("second"));
    REQUIRE(second.ok);
    CHECK(harness.coordinator->GetJob(second.job_id).state == "queued");
    state->reject = true;
    state->first_gate->Open();
    const auto both = harness.coordinator->WaitJobs({first.job_id, second.job_id}, 2000, true);
    REQUIRE(both.satisfied);
    FailedOnce(harness, second);
    CHECK(state->calls == 1);
    CHECK(quota->running == 0);
    state->reject = false;
    const auto third = harness.coordinator->StartJob(harness.Request("third"));
    const auto waited = harness.coordinator->WaitJobs({third.job_id}, 2000, true);
    REQUIRE(waited.satisfied);
    REQUIRE(waited.statuses.size() == 1);
    CHECK(waited.statuses.front().state == "succeeded");
    CHECK(state->calls == 2);
    CHECK(quota->running == 0);
    REQUIRE(harness.coordinator->Shutdown());
    Mark("queued-successor");
}

TEST_CASE("Job start transaction exposes an unconfirmed failed ledger without retaining running quota") {
    Watchdog watchdog;
    auto fault = std::make_shared<std::atomic<bool>>(false);
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    auto quota = std::make_shared<lubancode::tools::GlobalRunningQuota>();
    ToolJobCoordinator::Options options;
    options.global = quota;
    options.thread_starter = [fault](std::thread&, std::function<void()>) {
        fault->store(true);
        throw std::runtime_error("known no-thread failure before an unconfirmed terminal");
    };
    v3::V3WriterOptions writer_options;
    writer_options.inject_io_failure = [fault]() -> std::optional<std::string> {
        return fault->load() ? std::optional<std::string>("startup terminal IO") : std::nullopt;
    };
    Harness harness("unconfirmed", options,
        [calls](const JobExecutionContext&) { ++*calls; return Tool::Result{"not-run", false}; }, writer_options);
    const auto job = harness.coordinator->StartJob(harness.Request("unconfirmed"));
    REQUIRE(job.ok);
    const auto status = harness.coordinator->GetJob(job.job_id);
    CHECK(status.state == "unknown");
    CHECK(status.failure == "tool.job.worker_start_failed");
    CHECK(harness.writer->broken());
    CHECK(harness.coordinator->running_count() == 0);
    CHECK(harness.coordinator->queued_count() == 0);
    CHECK(quota->running == 0);
    CHECK(calls->load() == 0);
    const auto bytes = harness.Bytes();
    harness.coordinator->PumpCompletions();
    CHECK(harness.Bytes() == bytes);
    const auto ledger = harness.Ledger();
    CHECK(Count(ledger, job.action_id, v3::EventKindV3::ToolExecutionStarted) == 1);
    CHECK(Count(ledger, job.action_id, v3::EventKindV3::ToolExecutionFailed) == 0);
    CHECK(Observed(ledger, job.action_id, "failed") == 0);
    CHECK_FALSE(harness.coordinator->Shutdown());
    CHECK(harness.coordinator->shutdown_complete());
    CHECK(quota->running == 0);
    CHECK(calls->load() == 0);
    Mark("unconfirmed-terminal");
}

namespace {
struct DestructorState {
    std::atomic<bool> arm{false};
    std::atomic<unsigned> copies_destroyed{0};
    std::atomic<bool> queried_failed{false}, self_close_refused{false};
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    ToolJobCoordinator* coordinator = nullptr;
    std::string job_id;
};
struct DestructorProbe {
    std::shared_ptr<DestructorState> state;
    bool copied = false;
    explicit DestructorProbe(std::shared_ptr<DestructorState> value) : state(std::move(value)) {}
    DestructorProbe(const DestructorProbe& other) : state(other.state), copied(state->arm.load()) {}
    DestructorProbe(DestructorProbe&& other) noexcept : state(std::move(other.state)), copied(other.copied) { other.copied = false; }
    ~DestructorProbe() {
        if (!state || !copied) return;
        ++state->copies_destroyed;
        const auto status = state->coordinator->GetJob(state->job_id);
        state->queried_failed = status.state == "failed";
        state->self_close_refused = !state->coordinator->Shutdown();
        state->gate->Enter();
    }
    Tool::Result operator()(const JobExecutionContext&) const { return Tool::Result{"never-run", false}; }
};
}

TEST_CASE("Job start transaction clears failed captures outside job locks before concurrent close returns") {
    Watchdog watchdog;
    auto state = std::make_shared<DestructorState>();
    ToolJobCoordinator::Options options;
    options.thread_starter = [](std::thread&, std::function<void()>) { throw std::runtime_error("pre-thread rejection"); };
    std::future<void> reaping;
    std::future<bool> closing;
    Harness harness("capture-close", options, JobExecutor{DestructorProbe{state}});
    harness.gates.push_back(state->gate);
    JoinOnExit cleanup{{state->gate}, {&closing}, {&reaping}};
    state->coordinator = harness.coordinator.get();
    state->arm = true;
    const auto job = harness.coordinator->StartJob(harness.Request("capture-close"));
    REQUIRE(job.ok);
    state->job_id = job.job_id;
    // StartJob returned, but the failed owned executor must await lock-free
    // reaping. Its destructor itself queries this coordinator and refuses to
    // claim a successful reentrant close.
    CHECK(state->copies_destroyed == 0);
    reaping = std::async(std::launch::async, [&] { (void)harness.coordinator->GetJob(job.job_id); });
    REQUIRE(state->gate->entered_future.wait_for(2s) == std::future_status::ready);
    CHECK(state->queried_failed);
    CHECK(state->self_close_refused);
    closing = std::async(std::launch::async, [&] { return harness.coordinator->Shutdown(); });
    CHECK(closing.wait_for(50ms) == std::future_status::timeout);
    CHECK_FALSE(harness.coordinator->shutdown_complete());
    state->gate->Open();
    REQUIRE(reaping.wait_for(2s) == std::future_status::ready);
    reaping.get();
    REQUIRE(closing.wait_for(2s) == std::future_status::ready);
    CHECK(closing.get());
    CHECK(state->copies_destroyed == 1);
    CHECK(harness.coordinator->shutdown_complete());
    const auto ledger = harness.Ledger();
    CHECK(Count(ledger, job.action_id, v3::EventKindV3::ToolExecutionFailed) == 1);
    CHECK(Observed(ledger, job.action_id, "failed") == 1);
    Mark("capture-close");
}
