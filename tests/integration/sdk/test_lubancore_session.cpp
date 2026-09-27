#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "lubancore/core.hpp"
#include "runtime/session_service.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace {
using namespace std::chrono_literals;
namespace sdk = lubancore;
namespace fs = std::filesystem;

struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<int> serial{0};
        root = fs::temp_directory_path() / ("lubancore-sdk-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "cwd");
        fs::create_directories(root / "resources");
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    std::string Utf8(const fs::path& path) const { return lubancode::tools::PathToUtf8(path); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
    fs::path SessionDir(const std::string& id) const {
        auto identity = lubancode::workspace::ResolveWorkspaceIdentity(root / "cwd", root / "data");
        REQUIRE(identity.has_value());
        return root / "data" / "workspaces" / identity->workspace_key / "sessions" / id;
    }
};
class AnswerBackend final : public sdk::Backend {
public:
    AnswerBackend(std::shared_ptr<std::atomic<int>> calls, std::string answer)
        : calls_(std::move(calls)), answer_(std::move(answer)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
        ++*calls_;
        return sdk::ModelReply{answer_, {}, sdk::Usage{4, 5}};
    }
private:
    std::shared_ptr<std::atomic<int>> calls_;
    std::string answer_;
};
struct StopGate {
    std::mutex mutex;
    std::condition_variable cv;
    int entered = 0;
    int cancelled = 0;
    std::atomic<int> peer_timeouts{0};
};
class CoordinatedBackend final : public sdk::Backend {
public:
    explicit CoordinatedBackend(std::shared_ptr<StopGate> gate) : gate_(std::move(gate)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation cancel) override {
        {
            std::lock_guard lock(gate_->mutex);
            ++gate_->entered;
            gate_->cv.notify_all();
        }
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!cancel.requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
        std::unique_lock lock(gate_->mutex);
        if (cancel.requested()) ++gate_->cancelled;
        gate_->cv.notify_all();
        if (!gate_->cv.wait_for(lock, 3s, [&] { return gate_->cancelled == 2; })) ++gate_->peer_timeouts;
        return std::unexpected(sdk::Error{"fixture.cancelled", "closed"});
    }
private:
    std::shared_ptr<StopGate> gate_;
};
sdk::SessionOptions Options(const Fixture& fixture, std::shared_ptr<std::atomic<int>> calls,
                            std::string answer = "answer") {
    sdk::SessionOptions options;
    options.cwd = fixture.Utf8(fixture.root / "cwd");
    options.model = "fixture";
    options.system_prompt = "SDK fixture";
    options.backend = std::make_unique<AnswerBackend>(std::move(calls), std::move(answer));
    options.max_steps_per_turn = 8;
    return options;
}
lubancode::runtime::SessionLaunchRequest RawLaunch(const Fixture& fixture) {
    lubancode::runtime::SessionLaunchRequest launch;
    launch.cwd_utf8 = fixture.Utf8(fixture.root / "cwd");
    auto identity = lubancode::workspace::ResolveWorkspaceIdentity(fixture.root / "cwd", fixture.root / "data");
    REQUIRE(identity.has_value());
    launch.workspace_identity = *identity;
    launch.workspaces_root = fixture.root / "data" / "workspaces";
    launch.lubancode_version = "sdk-test";
    launch.v3_system_content = "SDK fixture";
    return launch;
}
} // namespace

TEST_CASE("SDK: durable operation references real V3 assistant and preserves full text") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto calls = std::make_shared<std::atomic<int>>(0);
    const std::string full(12000, 'x');
    auto session = (*runtime)->OpenSession(Options(fixture, calls, full));
    REQUIRE(session.has_value());
    auto submitted = (*session)->Submit("same-key", "question");
    REQUIRE(submitted.has_value());
    auto result = (*session)->WaitResult(submitted->operation_id, 15s);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(result->result_persisted);
    CHECK(result->final_text == full);
    auto duplicate = (*session)->Submit("same-key", "question");
    REQUIRE(duplicate.has_value());
    CHECK(duplicate->duplicate);
    CHECK(duplicate->operation_id == submitted->operation_id);
    CHECK(calls->load() == 1);
    auto conflict = (*session)->Submit("same-key", "changed");
    REQUIRE_FALSE(conflict.has_value());
    CHECK(conflict.error().code == "operation_conflict");
    const auto id = (*session)->id();
    REQUIRE((*session)->Close().has_value());
    auto stream = fixture.SessionDir(id) / (id + ".jsonl");
    const auto ledger = lubancode::trajectory::v3::ReadV3Ledger(stream);
    REQUIRE(ledger.has_value());
    const auto facts = lubancode::runtime::SessionService::ReadOperationFacts(fixture.SessionDir(id));
    bool found = false;
    for (const auto& fact : facts) if (fact.kind == "operation.final") {
        found = true;
        REQUIRE(fact.final_message_refs.size() == 1);
        CHECK(fact.usage_reported);
        bool valid = false;
        for (const auto& message : ledger->messages) {
            if (message.message_id == fact.final_message_refs[0]) {
                valid = message.message.value("role", "") == "assistant" && message.turn_id == result->turn_id;
            }
        }
        CHECK(valid);
    }
    CHECK(found);
    CHECK((*session)->ReadOperation(submitted->operation_id)->final_text == full);
}

TEST_CASE("SDK: resumes accepted work without waiting for a new submission") {
    Fixture fixture;
    std::string id, operation_id;
    {
        lubancode::runtime::SessionService source(RawLaunch(fixture));
        REQUIRE(source.runtime() != nullptr);
        id = source.trajectory()->session_id();
        auto receipt = source.SubmitInput({"recovered-key", "queued", {}});
        REQUIRE(receipt.accepted);
        operation_id = receipt.operation_id;
        REQUIRE(source.Close("test_checkpoint").error_code.empty());
    }
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(fixture, calls);
    options.resume_session_id = id;
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    CHECK((*session)->id() == id);
    auto result = (*session)->WaitResult(operation_id, 15s);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Succeeded);
    CHECK(calls->load() == 1);
    auto duplicate = (*session)->Submit("recovered-key", "queued");
    REQUIRE(duplicate.has_value());
    CHECK(duplicate->duplicate);
    CHECK(duplicate->operation_id == operation_id);
}

TEST_CASE("SDK: dispatched work without final stays indeterminate and is never retried") {
    Fixture fixture;
    std::string id, operation_id;
    {
        lubancode::runtime::SessionService source(RawLaunch(fixture));
        REQUIRE(source.runtime() != nullptr);
        id = source.trajectory()->session_id();
        auto receipt = source.SubmitInput({"unknown-key", "do not repeat", {}});
        REQUIRE(receipt.accepted);
        operation_id = receipt.operation_id;
        REQUIRE(source.PopPendingInput().status == lubancode::runtime::SessionService::PendingPop::Status::Ok);
        REQUIRE(source.Close("test_checkpoint").error_code.empty());
    }
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto options = Options(fixture, calls);
    options.resume_session_id = id;
    auto session = (*runtime)->OpenSession(std::move(options));
    REQUIRE(session.has_value());
    auto result = (*session)->WaitResult(operation_id, 1s);
    REQUIRE(result.has_value());
    CHECK(result->state == sdk::OperationState::Indeterminate);
    CHECK_FALSE(result->result_persisted);
    auto duplicate = (*session)->Submit("unknown-key", "do not repeat");
    REQUIRE(duplicate.has_value());
    CHECK(duplicate->duplicate);
    REQUIRE((*session)->Close().has_value());
    CHECK(calls->load() == 0);
}

TEST_CASE("SDK: stream overflow is explicit and unsubscribe wakes an in-flight read") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(fixture, std::make_shared<std::atomic<int>>(0)));
    REQUIRE(session.has_value());
    auto tiny = (*session)->Subscribe(1);
    REQUIRE(tiny.has_value());
    auto submitted = (*session)->Submit("event-key", "hello");
    REQUIRE(submitted.has_value());
    REQUIRE((*session)->WaitResult(submitted->operation_id, 15s).has_value());
    auto overflow = (*tiny)->Next(0ms);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().code == "sdk.events.overflow");
    auto idle = (*runtime)->OpenSession(Options(fixture, std::make_shared<std::atomic<int>>(0)));
    REQUIRE(idle.has_value());
    auto stream = (*idle)->Subscribe();
    REQUIRE(stream.has_value());
    auto next = std::async(std::launch::async, [state = *stream] { return state->Next(20s); });
    (*stream)->Close();
    REQUIRE(next.wait_for(2s) == std::future_status::ready);
    auto closed = next.get();
    REQUIRE_FALSE(closed.has_value());
    CHECK(closed.error().code == "sdk.events.closed");
}

TEST_CASE("SDK: runtime shutdown signals every session before joining a worker") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto gate = std::make_shared<StopGate>();
    auto first_options = Options(fixture, std::make_shared<std::atomic<int>>(0));
    first_options.backend = std::make_unique<CoordinatedBackend>(gate);
    auto second_options = Options(fixture, std::make_shared<std::atomic<int>>(0));
    second_options.backend = std::make_unique<CoordinatedBackend>(gate);
    auto first = (*runtime)->OpenSession(std::move(first_options));
    auto second = (*runtime)->OpenSession(std::move(second_options));
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE((*first)->Submit("first", "wait").has_value());
    REQUIRE((*second)->Submit("second", "wait").has_value());
    {
        std::unique_lock lock(gate->mutex);
        REQUIRE(gate->cv.wait_for(lock, 10s, [&] { return gate->entered == 2; }));
    }
    REQUIRE((*runtime)->Shutdown().has_value());
    CHECK(gate->cancelled == 2);
    CHECK(gate->peer_timeouts.load() == 0);
    CHECK_FALSE((*first)->Submit("after", "closed").has_value());
    CHECK_FALSE((*second)->Submit("after", "closed").has_value());
}
