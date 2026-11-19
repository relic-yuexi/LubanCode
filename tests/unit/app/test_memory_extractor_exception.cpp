#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/commands/memory_commands.hpp"
#include "app/turn_memory_extractor.hpp"
#include "app/turn_memory_extractor_test_hooks.hpp"
#include "cli/theme.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
using lubancode::app::TurnMemoryExtractor;
using lubancode::app::testing::MemoryWorkerPhase;

struct Probe {
    std::atomic<int> calls{0};
    std::atomic<int> destroyed{0};
    int throw_mode = 0;
    bool with_candidate = false;
};

struct ProbeBackend final : lubancode::api::Backend {
    explicit ProbeBackend(std::shared_ptr<Probe> value) : probe(std::move(value)) {}
    ~ProbeBackend() override { ++probe->destroyed; }
    std::shared_ptr<Probe> probe;

    std::expected<void, lubancode::api::Error> send_stream(
        const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>& emit,
        const std::atomic<bool>*) override {
        ++probe->calls;
        if (probe->throw_mode == 1) throw std::runtime_error("backend failure");
        if (probe->throw_mode == 2) throw 17;
        emit(lubancode::api::MessageStart{"request", "cheap-m"});
        const std::string reply = probe->with_candidate
            ? R"({"task_type":"research","summary":"包管理用 pnpm","retrieval_terms":["pnpm"],"candidates":[{"kind":"preference","title":"用 pnpm","summary":"统一走 pnpm","content":"装依赖用 pnpm","keywords":["pnpm"],"paths":[],"confidence":"user-stated"}]})"
            : R"({"task_type":"research","summary":"已核材料","retrieval_terms":[],"candidates":[]})";
        emit(lubancode::api::TextDelta{reply});
        emit(lubancode::api::ContentBlockDone{0});
        lubancode::api::MessageDone done;
        done.stop_reason = "end_turn";
        done.usage.input_tokens = 13;
        done.usage.output_tokens = 5;
        done.usage.cache_read_tokens = 3;
        done.usage.cache_creation_tokens = 2;
        done.usage.output_reasoning_tokens = 2;
        emit(done);
        return {};
    }
};

TurnMemoryExtractor::Inputs Inputs(const std::shared_ptr<Probe>& probe,
                                  std::uint64_t generation = 51,
                                  std::string turn = "turn-worker") {
    TurnMemoryExtractor::Inputs input;
    input.backend = std::make_unique<ProbeBackend>(probe);
    input.model = "cheap-m";
    input.system_prompt = "private prompt";
    input.transcript = "private transcript";
    input.task_type = "research";
    input.session_generation = generation;
    input.turn_id = std::move(turn);
    return input;
}

struct HookScope {
    explicit HookScope(lubancode::app::testing::MemoryWorkerExecutionHook hook)
        : previous(lubancode::app::testing::ExchangeMemoryWorkerExecutionHook(std::move(hook))) {}
    ~HookScope() {
        lubancode::app::testing::ExchangeMemoryWorkerExecutionHook(std::move(previous));
    }
    lubancode::app::testing::MemoryWorkerExecutionHook previous;
};

auto StandardFailureAt(MemoryWorkerPhase requested) {
    return [requested](MemoryWorkerPhase actual) {
        if (requested == actual) throw std::runtime_error("private exception secret");
    };
}

bool AwaitReady(TurnMemoryExtractor& extractor) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!extractor.Ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return extractor.Ready();
}

void CheckFailure(const TurnMemoryExtractor::Outcome& out,
                  std::uint64_t generation = 51, const std::string& turn = "turn-worker") {
    CHECK_FALSE(out.ok);
    CHECK(out.error.code == lubancode::app::ExtractionErrorCode::WorkerExecutionFailed);
    CHECK(lubancode::app::StableExtractErrorCode(out.error) == "worker_execution_failed");
    CHECK(out.error.message == "记忆抽取后台执行失败");
    CHECK(out.error.message.find("secret") == std::string::npos);
    CHECK(out.model == "cheap-m");
    CHECK(out.task_type == "research");
    CHECK(out.session_generation == generation);
    CHECK(out.turn_id == turn);
    CHECK(out.extraction.summary.empty());
    CHECK(out.extraction.retrieval_terms.empty());
    CHECK(out.extraction.candidates.empty());
}

void CheckUsage(const TurnMemoryExtractor::Outcome& out) {
    CHECK(out.extraction_invoked);
    CHECK(out.accounting.usage_reported);
    CHECK(out.accounting.usage.input_tokens == 13);
    CHECK(out.accounting.usage.output_tokens == 5);
    CHECK(out.accounting.usage.cache_read_tokens == 3);
    CHECK(out.accounting.usage.cache_creation_tokens == 2);
    CHECK(out.accounting.usage.output_reasoning_tokens == 2);
    CHECK(out.accounting.duration_ms >= 0);
    CHECK(out.extract_wall_ms >= out.accounting.duration_ms);
}
}  // namespace

TEST_CASE("Memory worker exception: standard failure before work produces a collectible terminal") {
    std::cout << "[memory-worker-exception-path] standard\n";
    auto probe = std::make_shared<Probe>();
    HookScope hook(StandardFailureAt(MemoryWorkerPhase::BeforeWork));
    TurnMemoryExtractor extractor;
    REQUIRE(extractor.Start(Inputs(probe)));
    REQUIRE(AwaitReady(extractor));
    CHECK(extractor.Busy());
    extractor.RequestCancel();
    auto out = extractor.TakeFinished();
    REQUIRE(out.has_value());
    CheckFailure(*out);
    CHECK_FALSE(out->extraction_invoked);
    CHECK_FALSE(out->accounting.usage_reported);
    lubancode::agent::ModelUsageLedger calls;
    CHECK_FALSE(lubancode::app::RecordTurnMemoryCall(calls, *out));
    CHECK(calls.by_role().empty());
    CHECK(probe->calls.load() == 0);
    CHECK(probe->destroyed.load() == 1);
    CHECK_FALSE(extractor.Busy());
    CHECK_FALSE(extractor.Ready());
    CHECK_FALSE(extractor.TakeFinished().has_value());
}

TEST_CASE("Memory worker exception: unknown failures preserve identity and bounded disposal") {
    std::cout << "[memory-worker-exception-path] nonstandard\n";
    auto probe = std::make_shared<Probe>();
    HookScope hook([](MemoryWorkerPhase phase) { if (phase == MemoryWorkerPhase::BeforeWork) throw 73; });
    TurnMemoryExtractor extractor;
    for (std::uint64_t generation = 1; generation <= 5; ++generation) {
        const auto turn = "turn-" + std::to_string(generation);
        REQUIRE(extractor.Start(Inputs(probe, generation, turn)));
        REQUIRE(AwaitReady(extractor));
        auto out = extractor.TakeFinished();
        REQUIRE(out.has_value());
        CheckFailure(*out, generation, turn);
        CHECK_FALSE(out->extraction_invoked);
    }
    const auto began = std::chrono::steady_clock::now();
    { TurnMemoryExtractor abandoned;
      REQUIRE(abandoned.Start(Inputs(probe, 99, "abandoned"))); }
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began).count() < 1000);
    CHECK(probe->calls.load() == 0);
    CHECK(probe->destroyed.load() == 6);
}

TEST_CASE("Memory worker exception: a failure after actual extraction keeps returned accounting") {
    std::cout << "[memory-worker-exception-path] accounting\n";
    auto probe = std::make_shared<Probe>();
    HookScope hook(StandardFailureAt(MemoryWorkerPhase::AfterExtraction));
    TurnMemoryExtractor extractor;
    REQUIRE(extractor.Start(Inputs(probe)));
    REQUIRE(AwaitReady(extractor));
    auto out = extractor.TakeFinished();
    REQUIRE(out.has_value());
    CheckFailure(*out);
    CheckUsage(*out);
    lubancode::agent::ModelUsageLedger calls;
    REQUIRE(lubancode::app::RecordTurnMemoryCall(calls, *out));
    const auto& cheap = calls.by_role().at(lubancode::agent::ModelRole::Cheap);
    CHECK(cheap.calls == 1);
    CHECK(cheap.input_tokens == 18);
    CHECK(cheap.output_tokens == 5);
    CHECK(probe->calls.load() == 1);
    CHECK(probe->destroyed.load() == 1);
}

TEST_CASE("Memory worker exception: publication failure discards a genuinely parsed candidate") {
    std::cout << "[memory-worker-exception-path] publication\n";
    auto probe = std::make_shared<Probe>();
    probe->with_candidate = true;
    TurnMemoryExtractor control;
    REQUIRE(control.Start(Inputs(probe)));
    REQUIRE(AwaitReady(control));
    auto success = control.TakeFinished();
    REQUIRE(success.has_value());
    REQUIRE(success->ok);
    REQUIRE(success->extraction.candidates.size() == 1);
    REQUIRE(success->extraction.retrieval_terms.size() == 1);
    REQUIRE_FALSE(success->extraction.summary.empty());
    HookScope hook(StandardFailureAt(MemoryWorkerPhase::BeforePublish));
    TurnMemoryExtractor failing;
    REQUIRE(failing.Start(Inputs(probe)));
    REQUIRE(AwaitReady(failing));
    auto out = failing.TakeFinished();
    REQUIRE(out.has_value());
    CheckFailure(*out);
    CheckUsage(*out);
    CHECK(probe->calls.load() == 2);
    CHECK(probe->destroyed.load() == 2);
}

TEST_CASE("Memory worker exception: captured hook does not poison another owner or later restart") {
    std::cout << "[memory-worker-exception-path] isolation\n";
    auto failed = std::make_shared<Probe>();
    auto healthy = std::make_shared<Probe>();
    std::atomic<int> injected{0};
    TurnMemoryExtractor extractor;
    { HookScope hook([&](MemoryWorkerPhase phase) {
        if (phase == MemoryWorkerPhase::BeforeWork) { ++injected; throw 73; }
      });
      REQUIRE(extractor.Start(Inputs(failed))); }
    TurnMemoryExtractor other;
    REQUIRE(other.Start(Inputs(healthy, 52, "other")));
    REQUIRE(AwaitReady(extractor));
    REQUIRE(AwaitReady(other));
    auto bad = extractor.TakeFinished();
    auto good = other.TakeFinished();
    REQUIRE(bad.has_value()); REQUIRE(good.has_value());
    CheckFailure(*bad);
    CHECK(good->ok);
    CHECK(good->turn_id == "other");
    CHECK(injected.load() == 1);
    CHECK(failed->calls.load() == 0);
    CHECK(failed->destroyed.load() == 1);
    REQUIRE(extractor.Start(Inputs(healthy, 53, "recovered")));
    REQUIRE(AwaitReady(extractor));
    auto recovered = extractor.TakeFinished();
    REQUIRE(recovered.has_value());
    CHECK(recovered->ok);
    CHECK(recovered->turn_id == "recovered");
    CHECK(recovered->session_generation == 53);
    CHECK(healthy->calls.load() == 2);
    CHECK(healthy->destroyed.load() == 2);
}

TEST_CASE("Memory worker exception: actual Backend throws retain the sampling transport classification") {
    std::cout << "[memory-worker-exception-path] transport\n";
    TurnMemoryExtractor extractor;
    lubancode::agent::ModelUsageLedger calls;
    for (int mode : {1, 2}) {
        auto probe = std::make_shared<Probe>();
        probe->throw_mode = mode;
        REQUIRE(extractor.Start(Inputs(probe)));
        REQUIRE(AwaitReady(extractor));
        auto out = extractor.TakeFinished();
        REQUIRE(out.has_value());
        CHECK_FALSE(out->ok);
        CHECK(out->error.code == lubancode::app::ExtractionErrorCode::TransportFailed);
        CHECK(out->extraction_invoked);
        CHECK_FALSE(out->accounting.usage_reported);
        REQUIRE(lubancode::app::RecordTurnMemoryCall(calls, *out));
        CHECK(probe->calls.load() == 1);
        CHECK(probe->destroyed.load() == 1);
    }
    CHECK(calls.by_role().at(lubancode::agent::ModelRole::Cheap).calls == 2);
    CHECK(calls.by_role().at(lubancode::agent::ModelRole::Cheap).input_tokens == 0);
}

TEST_CASE("Memory worker exception: actual CLI settlement records failure without accepting a candidate") {
    std::cout << "[memory-worker-exception-path] settlement\n";
    const auto run = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("lmb-memory-exception-" + std::to_string(run));
    fs::create_directories(root / "repo"); fs::create_directories(root / "home");
    lubancode::runtime::TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces"; options.workspace_root = root / "repo";
    options.workspace_identity = lubancode::workspace::MakeFallbackIdentity(root / "repo");
    options.lubancode_version = "test";
    auto session = lubancode::runtime::TrajectorySessionLedger::Open(options);
    REQUIRE(session.has_value());
    lubancode::app::MemoryTurnLedger ledger(&*session);
    auto identity = lubancode::memory::ResolveProjectIdentity(root / "repo", root / "home");
    REQUIRE(identity.has_value());
    lubancode::memory::Options memory_options;
    memory_options.enabled = true; memory_options.global_allowed = true;
    lubancode::memory::ProjectMemory store(std::move(*identity), root / "home", memory_options);
    const lubancode::cli::Theme theme;
    auto probe = std::make_shared<Probe>(); probe->with_candidate = true;
    TurnMemoryExtractor extractor;
    HookScope hook(StandardFailureAt(MemoryWorkerPhase::BeforePublish));
    ledger.BeginTurn(session->session_id(), "turn-settle", "后台异常须保账，不得采纳候选");
    ledger.NoteExtractionCalled();
    const auto stream = session->session_dir() / (session->session_dir().filename().string() + ".jsonl");
    const auto before = lubancode::trajectory::ReadJournalLines(stream);
    REQUIRE(before.has_value());
    REQUIRE(extractor.Start(Inputs(probe, 54, "turn-settle")));
    ledger.SuspendTurn();
    REQUIRE(AwaitReady(extractor));
    auto out = extractor.TakeFinished();
    REQUIRE(out.has_value());
    CheckFailure(*out, 54, "turn-settle");
    CheckUsage(*out);
    lubancode::app::SessionTailContext tail;
    tail.project_memory = &store; tail.theme = &theme; tail.memory_turns = &ledger;
    lubancode::app::SettleTurnMemory(tail, *out, 17);
    CHECK(ledger.funnel().extract_batches == 1);
    CHECK(ledger.funnel().extract_failures == 1);
    CHECK(store.ListCandidates().empty());
    auto lines = lubancode::trajectory::ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    REQUIRE(lines->size() == before->size() + 1);
    auto event = nlohmann::json::parse(lines->back());
    REQUIRE(event.value("kind", std::string()) == "memory.extraction.assessed");
    const auto& payload = event.at("payload");
    CHECK(payload.value("extractOutcome", std::string()) == "failed");
    CHECK(payload.value("errorCode", std::string()) == "worker_execution_failed");
    CHECK(payload.value("usageReported", false));
    CHECK(payload.value("inputTokens", std::int64_t{-1}) == 13);
    CHECK(payload.value("outputTokens", std::int64_t{-1}) == 5);
    CHECK(payload.value("cachedTokens", std::int64_t{-1}) == 5);
    CHECK_FALSE(extractor.TakeFinished().has_value());
    CHECK_FALSE(ledger.SettleSuspendedTurn("turn-settle", 18, nullptr));
    CHECK(probe->calls.load() == 1);
    CHECK(probe->destroyed.load() == 1);
}
