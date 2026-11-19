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
#include "workspace/index.hpp"

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
        auto workspace_dir = lubancode::workspace::index::ResolveDirByWorkspaceKey(
            root / "data" / "workspaces", identity->workspace_key);
        REQUIRE(workspace_dir.has_value());
        return *workspace_dir / "sessions" / id;
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


TEST_CASE("SDK five-field result: actual operation totals and coverage survive Close and same-ID recovery") {
    namespace facts = sdk::usage::v1;
    struct FiveFieldBackend final : sdk::Backend {
        std::shared_ptr<std::atomic<int>> calls;
        explicit FiveFieldBackend(std::shared_ptr<std::atomic<int>> value) : calls(std::move(value)) {}
        sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
            ++*calls;
            sdk::ModelReply reply{"five-field", {}, sdk::Usage{3, 7, 11, 13, 2}};
            reply.provider_response_id = "real-five-field-provider";
            facts::Observation observation; observation.provider_namespace = "fixture";
            const std::array<std::int64_t, 5> values{3, 7, 11, 13, 2};
            for (std::size_t i = 0; i < values.size(); ++i) {
                observation.raw_fields.push_back({"usage." + std::to_string(i), facts::RawKind::SignedInteger, values[i], {}, {}});
                auto& field = observation.fields[i];
                field.presence = facts::Presence::Present; field.validity = facts::Validity::ValidInteger;
                field.origin = facts::Origin::Reported; field.operands[0] = static_cast<std::uint16_t>(i);
                field.operand_count = 1;
            }
            reply.usage_observation = std::move(observation);
            return reply;
        }
    };
    Fixture fixture;
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto runtime = sdk::Runtime::Create(fixture.Roots()); REQUIRE(runtime);
    auto options = Options(fixture, calls);
    options.backend = std::make_unique<FiveFieldBackend>(calls);
    auto session = (*runtime)->OpenSession(std::move(options)); REQUIRE(session);
    const auto receipt = (*session)->Submit("five-field", "question"); REQUIRE(receipt);
    const auto result = (*session)->WaitResult(receipt->operation_id, 15s); REQUIRE(result);
    REQUIRE(result->state == sdk::OperationState::Succeeded); REQUIRE(result->result_persisted);
    const auto check = [](const sdk::Operation& operation) {
        REQUIRE(operation.usage);
        const auto& direct = operation.usage->direct;
        CHECK(direct.total.input_tokens == 3); CHECK(direct.total.output_tokens == 7);
        CHECK(direct.total.cache_read_tokens == 11); CHECK(direct.total.cache_creation_tokens == 13);
        CHECK(direct.total.output_reasoning_tokens == 2); CHECK(direct.coverage.samples == 1);
        for (const auto& field : direct.coverage.fields) {
            CHECK(field.observed == 1); CHECK(field.valid == 1); CHECK(field.missing == 0);
            CHECK(field.included == 1); CHECK(field.omitted == 0); CHECK(field.anomalous == 0);
        }
        CHECK(operation.usage->subordinate.coverage.samples == 0);
        CHECK(operation.usage->attempts_complete); REQUIRE(operation.usage->attempts.size() == 1);
        const auto& attempt = operation.usage->attempts.front();
        CHECK_FALSE(attempt.subordinate); CHECK_FALSE(attempt.incomplete); CHECK(attempt.reported_by_provider);
        CHECK_FALSE(attempt.trajectory_request_id.empty()); CHECK(attempt.provider_response_id == "real-five-field-provider");
        CHECK(attempt.turn_id == operation.turn_id); CHECK_FALSE(attempt.step_id.empty());
        CHECK_FALSE(attempt.model.empty()); CHECK(attempt.purpose == "main_turn"); CHECK(attempt.cache_epoch >= 0);
        CHECK_FALSE(attempt.source_session_id.empty()); CHECK_FALSE(attempt.source_run_id.empty());
        REQUIRE(attempt.observation); REQUIRE(attempt.observation->raw_fields.size() == 5);
        CHECK(attempt.observation->raw_fields[0].integer == 3); CHECK(attempt.observation->raw_fields[4].integer == 2);
    };
    check(*result);
    const auto id = (*session)->id(); REQUIRE((*session)->Close());
    const auto source = lubancode::trajectory::v3::ReadV3Ledger(fixture.SessionDir(id) / lubancode::tools::Utf8ToPath(id + ".jsonl"));
    REQUIRE(source); unsigned response_rows = 0;
    for (const auto& message : source->messages) {
        if (message.request_id != result->usage->attempts.front().trajectory_request_id ||
            message.message.value("role", std::string()) != "assistant") continue;
        ++response_rows;
        CHECK(message.message.at("provider_response_id") == "real-five-field-provider");
        REQUIRE(message.response_model); CHECK(message.response_model->is_null());
        REQUIRE(message.usage); CHECK(message.usage->at("inputTokens") == 3);
        CHECK(message.usage->at("outputTokens") == 7); CHECK(message.usage->at("cacheReadTokens") == 11);
        CHECK(message.usage->at("cacheWriteTokens") == 13); CHECK(message.usage->at("reasoningTokens") == 2);
    }
    CHECK(response_rows == 1);
    unsigned observed_rows = 0;
    for (const auto& event : source->events) {
        if (event.kind != lubancode::trajectory::v3::EventKindV3::ModelUsageObserved ||
            event.request_id != result->usage->attempts.front().trajectory_request_id) continue;
        ++observed_rows;
        CHECK(event.payload.at("numbers") == nlohmann::json::array({3, 7, 11, 13, 2}));
        CHECK(event.payload.at("providerResponseId") == "real-five-field-provider");
        CHECK(event.payload.at("incomplete") == false); CHECK(event.payload.at("reportedByProvider") == true);
        REQUIRE(event.payload.contains("observation"));
        CHECK(event.payload.at("observation").at("raw_fields").size() == 5);
    }
    CHECK(observed_rows == 1);
    auto resumed_options = Options(fixture, calls); resumed_options.resume_session_id = id;
    auto resumed = (*runtime)->OpenSession(std::move(resumed_options)); REQUIRE(resumed);
    const auto recovered = (*resumed)->ReadOperation(receipt->operation_id); REQUIRE(recovered);
    check(*recovered); CHECK(recovered->result_persisted); CHECK(calls->load() == 1);
    REQUIRE((*resumed)->Close());
    const auto result_path = fixture.SessionDir(id) / "sdk-results" / (receipt->operation_id + ".json");
    nlohmann::json saved;
    { std::ifstream input(result_path); input >> saved; }
    auto invalid = saved;
    invalid["usage"]["direct"]["fields"][0]["observed"] = 2;
    { std::ofstream output(result_path, std::ios::binary | std::ios::trunc); output << invalid.dump(); }
    auto invalid_options = Options(fixture, calls); invalid_options.resume_session_id = id;
    auto rejected = (*runtime)->OpenSession(std::move(invalid_options));
    REQUIRE_FALSE(rejected); CHECK(rejected.error().code == "sdk.usage.result_invalid");
    for (int corrupt = 0; corrupt < 14; ++corrupt) {
        auto bad_attempt = saved;
        if (corrupt == 0) bad_attempt["usage"]["attempts"][0]["numbers"][0] = 99;
        if (corrupt == 1) bad_attempt["usage"]["attempts"][0]["provider_response_id"] = std::string(257, 'x');
        if (corrupt == 2) bad_attempt["usage"]["attempts"][0]["observation"]["fields"][0]["operands"] = nlohmann::json::array({64});
        if (corrupt == 3) bad_attempt["usage"]["attempts"] = nlohmann::json::array();
        if (corrupt == 4) bad_attempt["usage"]["attempts"][0]["turn_id"] = "foreign-turn";
        if (corrupt == 5) bad_attempt["usage"]["attempts"][0]["trajectory_request_id"] = "foreign-request";
        if (corrupt == 6) bad_attempt["usage"]["attempts"][0]["model"] = "foreign-model";
        if (corrupt == 7) bad_attempt["usage"]["attempts"][0]["purpose"] = "memory_extract";
        if (corrupt == 8) bad_attempt["usage"]["attempts"][0]["cache_epoch"] = 12345;
        if (corrupt == 9) bad_attempt["usage"]["attempts"][0]["step_id"] = "foreign-step";
        if (corrupt == 10) bad_attempt["usage"]["attempts"][0]["source_session_id"] = "foreign-session";
        if (corrupt == 11) bad_attempt["usage"]["attempts"][0]["source_run_id"] = "foreign-run";
        if (corrupt == 12) bad_attempt["usage"]["attempts"][0]["provider_response_id"] = "foreign-response";
        if (corrupt == 13) bad_attempt["usage"]["attempts"][0]["observation"]["raw_fields"][0]["summary"] = "foreign-material";
        { std::ofstream output(result_path, std::ios::binary | std::ios::trunc); output << bad_attempt.dump(); }
        auto bad_options = Options(fixture, calls); bad_options.resume_session_id = id;
        const auto invalid_record = (*runtime)->OpenSession(std::move(bad_options));
        REQUIRE_FALSE(invalid_record); CHECK(invalid_record.error().code == "sdk.usage.result_invalid");
        CHECK(calls->load() == 1);
    }
    saved.erase("usage");
    { std::ofstream output(result_path, std::ios::binary | std::ios::trunc); output << saved.dump(); }
    auto legacy_options = Options(fixture, calls); legacy_options.resume_session_id = id;
    auto legacy = (*runtime)->OpenSession(std::move(legacy_options)); REQUIRE(legacy);
    const auto old = (*legacy)->ReadOperation(receipt->operation_id); REQUIRE(old);
    CHECK_FALSE(old->usage); CHECK(old->result_persisted); CHECK(calls->load() == 1);
    REQUIRE((*legacy)->Close());
}
