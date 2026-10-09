// Future native proof source. No local execution or acceptance claimed.
#include <doctest/doctest.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <exception>
#include <expected>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "app/bypass_worker_test_hooks.hpp"
#include "app/commands/memory_commands.hpp"
#include "app/session_title_refiner.hpp"
#include "app/turn_memory_extractor.hpp"
#include "agent/sample_model.hpp"
#include "hooks/hash.hpp"
#include "memory_legacy_fixture.hpp"
#include "runtime/trajectory_session.hpp"
#include "runtime/trajectory_bypass_lease.hpp"
#include "telemetry/wake.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace lmb = lubancode;
using Json = nlohmann::json;
using Ledger = lmb::runtime::TrajectorySessionLedger;
using Clock = std::chrono::steady_clock;

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    bool released = false;
    bool expired = false;
    void Hold() {
        std::unique_lock lock(mutex);
        ++arrived; cv.notify_all();
        if (!cv.wait_for(lock, std::chrono::seconds(10), [&] { return released; })) expired = true;
    }
    bool Await(int count = 1) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(5), [&] { return arrived >= count; });
    }
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
    bool Expired() { std::lock_guard lock(mutex); return expired; }
};
struct ReleaseGuard {
    std::shared_ptr<Gate> gate;
    ~ReleaseGuard() { gate->Release(); }
};
struct HookScope {
    lmb::app::testing::BypassWorkerHook previous;
    explicit HookScope(const std::shared_ptr<Gate>& gate)
        : previous(lmb::app::testing::ExchangeBypassWorkerHook(
              [gate](lmb::app::testing::BypassWorkerPurpose, lmb::app::testing::BypassWorkerPhase phase) {
                  if (phase == lmb::app::testing::BypassWorkerPhase::BeforeSampling) gate->Hold();
              })) {}
    ~HookScope() { lmb::app::testing::ExchangeBypassWorkerHook(std::move(previous)); }
};
struct Probe {
    std::atomic<int> calls{0}, destroyed{0};
    std::shared_ptr<Gate> backend_gate;
    bool memory = true;
};
struct FaultScope {
    lmb::app::testing::BypassWorkerHook previous;
    explicit FaultScope(int kind)
        : previous(lmb::app::testing::ExchangeBypassWorkerHook(
            [kind](lmb::app::testing::BypassWorkerPurpose purpose, lmb::app::testing::BypassWorkerPhase phase) {
                if (purpose != lmb::app::testing::BypassWorkerPurpose::Memory ||
                    phase != lmb::app::testing::BypassWorkerPhase::BeforeBinding) return;
                if (kind == 1) throw std::runtime_error("private binding secret");
                throw 73;
            })) {}
    ~FaultScope() { lmb::app::testing::ExchangeBypassWorkerHook(std::move(previous)); }
};
struct Backend final : lmb::api::Backend {
    explicit Backend(std::shared_ptr<Probe> value) : probe(std::move(value)) {}
    ~Backend() override { ++probe->destroyed; }
    std::shared_ptr<Probe> probe;
    std::expected<void,lmb::api::Error> send_stream(const lmb::api::Request&,
        const std::function<void(const lmb::api::StreamEvent&)>& emit,
        const std::atomic<bool>*) override {
        ++probe->calls;
        // This is after actual admission and physical Backend entry. It proves
        // late-return fencing, not the earlier frozen-trigger identity.
        if (probe->backend_gate) probe->backend_gate->Hold();
        emit(lmb::api::MessageStart{"real-bypass-response","cheap-m"});
        emit(lmb::api::TextDelta{probe->memory
            ? R"({"task_type":"research","summary":"实际旁路回包","retrieval_terms":[],"candidates":[]})"
            : "旁路关场"});
        emit(lmb::api::ContentBlockDone{0});
        lmb::api::MessageDone done; done.stop_reason = "end_turn";
        done.usage.input_tokens = 11; done.usage.output_tokens = 4;
        emit(done); return {};
    }
};
struct Worker {
    bool memory;
    lmb::app::TurnMemoryExtractor extractor;
    lmb::app::SessionTitleRefiner refiner;
    explicit Worker(bool kind) : memory(kind) {}
    bool Start(Ledger& ledger, const std::shared_ptr<Probe>& probe, const std::string& turn) {
        probe->memory = memory;
        if (memory) {
            lmb::app::TurnMemoryExtractor::Inputs input;
            input.backend = std::make_unique<Backend>(probe); input.model = "cheap-m";
            input.system_prompt = "test system"; input.transcript = "test transcript";
            input.turn_id = turn; input.session_generation = 23; input.task_type = "research";
            input.trajectory = &ledger; input.provider = "probe"; input.trajectory_wire = "responses";
            return extractor.Start(std::move(input));
        }
        lmb::app::SessionTitleRefiner::Inputs input;
        input.backend = std::make_unique<Backend>(probe); input.model = "cheap-m";
        input.first_query = "核对旁路寿命"; input.generation = 23;
        input.trajectory = &ledger; input.provider = "probe"; input.trajectory_wire = "responses";
        return refiner.Start(std::move(input));
    }
    bool Ready() { return memory ? extractor.Ready() : refiner.Ready(); }
    void Collect(bool success) {
        const auto until = Clock::now()+std::chrono::seconds(5);
        while (!Ready() && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(Ready());
        if (memory) {
            auto out = extractor.TakeFinished(); REQUIRE(out.has_value()); CHECK(out->ok == success);
            CHECK(out->turn_id.empty() == false); CHECK_FALSE(extractor.TakeFinished().has_value());
            if (success) {
                CHECK(out->accounting.usage_reported);
                CHECK(out->accounting.usage.input_tokens == 11);
                CHECK(out->accounting.usage.output_tokens == 4);
            }
        } else {
            auto out = refiner.TakeFinished(); REQUIRE(out.has_value()); CHECK(out->ok == success);
            CHECK(out->generation == 23); CHECK_FALSE(refiner.TakeFinished().has_value());
            if (success) {
                CHECK(out->accounting.usage_reported);
                CHECK(out->accounting.usage.input_tokens == 11);
                CHECK(out->accounting.usage.output_tokens == 4);
            }
        }
    }
};
struct WakeProbe { std::atomic<int> notifications{0}, destroyed{0}; };
struct Observer final : lmb::telemetry::CommitObserver {
    explicit Observer(std::shared_ptr<WakeProbe> value) : probe(std::move(value)) {}
    ~Observer() override { ++probe->destroyed; }
    void Notify(const lmb::telemetry::CommitWake&) noexcept override { ++probe->notifications; }
    std::shared_ptr<WakeProbe> probe;
};
struct Fixture {
    fs::path root, stream;
    std::unique_ptr<Ledger> ledger;
    std::unique_ptr<lmb::runtime::TrajectoryTurnBridge> main;
    bool v3;
    explicit Fixture(bool modern, const std::function<void(Ledger::Options&)>& setup = {}, bool managed = false) : v3(modern) {
        static std::atomic<unsigned> next{0};
        root = fs::temp_directory_path()/
            ("lmb-bypass-owner-"+std::to_string(Clock::now().time_since_epoch().count())+"-"+std::to_string(++next));
        fs::create_directories(root/"repo"); fs::create_directories(root/"home");
        if (managed) {
            REQUIRE(v3); REQUIRE_FALSE(static_cast<bool>(setup));
            ledger = lmb::runtime::testing::MemoryDurableLegacyFixtureAccess::OpenManaged(root);
        } else if (v3) {
            Ledger::Options options;
            options.workspace_root = root/"repo"; options.workspaces_root = root/"workspaces";
            options.workspace_identity = lmb::workspace::MakeFallbackIdentity(root/"repo");
            options.lubancode_version = "test"; options.v3_system_content = "test system";
            if (setup) setup(options);
            auto opened = Ledger::Open(options); REQUIRE(opened.has_value());
            ledger = std::make_unique<Ledger>(std::move(*opened));
        } else ledger = lmb::runtime::testing::OpenRecoveredMemoryLegacyLedger(root);
        REQUIRE((ledger->v3_main_writer() != nullptr) == v3);
        stream = ledger->session_dir()/(v3 ? ledger->session_id()+".jsonl" : "main.jsonl");
    }
    void Input(const std::string& id) {
        main = ledger->NewTurnBridge({"probe","responses","terminal"}); REQUIRE(main != nullptr);
        main->BeginTurn(id,"external_user");
        lmb::api::Message input; input.role = lmb::api::Role::User;
        input.content.push_back(lmb::api::TextBlock{"real-input-"+id});
        main->RecordInput(input); main->EndTurn(true,false,"");
    }
    void Close() { main.reset(); REQUIRE(ledger->CloseSession("test_done").error_code.empty()); }
};
std::string Bytes(const fs::path& path) {
    std::ifstream file(path,std::ios::binary); REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::vector<Json> Rows(const fs::path& path) {
    auto lines = lmb::trajectory::ReadJournalLines(path); REQUIRE(lines.has_value());
    std::vector<Json> rows;
    for (const auto& raw : *lines) {
        auto row = Json::parse(raw,nullptr,false); REQUIRE_FALSE(row.is_discarded());
        rows.push_back(std::move(row));
    }
    return rows;
}
void CheckProbe(const std::shared_ptr<Probe>& probe, int calls) {
    CHECK(probe->calls.load() == calls); CHECK(probe->destroyed.load() == 1);
}
}  // namespace

TEST_CASE("Memory bypass owner: real diagnostic producers and ledger snapshots share one protected sink") {
    std::cout << "[memory-bypass-owner-path] diagnostics\n";
    const auto observe = [](Ledger& ledger,const std::vector<std::function<void()>>& work) {
        auto gate = std::make_shared<Gate>();
        std::atomic<std::size_t> alive{work.size()};
        std::vector<std::exception_ptr> errors(work.size());
        std::vector<std::jthread> threads(work.size());
        ReleaseGuard release{gate};
        for (std::size_t i = 0; i < work.size(); ++i)
            threads[i] = std::jthread([&,i] {
                try {
                    gate->Hold();
                    for (int n = 0; n < 16; ++n) work[i]();
                } catch (...) { errors[i] = std::current_exception(); }
                alive.fetch_sub(1,std::memory_order_release);
            });
        REQUIRE(gate->Await(static_cast<int>(work.size())));
        auto previous = ledger.recent_io_errors().size();
        gate->Release();
        const auto until = Clock::now()+std::chrono::seconds(5);
        std::size_t snapshots = 0;
        do {
            const auto notes = ledger.recent_io_errors();
            CHECK(notes.size() >= previous);
            for (const auto& note : notes) CHECK_FALSE(note.empty());
            previous = notes.size(); ++snapshots;
            std::this_thread::yield();
        } while (alive.load(std::memory_order_acquire) != 0 && Clock::now() < until);
        CHECK(alive.load(std::memory_order_acquire) == 0);
        for (auto& thread : threads) thread.join();
        for (const auto& error : errors) REQUIRE(error == nullptr);
        CHECK(snapshots > 0); CHECK_FALSE(gate->Expired());
    };
    {
        auto armed = std::make_shared<std::atomic<bool>>(false);
        Fixture f(true,[&](Ledger::Options& options) {
            options.v3_main_io_fault = [armed]() -> std::optional<std::string> {
                if (armed->exchange(false)) return "diagnostics.main.failed";
                return std::nullopt;
            };
        });
        f.Input("turn-diagnostics");
        auto recorder = f.ledger->NewLeasedBypassRecorder(
            {"probe","responses","terminal"},lmb::accounting::RequestPurpose::MemoryExtract);
        REQUIRE(recorder != nullptr);
        lmb::api::Message input; input.role = lmb::api::Role::User;
        input.content.push_back(lmb::api::TextBlock{"real rejected input"});
        armed->store(true);
        f.main->RecordInput(input); // Actual writer fault, not a fabricated receipt.
        const auto before = Bytes(f.stream);
        const auto original = f.ledger->recent_io_errors().size();
        auto probe = std::make_shared<Probe>();
        {
            Backend backend(probe);
            lmb::agent::SampleRequest request;
            request.model = "cheap-m"; request.system = "test system"; request.messages.push_back(input);
            lmb::agent::SampleOptions options;
            options.boundary_recorder = recorder.get();
            options.purpose = lmb::accounting::RequestPurpose::MemoryExtract;
            observe(*f.ledger,{
                [&] { f.main->RecordInput(input); },
                [&] {
                    auto child = f.ledger->SpawnSubagent("diagnostics-call","actual child failure");
                    if (child.has_value() || child.error().error_code != "trajectory.subagent_v3_request")
                        throw std::runtime_error("actual child diagnostic boundary differs");
                },
                [&] {
                    if (lmb::agent::SampleModel(backend,request,options).ok)
                        throw std::runtime_error("broken writer admitted a real sample");
                }
            });
        }
        CheckProbe(probe,0); CHECK(Bytes(f.stream) == before);
        const auto notes = f.ledger->recent_io_errors();
        int main = 0, child = 0, bypass = 0;
        for (std::size_t i = original; i < notes.size(); ++i) {
            if (notes[i].find("user message:v3writer.broken") == 0) ++main;
            if (notes[i].find("subagent.start_failed:reserve_stream:trajectory.subagent_v3_request") == 0) ++child;
            if (notes[i].find("bypass system:v3writer.broken") == 0) ++bypass;
        }
        CHECK(main == 16); CHECK(child == 16); CHECK(bypass == 16);
        CHECK(notes.size() == original+48);
        // A truly failed writer cannot be falsely closed as clean.
        recorder.reset(); f.main.reset(); f.ledger.reset();
    }
    {
        Fixture f(false); f.Input("turn-workflow-diagnostics");
        lmb::runtime::testing::MemoryDurableLegacyFixtureAccess::SetWorkflowNodeFault(*f.ledger);
        lmb::runtime::TrajectoryWorkflowRunBridge::DefinitionInfo definition;
        definition.workflow_id = "diagnostics-flow"; definition.workflow_version = "1";
        definition.definition_json = R"({"id":"diagnostics-flow"})";
        definition.content_hash = lmb::hooks::Sha256Hex(definition.definition_json);
        definition.cwd = (f.root/"repo").generic_string();
        auto workflow = f.ledger->SpawnWorkflowRun("diagnostics-workflow",definition);
        REQUIRE(workflow.has_value());
        const auto original = f.ledger->recent_io_errors().size();
        const auto before = Bytes(f.stream);
        int node = 0;
        observe(*f.ledger,{[&] {
            auto failed = (*workflow)->SpawnNodeStream(
                "probe-node","diagnostics-node-"+std::to_string(++node),1,"llm",-1);
            if (failed.has_value() || failed.error().stage != "run_started" ||
                failed.error().error_code.find("diagnostics.node.refused") == std::string::npos)
                throw std::runtime_error("actual workflow node diagnostic boundary differs");
        }});
        const auto notes = f.ledger->recent_io_errors();
        REQUIRE(notes.size() == original+16);
        for (std::size_t i = original; i < notes.size(); ++i)
            CHECK(notes[i].find("workflow_node.start_failed:run_started:") == 0);
        CHECK(node == 16); CHECK(Bytes(f.stream) == before);
        (*workflow)->Finish(false,false,"diagnostics fixture complete"); workflow->reset();
        f.Close();
    }
}

TEST_CASE("Memory bypass owner: retirement fences an in-flight real bridge factory and all later admission") {
    std::cout << "[memory-bypass-owner-path] bind-retire\n";
    using Owner = lmb::runtime::TrajectoryBypassLeaseOwner;
    for (bool modern : {false,true}) {
        Fixture f(modern); f.Input("turn-bind-retire");
        const auto before = Bytes(f.stream);
        Owner owner;
        auto gate = std::make_shared<Gate>();
        std::atomic<int> factory_calls{0};
        std::atomic<bool> retirement_attempted{false}, retirement_returned{false};
        std::exception_ptr binding_error;
        std::expected<std::unique_ptr<lmb::agent::LoopBoundaryRecorder>,Owner::BindError>
            bound = std::unexpected(Owner::BindError::MissingRecorder);
        std::jthread binder([&] {
            try {
                bound = owner.Bind([&] {
                    ++factory_calls;
                    // Actual production factory, actual legacy or V3 writer.
                    // The ledger stays alive until both threads have joined.
                    auto bridge = f.ledger->NewBypassBridge(
                        {"probe","responses","terminal"},lmb::accounting::RequestPurpose::MemoryExtract);
                    gate->Hold();
                    return bridge;
                });
            } catch (...) { binding_error = std::current_exception(); }
        });
        std::jthread retiring;
        // Release precedes jthread destruction even if a REQUIRE aborts.
        ReleaseGuard release{gate};
        REQUIRE(gate->Await());
        retiring = std::jthread([&] {
            retirement_attempted.store(true,std::memory_order_release);
            owner.Retire();
            retirement_returned.store(true,std::memory_order_release);
        });
        const auto until = Clock::now()+std::chrono::seconds(5);
        while (!retirement_attempted.load(std::memory_order_acquire) && Clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(retirement_attempted.load(std::memory_order_acquire));
        CHECK_FALSE(retirement_returned.load(std::memory_order_acquire));
        gate->Release();
        binder.join(); retiring.join();
        REQUIRE(binding_error == nullptr); REQUIRE(bound.has_value()); REQUIRE(*bound != nullptr);
        CHECK(retirement_returned.load(std::memory_order_acquire));
        CHECK(factory_calls.load() == 1); CHECK_FALSE(gate->Expired());
        auto later = owner.Bind([&] {
            ++factory_calls;
            return f.ledger->NewBypassBridge(
                {"probe","responses","terminal"},lmb::accounting::RequestPurpose::MemoryExtract);
        });
        REQUIRE_FALSE(later.has_value()); CHECK(later.error() == Owner::BindError::Retired);
        CHECK(factory_calls.load() == 1);
        auto probe = std::make_shared<Probe>();
        {
            Backend backend(probe);
            lmb::agent::SampleRequest request;
            request.model = "cheap-m"; request.system = "test system";
            lmb::api::Message input; input.role = lmb::api::Role::User;
            input.content.push_back(lmb::api::TextBlock{"actual rejected sample"});
            request.messages.push_back(std::move(input));
            lmb::agent::SampleOptions options;
            options.boundary_recorder = bound->get();
            options.purpose = lmb::accounting::RequestPurpose::MemoryExtract;
            const auto result = lmb::agent::SampleModel(backend,request,options);
            CHECK_FALSE(result.ok);
        }
        CheckProbe(probe,0);
        CHECK(Bytes(f.stream) == before);
        (*bound)->OnOutputFailed("late-rejected-request","late completion");
        CHECK(Bytes(f.stream) == before);
        owner.Retire(); // Actual repeated retirement stays idempotent.
        bound->reset();
        f.Close();
    }
}

TEST_CASE("Memory bypass owner: actual Memory and Title freeze the first input before the next input") {
    std::cout << "[memory-bypass-owner-path] trigger\n";
    Fixture f(true); f.Input("turn-first");
    auto gate = std::make_shared<Gate>(); auto memory = std::make_shared<Probe>(); auto title = std::make_shared<Probe>();
    Worker mw(true), tw(false); ReleaseGuard release{gate};
    { HookScope scope(gate);
      REQUIRE(mw.Start(*f.ledger,memory,"turn-first")); REQUIRE(tw.Start(*f.ledger,title,"turn-first")); }
    REQUIRE(gate->Await(2)); CHECK(memory->calls.load() == 0); CHECK(title->calls.load() == 0);
    f.Input("turn-second"); gate->Release(); mw.Collect(true); tw.Collect(true);
    CheckProbe(memory,1); CheckProbe(title,1); CHECK_FALSE(gate->Expired());
    int prepared_memory = 0, prepared_title = 0, inputs = 0;
    for (const auto& row : Rows(f.stream)) {
        if (row.value("type",std::string()) == "message" &&
            row.contains("message") && row.at("message").value("role",std::string()) == "user" &&
            row.value("purpose",std::string()) == "conversation") ++inputs;
        if (row.value("kind",std::string()) != "model.request.prepared") continue;
        const auto purpose = row.at("payload").value("purpose",std::string());
        if (purpose == "memory_extract") {
            ++prepared_memory; CHECK(row.value("parentTurnId",std::string()) == "turn-first");
            CHECK(row.value("turnId",std::string()).find("memory-turn-") == 0);
        } else if (purpose == "title_refine") {
            ++prepared_title; CHECK(row.value("turnId",std::string()) == "turn-first");
        }
    }
    CHECK(prepared_memory == 1); CHECK(prepared_title == 1); CHECK(inputs == 2);
    f.Close(); CHECK(lmb::trajectory::v3::ReadV3Ledger(f.stream).has_value());
}

TEST_CASE("Memory bypass owner: closing before real admission rejects both workers without Backend entry") {
    std::cout << "[memory-bypass-owner-path] admission\n";
    for (bool modern : {false,true}) for (bool memory : {false,true}) {
        Fixture f(modern); f.Input("turn-close-before-send");
        auto gate = std::make_shared<Gate>(); auto probe = std::make_shared<Probe>();
        Worker worker(memory); ReleaseGuard release{gate};
        { HookScope scope(gate); REQUIRE(worker.Start(*f.ledger,probe,"turn-close-before-send")); }
        REQUIRE(gate->Await()); f.Close(); const auto closed = Bytes(f.stream);
        gate->Release(); worker.Collect(false); CheckProbe(probe,0);
        CHECK(Bytes(f.stream) == closed); CHECK_FALSE(gate->Expired());
    }
}

TEST_CASE("Memory bypass owner: physical Backend late return cannot append after actual close") {
    std::cout << "[memory-bypass-owner-path] late-close\n";
    for (bool modern : {false,true}) for (bool memory : {false,true}) {
        Fixture f(modern); f.Input("turn-late-close");
        auto gate = std::make_shared<Gate>(); auto probe = std::make_shared<Probe>(); probe->backend_gate = gate;
        Worker worker(memory); ReleaseGuard release{gate};
        REQUIRE(worker.Start(*f.ledger,probe,"turn-late-close")); REQUIRE(gate->Await());
        CHECK(probe->calls.load() == 1); f.Close(); const auto closed = Bytes(f.stream);
        // Real dispatched work may still return a successful value. The lease
        // fences late Journal callbacks; the CLI generation gate owns adoption.
        gate->Release(); worker.Collect(true); CheckProbe(probe,1);
        CHECK(Bytes(f.stream) == closed); CHECK_FALSE(gate->Expired());
    }
}

TEST_CASE("Memory bypass owner: moving the actual ledger revokes old workers and admits a new binding") {
    std::cout << "[memory-bypass-owner-path] move\n";
    for (bool modern : {false,true}) for (bool memory : {false,true}) {
        Fixture f(modern); f.Input("turn-move");
        auto gate = std::make_shared<Gate>(); auto stale = std::make_shared<Probe>();
        Worker old(memory); ReleaseGuard release{gate};
        { HookScope scope(gate); REQUIRE(old.Start(*f.ledger,stale,"turn-move")); }
        REQUIRE(gate->Await()); f.main.reset();
        auto moved = std::make_unique<Ledger>(std::move(*f.ledger)); f.ledger.reset(); f.ledger = std::move(moved);
        const auto before = Bytes(f.stream); gate->Release(); old.Collect(false); CheckProbe(stale,0);
        CHECK(Bytes(f.stream) == before);
        Worker fresh(memory); auto healthy = std::make_shared<Probe>();
        REQUIRE(fresh.Start(*f.ledger,healthy,"turn-move")); fresh.Collect(true); CheckProbe(healthy,1);
        CHECK(Bytes(f.stream).size() > before.size()); CHECK_FALSE(gate->Expired()); f.Close();
    }
}

TEST_CASE("Memory bypass owner: actual ledger destruction leaves no raw borrow in a late Backend return") {
    std::cout << "[memory-bypass-owner-path] destroy\n";
    for (bool modern : {false,true}) for (bool memory : {false,true}) {
        Fixture f(modern); f.Input("turn-destroy");
        auto gate = std::make_shared<Gate>(); auto probe = std::make_shared<Probe>(); probe->backend_gate = gate;
        Worker worker(memory); ReleaseGuard release{gate};
        REQUIRE(worker.Start(*f.ledger,probe,"turn-destroy")); REQUIRE(gate->Await());
        f.main.reset(); f.ledger.reset(); const auto final_bytes = Bytes(f.stream);
        gate->Release(); worker.Collect(true); CheckProbe(probe,1);
        CHECK(Bytes(f.stream) == final_bytes); CHECK_FALSE(gate->Expired());
    }
}

TEST_CASE("Memory bypass owner: replacing an Observer at the same address fences physical late callbacks") {
    std::cout << "[memory-bypass-owner-path] observer\n";
    for (bool modern : {false,true}) for (bool memory : {false,true}) {
        Fixture f(modern); f.Input("turn-observer"); f.main.reset();
        auto old_wake = std::make_shared<WakeProbe>(), new_wake = std::make_shared<WakeProbe>();
        std::optional<Observer> observer; observer.emplace(old_wake);
        const auto* old_address = &*observer; f.ledger->SetTelemetryWake(&*observer);
        auto gate = std::make_shared<Gate>(); auto probe = std::make_shared<Probe>(); probe->backend_gate = gate;
        Worker old(memory); ReleaseGuard release{gate};
        REQUIRE(old.Start(*f.ledger,probe,"turn-observer")); REQUIRE(gate->Await());
        const auto notifications = old_wake->notifications.load();
        CHECK(notifications > 0);
        // Calling the setter with the same pointer must retire old captures.
        // Retire before destroying the observer, even when its address is reused.
        f.ledger->SetTelemetryWake(&*observer);
        observer.reset(); CHECK(old_wake->destroyed.load() == 1);
        observer.emplace(new_wake); CHECK(&*observer == old_address);
        f.ledger->SetTelemetryWake(&*observer);
        const auto before = Bytes(f.stream);
        gate->Release(); old.Collect(true); CheckProbe(probe,1);
        CHECK(Bytes(f.stream) == before); CHECK(old_wake->notifications.load() == notifications);
        CHECK(new_wake->notifications.load() == 0);
        Worker fresh(memory); auto healthy = std::make_shared<Probe>();
        REQUIRE(fresh.Start(*f.ledger,healthy,"turn-observer")); fresh.Collect(true); CheckProbe(healthy,1);
        CHECK(new_wake->notifications.load() > 0); CHECK_FALSE(gate->Expired());
        f.ledger->SetTelemetryWake(nullptr); f.Close();
    }
}

TEST_CASE("Memory bypass owner: actual same-ID continuation never reactivates the old callback owner") {
    std::cout << "[memory-bypass-owner-path] resume\n";
    for (bool memory : {false,true}) {
        Fixture f(true); f.Input("turn-before-resume");
        const auto id = f.ledger->session_id();
        auto gate = std::make_shared<Gate>(); auto probe = std::make_shared<Probe>(); probe->backend_gate = gate;
        Worker old(memory); ReleaseGuard release{gate};
        REQUIRE(old.Start(*f.ledger,probe,"turn-before-resume")); REQUIRE(gate->Await());
        f.Close(); f.ledger.reset();
        Ledger::Options options;
        options.workspace_root = f.root/"repo"; options.workspaces_root = f.root/"workspaces";
        options.workspace_identity = lmb::workspace::MakeFallbackIdentity(f.root/"repo");
        options.lubancode_version = "test"; options.v3_system_content = "test system";
        options.resume_at_launch = true; options.resume_source_session_id = id;
        auto opened = Ledger::Open(options); REQUIRE(opened.has_value());
        f.ledger = std::make_unique<Ledger>(std::move(*opened));
        REQUIRE(f.ledger->resumed_at_launch()); CHECK(f.ledger->session_id() == id);
        f.Input("turn-after-resume"); const auto before = Bytes(f.stream);
        gate->Release(); old.Collect(true); CheckProbe(probe,1);
        CHECK(Bytes(f.stream) == before);
        Worker fresh(memory); auto healthy = std::make_shared<Probe>();
        REQUIRE(fresh.Start(*f.ledger,healthy,"turn-after-resume")); fresh.Collect(true); CheckProbe(healthy,1);
        CHECK(Bytes(f.stream).size() > before.size()); CHECK_FALSE(gate->Expired()); f.Close();
    }
}

TEST_CASE("Memory bypass owner: front binding exceptions keep real Memory terminal identity and disposal") {
    std::cout << "[memory-bypass-owner-path] binding-failure\n";
    for (bool modern : {false,true}) for (int kind : {1,2}) {
        Fixture f(modern); f.Input("turn-binding-failure"); const auto before = Bytes(f.stream);
        auto probe = std::make_shared<Probe>();
        lmb::app::TurnMemoryExtractor extractor;
        lmb::app::TurnMemoryExtractor::Inputs input;
        input.backend = std::make_unique<Backend>(probe); input.model = "cheap-m";
        input.task_type = "research"; input.turn_id = "turn-binding-failure"; input.session_generation = 31;
        input.system_prompt = "private prompt"; input.transcript = "private transcript";
        input.trajectory = f.ledger.get(); input.provider = "probe"; input.trajectory_wire = "responses";
        { FaultScope scope(kind); REQUIRE(extractor.Start(std::move(input))); }
        CHECK(extractor.Ready()); CHECK(extractor.Busy());
        CHECK(input.backend == nullptr); CheckProbe(probe,0);
        auto out = extractor.TakeFinished(); REQUIRE(out.has_value());
        CHECK_FALSE(out->ok); CHECK(out->turn_id == "turn-binding-failure"); CHECK(out->session_generation == 31);
        CHECK(out->model == "cheap-m"); CHECK(out->task_type == "research");
        CHECK(lmb::app::StableExtractErrorCode(out->error) == "worker_execution_failed");
        CHECK(out->error.message.find("secret") == std::string::npos);
        CHECK_FALSE(out->extraction_invoked); CHECK_FALSE(out->accounting.usage_reported);
        lmb::agent::ModelUsageLedger calls;
        CHECK_FALSE(lmb::app::RecordTurnMemoryCall(calls,*out));
        CHECK(calls.by_role().empty());
        CHECK_FALSE(extractor.Busy()); CHECK_FALSE(extractor.TakeFinished().has_value());
        CHECK(Bytes(f.stream) == before);
        // This fault scope was front-local; a real subsequent sample remains usable.
        Worker recovered(true); auto healthy = std::make_shared<Probe>();
        REQUIRE(recovered.Start(*f.ledger,healthy,"turn-binding-failure"));
        recovered.Collect(true); CheckProbe(healthy,1); f.Close();
    }
}

TEST_CASE("Memory bypass owner: actual clear revokes the old scene before rebinding the new scene") {
    std::cout << "[memory-bypass-owner-path] clear\n";
    for (bool modern : {false,true}) for (bool memory : {false,true}) {
        Fixture f(modern); f.Input("turn-before-clear"); f.main.reset();
        const auto old_id = f.ledger->session_id(); const auto old_stream = f.stream;
        auto gate = std::make_shared<Gate>(); auto old_probe = std::make_shared<Probe>(); old_probe->backend_gate = gate;
        Worker old(memory); ReleaseGuard release{gate};
        REQUIRE(old.Start(*f.ledger,old_probe,"turn-before-clear")); REQUIRE(gate->Await());
        lmb::trajectory::ClearRequest request;
        const auto cleared = f.ledger->ClearSession(request,nullptr);
        REQUIRE(cleared.error_code.empty()); CHECK(cleared.active_switched);
        CHECK(cleared.old_session_id == old_id); CHECK(cleared.new_session_id != old_id);
        CHECK_FALSE(old.Ready());  // Clear did not join the held physical Backend.
        f.stream = f.ledger->session_dir()/(modern ? f.ledger->session_id()+".jsonl" : "main.jsonl");
        f.Input("turn-after-clear");
        const auto old_bytes = Bytes(old_stream), new_bytes = Bytes(f.stream);
        gate->Release(); old.Collect(true); CheckProbe(old_probe,1);
        CHECK(Bytes(old_stream) == old_bytes); CHECK(Bytes(f.stream) == new_bytes);
        Worker fresh(memory); auto healthy = std::make_shared<Probe>();
        REQUIRE(fresh.Start(*f.ledger,healthy,"turn-after-clear")); fresh.Collect(true); CheckProbe(healthy,1);
        CHECK(Bytes(old_stream) == old_bytes); CHECK(Bytes(f.stream).size() > new_bytes.size());
        CHECK_FALSE(gate->Expired()); f.Close();
    }
}

TEST_CASE("Memory bypass owner: real resume source refusals preserve the actual old binding") {
    std::cout << "[memory-bypass-owner-path] resume-refusal\n";
    for (bool modern : {false,true}) for (bool memory : {false,true}) for (bool one_shot : {false,true}) {
        Fixture f(modern); f.Input("turn-refused-resume");
        std::string source = "../outside";
        if (one_shot) {
            Ledger::Options options;
            options.workspace_root = f.root/"repo"; options.workspaces_root = f.root/"workspaces";
            options.workspace_identity = lmb::workspace::MakeFallbackIdentity(f.root/"repo");
            options.lubancode_version = "test"; options.v3_system_content = "test system";
            options.one_shot = true;
            auto real_source = Ledger::Open(options); REQUIRE(real_source.has_value());
            source = real_source->session_id();
            REQUIRE(real_source->CloseSession("one_shot_done").error_code.empty());
        }
        const auto id = f.ledger->session_id(); const auto before = Bytes(f.stream);
        auto gate = std::make_shared<Gate>(); auto probe = std::make_shared<Probe>();
        Worker worker(memory); ReleaseGuard release{gate};
        { HookScope scope(gate); REQUIRE(worker.Start(*f.ledger,probe,"turn-refused-resume")); }
        REQUIRE(gate->Await()); const auto refused = f.ledger->ResumeInteractive(source,"resume");
        REQUIRE_FALSE(refused.outcome.error_code.empty());
        CHECK(f.ledger->session_id() == id); CHECK(Bytes(f.stream) == before);
        gate->Release(); worker.Collect(true); CheckProbe(probe,1);
        CHECK(Bytes(f.stream).size() > before.size()); CHECK_FALSE(gate->Expired()); f.Close();
    }
}

TEST_CASE("Memory bypass owner: actual new-scene write failure in clear step1 keeps old recorder live") {
    std::cout << "[memory-bypass-owner-path] clear-step1\n";
    for (bool memory : {false,true}) {
        auto armed = std::make_shared<std::atomic<bool>>(false);
        Fixture f(true,[armed](Ledger::Options& options) {
            options.v3_main_io_fault = [armed]() -> std::optional<std::string> {
                if (armed->load()) return "private-new-scene-write-fault";
                return std::nullopt;
            };
        });
        f.Input("turn-step1"); f.main.reset();
        auto gate = std::make_shared<Gate>(); auto probe = std::make_shared<Probe>();
        Worker worker(memory); ReleaseGuard release{gate};
        { HookScope scope(gate); REQUIRE(worker.Start(*f.ledger,probe,"turn-step1")); }
        REQUIRE(gate->Await()); const auto id = f.ledger->session_id(); const auto before = Bytes(f.stream);
        armed->store(true);
        lmb::trajectory::ClearRequest request; const auto failed = f.ledger->ClearSession(request,nullptr);
        armed->store(false);
        CHECK(failed.error_code == "clear.step1_failed"); CHECK_FALSE(failed.active_switched);
        CHECK(f.ledger->session_id() == id); CHECK(Bytes(f.stream) == before);
        gate->Release(); worker.Collect(true); CheckProbe(probe,1);
        CHECK(Bytes(f.stream).size() > before.size()); CHECK_FALSE(gate->Expired()); f.Close();
    }
}

TEST_CASE("Memory bypass owner: real Managed admission refuses Local clear without retiring its live worker") {
    std::cout << "[memory-bypass-owner-path] managed-clear\n";
    for (bool memory : {false,true}) {
        Fixture f(true,{},true);
        f.Input("turn-managed-clear"); f.main.reset();
        auto gate = std::make_shared<Gate>(); auto probe = std::make_shared<Probe>();
        Worker worker(memory); ReleaseGuard release{gate};
        { HookScope scope(gate); REQUIRE(worker.Start(*f.ledger,probe,"turn-managed-clear")); }
        REQUIRE(gate->Await()); CHECK(probe->calls.load() == 0);
        const auto id = f.ledger->session_id();
        const auto before = Bytes(f.stream);
        const auto marker = f.ledger->session_dir()/lmb::trajectory::kManagedSessionOwnershipFile;
        const auto ownership = Bytes(marker);
        const auto parent = f.ledger->session_dir().parent_path();
        const auto siblings = [&] {
            std::vector<std::string> names;
            for (const auto& child : fs::directory_iterator(parent))
                names.push_back(child.path().filename().generic_string());
            std::sort(names.begin(),names.end());
            return names;
        };
        const auto original_siblings = siblings();
        lmb::trajectory::ClearRequest request;
        const auto refused = f.ledger->ClearSession(request,nullptr);
        CHECK(refused.error_code == "managed.manager.operation_unavailable");
        CHECK_FALSE(refused.active_switched); CHECK_FALSE(refused.new_session_prepared);
        CHECK(f.ledger->session_id() == id); CHECK(Bytes(f.stream) == before);
        CHECK(Bytes(marker) == ownership); CHECK(siblings() == original_siblings);
        gate->Release(); worker.Collect(true); CheckProbe(probe,1);
        CHECK(Bytes(f.stream).size() > before.size()); CHECK(Bytes(marker) == ownership);
        CHECK_FALSE(gate->Expired()); f.Close();
        const auto closed = lmb::trajectory::v3::ReadV3Ledger(f.stream);
        REQUIRE(closed.has_value()); CHECK(closed->session_id == id);
        CHECK(Bytes(marker) == ownership);
        // This is storage/borrow admission only, never a public Managed API or
        // a Policy grant. Its trusted native Backend is deliberately private.
    }
}

TEST_CASE("Memory bypass owner: an actual busy clear refuses before retiring the current recorder owner") {
    std::cout << "[memory-bypass-owner-path] clear-busy\n";
    for (bool memory : {false,true}) {
        auto armed = std::make_shared<std::atomic<bool>>(false);
        auto gate = std::make_shared<Gate>();
        Ledger* live = nullptr;
        Worker* pending = nullptr;
        std::optional<lmb::trajectory::ClearOutcome> busy;
        bool callback_completed = false;
        Fixture f(true,[&](Ledger::Options& options) {
            options.v3_main_io_fault = [&]() -> std::optional<std::string> {
                if (!armed->exchange(false)) return std::nullopt;
                // This is the actual new-side first commit, while Manager owns
                // its boundary gate. The old writer remains independently live.
                lmb::trajectory::ClearRequest nested;
                busy = live->ClearSession(nested,nullptr);
                gate->Release();
                pending->Collect(true);
                callback_completed = true;
                return std::nullopt;
            };
        });
        live = f.ledger.get();
        f.Input("turn-busy-clear"); f.main.reset();
        Worker worker(memory); pending = &worker;
        auto probe = std::make_shared<Probe>(); ReleaseGuard release{gate};
        { HookScope scope(gate); REQUIRE(worker.Start(*f.ledger,probe,"turn-busy-clear")); }
        REQUIRE(gate->Await());
        CHECK(probe->calls.load() == 0);
        const auto old_id = f.ledger->session_id();
        const auto old_path = f.stream;
        armed->store(true);
        lmb::trajectory::ClearRequest outer;
        const auto switched = f.ledger->ClearSession(outer,nullptr);
        REQUIRE(busy.has_value()); CHECK(busy->error_code == "clear.busy");
        CHECK_FALSE(busy->active_switched); CHECK_FALSE(busy->new_session_prepared);
        REQUIRE(callback_completed); REQUIRE(switched.error_code.empty());
        CHECK(switched.active_switched); CHECK(f.ledger->session_id() != old_id);
        CheckProbe(probe,1); CHECK_FALSE(gate->Expired());
        int prepared = 0;
        for (const auto& row : Rows(old_path))
            if (row.value("kind",std::string()) == "model.request.prepared") ++prepared;
        CHECK(prepared == 1);
        CHECK(lmb::trajectory::v3::ReadV3Ledger(old_path).has_value());
        const auto sealed = Bytes(old_path);
        f.Input("turn-after-busy-clear");
        Worker next(memory); auto fresh = std::make_shared<Probe>();
        REQUIRE(next.Start(*f.ledger,fresh,"turn-after-busy-clear")); next.Collect(true);
        CheckProbe(fresh,1); CHECK(Bytes(old_path) == sealed);
        f.Close();
    }
}

TEST_CASE("Memory bypass owner: real clear step2 and step4 failures keep their distinct callback fences") {
    std::cout << "[memory-bypass-owner-path] clear-failure\n";
    for (bool memory : {false,true}) for (int stage : {2,4}) {
        auto countdown = std::make_shared<std::atomic<int>>(0);
        Fixture f(true,[countdown](Ledger::Options& options) {
            options.v3_main_io_fault = [countdown]() -> std::optional<std::string> {
                const int left = countdown->load();
                if (left > 0 && countdown->fetch_sub(1) == 1) return "private-clear-boundary-write-fault";
                return std::nullopt;
            };
        });
        f.Input("turn-clear-failure"); f.main.reset();
        auto gate = std::make_shared<Gate>(); auto probe = std::make_shared<Probe>();
        Worker worker(memory); ReleaseGuard release{gate};
        { HookScope scope(gate); REQUIRE(worker.Start(*f.ledger,probe,"turn-clear-failure")); }
        REQUIRE(gate->Await()); const auto id = f.ledger->session_id();
        // Real V3Writer::Start commits system then session.started. Only the
        // following old command.received / session.ended submission is faulted.
        countdown->store(stage == 2 ? 3 : 4);
        lmb::trajectory::ClearRequest request; const auto failed = f.ledger->ClearSession(request,nullptr);
        REQUIRE(countdown->load() == 0);
        REQUIRE(failed.error_code == (stage == 2 ? "clear.step2_failed" : "clear.step4_failed"));
        CHECK(failed.new_session_prepared); CHECK_FALSE(failed.active_switched);
        CHECK(f.ledger->session_id() == id);
        if (stage == 2) CHECK(failed.requested_event_id.empty());
        else REQUIRE_FALSE(failed.requested_event_id.empty());
        const auto before = Bytes(f.stream);
        const auto diagnostics = f.ledger->recent_io_errors().size();
        gate->Release(); worker.Collect(false); CheckProbe(probe,0);
        CHECK(Bytes(f.stream) == before);
        const auto notes = f.ledger->recent_io_errors();
        if (stage == 2) {
            // Cancellation was never entered. The old live bridge reaches the
            // actual broken writer and appends its real diagnostic, not a fake.
            REQUIRE(notes.size() > diagnostics);
            bool broken = false;
            for (std::size_t i = diagnostics; i < notes.size(); ++i)
                if (notes[i].find("bypass system:v3writer.broken") != std::string::npos) broken = true;
            CHECK(broken);
        } else {
            // Actual destructive boundary ran before sealing failed. The retired
            // proxy never calls the old bridge or its diagnostics again.
            CHECK(notes.size() == diagnostics);
        }
        Worker next(memory); auto rejected = std::make_shared<Probe>();
        REQUIRE(next.Start(*f.ledger,rejected,"turn-clear-failure")); next.Collect(false); CheckProbe(rejected,0);
        CHECK(Bytes(f.stream) == before); CHECK_FALSE(gate->Expired());
        // The real failed writer is not repaired or falsely marked clean here.
        f.ledger.reset();
    }
}
