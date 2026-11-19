#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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

#include <lubancore/core.hpp>
#include <lubancore/memory.hpp>
#include <lubancore/usage.hpp>
#include <nlohmann/json.hpp>

#include "memory/candidate_store.hpp"
#include "memory/project_commit.hpp"
#include "memory/project_memory.hpp"
#include "memory/topic_store.hpp"
#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "sdk/memory_learning_test_hooks.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/envelope.hpp"
#include "trajectory/v3/reader.hpp"

namespace {

namespace sdk = lubancore;
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
namespace mem = lubancode::memory;
using Json = nlohmann::json;
using sdk::memory::v1::LearningMode;
using sdk::memory::v1::LearningOptions;
using sdk::memory::v1::LearningReport;
using sdk::memory::v1::LearningSnapshot;
using sdk::memory::v1::LearningStage;
using namespace std::chrono_literals;

std::string Utf8(const fs::path& p) { return lubancode::tools::PathToUtf8(p); }

void WriteText(const fs::path& p, const std::string& content) {
    fs::create_directories(lubancode::platform::FileIoPath(p.parent_path()));
    std::ofstream out(lubancode::platform::FileIoPath(p), std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << content;
    out.close();
    REQUIRE_FALSE(out.fail());
}

std::string ReadText(const fs::path& p) {
    std::ifstream in(lubancode::platform::FileIoPath(p), std::ios::binary);
    REQUIRE(in.is_open());
    std::string value{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(in.bad());
    return value;
}

void PrintPathMarker(const char* marker) {
    std::cout << "[sdk-memory-learning-path] " << marker << '\n' << std::flush;
}

sdk::usage::v1::Observation FiveFieldObservation(const sdk::Usage& usage,
                                                 const std::string& ns = "sdk") {
    namespace facts = sdk::usage::v1;
    facts::Observation obs;
    obs.provider_namespace = ns;
    const std::array<std::int64_t, facts::kFieldCount> numbers{
        usage.input_tokens,
        usage.output_tokens,
        usage.cache_read_tokens,
        usage.cache_creation_tokens,
        usage.output_reasoning_tokens,
    };
    for (std::size_t i = 0; i != numbers.size(); ++i) {
        facts::RawField raw;
        raw.path = "reply.usage." + std::to_string(i);
        raw.kind = facts::RawKind::SignedInteger;
        raw.integer = numbers[i];
        raw.summary = std::to_string(numbers[i]);
        obs.raw_fields.push_back(std::move(raw));
        auto& field = obs.fields[i];
        field.presence = facts::Presence::Present;
        field.validity = facts::Validity::ValidInteger;
        field.origin = facts::Origin::Reported;
        field.operands[0] = static_cast<std::uint16_t>(i);
        field.operand_count = 1;
    }
    return obs;
}

std::string ValidExtractionPayload(const std::string& title = "Project build convention",
                                   const std::string& summary = "Prefer ninja and clang-tidy for C++ builds",
                                   const std::string& content = "Always build with ninja and run clang-tidy checks.") {
    Json payload{
        {"task_type", "config"},
        {"summary", summary},
        {"candidates", Json::array({
            Json{
                {"kind", "preference"},
                {"title", title},
                {"summary", summary},
                {"content", content},
                {"keywords", Json::array({"ninja", "clang-tidy"})},
                {"paths", Json::array({"cmake/LubanCore.cmake"})},
                {"confidence", "user-stated"},
            }
        })},
        {"retrieval_terms", Json::array({"ninja", "clang-tidy"})},
    };
    return payload.dump();
}

struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-learning-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "repo");
        fs::create_directories(root / "resources");
        WriteText(root / "repo" / "cmake" / "LubanCore.cmake", "# fixture marker\n");
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(lubancode::platform::FileIoPath(root), ec);
    }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
};

struct MainTrace {
    std::atomic<unsigned> calls{0};
    sdk::Usage usage{11, 7, 3, 2, 1};
    bool send_memory_save = false;
    bool pending_tool = false;
    std::string reply_text = "Understood, noted cleanly for this repository.";
};

class MainBackend final : public sdk::Backend {
public:
    explicit MainBackend(std::shared_ptr<MainTrace> trace) : trace_(std::move(trace)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation cancel) override {
        ++trace_->calls;
        if (cancel.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        if (trace_->send_memory_save && !trace_->pending_tool) {
            trace_->pending_tool = true;
            Json input{
                {"kind", "preference"},
                {"id", "preference.explicit-save"},
                {"title", "ExplicitSave"},
                {"summary", "Explicitly saved project preference"},
                {"content", "Saved explicitly during turn."},
                {"keywords", Json::array({"ExplicitSave"})},
                {"confidence", "user-stated"},
            };
            sdk::ModelReply r;
            r.tool_calls.push_back({"call-save-1", "memory_save", input.dump()});
            r.usage = trace_->usage;
            r.usage_observation = FiveFieldObservation(*r.usage);
            r.provider_response_id = "main-resp-tool";
            return r;
        }
        trace_->pending_tool = false;
        sdk::ModelReply r;
        r.text = trace_->reply_text;
        r.usage = trace_->usage;
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "main-resp-text";
        return r;
    }
private:
    std::shared_ptr<MainTrace> trace_;
};

struct LearningTrace {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<unsigned> calls{0};
    std::atomic<unsigned> in_generate{0};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> destroyed_while_active{false};
    std::vector<sdk::ModelRequest> requests;

    struct Step {
        std::optional<sdk::Result<sdk::ModelReply>> reply;
        bool block_until_released = false;
        bool block_until_cancel = false;
        bool return_reply_on_cancel = false;
    };
    std::vector<Step> script;
    bool entered = false;
    bool release = false;

    void Release() {
        {
            std::lock_guard lock(mutex);
            release = true;
        }
        cv.notify_all();
    }

    bool WaitEntered(std::chrono::milliseconds timeout = 10s) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return entered; });
    }
};

class LearningBackend final : public sdk::Backend {
public:
    explicit LearningBackend(std::shared_ptr<LearningTrace> trace) : trace_(std::move(trace)) {}
    ~LearningBackend() override {
        if (trace_->in_generate.load() != 0) trace_->destroyed_while_active.store(true);
        trace_->destroyed.store(true);
    }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancel) override {
        struct ActiveGuard {
            std::atomic<unsigned>& counter;
            explicit ActiveGuard(std::atomic<unsigned>& c) : counter(c) { ++counter; }
            ~ActiveGuard() { --counter; }
        } active(trace_->in_generate);

        const unsigned idx = trace_->calls.fetch_add(1);
        LearningTrace::Step step;
        {
            std::lock_guard lock(trace_->mutex);
            trace_->requests.push_back(request);
            if (idx < trace_->script.size()) {
                step = trace_->script[idx];
            } else {
                sdk::ModelReply r;
                r.text = ValidExtractionPayload();
                r.usage = sdk::Usage{19, 13, 5, 0, 2};
                r.usage_observation = FiveFieldObservation(*r.usage);
                r.provider_response_id = "learn-resp-default";
                step.reply = r;
            }
            trace_->entered = true;
        }
        trace_->cv.notify_all();

        if (step.block_until_released) {
            std::unique_lock lock(trace_->mutex);
            trace_->cv.wait_for(lock, 15s, [&] { return trace_->release || cancel.requested(); });
        }
        if (step.block_until_cancel) {
            const auto deadline = std::chrono::steady_clock::now() + 15s;
            while (!cancel.requested() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(5ms);
            }
            if (!step.return_reply_on_cancel) {
                return std::unexpected(sdk::Error{"fixture.cancelled", "learning cancelled"});
            }
        }
        if (!step.return_reply_on_cancel && cancel.requested()) {
            return std::unexpected(sdk::Error{"fixture.cancelled", "learning cancelled"});
        }
        if (step.reply) return *step.reply;
        return std::unexpected(sdk::Error{"fixture.missing_step", "no scripted reply"});
    }
private:
    std::shared_ptr<LearningTrace> trace_;
};

class ScopedHooks {
public:
    ScopedHooks() = default;
    ~ScopedHooks() {
        (void)sdk::detail::testing::ReplaceLearningWorkerStartHook({});
        (void)sdk::detail::testing::ReplaceLearningWorkerExecutionHook({});
        (void)sdk::detail::testing::ReplaceLearningCandidateStoreHook({});
        (void)sdk::detail::testing::ReplaceLearningCommitWriteHook({});
        (void)sdk::detail::testing::ReplaceLearningAutoAdmissionHook({});
    }
    void SetStart(sdk::detail::testing::LearningWorkerStartHook fn) {
        (void)sdk::detail::testing::ReplaceLearningWorkerStartHook(
            std::make_shared<const sdk::detail::testing::LearningWorkerStartHook>(std::move(fn)));
    }
    void SetExecution(sdk::detail::testing::LearningWorkerExecutionHook fn) {
        (void)sdk::detail::testing::ReplaceLearningWorkerExecutionHook(
            std::make_shared<const sdk::detail::testing::LearningWorkerExecutionHook>(std::move(fn)));
    }
    void SetCandidateStore(sdk::detail::testing::LearningCandidateStoreHook fn) {
        (void)sdk::detail::testing::ReplaceLearningCandidateStoreHook(
            std::make_shared<const sdk::detail::testing::LearningCandidateStoreHook>(std::move(fn)));
    }
    void SetCommitWrite(sdk::detail::testing::LearningCommitWriteHook fn) {
        (void)sdk::detail::testing::ReplaceLearningCommitWriteHook(
            std::make_shared<const sdk::detail::testing::LearningCommitWriteHook>(std::move(fn)));
    }
    void SetAutoAdmission(sdk::detail::testing::LearningAutoAdmissionHook fn) {
        (void)sdk::detail::testing::ReplaceLearningAutoAdmissionHook(
            std::make_shared<const sdk::detail::testing::LearningAutoAdmissionHook>(std::move(fn)));
    }
};

sdk::SessionOptions MakeBaseOptions(const Fixture& fixture,
                                    const std::shared_ptr<MainTrace>& main_trace,
                                    std::optional<LearningOptions> learning_opts = std::nullopt,
                                    const std::shared_ptr<LearningTrace>& learning_trace = nullptr) {
    sdk::SessionOptions opts;
    opts.cwd = Utf8(fixture.root / "repo");
    opts.model = "main-model";
    opts.system_prompt = "HOST_PROMPT";
    opts.backend = std::make_unique<MainBackend>(main_trace);
    opts.memory_learning = std::move(learning_opts);
    if (learning_trace) {
        opts.memory_learning_backend = std::make_unique<LearningBackend>(learning_trace);
    }
    return opts;
}

bool IsTerminalLearningStage(LearningStage s) {
    return s != LearningStage::Running;
}

LearningReport WaitForLearning(const std::shared_ptr<sdk::Session>& session,
                               const std::string& operation_id,
                               std::chrono::milliseconds timeout = 15s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto r = session->GetMemoryLearning(operation_id);
        if (r && IsTerminalLearningStage(r->stage)) return *r;
        std::this_thread::sleep_for(10ms);
    }
    auto last = session->GetMemoryLearning(operation_id);
    REQUIRE(last.has_value());
    return *last;
}

fs::path MemoryDir(const std::shared_ptr<sdk::Session>& session) {
    auto snap = session->DescribeMemoryLearning();
    REQUIRE(snap.has_value());
    return lubancode::tools::Utf8ToPath(snap->memory_directory);
}

fs::path SessionDir(const std::shared_ptr<sdk::Session>& session) {
    return MemoryDir(session).parent_path() / "sessions" / session->id();
}

const char* DurablePrompt() {
    return "请记住这个项目统一使用 ninja 和 clang-tidy 做构建检查，禁止私自改成别的构建器。";
}

} // namespace

TEST_CASE("SDK memory learning: Off default and unexpected backend rejected") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    {
        auto learn_trace = std::make_shared<LearningTrace>();
        auto opts = MakeBaseOptions(fixture, std::make_shared<MainTrace>(), std::nullopt, learn_trace);
        auto bad = (*runtime)->OpenSession(std::move(opts));
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().code == "sdk.memory_learning.unexpected_backend");
    }
    // Verify explicit Off + Connection rejection with a valid Wire enum.
    {
        LearningOptions off_opts;
        off_opts.mode = LearningMode::Off;
        auto opts = MakeBaseOptions(fixture, std::make_shared<MainTrace>(), off_opts);
        opts.memory_learning_connection = sdk::Connection{sdk::Wire::ChatCompletions, "http://127.0.0.1:1/v1", "k"};
        auto bad = (*runtime)->OpenSession(std::move(opts));
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().code == "sdk.memory_learning.unexpected_backend");
    }

    auto main_trace = std::make_shared<MainTrace>();
    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace));
    REQUIRE(session.has_value());
    auto snap = (*session)->DescribeMemoryLearning();
    REQUIRE(snap.has_value());
    CHECK(snap->mode == LearningMode::Off);

    auto receipt = (*session)->Submit("op-off-1", DurablePrompt());
    REQUIRE(receipt.has_value());
    auto op = (*session)->WaitResult(receipt->operation_id, 15s);
    REQUIRE(op.has_value());
    CHECK(op->state == sdk::OperationState::Succeeded);

    auto report = (*session)->GetMemoryLearning(receipt->operation_id);
    REQUIRE(report.has_value());
    CHECK(report->mode == LearningMode::Off);
    CHECK(report->stage == LearningStage::NotAttempted);
    CHECK_FALSE(report->model_called);
    CHECK(report->candidate_ids.empty());
    CHECK(report->usage.attempts.empty());

    REQUIRE((*session)->Close().has_value());
    auto post_close = (*session)->GetMemoryLearning(receipt->operation_id);
    REQUIRE(post_close.has_value());
    CHECK(post_close->stage == LearningStage::NotAttempted);
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("off-default-and-unexpected-backend");
}

TEST_CASE("SDK memory learning: Auto and invalid options rejected") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    {
        LearningOptions auto_opts;
        auto_opts.mode = LearningMode::Auto;
        auto learn_trace = std::make_shared<LearningTrace>();
        auto res = (*runtime)->OpenSession(MakeBaseOptions(fixture, std::make_shared<MainTrace>(), auto_opts, learn_trace));
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == "sdk.memory_learning.invalid_options");
    }
    {
        LearningOptions auto_missing_backend;
        auto_missing_backend.mode = LearningMode::Auto;
        auto_missing_backend.model = "extract-model";
        auto res = (*runtime)->OpenSession(MakeBaseOptions(fixture, std::make_shared<MainTrace>(), auto_missing_backend));
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == "sdk.memory_learning.missing_backend");
    }
    {
        LearningOptions missing_model;
        missing_model.mode = LearningMode::Review;
        auto learn_trace = std::make_shared<LearningTrace>();
        auto res = (*runtime)->OpenSession(MakeBaseOptions(fixture, std::make_shared<MainTrace>(), missing_model, learn_trace));
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == "sdk.memory_learning.invalid_options");
    }
    {
        LearningOptions missing_backend;
        missing_backend.mode = LearningMode::Review;
        missing_backend.model = "extract-model";
        auto res = (*runtime)->OpenSession(MakeBaseOptions(fixture, std::make_shared<MainTrace>(), missing_backend));
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == "sdk.memory_learning.missing_backend");
    }
    {
        LearningOptions conflict;
        conflict.mode = LearningMode::Review;
        conflict.model = "extract-model";
        auto opts = MakeBaseOptions(fixture, std::make_shared<MainTrace>(), conflict, std::make_shared<LearningTrace>());
        opts.memory_learning_connection = sdk::Connection{sdk::Wire::ChatCompletions, "http://127.0.0.1:1/v1", "k"};
        auto res = (*runtime)->OpenSession(std::move(opts));
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == "sdk.memory_learning.backend_conflict");
    }
    {
        LearningOptions bad_budget;
        bad_budget.mode = LearningMode::Review;
        bad_budget.model = "extract-model";
        bad_budget.max_candidates = 4;
        auto res = (*runtime)->OpenSession(MakeBaseOptions(fixture, std::make_shared<MainTrace>(), bad_budget, std::make_shared<LearningTrace>()));
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == "sdk.memory_learning.invalid_options");
    }
    {
        LearningOptions bad_tx;
        bad_tx.mode = LearningMode::Review;
        bad_tx.model = "extract-model";
        bad_tx.max_transcript_bytes = 64;
        auto res = (*runtime)->OpenSession(MakeBaseOptions(fixture, std::make_shared<MainTrace>(), bad_tx, std::make_shared<LearningTrace>()));
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == "sdk.memory_learning.invalid_options");
    }

    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("auto-and-invalid-options-rejected");
}

TEST_CASE("SDK memory learning: Review creates candidates only and isolates usage") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    LearningOptions opts_learning;
    opts_learning.mode = LearningMode::Review;
    opts_learning.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, opts_learning, learn_trace));
    REQUIRE(session.has_value());
    auto snap = (*session)->DescribeMemoryLearning();
    REQUIRE(snap.has_value());
    CHECK(snap->mode == LearningMode::Review);
    CHECK(snap->model == "extract-model");
    CHECK_FALSE(snap->plan_sha256.empty());

    auto receipt = (*session)->Submit("op-review-1", DurablePrompt());
    REQUIRE(receipt.has_value());
    auto op = (*session)->WaitResult(receipt->operation_id, 15s);
    REQUIRE(op.has_value());
    CHECK(op->state == sdk::OperationState::Succeeded);
    REQUIRE(op->usage.has_value());
    CHECK(op->usage->direct.total.input_tokens == 11);
    CHECK(op->usage->direct.total.output_tokens == 7);
    CHECK(op->usage->direct.total.cache_read_tokens == 3);
    CHECK(op->usage->direct.total.cache_creation_tokens == 2);
    CHECK(op->usage->direct.total.output_reasoning_tokens == 1);

    const auto report = WaitForLearning(*session, receipt->operation_id);
    CHECK(report.stage == LearningStage::Completed);
    CHECK(report.mode == LearningMode::Review);
    CHECK(report.session_id == (*session)->id());
    CHECK(report.operation_id == receipt->operation_id);
    CHECK(report.turn_id == op->turn_id);
    CHECK_FALSE(report.learning_turn_id.empty());
    CHECK(report.learning_turn_id != report.turn_id);
    CHECK(report.model_called);
    CHECK(report.candidate_ids.size() == 1);
    CHECK(report.retrieval_hints == std::vector<std::string>{"ninja", "clang-tidy"});
    REQUIRE(report.usage.attempts.size() == 1);
    CHECK(report.usage.direct.total.input_tokens == 19);
    CHECK(report.usage.direct.total.output_tokens == 13);
    CHECK(report.usage.direct.total.cache_read_tokens == 5);
    CHECK(report.usage.direct.total.cache_creation_tokens == 0);
    CHECK(report.usage.direct.total.output_reasoning_tokens == 2);
    CHECK(report.usage.attempts[0].purpose == "memory_extract");
    CHECK(report.usage.attempts[0].turn_id == report.learning_turn_id);
    CHECK(report.usage.attempts[0].provider_response_id == "learn-resp-default");
    CHECK(report.usage.attempts[0].reported_by_provider);

    // Candidate landed pending review; formal MEMORY.md was NOT written in Review mode.
    const fs::path mem_dir = MemoryDir(*session);
    const auto cands = mem::candidates::ListCandidates(mem_dir);
    REQUIRE(cands.size() == 1);
    CHECK(cands[0].id == report.candidate_ids[0]);
    CHECK_FALSE(fs::exists(mem_dir / "MEMORY.md"));

    // Main operation usage remained isolated from learning usage even after Close().
    REQUIRE((*session)->Close().has_value());
    auto final_op = (*session)->ReadOperation(receipt->operation_id);
    REQUIRE(final_op.has_value());
    REQUIRE(final_op->usage.has_value());
    CHECK(final_op->usage->direct.total.input_tokens == 11);
    CHECK(final_op->usage->attempts.size() == 1);

    const auto ledger = v3::ReadV3Ledger(SessionDir(*session) / ((*session)->id() + ".jsonl"));
    REQUIRE(ledger.has_value());
    unsigned bypass_prepared = 0;
    unsigned bypass_observed = 0;
    for (const auto& ev : ledger->events) {
        if (ev.turn_id != report.learning_turn_id) continue;
        if (ev.kind == v3::EventKindV3::ModelRequestPrepared) {
            ++bypass_prepared;
            REQUIRE(ev.parent_turn_id.has_value());
            CHECK(*ev.parent_turn_id == report.turn_id);
            CHECK(ev.payload.value("purpose", "") == "memory_extract");
        } else if (ev.kind == v3::EventKindV3::ModelUsageObserved) {
            ++bypass_observed;
            CHECK(ev.payload.at("numbers") == Json::array({19, 13, 5, 0, 2}));
        }
    }
    CHECK(bypass_prepared == 1);
    CHECK(bypass_observed == 1);

    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("review-creates-candidates-and-isolates-usage");
}

TEST_CASE("SDK memory learning: lexical, signal and already_mutated gates") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";
    auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
    opts.memory_write = sdk::memory::v1::WriteOptions{};

    auto session = (*runtime)->OpenSession(std::move(opts));
    REQUIRE(session.has_value());

    // 1) Short acknowledgment text -> skipped without calling learning backend.
    {
        auto rc = (*session)->Submit("op-short", "好的");
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        auto rep = WaitForLearning(*session, rc->operation_id);
        CHECK(rep.stage == LearningStage::Skipped);
        CHECK_FALSE(rep.skip_reason.empty());
        CHECK_FALSE(rep.model_called);
        CHECK(learn_trace->calls.load() == 0);
    }

    // 2) Non-short casual question with no durable signal -> skipped without calling model.
    {
        auto rc = (*session)->Submit("op-nosig", "Please summarize what two plus two equals in one brief sentence.");
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        auto rep = WaitForLearning(*session, rc->operation_id);
        CHECK(rep.stage == LearningStage::Skipped);
        CHECK(rep.skip_reason == "no_durable_signal");
        CHECK_FALSE(rep.model_called);
        CHECK(learn_trace->calls.load() == 0);
    }

    // 3) Turn that commits memory_save -> skipped as already_mutated.
    {
        main_trace->send_memory_save = true;
        auto rc = (*session)->Submit("op-mutated", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        auto rep = WaitForLearning(*session, rc->operation_id);
        CHECK(rep.stage == LearningStage::Skipped);
        CHECK(rep.skip_reason == "already_mutated");
        CHECK_FALSE(rep.model_called);
        CHECK(learn_trace->calls.load() == 0);
    }

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("lexical-and-signal-gates");
}

TEST_CASE("SDK memory learning: single-flight busy drop") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    LearningTrace::Step held_step;
    held_step.block_until_released = true;
    sdk::ModelReply ok_reply;
    ok_reply.text = ValidExtractionPayload();
    ok_reply.usage = sdk::Usage{10, 5, 1, 0, 1};
    ok_reply.usage_observation = FiveFieldObservation(*ok_reply.usage);
    ok_reply.provider_response_id = "learn-held";
    held_step.reply = ok_reply;
    learn_trace->script.push_back(held_step);

    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());

    auto r1 = (*session)->Submit("op-busy-1", DurablePrompt());
    REQUIRE(r1.has_value());
    REQUIRE((*session)->WaitResult(r1->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    REQUIRE(learn_trace->WaitEntered());

    auto r2 = (*session)->Submit("op-busy-2", DurablePrompt());
    REQUIRE(r2.has_value());
    REQUIRE((*session)->WaitResult(r2->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    auto rep2 = (*session)->GetMemoryLearning(r2->operation_id);
    REQUIRE(rep2.has_value());
    CHECK(rep2->stage == LearningStage::BusyDropped);
    CHECK(rep2->error_code == "in_flight_dropped");
    CHECK_FALSE(rep2->model_called);
    CHECK(learn_trace->calls.load() == 1);

    learn_trace->Release();
    auto rep1 = WaitForLearning(*session, r1->operation_id);
    CHECK(rep1.stage == LearningStage::Completed);
    CHECK(learn_trace->calls.load() == 1);

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("single-flight-busy-drop");
}

TEST_CASE("SDK memory learning: worker start failure writes pre-receipt") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    ScopedHooks hooks;
    hooks.SetStart([] {
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "simulated thread start failure");
    });

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());

    auto r1 = (*session)->Submit("op-start-fail", DurablePrompt());
    REQUIRE(r1.has_value());
    auto op1 = (*session)->WaitResult(r1->operation_id, 15s);
    REQUIRE(op1.has_value());
    CHECK(op1->state == sdk::OperationState::Succeeded);

    auto rep = WaitForLearning(*session, r1->operation_id);
    CHECK(rep.stage == LearningStage::StartFailed);
    CHECK(rep.error_code == "worker_start_failed");
    CHECK_FALSE(rep.error.empty());
    CHECK_FALSE(rep.model_called);
    CHECK(learn_trace->calls.load() == 0);

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("worker-start-failure-pre-receipt");
}

TEST_CASE("SDK memory learning: worker exceptions preserve five-field usage") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    ScopedHooks hooks;

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    for (int i = 0; i < 2; ++i) {
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = ValidExtractionPayload("Conv " + std::to_string(i), "Summary " + std::to_string(i), "Content " + std::to_string(i));
        r.usage = sdk::Usage{23, 17, 4, 1, 5};
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "learn-exc-" + std::to_string(i);
        step.reply = r;
        learn_trace->script.push_back(step);
    }

    std::atomic<unsigned> store_throws{0};
    hooks.SetCandidateStore([&](const fs::path&, mem::MemoryCandidate) -> std::expected<std::string, std::string> {
        const unsigned n = store_throws.fetch_add(1);
        if (n == 0) throw std::runtime_error("boom std exception");
        throw 42;
    });

    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());

    // First operation: std::exception thrown after extraction usage is captured.
    auto r1 = (*session)->Submit("op-exc-std", DurablePrompt());
    REQUIRE(r1.has_value());
    REQUIRE((*session)->WaitResult(r1->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    auto rep1 = WaitForLearning(*session, r1->operation_id);
    CHECK(rep1.stage == LearningStage::Failed);
    CHECK(rep1.error_code == "worker_exception");
    CHECK(rep1.model_called);
    CHECK(rep1.usage.direct.total.input_tokens == 23);
    CHECK(rep1.usage.direct.total.output_tokens == 17);
    CHECK(rep1.usage.direct.total.cache_read_tokens == 4);
    CHECK(rep1.usage.direct.total.cache_creation_tokens == 1);
    CHECK(rep1.usage.direct.total.output_reasoning_tokens == 5);
    REQUIRE(rep1.usage.attempts.size() == 1);

    // Second operation: non-std exception (throw 42) still preserves all 5 usage fields.
    auto r2 = (*session)->Submit("op-exc-unknown", DurablePrompt());
    REQUIRE(r2.has_value());
    REQUIRE((*session)->WaitResult(r2->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    auto rep2 = WaitForLearning(*session, r2->operation_id);
    CHECK(rep2.stage == LearningStage::Failed);
    CHECK(rep2.error_code == "worker_unknown_exception");
    CHECK(rep2.model_called);
    CHECK(rep2.usage.direct.total.input_tokens == 23);
    CHECK(rep2.usage.direct.total.output_reasoning_tokens == 5);
    REQUIRE(rep2.usage.attempts.size() == 1);

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("worker-exceptions-preserve-usage");
}

TEST_CASE("SDK memory learning: five-field usage provenance variants") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    // 1) Reported full 5-field usage with explicit Observation.
    {
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = ValidExtractionPayload("FullObs", "Full summary", "Full content");
        r.usage = sdk::Usage{31, 14, 7, 3, 9};
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "learn-full-obs";
        step.reply = r;
        learn_trace->script.push_back(step);
    }
    // 2) Partial legacy usage (no usage_observation): cache/reasoning missing in coverage.
    {
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = ValidExtractionPayload("LegacyObs", "Legacy summary", "Legacy content");
        r.usage = sdk::Usage{29, 11, 0, 0, 0};
        r.provider_response_id = "learn-legacy-obs";
        step.reply = r;
        learn_trace->script.push_back(step);
    }
    // 3) Transport failure with zero usage observation.
    {
        LearningTrace::Step step;
        step.reply = std::unexpected(sdk::Error{"provider.transport", "connection reset by peer"});
        learn_trace->script.push_back(step);
    }

    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";
    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());

    auto r1 = (*session)->Submit("op-prov-1", DurablePrompt());
    REQUIRE(r1.has_value());
    REQUIRE((*session)->WaitResult(r1->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    auto rep1 = WaitForLearning(*session, r1->operation_id);
    CHECK(rep1.stage == LearningStage::Completed);
    REQUIRE(rep1.usage.attempts.size() == 1);
    CHECK(rep1.usage.attempts[0].reported_by_provider);
    CHECK(rep1.usage.direct.coverage.fields[2].valid == 1);
    CHECK(rep1.usage.direct.coverage.fields[2].missing == 0);
    CHECK(rep1.usage.direct.coverage.fields[4].valid == 1);

    auto r2 = (*session)->Submit("op-prov-2", DurablePrompt());
    REQUIRE(r2.has_value());
    REQUIRE((*session)->WaitResult(r2->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    auto rep2 = WaitForLearning(*session, r2->operation_id);
    CHECK(rep2.stage == LearningStage::Completed);
    REQUIRE(rep2.usage.attempts.size() == 1);
    CHECK(rep2.usage.attempts[0].reported_by_provider);
    CHECK(rep2.usage.direct.total.input_tokens == 29);
    CHECK(rep2.usage.direct.total.output_tokens == 11);
    CHECK(rep2.usage.direct.coverage.fields[2].missing == 1);
    CHECK(rep2.usage.direct.coverage.fields[4].missing == 1);

    auto r3 = (*session)->Submit("op-prov-3", DurablePrompt());
    REQUIRE(r3.has_value());
    REQUIRE((*session)->WaitResult(r3->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    auto rep3 = WaitForLearning(*session, r3->operation_id);
    CHECK(rep3.stage == LearningStage::Failed);
    CHECK(rep3.error_code == "transport_failed");
    CHECK_FALSE(rep3.error.empty());
    CHECK(rep3.usage.attempts.empty());

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("five-field-usage-provenance-variants");
}

TEST_CASE("SDK memory learning: candidate store failure preserves usage") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    ScopedHooks hooks;
    hooks.SetCandidateStore([](const fs::path&, mem::MemoryCandidate) -> std::expected<std::string, std::string> {
        return std::unexpected("disk full writing candidate");
    });

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());

    auto rc = (*session)->Submit("op-store-fail", DurablePrompt());
    REQUIRE(rc.has_value());
    REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    auto rep = WaitForLearning(*session, rc->operation_id);
    CHECK(rep.stage == LearningStage::Failed);
    CHECK(rep.error_code == "sdk.memory_learning.candidate_store_failed");
    CHECK(rep.error == "disk full writing candidate");
    CHECK(rep.candidate_ids.empty());
    CHECK(rep.model_called);
    CHECK(rep.usage.direct.total.input_tokens == 19);
    CHECK(rep.usage.direct.total.output_tokens == 13);
    CHECK(rep.usage.direct.total.cache_read_tokens == 5);
    CHECK(rep.usage.direct.total.output_reasoning_tokens == 2);
    REQUIRE(rep.usage.attempts.size() == 1);

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("candidate-store-failure-preserves-usage");
}

TEST_CASE("SDK memory learning: cancellation stops publication and keeps observed usage") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    LearningTrace::Step step;
    step.block_until_cancel = true;
    step.return_reply_on_cancel = true;
    sdk::ModelReply late_reply;
    late_reply.text = ValidExtractionPayload();
    late_reply.usage = sdk::Usage{7, 5, 1, 0, 1};
    late_reply.usage_observation = FiveFieldObservation(*late_reply.usage);
    late_reply.provider_response_id = "learn-late-cancel";
    step.reply = late_reply;
    learn_trace->script.push_back(step);

    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());
    const fs::path mem_dir = MemoryDir(*session);

    auto rc = (*session)->Submit("op-cancel-post", DurablePrompt());
    REQUIRE(rc.has_value());
    REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    REQUIRE(learn_trace->WaitEntered());

    REQUIRE((*session)->Close().has_value());
    auto rep = (*session)->GetMemoryLearning(rc->operation_id);
    REQUIRE(rep.has_value());
    CHECK(rep->stage == LearningStage::Cancelled);
    CHECK(rep->error_code == "cancelled");
    CHECK(rep->candidate_ids.empty());
    CHECK(mem::candidates::ListCandidates(mem_dir).empty());
    CHECK(rep->usage.direct.total.input_tokens == 7);
    CHECK(rep->usage.direct.total.output_tokens == 5);
    CHECK(rep->usage.direct.total.cache_read_tokens == 1);
    CHECK(rep->usage.direct.total.output_reasoning_tokens == 1);

    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("cancellation-stops-publication-keeps-usage");
}

TEST_CASE("SDK memory learning: concurrent Close joins worker before destroying Backend") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    LearningTrace::Step step;
    step.block_until_cancel = true;
    step.return_reply_on_cancel = false;
    learn_trace->script.push_back(step);

    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());

    auto rc = (*session)->Submit("op-close-join", DurablePrompt());
    REQUIRE(rc.has_value());
    REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    REQUIRE(learn_trace->WaitEntered());

    REQUIRE((*session)->Close().has_value());
    CHECK(learn_trace->in_generate.load() == 0);
    CHECK(learn_trace->destroyed.load());
    CHECK_FALSE(learn_trace->destroyed_while_active.load());

    auto rep = (*session)->GetMemoryLearning(rc->operation_id);
    REQUIRE(rep.has_value());
    CHECK(rep->stage == LearningStage::Cancelled);

    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("concurrent-close-and-backend-destruction");
}

TEST_CASE("SDK memory learning: same-ID zero-replay recovery and running recovery") {
    Fixture fixture;
    std::string session_id;
    std::string op_completed_id;
    std::string op_running_id;
    fs::path session_dir;

    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";
    learn_opts.max_candidates = 2;

    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
        REQUIRE(session.has_value());
        session_id = (*session)->id();
        session_dir = SessionDir(*session);

        auto r1 = (*session)->Submit("op-rec-1", DurablePrompt());
        REQUIRE(r1.has_value());
        op_completed_id = r1->operation_id;
        REQUIRE((*session)->WaitResult(op_completed_id, 15s)->state == sdk::OperationState::Succeeded);
        CHECK(WaitForLearning(*session, op_completed_id).stage == LearningStage::Completed);

        auto r2 = (*session)->Submit("op-rec-2", "好的");
        REQUIRE(r2.has_value());
        op_running_id = r2->operation_id;
        REQUIRE((*session)->WaitResult(op_running_id, 15s)->state == sdk::OperationState::Succeeded);
        CHECK(WaitForLearning(*session, op_running_id).stage == LearningStage::Skipped);

        REQUIRE((*session)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // Reopen with drifted LearningOptions -> rejected cleanly.
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        LearningOptions drifted = learn_opts;
        drifted.max_candidates = 3;
        auto opts = MakeBaseOptions(fixture, std::make_shared<MainTrace>(), drifted, std::make_shared<LearningTrace>());
        opts.resume_session_id = session_id;
        auto bad = (*runtime)->OpenSession(std::move(opts));
        REQUIRE_FALSE(bad.has_value());
        CHECK(bad.error().code == "sdk.memory_learning.resume_mismatch");
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // Simulate crash-interrupted worker leaving op_running_id report in "running" state on disk.
    {
        const fs::path rep_path = session_dir / "sdk-memory-learnings" / (op_running_id + ".json");
        auto doc = Json::parse(ReadText(rep_path));
        doc.erase("sha256");
        doc["stage"] = "running";
        doc["skipReason"] = "";
        doc["sha256"] = lubancode::platform::Sha256Hex(doc.dump());
        WriteText(rep_path, doc.dump());
    }

    // Reopen with matching options -> zero model replay, completed restored, running -> Indeterminate.
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.resume_session_id = session_id;
        auto resumed = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(resumed.has_value());
        CHECK((*resumed)->id() == session_id);
        CHECK(main_trace->calls.load() == 0);
        CHECK(learn_trace->calls.load() == 0);

        auto rep1 = (*resumed)->GetMemoryLearning(op_completed_id);
        REQUIRE(rep1.has_value());
        CHECK(rep1->stage == LearningStage::Completed);
        CHECK(rep1->candidate_ids.size() == 1);
        CHECK(rep1->usage.direct.total.input_tokens == 19);

        auto rep2 = (*resumed)->GetMemoryLearning(op_running_id);
        REQUIRE(rep2.has_value());
        CHECK(rep2->stage == LearningStage::Indeterminate);
        CHECK(rep2->error_code == "sdk.memory_learning.interrupted");
        CHECK_FALSE(rep2->usage.attempts_complete);

        REQUIRE((*resumed)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    PrintPathMarker("same-id-zero-replay-recovery");
}

TEST_CASE("SDK memory learning: Off mode rejects candidate query, edit, reject, and accept") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    auto main_trace = std::make_shared<MainTrace>();
    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace));
    REQUIRE(session.has_value());

    const auto list_res = (*session)->ListMemoryCandidates();
    REQUIRE_FALSE(list_res.has_value());
    CHECK(list_res.error().code == "sdk.memory_learning.disabled");

    const auto get_res = (*session)->GetMemoryCandidate("cand-1");
    REQUIRE_FALSE(get_res.has_value());
    CHECK(get_res.error().code == "sdk.memory_learning.disabled");

    const auto edit_res = (*session)->EditMemoryCandidate("cand-1", "Title", "Content");
    REQUIRE_FALSE(edit_res.has_value());
    CHECK(edit_res.error().code == "sdk.memory_learning.disabled");

    const auto reject_res = (*session)->RejectMemoryCandidate("cand-1", "reason");
    REQUIRE_FALSE(reject_res.has_value());
    CHECK(reject_res.error().code == "sdk.memory_learning.disabled");

    const auto accept_res = (*session)->AcceptMemoryCandidate("cand-1");
    REQUIRE_FALSE(accept_res.has_value());
    CHECK(accept_res.error().code == "sdk.memory_learning.disabled");

    const auto accepted_res = (*session)->GetAcceptedMemoryCandidates();
    REQUIRE(accepted_res.has_value());
    CHECK(accepted_res->empty());

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("candidate-off-mode-rejected");
}

TEST_CASE("SDK memory learning: candidate query, edit, validation, and reject") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    {
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = ValidExtractionPayload(
            "Initial Ninja Rule",
            "Prefer ninja builds",
            "Use ninja for fast incremental builds.");
        r.usage = sdk::Usage{19, 13, 5, 1, 2};
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "learn-cand-edit";
        step.reply = r;
        learn_trace->script.push_back(step);
    }
    {
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = ValidExtractionPayload(
            "Updated Ninja Rule",
            "Prefer ninja builds always",
            "Re-suggested after rejection.");
        r.usage = sdk::Usage{17, 11, 3, 0, 1};
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "learn-cand-readd-blocked";
        step.reply = r;
        learn_trace->script.push_back(step);
    }

    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Review;
    learn_opts.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());

    // Initially empty workspace has no candidates yet.
    const auto initial_list = (*session)->ListMemoryCandidates();
    REQUIRE(initial_list.has_value());
    CHECK(initial_list->empty());

    auto r1 = (*session)->Submit("op-cand-1", DurablePrompt());
    REQUIRE(r1.has_value());
    REQUIRE((*session)->WaitResult(r1->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    const auto rep1 = WaitForLearning(*session, r1->operation_id);
    REQUIRE(rep1.stage == LearningStage::Completed);
    REQUIRE(rep1.candidate_ids.size() == 1);
    const std::string cand_id = rep1.candidate_ids.front();

    // List and Get reflect extracted candidate fields.
    const auto listed = (*session)->ListMemoryCandidates();
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().id == cand_id);
    CHECK(listed->front().kind == "preference");
    CHECK(listed->front().title == "Initial Ninja Rule");
    CHECK(listed->front().confidence == "user-stated");

    const auto fetched = (*session)->GetMemoryCandidate(cand_id);
    REQUIRE(fetched.has_value());
    CHECK(*fetched == listed->front());

    // Malformed / path-escaping IDs and missing IDs fail cleanly.
    const auto bad_escape = (*session)->GetMemoryCandidate("../rejected");
    REQUIRE_FALSE(bad_escape.has_value());
    CHECK(bad_escape.error().code == "sdk.memory_learning.invalid_input");

    const auto bad_reserved = (*session)->GetMemoryCandidate("rejected");
    REQUIRE_FALSE(bad_reserved.has_value());
    CHECK(bad_reserved.error().code == "sdk.memory_learning.invalid_input");

    const auto missing = (*session)->GetMemoryCandidate("cand-does-not-exist");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == "sdk.memory_learning.candidate_not_found");

    // Empty edit fails; valid edit updates title and content while keeping topic in inbox.
    const auto empty_edit = (*session)->EditMemoryCandidate(cand_id, "", "");
    REQUIRE_FALSE(empty_edit.has_value());
    CHECK(empty_edit.error().code == "sdk.memory_learning.invalid_input");

    const auto edited = (*session)->EditMemoryCandidate(
        cand_id, "Updated Ninja Rule", "Always run cmake with Ninja generator.");
    REQUIRE(edited.has_value());
    CHECK(edited->id == cand_id);
    CHECK(edited->title == "Updated Ninja Rule");
    CHECK(edited->content == "Always run cmake with Ninja generator.");
    CHECK(edited->kind == "preference");
    CHECK(edited->confidence == "user-stated");

    const auto after_edit = (*session)->GetMemoryCandidate(cand_id);
    REQUIRE(after_edit.has_value());
    CHECK(*after_edit == *edited);

    // Reject removes candidate and blocks re-adding the same topic via rejected.json.
    const auto rejected = (*session)->RejectMemoryCandidate(cand_id, "obsolete preference");
    REQUIRE(rejected.has_value());
    const auto after_reject = (*session)->GetMemoryCandidate(cand_id);
    REQUIRE_FALSE(after_reject.has_value());
    CHECK(after_reject.error().code == "sdk.memory_learning.candidate_not_found");
    CHECK((*session)->ListMemoryCandidates()->empty());

    auto r2 = (*session)->Submit("op-cand-2", DurablePrompt());
    REQUIRE(r2.has_value());
    REQUIRE((*session)->WaitResult(r2->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    const auto rep2 = WaitForLearning(*session, r2->operation_id);
    CHECK(rep2.stage == LearningStage::Failed);
    CHECK(rep2.error_code == "sdk.memory_learning.candidate_store_failed");
    CHECK((*session)->ListMemoryCandidates()->empty());

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("candidate-query-edit-reject");
}

TEST_CASE("SDK memory learning: accept preconditions and business rules") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    // First verify write_disabled rejection when memory_write is not enabled.
    {
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = ValidExtractionPayload();
        r.usage = sdk::Usage{15, 11, 2, 0, 1};
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "learn-no-write";
        step.reply = r;
        learn_trace->script.push_back(step);

        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Review;
        learn_opts.model = "extract-model";

        auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
        REQUIRE(session.has_value());

        auto rc = (*session)->Submit("op-no-write", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep = WaitForLearning(*session, rc->operation_id);
        REQUIRE(rep.candidate_ids.size() == 1);

        const auto accept_res = (*session)->AcceptMemoryCandidate(rep.candidate_ids.front());
        REQUIRE_FALSE(accept_res.has_value());
        CHECK(accept_res.error().code == "sdk.memory_learning.write_disabled");
        CHECK((*session)->ListMemoryCandidates()->size() == 1);

        REQUIRE((*session)->Close().has_value());
    }

    // Second verify business rules with memory_write enabled:
    // - inferred candidate rejected
    // - fact candidate without paths rejected
    // - feedback candidate with confidence != user-stated rejected
    {
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = Json{
            {"task_type", "config"},
            {"summary", "Mixed candidate rule validation"},
            {"candidates", Json::array({
                Json{
                    {"kind", "preference"},
                    {"title", "Inferred pref"},
                    {"summary", "Inferred preference"},
                    {"content", "Should not promote inferred confidence."},
                    {"keywords", Json::array({"inferred"})},
                    {"paths", Json::array({"cmake/LubanCore.cmake"})},
                    {"confidence", "inferred"},
                },
                Json{
                    {"kind", "fact"},
                    {"title", "Fact missing evidence"},
                    {"summary", "Fact without paths"},
                    {"content", "Fact claims file behavior with empty paths."},
                    {"keywords", Json::array({"missing-paths"})},
                    {"paths", Json::array()},
                    {"confidence", "verified"},
                },
                Json{
                    {"kind", "feedback"},
                    {"title", "Verified feedback"},
                    {"summary", "Feedback without user-stated"},
                    {"content", "Feedback requires user-stated confidence."},
                    {"keywords", Json::array({"feedback"})},
                    {"paths", Json::array({"cmake/LubanCore.cmake"})},
                    {"confidence", "verified"},
                },
            })},
            {"retrieval_terms", Json::array({"inferred"})},
        }.dump();
        r.usage = sdk::Usage{21, 15, 4, 1, 2};
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "learn-rules";
        step.reply = r;
        learn_trace->script.push_back(step);

        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Review;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.memory_write = sdk::memory::v1::WriteOptions{true};
        auto session = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(session.has_value());

        auto rc = (*session)->Submit("op-rules", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep = WaitForLearning(*session, rc->operation_id);
        REQUIRE(rep.stage == LearningStage::Completed);
        const auto cands = (*session)->ListMemoryCandidates();
        REQUIRE(cands.has_value());
        REQUIRE(cands->size() >= 3);

        for (const auto& c : *cands) {
            if (c.confidence == "inferred") {
                const auto res = (*session)->AcceptMemoryCandidate(c.id);
                REQUIRE_FALSE(res.has_value());
                CHECK(res.error().code == "sdk.memory_learning.candidate_inferred");
            } else if (c.kind == "fact" && c.paths.empty()) {
                const auto res = (*session)->AcceptMemoryCandidate(c.id);
                REQUIRE_FALSE(res.has_value());
                CHECK(res.error().code == "sdk.memory_learning.candidate_missing_evidence");
            } else if (c.kind == "feedback" && c.confidence != "user-stated") {
                const auto res = (*session)->AcceptMemoryCandidate(c.id);
                REQUIRE_FALSE(res.has_value());
                CHECK(res.error().code == "sdk.memory_learning.candidate_invalid");
            }
        }
        CHECK((*session)->GetAcceptedMemoryCandidates()->empty());
        REQUIRE((*session)->Close().has_value());
    }

    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("candidate-accept-validation");
}

TEST_CASE("SDK memory learning: accept commits through project gate, removes candidate, and recalls cleanly") {
    Fixture fixture;
    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());

    std::string memory_id;
    {
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = Json{
            {"task_type", "config"},
            {"summary", "CMake ninja build rule fact"},
            {"candidates", Json::array({
                Json{
                    {"kind", "fact"},
                    {"title", "LubanCore build target fact"},
                    {"summary", "LubanCore defines the core SDK target"},
                    {"content", "Always verify cmake/LubanCore.cmake exports when changing Session API."},
                    {"keywords", Json::array({"LubanCore", "Session", "cmake"})},
                    {"paths", Json::array({"cmake/LubanCore.cmake"})},
                    {"confidence", "verified"},
                },
            })},
            {"retrieval_terms", Json::array({"LubanCore", "Session"})},
        }.dump();
        r.usage = sdk::Usage{23, 17, 6, 1, 3};
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "learn-accept-fact";
        step.reply = r;
        learn_trace->script.push_back(step);

        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Review;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.memory_write = sdk::memory::v1::WriteOptions{true};
        auto session = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(session.has_value());

        auto rc = (*session)->Submit("op-accept", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep = WaitForLearning(*session, rc->operation_id);
        REQUIRE(rep.stage == LearningStage::Completed);
        REQUIRE(rep.candidate_ids.size() == 1);
        const std::string cand_id = rep.candidate_ids.front();

        const auto accept = (*session)->AcceptMemoryCandidate(cand_id);
        REQUIRE(accept.has_value());
        CHECK(accept->state == "committed");
        CHECK(accept->candidate_id == cand_id);
        CHECK(accept->session_id == (*session)->id());
        CHECK_FALSE(accept->workspace_key.empty());
        CHECK_FALSE(accept->learning_plan_sha256.empty());
        CHECK_FALSE(accept->write_plan_sha256.empty());
        CHECK(accept->commit_key.starts_with("sdk-cand-"));
        CHECK_FALSE(accept->memory_id.empty());
        CHECK_FALSE(accept->memory_path.empty());
        CHECK_FALSE(accept->content_sha256.empty());
        CHECK_FALSE(accept->committed_at.empty());
        REQUIRE_FALSE(accept->stages.empty());
        CHECK(accept->stages.back() == sdk::memory::v1::SaveStageReceipt{"result", "durable"});
        memory_id = accept->memory_id;

        // Candidate was removed cleanly from the review box.
        const auto after_get = (*session)->GetMemoryCandidate(cand_id);
        REQUIRE_FALSE(after_get.has_value());
        CHECK(after_get.error().code == "sdk.memory_learning.candidate_not_found");
        CHECK((*session)->ListMemoryCandidates()->empty());

        // Duplicate accept of same candidate rejects cleanly without duplicating commit.
        const auto dup = (*session)->AcceptMemoryCandidate(cand_id);
        REQUIRE_FALSE(dup.has_value());
        CHECK(dup.error().code == "sdk.memory_learning.already_committed");

        const auto accepted_list = (*session)->GetAcceptedMemoryCandidates();
        REQUIRE(accepted_list.has_value());
        REQUIRE(accepted_list->size() == 1);
        CHECK(accepted_list->front() == *accept);

        REQUIRE((*session)->Close().has_value());
    }

    // New session with recall enabled retrieves the committed topic from project memory.
    {
        auto main_trace = std::make_shared<MainTrace>();
        auto opts = MakeBaseOptions(fixture, main_trace);
        opts.memory = sdk::memory::v1::RecallOptions{};
        auto recall_session = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(recall_session.has_value());

        auto r = (*recall_session)->Submit("op-recall", "Check cmake/LubanCore.cmake Session exports.");
        REQUIRE(r.has_value());
        REQUIRE((*recall_session)->WaitResult(r->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto recall = (*recall_session)->GetMemoryRecall(r->operation_id);
        REQUIRE(recall.has_value());
        CHECK(recall->state == "admitted");
        CHECK(std::any_of(recall->entries.begin(), recall->entries.end(), [&](const auto& entry) {
            return entry.id == memory_id;
        }));

        REQUIRE((*recall_session)->Close().has_value());
    }

    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("candidate-accept-commit-and-recall");
}

TEST_CASE("SDK memory learning: accept receipt durability, Close, and same-ID recovery") {
    Fixture fixture;
    std::string session_id;
    std::string cand_id;
    sdk::memory::v1::CandidateAcceptReport expected_accept;
    fs::path lifecycle_dir;
    std::string workspace_key;

    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());

        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = ValidExtractionPayload(
            "Recovery Convention",
            "Always persist durable receipts",
            "Verify recovery without replay.");
        r.usage = sdk::Usage{19, 13, 5, 1, 2};
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "learn-accept-recover";
        step.reply = r;
        learn_trace->script.push_back(step);

        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Review;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.memory_write = sdk::memory::v1::WriteOptions{true};
        auto session = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(session.has_value());
        session_id = (*session)->id();
        workspace_key = (*session)->DescribeMemoryLearning()->workspace_key;

        auto rc = (*session)->Submit("op-recover", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep = WaitForLearning(*session, rc->operation_id);
        REQUIRE(rep.candidate_ids.size() == 1);
        cand_id = rep.candidate_ids.front();

        const auto accepted = (*session)->AcceptMemoryCandidate(cand_id);
        REQUIRE(accepted.has_value());
        REQUIRE(accepted->state == "committed");
        expected_accept = *accepted;
        lifecycle_dir = MemoryDir(*session).parent_path() / "lifecycle";

        REQUIRE((*session)->Close().has_value());

        // Post-Close: pure owned receipts and read-only queries remain available; mutators reject.
        const auto post_close_accepted = (*session)->GetAcceptedMemoryCandidates();
        REQUIRE(post_close_accepted.has_value());
        REQUIRE(post_close_accepted->size() == 1);
        CHECK(post_close_accepted->front() == expected_accept);

        const auto post_close_list = (*session)->ListMemoryCandidates();
        REQUIRE(post_close_list.has_value());
        CHECK(post_close_list->empty());

        const auto edit_closed = (*session)->EditMemoryCandidate(cand_id, "X", "Y");
        REQUIRE_FALSE(edit_closed.has_value());
        CHECK(edit_closed.error().code == "sdk.session.closed");

        const auto reject_closed = (*session)->RejectMemoryCandidate(cand_id, "reason");
        REQUIRE_FALSE(reject_closed.has_value());
        CHECK(reject_closed.error().code == "sdk.session.closed");

        const auto accept_closed = (*session)->AcceptMemoryCandidate(cand_id);
        REQUIRE_FALSE(accept_closed.has_value());
        CHECK(accept_closed.error().code == "sdk.session.closed");

        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // Same-ID OpenSession restores accepted receipts with 0 learning model replay.
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());

        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Review;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.resume_session_id = session_id;
        opts.memory_write = sdk::memory::v1::WriteOptions{true};
        auto resumed = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(resumed.has_value());

        CHECK(learn_trace->calls.load() == 0);
        const auto restored = (*resumed)->GetAcceptedMemoryCandidates();
        REQUIRE(restored.has_value());
        REQUIRE(restored->size() == 1);
        CHECK(restored->front() == expected_accept);

        const auto dup = (*resumed)->AcceptMemoryCandidate(cand_id);
        REQUIRE_FALSE(dup.has_value());
        CHECK(dup.error().code == "sdk.memory_learning.already_committed");

        REQUIRE((*resumed)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // Tampered snapshot or accept receipt is caught on OpenSession resume.
    {
        const fs::path snapshot_path =
            lifecycle_dir / expected_accept.commit_key / "topic.snapshot.md";
        const std::string original_snapshot = ReadText(snapshot_path);
        WriteText(snapshot_path, original_snapshot + "\nTAMPERED\n");

        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Review;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.resume_session_id = session_id;
        opts.memory_write = sdk::memory::v1::WriteOptions{true};
        const auto tampered_open = (*runtime)->OpenSession(std::move(opts));
        REQUIRE_FALSE(tampered_open.has_value());

        REQUIRE((*runtime)->Shutdown().has_value());
    }

    PrintPathMarker("candidate-accept-resume-and-close");
}

TEST_CASE("SDK memory learning: accept stage failure marks indeterminate, keeps candidate, and stops") {
    Fixture fixture;
    ScopedHooks hooks;
    std::string session_id;
    std::string cand_topic_fail;
    std::string cand_result_fail;

    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());

        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        {
            LearningTrace::Step step;
            sdk::ModelReply r;
            r.text = Json{
                {"task_type", "config"},
                {"summary", "Two candidates for commit stage tests"},
                {"candidates", Json::array({
                    Json{
                        {"kind", "preference"},
                        {"title", "Topic Fail Candidate"},
                        {"summary", "Fails cleanly at Topic stage"},
                        {"content", "Content for topic stage failure."},
                        {"keywords", Json::array({"topic-fail"})},
                        {"paths", Json::array({"cmake/LubanCore.cmake"})},
                        {"confidence", "user-stated"},
                    },
                    Json{
                        {"kind", "preference"},
                        {"title", "Result Fail Candidate"},
                        {"summary", "Fails at Result stage -> Indeterminate"},
                        {"content", "Content for result stage failure."},
                        {"keywords", Json::array({"result-fail"})},
                        {"paths", Json::array({"cmake/LubanCore.cmake"})},
                        {"confidence", "user-stated"},
                    },
                })},
                {"retrieval_terms", Json::array({"topic-fail"})},
            }.dump();
            r.usage = sdk::Usage{23, 17, 5, 1, 2};
            r.usage_observation = FiveFieldObservation(*r.usage);
            r.provider_response_id = "learn-stage-fail";
            step.reply = r;
            learn_trace->script.push_back(step);
        }

        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Review;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.memory_write = sdk::memory::v1::WriteOptions{true};
        auto session = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(session.has_value());
        session_id = (*session)->id();

        auto rc = (*session)->Submit("op-stage-fail", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep = WaitForLearning(*session, rc->operation_id);
        REQUIRE(rep.candidate_ids.size() == 2);
        cand_topic_fail = rep.candidate_ids[0];
        cand_result_fail = rep.candidate_ids[1];

        // 1) Inject Topic failure (NotCommitted): returns not_started, keeps candidate intact, does not stop.
        hooks.SetCommitWrite([](
            const fs::path& path,
            std::string_view content,
            lubancode::platform::WriteDurability durability)
                -> std::expected<lubancode::platform::AtomicWriteReceipt, lubancode::platform::AtomicWriteError> {
            const auto parent = path.parent_path().filename().string();
            if ((parent == "preferences" || parent == "facts" || parent == "feedback") &&
                path.extension() == ".md") {
                return std::unexpected(lubancode::platform::AtomicWriteError{
                    "atomic.replace_failed",
                    "simulated topic write failure",
                    lubancode::platform::WriteOutcome::NotCommitted,
                    lubancode::platform::WriteFailureKind::Permanent,
                });
            }
            return lubancode::platform::AtomicWriteFile(path, content, durability);
        });

        const auto topic_res = (*session)->AcceptMemoryCandidate(cand_topic_fail);
        REQUIRE(topic_res.has_value());
        CHECK(topic_res->state == "not_started");
        CHECK_FALSE(topic_res->error_code.empty());
        CHECK((*session)->GetMemoryCandidate(cand_topic_fail).has_value());

        // 2) Inject Result write failure -> Indeterminate: keeps candidate intact and stops further accepts.
        hooks.SetCommitWrite([](
            const fs::path& path,
            std::string_view content,
            lubancode::platform::WriteDurability durability)
                -> std::expected<lubancode::platform::AtomicWriteReceipt, lubancode::platform::AtomicWriteError> {
            if (path.filename() == "result.json") {
                return std::unexpected(lubancode::platform::AtomicWriteError{
                    "atomic.replace_failed",
                    "simulated result write failure",
                    lubancode::platform::WriteOutcome::NotCommitted,
                    lubancode::platform::WriteFailureKind::Permanent,
                });
            }
            return lubancode::platform::AtomicWriteFile(path, content, durability);
        });

        const auto result_res = (*session)->AcceptMemoryCandidate(cand_result_fail);
        REQUIRE(result_res.has_value());
        CHECK(result_res->state == "indeterminate");
        CHECK((*session)->GetMemoryCandidate(cand_result_fail).has_value());

        // Clear hook; subsequent accepts still refuse cleanly with sdk.memory_learning.stopped.
        hooks.SetCommitWrite({});
        const auto stopped = (*session)->AcceptMemoryCandidate(cand_result_fail);
        REQUIRE_FALSE(stopped.has_value());
        CHECK(stopped.error().code == "sdk.memory_learning.stopped");

        REQUIRE((*session)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // Same-ID recovery preserves the indeterminate stop state across restart.
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());

        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Review;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.resume_session_id = session_id;
        opts.memory_write = sdk::memory::v1::WriteOptions{true};
        auto resumed = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(resumed.has_value());

        const auto stopped_resumed = (*resumed)->AcceptMemoryCandidate(cand_result_fail);
        REQUIRE_FALSE(stopped_resumed.has_value());
        CHECK(stopped_resumed.error().code == "sdk.memory_learning.stopped");

        REQUIRE((*resumed)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    PrintPathMarker("candidate-accept-indeterminate-stop");
}

TEST_CASE("SDK memory learning: Auto eligibility gate commits eligible items and routes others to candidate review") {
    Fixture fixture;
    std::string session_id;
    std::string op1_id;

    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());

        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        // Turn 1: verified fact+paths (auto), user-stated feedback (auto), inferred preference (review fallback).
        {
            LearningTrace::Step step;
            sdk::ModelReply r;
            r.text = Json{
                {"task_type", "config"},
                {"summary", "Mixed auto-eligible and fallback candidates"},
                {"candidates", Json::array({
                    Json{
                        {"kind", "fact"},
                        {"title", "Auto Verified Fact"},
                        {"summary", "Fact with verified confidence and path"},
                        {"content", "Verify CMake targets in cmake/LubanCore.cmake."},
                        {"keywords", Json::array({"cmake", "fact"})},
                        {"paths", Json::array({"cmake/LubanCore.cmake"})},
                        {"confidence", "verified"},
                    },
                    Json{
                        {"kind", "feedback"},
                        {"title", "Auto User Feedback"},
                        {"summary", "User-stated feedback auto commits"},
                        {"content", "Never skip ninja checks before submitting patches."},
                        {"keywords", Json::array({"ninja", "feedback"})},
                        {"paths", Json::array()},
                        {"confidence", "user-stated"},
                    },
                    Json{
                        {"kind", "preference"},
                        {"title", "Fallback Inferred Preference"},
                        {"summary", "Inferred preference must fall back to review"},
                        {"content", "Prefer concise build summaries."},
                        {"keywords", Json::array({"summary"})},
                        {"paths", Json::array({"cmake/LubanCore.cmake"})},
                        {"confidence", "inferred"},
                    },
                })},
                {"retrieval_terms", Json::array({"LubanCore", "ninja"})},
            }.dump();
            r.usage = sdk::Usage{25, 19, 4, 1, 2};
            r.usage_observation = FiveFieldObservation(*r.usage);
            r.provider_response_id = "learn-auto-mix-1";
            step.reply = r;
            learn_trace->script.push_back(step);
        }
        // Turn 2: verified fact WITHOUT paths (fallback), verified feedback (fallback), verified preference (auto).
        {
            LearningTrace::Step step;
            sdk::ModelReply r;
            r.text = Json{
                {"task_type", "config"},
                {"summary", "Edge eligibility cases"},
                {"candidates", Json::array({
                    Json{
                        {"kind", "fact"},
                        {"title", "Fallback Fact No Paths"},
                        {"summary", "Fact without paths cannot auto commit"},
                        {"content", "Fact missing evidence paths."},
                        {"keywords", Json::array({"fact"})},
                        {"paths", Json::array()},
                        {"confidence", "verified"},
                    },
                    Json{
                        {"kind", "feedback"},
                        {"title", "Fallback Verified Feedback"},
                        {"summary", "Feedback with verified instead of user-stated falls back"},
                        {"content", "Feedback needing user-stated confidence."},
                        {"keywords", Json::array({"feedback"})},
                        {"paths", Json::array()},
                        {"confidence", "verified"},
                    },
                    Json{
                        {"kind", "preference"},
                        {"title", "Auto Verified Preference"},
                        {"summary", "Preference with verified confidence auto commits"},
                        {"content", "Always keep Auto mode scope restricted to project."},
                        {"keywords", Json::array({"preference"})},
                        {"paths", Json::array()},
                        {"confidence", "verified"},
                    },
                })},
                {"retrieval_terms", Json::array({"project"})},
            }.dump();
            r.usage = sdk::Usage{21, 15, 3, 1, 1};
            r.usage_observation = FiveFieldObservation(*r.usage);
            r.provider_response_id = "learn-auto-mix-2";
            step.reply = r;
            learn_trace->script.push_back(step);
        }
        // Turn 3: empty confidence preference -> must fall back to review candidate store.
        {
            LearningTrace::Step step;
            sdk::ModelReply r;
            r.text = Json{
                {"task_type", "config"},
                {"summary", "Empty confidence fallback"},
                {"candidates", Json::array({
                    Json{
                        {"kind", "preference"},
                        {"title", "Fallback Empty Confidence Preference"},
                        {"summary", "Empty confidence preference falls back"},
                        {"content", "Candidate with empty confidence string."},
                        {"keywords", Json::array({"empty"})},
                        {"paths", Json::array()},
                        {"confidence", ""},
                    },
                })},
                {"retrieval_terms", Json::array({"empty"})},
            }.dump();
            r.usage = sdk::Usage{17, 11, 2, 0, 1};
            r.usage_observation = FiveFieldObservation(*r.usage);
            r.provider_response_id = "learn-auto-mix-3";
            step.reply = r;
            learn_trace->script.push_back(step);
        }

        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";

        auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
        REQUIRE(session.has_value());
        session_id = (*session)->id();
        CHECK((*session)->DescribeMemoryLearning()->mode == LearningMode::Auto);
        CHECK((*session)->DescribeMemoryLearning()->auto_write_enabled);

        auto r1 = (*session)->Submit("op-auto-1", DurablePrompt());
        REQUIRE(r1.has_value());
        op1_id = r1->operation_id;
        REQUIRE((*session)->WaitResult(r1->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep1 = WaitForLearning(*session, r1->operation_id);
        REQUIRE(rep1.stage == LearningStage::Completed);
        CHECK(rep1.candidate_ids.size() == 1);
        REQUIRE(rep1.auto_writes.size() == 3);

        CHECK(rep1.auto_writes[0].queue_stage == "completed");
        CHECK(rep1.auto_writes[0].worker_stage == "finished");
        CHECK(rep1.auto_writes[0].admission_stage == "admitted");
        CHECK(rep1.auto_writes[0].commit_state == "committed");
        CHECK(rep1.auto_writes[0].commit_key.starts_with("sdk-learn-"));
        CHECK_FALSE(rep1.auto_writes[0].source_event_ref.empty());
        CHECK_FALSE(rep1.auto_writes[0].stages.empty());
        CHECK(rep1.auto_writes[0].stages.back() == sdk::memory::v1::SaveStageReceipt{"result", "durable"});

        CHECK(rep1.auto_writes[1].queue_stage == "completed");
        CHECK(rep1.auto_writes[1].commit_state == "committed");

        CHECK(rep1.auto_writes[2].queue_stage == "not_queued");
        CHECK(rep1.auto_writes[2].admission_stage == "denied");
        CHECK(rep1.auto_writes[2].commit_state == "not_started");
        CHECK(rep1.auto_writes[2].fallback_candidate_id == rep1.candidate_ids.front());

        auto r2 = (*session)->Submit("op-auto-2", DurablePrompt());
        REQUIRE(r2.has_value());
        REQUIRE((*session)->WaitResult(r2->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep2 = WaitForLearning(*session, r2->operation_id);
        REQUIRE(rep2.stage == LearningStage::Completed);
        CHECK(rep2.candidate_ids.size() == 2);
        REQUIRE(rep2.auto_writes.size() == 3);
        CHECK(rep2.auto_writes[0].queue_stage == "not_queued");
        CHECK(rep2.auto_writes[0].fallback_candidate_id == rep2.candidate_ids[0]);
        CHECK(rep2.auto_writes[1].queue_stage == "not_queued");
        CHECK(rep2.auto_writes[1].fallback_candidate_id == rep2.candidate_ids[1]);
        CHECK(rep2.auto_writes[2].queue_stage == "completed");
        CHECK(rep2.auto_writes[2].commit_state == "committed");

        auto r3 = (*session)->Submit("op-auto-3", DurablePrompt());
        REQUIRE(r3.has_value());
        REQUIRE((*session)->WaitResult(r3->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep3 = WaitForLearning(*session, r3->operation_id);
        REQUIRE(rep3.stage == LearningStage::Completed);
        CHECK(rep3.candidate_ids.size() == 1);
        REQUIRE(rep3.auto_writes.size() == 1);
        CHECK(rep3.auto_writes[0].queue_stage == "not_queued");
        CHECK(rep3.auto_writes[0].fallback_candidate_id == rep3.candidate_ids.front());

        // All 3 committed topics exist in project memory; 4 ineligible candidates live in candidate store.
        const auto mem_dir = MemoryDir(*session);
        CHECK(fs::exists(mem_dir / "facts" / "auto-verified-fact.md"));
        CHECK(fs::exists(mem_dir / "feedback" / "auto-user-feedback.md"));
        CHECK(fs::exists(mem_dir / "preferences" / "auto-verified-preference.md"));

        const auto cands = (*session)->ListMemoryCandidates();
        REQUIRE(cands.has_value());
        CHECK(cands->size() == 4);

        REQUIRE((*session)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // Same-ID recovery preserves Auto receipts with zero model replay.
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.resume_session_id = session_id;
        auto resumed = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(resumed.has_value());
        CHECK(learn_trace->calls.load() == 0);
        const auto rep1 = (*resumed)->GetMemoryLearning(op1_id);
        REQUIRE(rep1.has_value());
        CHECK(rep1->stage == LearningStage::Completed);
        CHECK(rep1->auto_writes.size() == 3);
        CHECK(rep1->auto_writes[0].commit_state == "committed");

        REQUIRE((*resumed)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    PrintPathMarker("auto-eligibility-and-fallback");
}

TEST_CASE("SDK memory learning: Auto denied option and mid-flight revocation block commit without candidate fallback") {
    Fixture fixture;
    ScopedHooks hooks;

    // 1) Frozen auto_write_enabled = false denies commit after enqueueing without silent fallback.
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";
        learn_opts.auto_write_enabled = false;

        auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
        REQUIRE(session.has_value());

        auto rc = (*session)->Submit("op-denied", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep = WaitForLearning(*session, rc->operation_id);
        REQUIRE(rep.stage == LearningStage::Failed);
        CHECK(rep.error_code == "sdk.memory_learning.auto_write_denied");
        REQUIRE(rep.auto_writes.size() == 1);
        CHECK(rep.auto_writes[0].queue_stage == "queued");
        CHECK(rep.auto_writes[0].worker_stage == "finished");
        CHECK(rep.auto_writes[0].admission_stage == "denied");
        CHECK(rep.auto_writes[0].commit_state == "not_started");
        CHECK(rep.auto_writes[0].fallback_candidate_id.empty());
        CHECK((*session)->ListMemoryCandidates()->empty());
        CHECK(rep.candidate_ids.empty());

        // Denial does not leave partial topic mutation, so subsequent Submit succeeds.
        auto r_next = (*session)->Submit("op-after-deny", "好的");
        REQUIRE(r_next.has_value());
        REQUIRE((*session)->WaitResult(r_next->operation_id, 15s)->state == sdk::OperationState::Succeeded);

        REQUIRE((*session)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // 2) Live RevokeAutoMemoryLearning after queue persistence and before commit.
    std::string revoked_session_id;
    std::string revoked_op_id;
    std::string revoked_commit_key;
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";

        auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
        REQUIRE(session.has_value());
        revoked_session_id = (*session)->id();
        std::shared_ptr<sdk::Session> raw_session = *session;

        bool saw_queued_file = false;
        hooks.SetAutoAdmission([&](const std::string& op_id, const std::string& commit_key) {
            revoked_op_id = op_id;
            revoked_commit_key = commit_key;
            const fs::path qpath =
                SessionDir(raw_session) / "sdk-memory-learning-queue" / (commit_key + ".json");
            saw_queued_file = fs::exists(lubancode::platform::FileIoPath(qpath));
            REQUIRE(raw_session->RevokeAutoMemoryLearning("operator_revoked").has_value());
        });

        auto rc = (*session)->Submit("op-revoke", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep = WaitForLearning(*session, rc->operation_id);
        hooks.SetAutoAdmission({});
        CHECK(revoked_op_id == rc->operation_id);

        CHECK(saw_queued_file);
        REQUIRE(rep.stage == LearningStage::Failed);
        CHECK(rep.error_code == "sdk.memory_learning.auto_write_revoked");
        REQUIRE(rep.auto_writes.size() == 1);
        CHECK(rep.auto_writes[0].queue_stage == "queued");
        CHECK(rep.auto_writes[0].worker_stage == "finished");
        CHECK(rep.auto_writes[0].admission_stage == "revoked");
        CHECK(rep.auto_writes[0].commit_state == "not_started");
        CHECK(rep.auto_writes[0].error == "operator_revoked");
        CHECK(rep.auto_writes[0].fallback_candidate_id.empty());
        CHECK((*session)->ListMemoryCandidates()->empty());
        CHECK_FALSE(fs::exists(
            lubancode::platform::FileIoPath(MemoryDir(*session).parent_path() / "lifecycle" / revoked_commit_key / "intent.json")));

        REQUIRE((*session)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // Resume after live revocation: zero replay, keeps revoked state, Submit stays healthy.
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.resume_session_id = revoked_session_id;
        auto resumed = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(resumed.has_value());
        CHECK(learn_trace->calls.load() == 0);
        const auto rep = (*resumed)->GetMemoryLearning(revoked_op_id);
        REQUIRE(rep.has_value());
        CHECK(rep->stage == LearningStage::Failed);
        REQUIRE(rep->auto_writes.size() == 1);
        CHECK(rep->auto_writes[0].admission_stage == "revoked");
        CHECK(rep->auto_writes[0].commit_state == "not_started");

        auto r_next = (*resumed)->Submit("op-post-revoke", "好的");
        REQUIRE(r_next.has_value());
        REQUIRE((*resumed)->WaitResult(r_next->operation_id, 15s)->state == sdk::OperationState::Succeeded);

        REQUIRE((*resumed)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    PrintPathMarker("auto-authorization-revocation");
}

TEST_CASE("SDK memory learning: queued-but-uncommitted crash recovery preserves facts, prevents replay, and fences Submit") {
    Fixture fixture;
    ScopedHooks hooks;
    std::string session_id;
    std::string crash_op_id;
    std::string captured_commit_key;
    std::string crash_queue_bytes;
    std::string crash_report_bytes;
    fs::path session_dir;
    fs::path memory_dir;
    fs::path lifecycle_dir;

    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";

        auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
        REQUIRE(session.has_value());
        session_id = (*session)->id();
        session_dir = SessionDir(*session);
        memory_dir = MemoryDir(*session);
        lifecycle_dir = memory_dir.parent_path() / "lifecycle";

        // Snapshot exact durable bytes right after Enqueue (queue_stage = queued) and before Commit.
        hooks.SetAutoAdmission([&](const std::string& op_id, const std::string& commit_key) {
            crash_op_id = op_id;
            captured_commit_key = commit_key;
            crash_queue_bytes =
                ReadText(session_dir / "sdk-memory-learning-queue" / (commit_key + ".json"));
            crash_report_bytes =
                ReadText(session_dir / "sdk-memory-learnings" / (op_id + ".json"));
        });

        auto rc = (*session)->Submit("op-crash", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
        const auto rep = WaitForLearning(*session, rc->operation_id);
        REQUIRE(rep.stage == LearningStage::Completed);
        hooks.SetAutoAdmission({});

        REQUIRE((*session)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // Restore exact pre-commit crash point on disk (queued + Running report + no lifecycle/topic).
    REQUIRE_FALSE(crash_op_id.empty());
    REQUIRE_FALSE(captured_commit_key.empty());
    WriteText(session_dir / "sdk-memory-learning-queue" / (captured_commit_key + ".json"), crash_queue_bytes);
    WriteText(session_dir / "sdk-memory-learnings" / (crash_op_id + ".json"), crash_report_bytes);
    {
        std::error_code ec;
        fs::remove_all(lubancode::platform::FileIoPath(lifecycle_dir / captured_commit_key), ec);
        fs::remove_all(lubancode::platform::FileIoPath(memory_dir), ec);
    }

    // Reopen with same session ID: zero replay, honest Indeterminate + queued/not_started, Submit fenced.
    {
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";

        auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
        opts.resume_session_id = session_id;
        auto resumed = (*runtime)->OpenSession(std::move(opts));
        REQUIRE(resumed.has_value());
        CHECK(main_trace->calls.load() == 0);
        CHECK(learn_trace->calls.load() == 0);

        const auto rep = (*resumed)->GetMemoryLearning(crash_op_id);
        REQUIRE(rep.has_value());
        CHECK(rep->stage == LearningStage::Indeterminate);
        CHECK(rep->error_code == "sdk.memory_learning.interrupted");
        REQUIRE(rep->auto_writes.size() == 1);
        CHECK(rep->auto_writes[0].commit_key == captured_commit_key);
        CHECK(rep->auto_writes[0].queue_stage == "queued");
        CHECK(rep->auto_writes[0].worker_stage == "running");
        CHECK(rep->auto_writes[0].commit_state == "not_started");
        CHECK((*resumed)->ListMemoryCandidates()->empty());
        CHECK_FALSE(fs::exists(memory_dir / "preferences" / "project-build-convention.md"));

        const auto fenced = (*resumed)->Submit("op-after-crash", DurablePrompt());
        REQUIRE_FALSE(fenced.has_value());
        CHECK(fenced.error().code == "sdk.memory_learning.indeterminate");

        REQUIRE((*resumed)->Close().has_value());
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // Forged committed claim without lifecycle receipt is rejected on OpenSession.
    {
        auto forged_doc = Json::parse(ReadText(session_dir / "sdk-memory-learnings" / (crash_op_id + ".json")));
        auto qdoc = Json::parse(crash_queue_bytes);
        auto forged_attempt = qdoc.at("attempt");
        forged_attempt["queueStage"] = "completed";
        forged_attempt["admissionStage"] = "admitted";
        forged_attempt["commitState"] = "committed";
        forged_attempt["memoryId"] = "preference.project-build-convention";
        forged_doc["stage"] = "completed";
        forged_doc["autoWrites"] = Json::array({forged_attempt});
        forged_doc.erase("sha256");
        forged_doc["sha256"] = lubancode::platform::Sha256Hex(forged_doc.dump());
        WriteText(session_dir / "sdk-memory-learnings" / (crash_op_id + ".json"), forged_doc.dump());

        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";
        auto opts = MakeBaseOptions(
            fixture, std::make_shared<MainTrace>(), learn_opts, std::make_shared<LearningTrace>());
        opts.resume_session_id = session_id;
        const auto bad = (*runtime)->OpenSession(std::move(opts));
        REQUIRE_FALSE(bad.has_value());

        REQUIRE((*runtime)->Shutdown().has_value());
    }

    PrintPathMarker("auto-queued-crash-recovery");
}

TEST_CASE("SDK memory learning: Auto CAS conflict does not clobber existing topic and preserves conflict state") {
    Fixture fixture;

    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();
    {
        LearningTrace::Step step;
        sdk::ModelReply r;
        r.text = ValidExtractionPayload(
            "Shared Slug",
            "Auto attempt targeting an occupied topic file with different id",
            "Clobber check content.");
        r.usage = sdk::Usage{19, 11, 2, 0, 1};
        r.usage_observation = FiveFieldObservation(*r.usage);
        r.provider_response_id = "learn-cas-1";
        step.reply = r;
        learn_trace->script.push_back(step);
    }

    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Auto;
    learn_opts.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());

    // Pre-seed topic file preferences/shared-slug.md with a distinct memory id so PrepareUpsert hits topic_conflict.
    const fs::path mem_dir = MemoryDir(*session);
    const fs::path existing_topic = mem_dir / "preferences" / "shared-slug.md";
    {
        mem::SaveRequest seed;
        seed.kind = mem::MemoryKind::Preference;
        seed.id = "preference.other-owner-id";
        seed.title = "Other Owner";
        seed.summary = "Existing occupied topic file";
        seed.content = "Original existing body.";
        seed.confidence = "user-stated";
        seed.source_session = (*session)->id();
        seed.scope.level = "project";
        seed.scope.kind = "project";
        const mem::ProjectCommitContext seed_gate{
            fixture.root / "repo",
            mem_dir,
            mem_dir.parent_path() / "lifecycle",
            (*session)->DescribeMemoryLearning()->workspace_key,
            "seed-other-owner",
            (*session)->id(),
            "seed-other-owner",
        };
        REQUIRE(mem::CommitProjectUpsert(seed_gate, seed).state == mem::ProjectCommitState::Committed);
        fs::rename(mem_dir / "preferences" / "other-owner-id.md", existing_topic);
        REQUIRE(mem::RebuildMemoryIndex(mem_dir).has_value());
    }
    const std::string original_content = ReadText(existing_topic);

    auto rc = (*session)->Submit("op-cas", DurablePrompt());
    REQUIRE(rc.has_value());
    REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    const auto rep = WaitForLearning(*session, rc->operation_id);

    REQUIRE(rep.stage == LearningStage::Failed);
    CHECK(rep.error_code == "memory.commit.topic_conflict");
    REQUIRE(rep.auto_writes.size() == 1);
    CHECK(rep.auto_writes[0].queue_stage == "failed");
    CHECK(rep.auto_writes[0].admission_stage == "admitted");
    CHECK(rep.auto_writes[0].commit_state == "not_started");
    CHECK(rep.auto_writes[0].error_code == "memory.commit.topic_conflict");
    CHECK(ReadText(existing_topic) == original_content);
    CHECK((*session)->ListMemoryCandidates()->empty());

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());

    PrintPathMarker("auto-cas-conflict");
}

TEST_CASE("SDK memory learning: Auto exercises and validates all 7 ProjectCommitStage failures") {
    struct StageCase {
        const char* name;
        bool expect_indeterminate;
        bool seed_rename_source;
    };
    const std::array<StageCase, 7> cases{{
        {"intent", false, false},
        {"snapshot", false, false},
        {"topic", false, false},
        {"cleanup", true, true},
        {"catalog", true, false},
        {"index", true, false},
        {"result", true, false},
    }};

    for (const auto& sc : cases) {
        CAPTURE(sc.name);
        Fixture fixture;
        ScopedHooks hooks;
        std::string session_id;

        {
            auto runtime = sdk::Runtime::Create(fixture.Roots());
            REQUIRE(runtime.has_value());
            auto main_trace = std::make_shared<MainTrace>();
            auto learn_trace = std::make_shared<LearningTrace>();
            {
                LearningTrace::Step step;
                sdk::ModelReply r;
                r.text = ValidExtractionPayload(
                    std::string("Stage Target ") + sc.name,
                    std::string("Testing failure at stage ") + sc.name,
                    "Stage test body.");
                r.usage = sdk::Usage{19, 11, 2, 0, 1};
                r.usage_observation = FiveFieldObservation(*r.usage);
                r.provider_response_id = std::string("learn-stage-") + sc.name;
                step.reply = r;
                learn_trace->script.push_back(step);
            }

            LearningOptions learn_opts;
            learn_opts.mode = LearningMode::Auto;
            learn_opts.model = "extract-model";

            auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
            REQUIRE(session.has_value());
            session_id = (*session)->id();
            const fs::path mem_dir = MemoryDir(*session);

            if (sc.seed_rename_source) {
                mem::SaveRequest seed;
                seed.kind = mem::MemoryKind::Preference;
                seed.id = "preference.stage-target-cleanup";
                seed.title = "Legacy Cleanup Source";
                seed.summary = "Old topic file to trigger cleanup unlink";
                seed.content = "Old topic content.";
                seed.confidence = "user-stated";
                seed.source_session = session_id;
                seed.scope.level = "project";
                seed.scope.kind = "project";
                const mem::ProjectCommitContext seed_gate{
                    fixture.root / "repo",
                    mem_dir,
                    mem_dir.parent_path() / "lifecycle",
                    (*session)->DescribeMemoryLearning()->workspace_key,
                    "seed-cleanup-source",
                    session_id,
                    "seed-cleanup-source",
                };
                REQUIRE(mem::CommitProjectUpsert(seed_gate, seed).state == mem::ProjectCommitState::Committed);
                fs::rename(
                    mem_dir / "preferences" / "stage-target-cleanup.md",
                    mem_dir / "preferences" / "legacy-cleanup-source.md");
                REQUIRE(mem::RebuildMemoryIndex(mem_dir).has_value());
            }

            const std::string fail_stage = sc.name;
            hooks.SetCommitWrite([fail_stage](
                const fs::path& path,
                std::string_view content,
                lubancode::platform::WriteDurability durability)
                    -> std::expected<lubancode::platform::AtomicWriteReceipt, lubancode::platform::AtomicWriteError> {
                const auto fname = path.filename().string();
                const auto parent = path.parent_path().filename().string();
                bool match = false;
                if (fail_stage == "intent") match = (fname == "intent.json");
                else if (fail_stage == "snapshot") match = (fname == "topic.snapshot.md");
                else if (fail_stage == "topic") {
                    match = ((parent == "preferences" || parent == "facts" || parent == "feedback") &&
                             path.extension() == ".md");
                } else if (fail_stage == "cleanup") match = (fname == ".memory-commit-cleanup.json");
                else if (fail_stage == "catalog") match = (fname == "catalog.json");
                else if (fail_stage == "index") match = (fname == "index.md");
                else if (fail_stage == "result") match = (fname == "result.json");
                if (match) {
                    return std::unexpected(lubancode::platform::AtomicWriteError{
                        "atomic.replace_failed",
                        "simulated failure at stage " + fail_stage,
                        lubancode::platform::WriteOutcome::NotCommitted,
                        lubancode::platform::WriteFailureKind::Permanent,
                    });
                }
                return lubancode::platform::AtomicWriteFile(path, content, durability);
            });

            auto rc = (*session)->Submit(std::string("op-stage-") + sc.name, DurablePrompt());
            REQUIRE(rc.has_value());
            REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
            const auto rep = WaitForLearning(*session, rc->operation_id);
            hooks.SetCommitWrite({});

            REQUIRE(rep.auto_writes.size() == 1);
            if (sc.expect_indeterminate) {
                CHECK(rep.stage == LearningStage::Indeterminate);
                CHECK(rep.auto_writes[0].commit_state == "indeterminate");
                CHECK(rep.auto_writes[0].queue_stage == "indeterminate");
                const auto fenced = (*session)->Submit("op-after-indet", "好的");
                REQUIRE_FALSE(fenced.has_value());
                CHECK(fenced.error().code == "sdk.memory_learning.indeterminate");
            } else {
                CHECK(rep.stage == LearningStage::Failed);
                CHECK(rep.auto_writes[0].commit_state == "not_started");
                CHECK(rep.auto_writes[0].queue_stage == "failed");
            }
            CHECK((*session)->ListMemoryCandidates()->empty());

            REQUIRE((*session)->Close().has_value());
            REQUIRE((*runtime)->Shutdown().has_value());
        }

        // Same-ID recovery preserves outcome across restart without replay.
        {
            auto runtime = sdk::Runtime::Create(fixture.Roots());
            REQUIRE(runtime.has_value());
            auto main_trace = std::make_shared<MainTrace>();
            auto learn_trace = std::make_shared<LearningTrace>();
            LearningOptions learn_opts;
            learn_opts.mode = LearningMode::Auto;
            learn_opts.model = "extract-model";
            auto opts = MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace);
            opts.resume_session_id = session_id;
            auto resumed = (*runtime)->OpenSession(std::move(opts));
            REQUIRE(resumed.has_value());
            CHECK(learn_trace->calls.load() == 0);

            if (sc.expect_indeterminate) {
                const auto fenced = (*resumed)->Submit("op-after-restart", "好的");
                REQUIRE_FALSE(fenced.has_value());
                CHECK(fenced.error().code == "sdk.memory_learning.indeterminate");
            }
            REQUIRE((*resumed)->Close().has_value());
            REQUIRE((*runtime)->Shutdown().has_value());
        }
    }

    PrintPathMarker("auto-all-stage-failures");
}

TEST_CASE("SDK memory learning: duplicate commit key confirms without rewriting topic") {
    Fixture fixture;
    ScopedHooks hooks;

    auto runtime = sdk::Runtime::Create(fixture.Roots());
    REQUIRE(runtime.has_value());
    auto main_trace = std::make_shared<MainTrace>();
    auto learn_trace = std::make_shared<LearningTrace>();

    LearningOptions learn_opts;
    learn_opts.mode = LearningMode::Auto;
    learn_opts.model = "extract-model";

    auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
    REQUIRE(session.has_value());
    const fs::path sdir = SessionDir(*session);
    const fs::path mem_dir = MemoryDir(*session);
    const fs::path lifecycle_dir = mem_dir.parent_path() / "lifecycle";
    const std::string wk = (*session)->DescribeMemoryLearning()->workspace_key;
    const std::string sid = (*session)->id();

    // Before committer runs, pre-commit the identical commit_key and saveRequest directly so
    // the worker hits an already-committed duplicate commit key without rewriting the topic.
    bool precommitted = false;
    hooks.SetAutoAdmission([&](const std::string&, const std::string& commit_key) {
        if (!precommitted) {
            precommitted = true;
            const auto qdoc = Json::parse(ReadText(sdir / "sdk-memory-learning-queue" / (commit_key + ".json")));
            auto parsed_req = mem::store::ParseUpsertJob(qdoc.at("saveRequest"), true);
            REQUIRE(parsed_req.has_value());
            const std::string source_ref = qdoc.at("attempt").at("sourceEventRef").get<std::string>();
            const mem::ProjectCommitContext gate{
                fixture.root / "repo", mem_dir, lifecycle_dir, wk, commit_key, sid, source_ref};
            const auto pre = mem::CommitProjectUpsert(gate, *parsed_req);
            REQUIRE(pre.state == mem::ProjectCommitState::Committed);
        }
    });

    auto rc = (*session)->Submit("op-dup", DurablePrompt());
    REQUIRE(rc.has_value());
    REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);
    const auto rep = WaitForLearning(*session, rc->operation_id);
    hooks.SetAutoAdmission({});
    CHECK(precommitted);

    REQUIRE(rep.stage == LearningStage::Completed);
    REQUIRE(rep.auto_writes.size() == 1);
    CHECK(rep.auto_writes[0].commit_state == "committed");
    CHECK(rep.auto_writes[0].queue_stage == "completed");
    CHECK(rep.auto_writes[0].duplicate);
    CHECK(rep.auto_writes[0].stages.back() == sdk::memory::v1::SaveStageReceipt{"result", "durable"});

    // Duplicate Submit key also returns duplicate receipt without re-running learning.
    auto dup_submit = (*session)->Submit("op-dup", DurablePrompt());
    REQUIRE(dup_submit.has_value());
    CHECK(dup_submit->duplicate);
    CHECK(learn_trace->calls.load() == 1);

    REQUIRE((*session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value());
    PrintPathMarker("auto-duplicate-commit-key");
}

TEST_CASE("SDK memory learning: Auto handles queued and mid-commit Close races without blocking queries") {
    // 1) Queued-before-commit Close() cancellation keeps queue_stage=queued, worker_stage=cancelled, no topic write.
    {
        Fixture fixture;
        std::mutex sync_mu;
        std::condition_variable sync_cv;
        bool queued_reached = false;
        bool close_dispatched = false;
        ScopedHooks hooks;
        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";

        auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
        REQUIRE(session.has_value());
        std::shared_ptr<sdk::Session> raw_session = *session;
        const fs::path mem_dir = MemoryDir(*session);

        hooks.SetAutoAdmission([&](const std::string&, const std::string&) {
            {
                std::lock_guard lock(sync_mu);
                queued_reached = true;
            }
            sync_cv.notify_all();
            {
                std::unique_lock lock(sync_mu);
                sync_cv.wait_for(lock, 15s, [&] { return close_dispatched; });
            }
            while (true) {
                auto probe = raw_session->Submit("probe-closing-1", "x");
                if (!probe && probe.error().code == "sdk.session.closed") break;
                std::this_thread::sleep_for(5ms);
            }
            std::this_thread::sleep_for(50ms);
        });

        auto rc = (*session)->Submit("op-cancel-queued", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);

        {
            std::unique_lock lock(sync_mu);
            REQUIRE(sync_cv.wait_for(lock, 15s, [&] { return queued_reached; }));
        }

        sdk::Result<void> close_result;
        std::thread closer([&] {
            {
                std::lock_guard lock(sync_mu);
                close_dispatched = true;
            }
            sync_cv.notify_all();
            close_result = raw_session->Close();
        });
        closer.join();
        hooks.SetAutoAdmission({});
        REQUIRE(close_result.has_value());

        const auto post_close = (*session)->GetMemoryLearning(rc->operation_id);
        REQUIRE(post_close.has_value());
        CHECK(post_close->stage == LearningStage::Cancelled);
        REQUIRE(post_close->auto_writes.size() == 1);
        CHECK(post_close->auto_writes[0].queue_stage == "queued");
        CHECK(post_close->auto_writes[0].worker_stage == "cancelled");
        CHECK(post_close->auto_writes[0].commit_state == "not_started");
        CHECK_FALSE(fs::exists(mem_dir / "preferences" / "project-build-convention.md"));
        REQUIRE((*runtime)->Shutdown().has_value());
    }

    // 2) Concurrent Close() interrupting mid-commit after Snapshot before Topic cleanly stops before Topic.
    {
        Fixture fixture;
        std::mutex sync_mu;
        std::condition_variable sync_cv;
        bool snapshot_reached = false;
        bool close_dispatched = false;
        ScopedHooks hooks;

        auto runtime = sdk::Runtime::Create(fixture.Roots());
        REQUIRE(runtime.has_value());
        auto main_trace = std::make_shared<MainTrace>();
        auto learn_trace = std::make_shared<LearningTrace>();
        LearningOptions learn_opts;
        learn_opts.mode = LearningMode::Auto;
        learn_opts.model = "extract-model";

        auto session = (*runtime)->OpenSession(MakeBaseOptions(fixture, main_trace, learn_opts, learn_trace));
        REQUIRE(session.has_value());
        std::shared_ptr<sdk::Session> raw_session = *session;

        hooks.SetCommitWrite([&](
            const fs::path& path,
            std::string_view content,
            lubancode::platform::WriteDurability durability) {
            if (path.filename() == "topic.snapshot.md") {
                {
                    std::lock_guard lock(sync_mu);
                    snapshot_reached = true;
                }
                sync_cv.notify_all();
                {
                    std::unique_lock lock(sync_mu);
                    sync_cv.wait_for(lock, 15s, [&] { return close_dispatched; });
                }
                while (true) {
                    auto probe = raw_session->Submit("probe-closing-2", "x");
                    if (!probe && probe.error().code == "sdk.session.closed") break;
                    std::this_thread::sleep_for(5ms);
                }
                std::this_thread::sleep_for(50ms);
            }
            return lubancode::platform::AtomicWriteFile(path, content, durability);
        });

        auto rc = (*session)->Submit("op-close-midcommit", DurablePrompt());
        REQUIRE(rc.has_value());
        REQUIRE((*session)->WaitResult(rc->operation_id, 15s)->state == sdk::OperationState::Succeeded);

        {
            std::unique_lock lock(sync_mu);
            REQUIRE(sync_cv.wait_for(lock, 15s, [&] { return snapshot_reached; }));
        }

        sdk::Result<void> close_result;
        std::thread closer([&] {
            {
                std::lock_guard lock(sync_mu);
                close_dispatched = true;
            }
            sync_cv.notify_all();
            close_result = raw_session->Close();
        });
        closer.join();
        hooks.SetCommitWrite({});
        REQUIRE(close_result.has_value());

        const auto post_close = (*session)->GetMemoryLearning(rc->operation_id);
        REQUIRE(post_close.has_value());
        CHECK(post_close->stage == LearningStage::Cancelled);
        REQUIRE(post_close->auto_writes.size() == 1);
        CHECK(post_close->auto_writes[0].commit_state == "not_started");
        CHECK(post_close->auto_writes[0].queue_stage == "queued");
        CHECK(post_close->auto_writes[0].worker_stage == "cancelled");
        CHECK_FALSE(fs::exists(MemoryDir(*session) / "preferences" / "project-build-convention.md"));

        REQUIRE((*runtime)->Shutdown().has_value());
    }

    PrintPathMarker("auto-close-races");
}
