// Future source preparation only; contract-first adoption and remote proof required.
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "app/commands/memory_commands.hpp"
#include "app/memory_ledger_bridge.hpp"
#include "app/turn_memory_extractor.hpp"
#include "cli/terminal_port.hpp"
#include "cli/theme.hpp"
#include "memory/project_memory.hpp"
#include "runtime/trajectory_session.hpp"
#include "runtime/trajectory_session_impl.hpp"
#include "trajectory/journal.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "workspace/identity.hpp"

// Test-only assembly of a real recovered legacy owner. No SDK entry point,
// alternate creation switch, fabricated assessment or replacement recorder.
#include "memory_legacy_fixture.hpp"

namespace {
namespace fs = std::filesystem;
namespace lmb = lubancode;
using Json = nlohmann::json;
using Extractor = lmb::app::TurnMemoryExtractor;
struct EnvGuard {
    std::optional<std::string> previous;
    // This switch does not create V2; that fixture uses real legacy recovery.
    explicit EnvGuard(bool v3) {
        if (const char* old = std::getenv(Name())) previous = old;
        Set(v3 ? "1" : "0");
    }
    ~EnvGuard() { Set(previous ? previous->c_str() : nullptr); }
    static const char* Name() { return "LUBANCODE_TRAJECTORY_V3_NEW_SESSIONS"; }
    static void Set(const char* value) {
#ifdef _WIN32
        _putenv_s(Name(), value ? value : "");
#else
        if (value) setenv(Name(), value, 1); else unsetenv(Name());
#endif
    }
};
struct OutputCapture {
    std::ostringstream stream;
    OutputCapture() { lmb::cli::TermPort().Redirect(&stream, nullptr); }
    ~OutputCapture() { lmb::cli::TermPort().Reset(); }
};
fs::path FreshRoot() {
    static std::atomic<unsigned> next{0};
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() /
        ("lmb-memory-identity-" + std::to_string(stamp) + "-" + std::to_string(++next));
    fs::create_directories(root / "repo"); fs::create_directories(root / "home");
    return root;
}
std::unique_ptr<lmb::runtime::TrajectorySessionLedger> Open(const fs::path& root, const std::string& resume = {}) {
    lmb::runtime::TrajectorySessionLedger::Options options;
    options.workspaces_root = root / "workspaces"; options.workspace_root = root / "repo";
    options.workspace_identity = lmb::workspace::MakeFallbackIdentity(root / "repo");
    options.lubancode_version = "test"; options.v3_system_content = "test system";
    if (!resume.empty()) { options.resume_at_launch=true; options.resume_source_session_id=resume; }
    auto opened = lmb::runtime::TrajectorySessionLedger::Open(options);
    REQUIRE(opened.has_value());
    return std::make_unique<lmb::runtime::TrajectorySessionLedger>(std::move(*opened));
}
using lmb::runtime::testing::OpenRecoveredMemoryLegacyLedger;

std::vector<Json> Read(const fs::path& stream) {
    auto lines = lmb::trajectory::ReadJournalLines(stream);
    REQUIRE(lines.has_value());
    std::vector<Json> result;
    for (const auto& raw : *lines) {
        auto row = Json::parse(raw, nullptr, false);
        REQUIRE_FALSE(row.is_discarded()); result.push_back(std::move(row));
    }
    return result;
}
struct Probe { std::atomic<int> calls{0}, destroyed{0}; bool fail = false; };
struct Backend final : lmb::api::Backend {
    explicit Backend(std::shared_ptr<Probe> value) : probe(std::move(value)) {}
    ~Backend() override { ++probe->destroyed; }
    std::shared_ptr<Probe> probe;
    std::expected<void,lmb::api::Error> send_stream(const lmb::api::Request&,
        const std::function<void(const lmb::api::StreamEvent&)>& emit,
        const std::atomic<bool>*) override {
        ++probe->calls;
        if (probe->fail) return std::unexpected(lmb::api::Error{lmb::api::ErrorKind::Api,"failed",0});
        emit(lmb::api::MessageStart{"memory-response","cheap-m"});
        emit(lmb::api::TextDelta{R"({"task_type":"research","summary":"已核触发身份","retrieval_terms":[],"candidates":[]})"});
        emit(lmb::api::ContentBlockDone{0});
        lmb::api::MessageDone done; done.stop_reason="end_turn";
        done.usage.input_tokens=9; done.usage.output_tokens=3; emit(done);
        return {};
    }
};
Extractor::Outcome Run(const std::string& turn, bool fail=false) {
    auto probe=std::make_shared<Probe>(); probe->fail=fail;
    Extractor::Inputs input;
    input.backend=std::make_unique<Backend>(probe); input.model="cheap-m";
    input.task_type="research"; input.turn_id=turn; input.session_generation=19;
    input.system_prompt="private prompt"; input.transcript="actual turn transcript";
    // Bypass parent identity and its revocation have their own contract. This
    // source tests real CLI assessment settlement, not borrowed recorder safety.
    Extractor worker; REQUIRE(worker.Start(std::move(input)));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!worker.Ready() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(worker.Ready()); auto out=worker.TakeFinished(); REQUIRE(out.has_value());
    CHECK(probe->calls.load()==1); CHECK(probe->destroyed.load()==1);
    CHECK(out->turn_id==turn); CHECK(out->session_generation==19);
    CHECK(out->ok==!fail); CHECK(out->extraction_invoked);
    CHECK_FALSE(worker.TakeFinished().has_value());
    return std::move(*out);
}
struct Fixture {
    EnvGuard environment;
    OutputCapture capture;
    bool v3;
    fs::path root;
    std::unique_ptr<lmb::runtime::TrajectorySessionLedger> ledger;
    lmb::app::MemoryTurnLedger turns;
    std::unique_ptr<lmb::app::MemoryLedgerBridge> accounting;
    std::unique_ptr<lmb::memory::ProjectMemory> store;
    std::unique_ptr<lmb::runtime::TrajectoryTurnBridge> main;
    lmb::cli::Theme theme;
    fs::path stream;
    std::string session;
    explicit Fixture(bool format, const fs::path& existing_root = {}, const std::string& resume = {})
        : environment(format),v3(format),root(existing_root.empty()?FreshRoot():existing_root),
          ledger(format?Open(root,resume):OpenRecoveredMemoryLegacyLedger(root)),turns(ledger.get()),theme(lmb::cli::BuiltinTheme("plain")) {
        REQUIRE((ledger->v3_main_writer()!=nullptr)==v3);
        session=ledger->session_id();
        if (v3) {
            auto file=lmb::trajectory::v3::FindV3SessionStream(ledger->session_dir());
            REQUIRE(file.has_value()); stream=*file;
        } else stream=ledger->session_dir()/"main.jsonl";
        auto identity=lmb::memory::ResolveProjectIdentity(root/"repo",root/"home");
        REQUIRE(identity.has_value());
        lmb::memory::Options options; options.enabled=true; options.global_allowed=true;
        accounting=std::make_unique<lmb::app::MemoryLedgerBridge>(*ledger);
        // No executable: actual queued jobs stay pending with worker_unavailable.
        store=std::make_unique<lmb::memory::ProjectMemory>(std::move(*identity),root/"home",options);
        store->set_accounting(accounting.get()); store->set_write_receipt_sink(&turns);
    }
    const char* Key(const char* modern,const char* legacy) const { return v3?modern:legacy; }
    void Begin(const std::string& turn) {
        main=ledger->NewTurnBridge({"probe","responses","terminal"}); REQUIRE(main!=nullptr);
        main->BeginTurn(turn,"external_user");
        lmb::api::Message input; input.role=lmb::api::Role::User;
        input.content.push_back(lmb::api::TextBlock{"input-for-"+turn}); main->RecordInput(input);
        turns.BeginTurn(session,turn,"记住：触发轮号不能丢");
        bool found=false;
        for (const auto& row:Read(stream)) {
            if (v3) {
                // The first V3 system message legitimately has turnId:null.
                // Only actual user inputs require a non-null string identity.
                if (row.value("type",std::string())=="message" &&
                    row.at("message").value("role",std::string())=="user") {
                    REQUIRE(row.contains("turnId"));
                    REQUIRE(row.at("turnId").is_string());
                    if (row.at("turnId").get<std::string>()==turn) found=true;
                }
            } else if (row.value("kind",std::string())=="input.received" &&
                       row.value("turn_id",std::string())==turn) found=true;
        }
        REQUIRE(found);
    }
    void Suspend() { main->EndTurn(true,false,""); turns.SuspendTurn(); }
    void Settle(const Extractor::Outcome& out) {
        lmb::app::SessionTailContext tail; tail.project_memory=store.get();
        tail.theme=&theme; tail.memory_turns=&turns;
        lmb::app::SettleTurnMemory(tail,out,17);
    }
    std::vector<Json> Events(const std::string& kind) const {
        std::vector<Json> result;
        for (const auto& row:Read(stream))
            if (row.value("kind",std::string())==kind) result.push_back(row);
        return result;
    }
    std::vector<Json> Assessed() const { return Events("memory.extraction.assessed"); }
    void Identity(const Json& event,const std::string& turn) const {
        CHECK(event.value(Key("sessionId","session_id"),std::string())==session);
        CHECK(event.at("payload").value(Key("turnId","turn_id"),std::string())==turn);
        if (v3) CHECK(event.value("turnId",std::string())==turn);
        else CHECK_FALSE(event.contains("turn_id")); // V2 host fact uses payload identity.
    }
};
}

TEST_CASE("Memory durable identity: real successful worker settlement keeps its triggering main input") {
    std::cout<<"[memory-durable-identity-path] success\n";
    for (bool format:{false,true}) {
        Fixture f(format); f.Begin("turn-success"); f.turns.NoteExtractionCalled(); f.Suspend();
        const auto before=Read(f.stream).size(); auto out=Run("turn-success");
        f.Settle(out); REQUIRE(Read(f.stream).size()==before+1);
        auto rows=f.Assessed(); REQUIRE(rows.size()==1); f.Identity(rows[0],"turn-success");
        const auto& p=rows[0].at("payload");
        CHECK(p.value(f.Key("extractOutcome","extract_outcome"),std::string())=="completed");
        CHECK(p.value(f.Key("usageReported","usage_reported"),false));
        CHECK(p.value(f.Key("inputTokens","input_tokens"),0)==9);
        CHECK(p.value(f.Key("outputTokens","output_tokens"),0)==3);
    }
}
TEST_CASE("Memory durable identity: actual transport failure has one assessment on the old trigger") {
    std::cout<<"[memory-durable-identity-path] failure\n";
    for (bool format:{false,true}) {
        Fixture f(format); f.Begin("turn-failure"); f.turns.NoteExtractionCalled(); f.Suspend();
        const auto before=Read(f.stream).size(); auto out=Run("turn-failure",true); f.Settle(out);
        REQUIRE(Read(f.stream).size()==before+1);
        auto rows=f.Assessed(); REQUIRE(rows.size()==1); f.Identity(rows[0],"turn-failure");
        const auto& p=rows[0].at("payload");
        CHECK(p.value(f.Key("errorCode","error_code"),std::string())=="transport_failed");
        CHECK_FALSE(p.contains(f.Key("usageReported","usage_reported")));
        CHECK_FALSE(p.contains(f.Key("inputTokens","input_tokens")));
    }
}
TEST_CASE("Memory durable identity: opening the next real input flushes the old suspended assessment") {
    std::cout<<"[memory-durable-identity-path] supersession\n";
    for (bool format:{false,true}) {
        Fixture f(format); f.Begin("turn-old"); f.turns.NoteExtractionCalled(); f.Suspend();
        auto out=Run("turn-old"); f.Begin("turn-new");
        auto rows=f.Assessed(); REQUIRE(rows.size()==1); f.Identity(rows[0],"turn-old");
        CHECK(rows[0].at("payload").value(f.Key("errorCode","error_code"),std::string())=="aborted");
        const auto before=Read(f.stream).size(); f.Settle(out); CHECK(Read(f.stream).size()==before);
        f.turns.NoteExtractionSkipped(lmb::app::ExtractionSkipReason::ShortText);
        f.main->EndTurn(true,false,""); f.turns.FinishTurn(4);
        rows=f.Assessed(); REQUIRE(rows.size()==2); f.Identity(rows[1],"turn-new");
    }
}
TEST_CASE("Memory durable identity: actual inter-turn remember command queues without borrowing old turn") {
    std::cout<<"[memory-durable-identity-path] receipt\n";
    for (bool format:{false,true}) {
        Fixture f(format); f.Begin("turn-receipt"); f.turns.NoteExtractionCalled(); f.Suspend();
        lmb::app::MemoryCommandContext command; command.project_memory=f.store.get(); command.theme=&f.theme;
        lmb::app::HandleMemoryCommand(command,"remember project preference 包管理 :: 统一走 pnpm");
        auto receipts=f.Events("memory.write.receipted"); REQUIRE(receipts.size()==1);
        CHECK_FALSE(receipts[0].contains(f.Key("turnId","turn_id")));
        const auto& p=receipts[0].at("payload"); CHECK_FALSE(p.contains(f.Key("turnId","turn_id")));
        CHECK(p.value("outcome",std::string())=="queued");
        const auto job=p.value(f.Key("jobId","job_id"),std::string()); REQUIRE_FALSE(job.empty());
        CHECK(f.store->Status().pending_jobs==1);
        bool pending=false;
        for (const auto& file:fs::directory_iterator(f.root/"home"/"memory-jobs"/"pending"))
            if (file.is_regular_file() && file.path().filename().string().find(job)!=std::string::npos) pending=true;
        CHECK(pending);
        auto out=Run("turn-receipt"); f.Settle(out);
        auto rows=f.Assessed(); REQUIRE(rows.size()==1); f.Identity(rows[0],"turn-receipt");
        CHECK(f.Events("memory.write.receipted").size()==1);
    }
}
TEST_CASE("Memory durable identity: abandoned suspension cannot place an old fact in the next input") {
    std::cout<<"[memory-durable-identity-path] abandonment\n";
    for (bool format:{false,true}) {
        Fixture f(format); f.Begin("turn-abandoned"); f.turns.NoteExtractionCalled(); f.Suspend();
        auto out=Run("turn-abandoned"); f.turns.AbandonSuspendedTurn();
        const auto before=Read(f.stream).size(); f.Settle(out); CHECK(Read(f.stream).size()==before);
        CHECK(f.Assessed().empty());
        f.Begin("turn-after-abandon"); f.turns.NoteExtractionSkipped(lmb::app::ExtractionSkipReason::ShortText);
        f.main->EndTurn(true,false,""); f.turns.FinishTurn(4);
        auto rows=f.Assessed(); REQUIRE(rows.size()==1); f.Identity(rows[0],"turn-after-abandon");
    }
}
TEST_CASE("Memory durable identity: a wrong real worker identity is rejected and matching settlement is once") {
    std::cout<<"[memory-durable-identity-path] once\n";
    for (bool format:{false,true}) {
        Fixture f(format); f.Begin("turn-match"); f.turns.NoteExtractionCalled(); f.Suspend();
        auto wrong=Run("turn-not-match"); const auto before=Read(f.stream).size(); f.Settle(wrong);
        CHECK(Read(f.stream).size()==before); CHECK(f.Assessed().empty());
        auto right=Run("turn-match"); f.Settle(right);
        const auto after=Read(f.stream).size(); REQUIRE(after==before+1); f.Settle(right);
        CHECK(Read(f.stream).size()==after);
        auto rows=f.Assessed(); REQUIRE(rows.size()==1); f.Identity(rows[0],"turn-match");
    }
}
TEST_CASE("Memory durable identity: closed material reload retains the actual input and assessment identities") {
    std::cout<<"[memory-durable-identity-path] recovery\n";
    for (bool format:{false,true}) {
        Fixture f(format); f.Begin("turn-reload"); f.turns.NoteExtractionCalled(); f.Suspend();
        auto out=Run("turn-reload"); f.Settle(out);
        auto assessed=f.Assessed(); REQUIRE(assessed.size()==1); f.Identity(assessed[0],"turn-reload");
        const auto stream=f.stream; const auto session=f.session;
        REQUIRE(f.ledger->CloseSession("test_done").error_code.empty());
        if (format) {
            auto read=lmb::trajectory::v3::ReadV3Ledger(stream); REQUIRE(read.has_value());
            CHECK(read->session_id==session);
            int inputs=0, assessments=0;
            for (const auto& message:read->messages)
                if (message.turn_id.value_or("")=="turn-reload" &&
                    message.message.value("role",std::string())=="user") ++inputs;
            for (const auto& event:read->events)
                if (event.kind==lmb::trajectory::v3::EventKindV3::MemoryExtractionAssessed) {
                    ++assessments; CHECK(event.turn_id.value_or("")=="turn-reload");
                    CHECK(event.payload.value("turnId",std::string())=="turn-reload");
                }
            CHECK(inputs==1); CHECK(assessments==1);
            auto second=lmb::trajectory::v3::ReadV3Ledger(stream); REQUIRE(second.has_value());
            CHECK(second->lines==read->lines); CHECK(second->events.size()==read->events.size());
            // Actual --continue owner reopening, not only parsing an old file.
            Fixture resumed(true,f.root,session);
            REQUIRE(resumed.ledger->resumed_at_launch()); CHECK(resumed.session==session);
            REQUIRE(resumed.ledger->LaunchResumeHistory().size()==1);
            CHECK(resumed.ledger->LaunchResumeHistory()[0].role==lmb::api::Role::User);
            resumed.Begin("turn-resumed"); resumed.turns.NoteExtractionCalled(); resumed.Suspend();
            auto next=Run("turn-resumed"); resumed.Settle(next);
            auto continued=resumed.Assessed(); REQUIRE(continued.size()==2);
            resumed.Identity(continued[0],"turn-reload"); resumed.Identity(continued[1],"turn-resumed");
            REQUIRE(resumed.ledger->CloseSession("test_done").error_code.empty());
        } else {
            auto first=lmb::trajectory::FoldStreamReplay(stream); REQUIRE(first.ok());
            CHECK(first.state.session_id==session); CHECK(first.state.effective_conversation.size()==1);
            auto second=lmb::trajectory::FoldStreamReplay(stream); REQUIRE(second.ok());
            CHECK(second.state.ToJson()==first.state.ToJson());
            int assessments=0;
            for (const auto& row:Read(stream)) if (row.value("kind",std::string())=="memory.extraction.assessed") {
                ++assessments; CHECK(row.at("payload").value("turn_id",std::string())=="turn-reload");
            }
            CHECK(assessments==1);
        }
    }
}
