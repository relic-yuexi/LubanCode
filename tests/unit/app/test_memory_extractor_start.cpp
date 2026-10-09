#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <system_error>
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
struct Probe { std::atomic<int> calls{0}; std::atomic<int> destroyed{0}; };
struct ProbeBackend final : lubancode::api::Backend {
    explicit ProbeBackend(std::shared_ptr<Probe> value) : probe(std::move(value)) {}
    ~ProbeBackend() override { ++probe->destroyed; }
    std::shared_ptr<Probe> probe;
    std::expected<void,lubancode::api::Error> send_stream(const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>& emit,
        const std::atomic<bool>*) override {
        ++probe->calls;
        emit(lubancode::api::MessageStart{"request", "cheap-m"});
        emit(lubancode::api::TextDelta{R"({"task_type":"research","summary":"已核材料","retrieval_terms":[],"candidates":[]})"});
        emit(lubancode::api::ContentBlockDone{0});
        lubancode::api::MessageDone done;
        done.stop_reason="end_turn";
        done.usage.input_tokens=3;
        done.usage.output_tokens=1;
        emit(done);
        return {};
    }
};
TurnMemoryExtractor::Inputs Inputs(const std::shared_ptr<Probe>& probe,
                                  std::uint64_t generation=37,std::string turn="turn-start") {
    TurnMemoryExtractor::Inputs input;
    input.backend=std::make_unique<ProbeBackend>(probe);
    input.model="cheap-m"; input.effort="low";
    input.system_prompt="private prompt"; input.transcript="private transcript";
    input.task_type="research"; input.session_generation=generation; input.turn_id=std::move(turn);
    return input;
}
struct HookScope {
    explicit HookScope(std::function<void()> hook)
        : previous(lubancode::app::testing::ExchangeMemoryWorkerStartHook(std::move(hook))) {}
    ~HookScope() { lubancode::app::testing::ExchangeMemoryWorkerStartHook(std::move(previous)); }
    std::function<void()> previous;
};
void StandardFailure() {
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                            "private exception secret");
}
void CheckFailure(const TurnMemoryExtractor::Outcome& out,std::uint64_t generation,
                  const std::string& turn) {
    CHECK_FALSE(out.ok);
    CHECK(out.error.code==lubancode::app::ExtractionErrorCode::WorkerStartFailed);
    CHECK(lubancode::app::StableExtractErrorCode(out.error)=="worker_start_failed");
    CHECK(out.error.message=="记忆抽取线程未能启动");
    CHECK(out.error.message.find("secret")==std::string::npos);
    CHECK(out.session_generation==generation); CHECK(out.turn_id==turn);
    CHECK(out.model=="cheap-m"); CHECK(out.task_type=="research");
    CHECK_FALSE(out.accounting.usage_reported);
    CHECK(out.accounting.usage.input_tokens==0); CHECK(out.accounting.usage.output_tokens==0);
    CHECK(out.accounting.usage.cache_read_tokens==0);
    CHECK(out.accounting.usage.cache_creation_tokens==0);
    CHECK(out.accounting.usage.reasoning_tokens==0);
    CHECK(out.accounting.duration_ms==0); CHECK(out.extract_wall_ms==0);
    CHECK(out.extraction.candidates.empty()); CHECK(out.extraction.retrieval_terms.empty());
}
bool AwaitReady(TurnMemoryExtractor& extractor) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (!extractor.Ready() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return extractor.Ready();
}
}  // namespace

TEST_CASE("Memory worker creation: standard failure preserves identity and destroys unused Backend") {
    std::cout << "[memory-worker-start-path] standard\n";
    HookScope hook(StandardFailure);
    auto probe=std::make_shared<Probe>();
    TurnMemoryExtractor extractor;
    REQUIRE(extractor.Start(Inputs(probe)));
    CHECK(probe->calls.load()==0); CHECK(probe->destroyed.load()==1);
    CHECK(extractor.Busy()); CHECK(extractor.Ready());
    extractor.RequestCancel();
    auto out=extractor.TakeFinished(); REQUIRE(out.has_value()); CheckFailure(*out,37,"turn-start");
    CHECK_FALSE(extractor.Busy()); CHECK_FALSE(extractor.Ready());
    CHECK_FALSE(extractor.TakeFinished().has_value());
}

TEST_CASE("Memory worker creation: unknown failures do not leak state across repeated starts") {
    std::cout << "[memory-worker-start-path] nonstandard\n";
    HookScope hook([] { throw 73; });
    auto probe=std::make_shared<Probe>();
    TurnMemoryExtractor extractor;
    for (std::uint64_t generation=1;generation<=10;++generation) {
        const auto turn="turn-"+std::to_string(generation);
        REQUIRE(extractor.Start(Inputs(probe,generation,turn)));
        auto out=extractor.TakeFinished(); REQUIRE(out.has_value()); CheckFailure(*out,generation,turn);
        CHECK_FALSE(extractor.Busy());
    }
    CHECK(probe->calls.load()==0); CHECK(probe->destroyed.load()==10);
    // A completed failure may also be abandoned without a thread to join or detach.
    const auto began=std::chrono::steady_clock::now();
    { TurnMemoryExtractor abandoned; REQUIRE(abandoned.Start(Inputs(probe,99,"abandoned"))); }
    CHECK(std::chrono::steady_clock::now()-began<std::chrono::seconds(1));
    CHECK(probe->destroyed.load()==11);
}

TEST_CASE("Memory worker creation: pending failure keeps single flight and collection permits real restart") {
    std::cout << "[memory-worker-start-path] restart\n";
    auto failed=std::make_shared<Probe>();
    auto rejected=std::make_shared<Probe>();
    auto recovered=std::make_shared<Probe>();
    TurnMemoryExtractor extractor;
    int attempts=0;
    { HookScope hook([&] { ++attempts; StandardFailure(); });
      REQUIRE(extractor.Start(Inputs(failed)));
      CHECK_FALSE(extractor.Start(Inputs(rejected,38,"rejected")));
      CHECK(attempts==1);
    }
    auto out=extractor.TakeFinished(); REQUIRE(out.has_value()); CheckFailure(*out,37,"turn-start");
    REQUIRE(extractor.Start(Inputs(recovered,39,"recovered")));
    REQUIRE(AwaitReady(extractor));
    auto success=extractor.TakeFinished(); REQUIRE(success.has_value());
    CHECK(success->ok); CHECK(success->turn_id=="recovered"); CHECK(success->session_generation==39);
    CHECK(recovered->calls.load()==1); CHECK(recovered->destroyed.load()==1);
    CHECK(failed->calls.load()==0); CHECK(rejected->calls.load()==0); CHECK(rejected->destroyed.load()==1);
    CHECK_FALSE(extractor.Busy());
}

TEST_CASE("Memory worker creation: invalid inputs and existing task never enter the creation boundary") {
    std::cout << "[memory-worker-start-path] gates\n";
    auto probe=std::make_shared<Probe>();
    TurnMemoryExtractor extractor;
    REQUIRE(extractor.Start(Inputs(probe)));
    int attempts=0;
    { HookScope hook([&] { ++attempts; StandardFailure(); });
      CHECK_FALSE(extractor.Start(Inputs(probe)));
      REQUIRE(AwaitReady(extractor));
      CHECK_FALSE(extractor.Start(Inputs(probe)));
      REQUIRE(extractor.TakeFinished().has_value());
      TurnMemoryExtractor::Inputs missing;
      CHECK_FALSE(extractor.Start(std::move(missing)));
      auto empty_model=Inputs(probe); empty_model.model.clear();
      CHECK_FALSE(extractor.Start(std::move(empty_model)));
      CHECK(attempts==0);
    }
    CHECK(probe->calls.load()==1);
}

TEST_CASE("Memory worker creation: actual failure settles the original CLI suspended turn as failed") {
    std::cout << "[memory-worker-start-path] settlement\n";
    const auto run=std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto root=fs::temp_directory_path()/("lmb-memory-start-"+std::to_string(run));
    fs::create_directories(root/"repo"); fs::create_directories(root/"home");
    lubancode::runtime::TrajectorySessionLedger::Options options;
    options.workspaces_root=root/"workspaces"; options.workspace_root=root/"repo";
    options.workspace_identity=lubancode::workspace::MakeFallbackIdentity(root/"repo");
    options.lubancode_version="test";
    auto session=lubancode::runtime::TrajectorySessionLedger::Open(options); REQUIRE(session.has_value());
    lubancode::app::MemoryTurnLedger ledger(&*session);
    auto identity=lubancode::memory::ResolveProjectIdentity(root/"repo",root/"home"); REQUIRE(identity.has_value());
    lubancode::memory::Options memory_options; memory_options.enabled=true; memory_options.global_allowed=true;
    lubancode::memory::ProjectMemory store(std::move(*identity),root/"home",memory_options);
    const lubancode::cli::Theme theme;
    auto probe=std::make_shared<Probe>();
    TurnMemoryExtractor extractor;
    HookScope hook(StandardFailure);
    ledger.BeginTurn(session->session_id(),"turn-settle","抽取线程创建失败仍须如实落账");
    ledger.NoteExtractionCalled();
    const auto stream=session->session_dir()/(session->session_dir().filename().string()+".jsonl");
    const auto before_start=lubancode::trajectory::ReadJournalLines(stream);
    REQUIRE(before_start.has_value());
    auto input=Inputs(probe,41,"turn-settle"); input.trajectory=&*session;
    input.trajectory_wire="anthropic"; input.provider="test";
    REQUIRE(extractor.Start(std::move(input))); ledger.SuspendTurn();
    auto out=extractor.TakeFinished(); REQUIRE(out.has_value()); CheckFailure(*out,41,"turn-settle");
    const auto after_start=lubancode::trajectory::ReadJournalLines(stream);
    REQUIRE(after_start.has_value());
    CHECK(*before_start==*after_start);  // No bypass request, message or usage was invented.
    lubancode::app::SessionTailContext tail; tail.project_memory=&store; tail.theme=&theme; tail.memory_turns=&ledger;
    lubancode::app::SettleTurnMemory(tail,*out,17);
    CHECK(ledger.funnel().extract_batches==1); CHECK(ledger.funnel().extract_failures==1);
    CHECK(store.ListCandidates().empty()); CHECK(probe->calls.load()==0); CHECK(probe->destroyed.load()==1);
    auto lines=lubancode::trajectory::ReadJournalLines(stream); REQUIRE(lines.has_value());
    CHECK(lines->size()==before_start->size()+1);  // Only the actual failed assessment is appended.
    std::vector<nlohmann::json> assessed;
    for (const auto& line:*lines) {
        auto event=nlohmann::json::parse(line,nullptr,false);
        if (!event.is_discarded() && event.value("kind",std::string())=="memory.extraction.assessed")
            assessed.push_back(std::move(event));
    }
    REQUIRE(assessed.size()==1);
    const auto& payload=assessed[0].at("payload");
    CHECK(payload.value("decision",std::string())=="called");
    CHECK(payload.value("extractOutcome",std::string())=="failed");
    CHECK(payload.value("errorCode",std::string())=="worker_start_failed");
    CHECK_FALSE(payload.contains("usageReported"));
    CHECK_FALSE(payload.contains("inputTokens")); CHECK_FALSE(payload.contains("outputTokens"));
    CHECK_FALSE(extractor.TakeFinished().has_value());
    auto pending=ledger.SettleSuspendedTurn("turn-settle",18,nullptr); CHECK_FALSE(pending);
}
