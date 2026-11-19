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
#include "memory/project_memory.hpp"
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
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << content;
    out.close();
    REQUIRE_FALSE(out.fail());
}

std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
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
        fs::remove_all(root, ec);
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
        auto_opts.model = "extract-model";
        auto learn_trace = std::make_shared<LearningTrace>();
        auto res = (*runtime)->OpenSession(MakeBaseOptions(fixture, std::make_shared<MainTrace>(), auto_opts, learn_trace));
        REQUIRE_FALSE(res.has_value());
        CHECK(res.error().code == "sdk.memory_learning.auto_unsupported");
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
