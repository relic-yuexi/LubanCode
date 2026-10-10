// Future native source; formal contract and remote execution still required.
#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <nlohmann/json.hpp>
#include "app/bypass_worker_test_hooks.hpp"
#include "app/session_title_account.hpp"
#include "app/session_title_refiner_test_hooks.hpp"
#include "runtime/trajectory_session.hpp"
#include "trajectory/journal.hpp"
#include "workspace/identity.hpp"

namespace {
using Refiner = lubancode::app::SessionTitleRefiner;
namespace testing = lubancode::app::testing;
struct Probe {
    std::atomic<int> calls{0}, destroyed{0};
    bool fail = false;
};
struct Backend final : lubancode::api::Backend {
    std::shared_ptr<Probe> probe;
    explicit Backend(std::shared_ptr<Probe> value):probe(std::move(value)) {}
    ~Backend() override { ++probe->destroyed; }
    std::expected<void,lubancode::api::Error> send_stream(const lubancode::api::Request&,
        const std::function<void(const lubancode::api::StreamEvent&)>& emit,
        const std::atomic<bool>*) override {
        ++probe->calls;
        if (probe->fail) return std::unexpected(lubancode::api::Error{lubancode::api::ErrorKind::Api,"transport-failed",0});
        emit(lubancode::api::MessageStart{"title-response","cheap-title"});
        emit(lubancode::api::TextDelta{"修标题收场"});
        emit(lubancode::api::ContentBlockDone{0});
        lubancode::api::MessageDone done;
        done.stop_reason="end_turn"; done.usage.input_tokens=9; done.usage.output_tokens=3;
        done.usage.cache_read_tokens=13; done.usage.cache_creation_tokens=17; done.usage.output_reasoning_tokens=19;
        done.usage_reported=true; done.cache_read_reported=true; done.cache_creation_reported=true; emit(done);
        return {};
    }
};
Refiner::Inputs Inputs(const std::shared_ptr<Probe>& probe,std::uint64_t generation=31) {
    Refiner::Inputs input;
    input.backend=std::make_unique<Backend>(probe); input.model="cheap-title";
    input.effort="low"; input.first_query="修标题线程收场";
    input.generation=generation; input.timeout_secs=3;
    return input;
}
struct StartHook {
    std::function<void()> previous;
    explicit StartHook(std::function<void()> hook):previous(testing::ExchangeTitleWorkerStartHook(std::move(hook))) {}
    ~StartHook() { testing::ExchangeTitleWorkerStartHook(std::move(previous)); }
};
struct BindHook {
    testing::BypassWorkerHook previous;
    explicit BindHook(testing::BypassWorkerHook hook):previous(testing::ExchangeBypassWorkerHook(std::move(hook))) {}
    ~BindHook() { testing::ExchangeBypassWorkerHook(std::move(previous)); }
};
void StandardFailure() { throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),"private-secret"); }
bool Await(Refiner& refiner) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!refiner.Ready() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return refiner.Ready();
}
void Failure(const Refiner::Outcome& out,std::uint64_t generation,const std::string& error) {
    CHECK_FALSE(out.ok); CHECK_FALSE(out.refinement_invoked); CHECK(out.title.empty());
    CHECK(out.model=="cheap-title"); CHECK(out.generation==generation); CHECK(out.error==error);
    CHECK(out.error.find("secret")==std::string::npos); CHECK_FALSE(out.accounting.usage_reported);
    CHECK(out.accounting.usage.input_tokens==0); CHECK(out.accounting.usage.output_tokens==0);
    CHECK(out.accounting.usage.cache_read_tokens==0); CHECK(out.accounting.usage.cache_creation_tokens==0);
    CHECK(out.accounting.usage.output_reasoning_tokens==0); CHECK(out.accounting.duration_ms==0);
    lubancode::agent::ModelUsageLedger ledger;
    CHECK_FALSE(lubancode::app::RecordTitleRefinementCall(ledger,out)); CHECK(ledger.by_role().empty());
}
struct Scene {
    std::filesystem::path root;
    std::optional<lubancode::runtime::TrajectorySessionLedger> ledger;
    std::string title;
    std::unique_ptr<lubancode::app::SessionTitleAccount> account;
    Scene() {
        root=std::filesystem::temp_directory_path()/
            ("lubancode-title-start-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root/"repo");
        lubancode::runtime::TrajectorySessionLedger::Options options;
        options.workspaces_root=root/"workspaces";
        options.workspace_identity=lubancode::workspace::MakeFallbackIdentity(root/"repo");
        options.lubancode_version="test";
        auto opened=lubancode::runtime::TrajectorySessionLedger::Open(options); REQUIRE(opened.has_value());
        ledger.emplace(std::move(*opened)); REQUIRE(ledger->v3_main_writer()!=nullptr);
        auto main=ledger->NewTurnBridge({"test","wire","host"}); REQUIRE(main!=nullptr);
        main->BeginTurn("title-trigger","external_user");
        lubancode::api::Message user; user.role=lubancode::api::Role::User;
        user.content.push_back(lubancode::api::TextBlock{"修标题线程收场"}); main->RecordInput(user);
        main->EndTurn(true,false,"done");
        account=std::make_unique<lubancode::app::SessionTitleAccount>(title,&*ledger);
        REQUIRE(account->BeginLocalTitle("本地标题须留住")==lubancode::app::SessionTitleAccount::LocalResult::Set);
    }
    std::string JournalBytes() const {
        std::ifstream file(ledger->session_dir()/(ledger->session_id()+".jsonl"),std::ios::binary);
        REQUIRE(file.is_open());
        return std::string(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
    }
    ~Scene() {
        account.reset(); ledger.reset(); std::error_code ignored;
        std::filesystem::remove_all(root,ignored);
    }
};
} // namespace

TEST_CASE("Title creation: standard failure preserves identity and releases unused Backend") {
    std::cout<<"[title-worker-start-path] standard\n";
    StartHook hook(StandardFailure); auto probe=std::make_shared<Probe>(); Refiner refiner;
    REQUIRE(refiner.Start(Inputs(probe))); CHECK(refiner.Busy()); CHECK(refiner.Ready());
    CHECK(probe->calls.load()==0); CHECK(probe->destroyed.load()==1); refiner.RequestCancel();
    auto out=refiner.TakeFinished(); REQUIRE(out.has_value()); Failure(*out,31,"标题精炼线程未能启动");
    CHECK_FALSE(refiner.Busy()); CHECK_FALSE(refiner.Ready()); CHECK_FALSE(refiner.TakeFinished().has_value());
}
TEST_CASE("Title creation: unknown repeated failures settle without starting a thread") {
    std::cout<<"[title-worker-start-path] nonstandard\n";
    StartHook hook([]{throw 73;}); auto probe=std::make_shared<Probe>(); Refiner refiner;
    for (std::uint64_t generation=1;generation<=10;++generation) {
        REQUIRE(refiner.Start(Inputs(probe,generation))); auto out=refiner.TakeFinished();
        REQUIRE(out.has_value()); Failure(*out,generation,"标题精炼线程未能启动");
    }
    CHECK(probe->calls.load()==0); CHECK(probe->destroyed.load()==10);
    auto began=std::chrono::steady_clock::now();
    { Refiner abandoned; REQUIRE(abandoned.Start(Inputs(probe))); }
    CHECK(std::chrono::steady_clock::now()-began<std::chrono::seconds(1)); CHECK(probe->destroyed.load()==11);
}
TEST_CASE("Title creation: pending failure preserves single flight and real restart accounting") {
    std::cout<<"[title-worker-start-path] restart\n";
    Refiner refiner; auto failed=std::make_shared<Probe>(); auto rejected=std::make_shared<Probe>();
    int attempts=0;
    { StartHook hook([&]{++attempts;StandardFailure();});
      REQUIRE(refiner.Start(Inputs(failed))); auto input=Inputs(rejected);
      CHECK_FALSE(refiner.Start(std::move(input))); CHECK(input.backend!=nullptr); CHECK(attempts==1); }
    auto failure=refiner.TakeFinished(); REQUIRE(failure.has_value());
    lubancode::agent::ModelUsageLedger ledger; CHECK_FALSE(lubancode::app::RecordTitleRefinementCall(ledger,*failure));
    auto success=std::make_shared<Probe>(); REQUIRE(refiner.Start(Inputs(success,32))); REQUIRE(Await(refiner));
    auto out=refiner.TakeFinished(); REQUIRE(out.has_value()); CHECK(out->ok); CHECK(out->refinement_invoked);
    CHECK(out->generation==32); CHECK(success->calls.load()==1); CHECK(success->destroyed.load()==1);
    REQUIRE(lubancode::app::RecordTitleRefinementCall(ledger,*out));
    auto transport=std::make_shared<Probe>(); transport->fail=true;
    REQUIRE(refiner.Start(Inputs(transport,33))); REQUIRE(Await(refiner));
    auto failed_send=refiner.TakeFinished(); REQUIRE(failed_send.has_value()); CHECK_FALSE(failed_send->ok);
    CHECK(failed_send->refinement_invoked); CHECK_FALSE(failed_send->accounting.usage_reported);
    REQUIRE(lubancode::app::RecordTitleRefinementCall(ledger,*failed_send));
    const auto& entry=ledger.by_role().at(lubancode::agent::ModelRole::Cheap);
    CHECK(entry.calls==2); CHECK(entry.input_tokens==9); CHECK(entry.output_tokens==3);
}
TEST_CASE("Title creation: invalid inputs never consume Backend or enter creation hook") {
    std::cout<<"[title-worker-start-path] gates\n";
    int attempts=0; StartHook hook([&]{++attempts;StandardFailure();}); Refiner refiner;
    Refiner::Inputs empty; empty.model="cheap-title"; CHECK_FALSE(refiner.Start(std::move(empty)));
    auto probe=std::make_shared<Probe>(); auto input=Inputs(probe); input.model.clear();
    CHECK_FALSE(refiner.Start(std::move(input))); CHECK(input.backend!=nullptr);
    CHECK(attempts==0); CHECK(probe->calls.load()==0); CHECK(probe->destroyed.load()==0); CHECK_FALSE(refiner.Busy());
}
TEST_CASE("Title creation: private hook is isolated from another frontend thread") {
    std::cout<<"[title-worker-start-path] isolation\n";
    StartHook hook(StandardFailure); auto peer=std::make_shared<Probe>();
    bool started=false,ready=false,ok=false; std::exception_ptr error;
    std::jthread frontend([&]{try { Refiner refiner; started=refiner.Start(Inputs(peer,34));
        ready=Await(refiner); auto out=refiner.TakeFinished(); ok=out.has_value() && out->ok;
    } catch (...) {error=std::current_exception();}});
    frontend.join(); CHECK(error==nullptr); CHECK(started); CHECK(ready); CHECK(ok);
    CHECK(peer->calls.load()==1); CHECK(peer->destroyed.load()==1);
    auto local=std::make_shared<Probe>(); Refiner refiner; REQUIRE(refiner.Start(Inputs(local)));
    auto out=refiner.TakeFinished(); REQUIRE(out.has_value()); Failure(*out,31,"标题精炼线程未能启动");
    CHECK(local->calls.load()==0); CHECK(local->destroyed.load()==1);
}
TEST_CASE("Title binding: actual frontend bridge boundary failures never reach Backend") {
    std::cout<<"[title-worker-start-path] binding\n";
    Scene scene; const auto local=scene.title; const auto original=scene.JournalBytes();
    for (bool standard:{true,false}) {
        auto probe=std::make_shared<Probe>(); Refiner refiner; auto input=Inputs(probe);
        input.trajectory=&*scene.ledger; input.provider="test"; input.trajectory_wire="wire";
        BindHook hook([standard](testing::BypassWorkerPurpose purpose,testing::BypassWorkerPhase phase){
            if (purpose==testing::BypassWorkerPurpose::Title && phase==testing::BypassWorkerPhase::BeforeBinding) {
                if (standard) StandardFailure(); else throw 73;
            }});
        REQUIRE(refiner.Start(std::move(input))); CHECK(refiner.Ready()); CHECK(refiner.Busy());
        auto out=refiner.TakeFinished(); REQUIRE(out.has_value()); Failure(*out,31,"标题精炼旁路绑定失败");
        CHECK(probe->calls.load()==0); CHECK(probe->destroyed.load()==1);
        CHECK(scene.account->AdoptRefined(*out)==lubancode::app::SessionTitleAccount::AdoptResult::Ignored);
        CHECK(scene.title==local); CHECK(scene.JournalBytes()==original); CHECK_FALSE(refiner.TakeFinished().has_value());
    }
}
TEST_CASE("Title creation: actual title account preserves local title on failure and adopts real restart") {
    std::cout<<"[title-worker-start-path] adoption\n";
    Scene scene; const auto local=scene.title; const auto original=scene.JournalBytes(); auto& refiner=scene.account->refiner();
    auto failed=std::make_shared<Probe>(); auto input=Inputs(failed,scene.account->generation());
    input.trajectory=&*scene.ledger; input.provider="test"; input.trajectory_wire="wire";
    { StartHook hook(StandardFailure); REQUIRE(refiner.Start(std::move(input))); }
    scene.account->NoteTitleGenerationStarted("cheap-title","test");
    const auto requested_failure=scene.JournalBytes(); CHECK(requested_failure!=original);
    auto out=refiner.TakeFinished(); REQUIRE(out.has_value());
    lubancode::agent::ModelUsageLedger ledger; CHECK_FALSE(lubancode::app::RecordTitleRefinementCall(ledger,*out));
    CHECK(scene.account->AdoptRefined(*out)==lubancode::app::SessionTitleAccount::AdoptResult::Ignored);
    CHECK(scene.title==local); CHECK(ledger.by_role().empty()); CHECK(scene.JournalBytes()==requested_failure);
    scene.account->BumpGeneration(); // Explicit new attempt, not automatic CLI retry.
    auto success=std::make_shared<Probe>(); auto restarted=Inputs(success,scene.account->generation());
    restarted.trajectory=&*scene.ledger; restarted.provider="test"; restarted.trajectory_wire="wire";
    REQUIRE(refiner.Start(std::move(restarted)));
    scene.account->NoteTitleGenerationStarted("cheap-title","test");
    REQUIRE(Await(refiner));
    auto real=refiner.TakeFinished(); REQUIRE(real.has_value()); REQUIRE(real->ok);
    REQUIRE(lubancode::app::RecordTitleRefinementCall(ledger,*real));
    CHECK(scene.account->AdoptRefined(*real)==lubancode::app::SessionTitleAccount::AdoptResult::Adopted);
    int requested=0,extracted=0,generated=0;
    std::istringstream material(scene.JournalBytes()); std::string line;
    while (std::getline(material,line)) {
        auto row=nlohmann::json::parse(line); const auto kind=row.value("kind",std::string());
        if (kind=="title.requested") ++requested;
        if (kind=="title.extracted") ++extracted;
        if (kind=="session.title.applied" && row.at("payload").value("source",std::string())=="generated") ++generated;
    }
    CHECK(requested==2); CHECK(extracted==1); CHECK(generated==1);
    CHECK(scene.title=="修标题收场"); CHECK(ledger.by_role().at(lubancode::agent::ModelRole::Cheap).calls==1);
    CHECK_FALSE(refiner.TakeFinished().has_value()); CHECK(success->calls.load()==1); CHECK(success->destroyed.load()==1);
}

namespace {
struct ExecutionHook {
    testing::TitleWorkerHook previous;
    explicit ExecutionHook(testing::TitleWorkerHook hook):previous(testing::ExchangeTitleWorkerExecutionHook(std::move(hook))) {}
    ~ExecutionHook() { testing::ExchangeTitleWorkerExecutionHook(std::move(previous)); }
};
void ExecutionFailure(const Refiner::Outcome& out,std::uint64_t generation,bool invoked) {
    CHECK_FALSE(out.ok); CHECK(out.title.empty()); CHECK(out.refinement_invoked==invoked);
    CHECK(out.model=="cheap-title"); CHECK(out.generation==generation); CHECK(out.error=="标题精炼后台执行失败");
    CHECK(out.error.find("secret")==std::string::npos); CHECK(out.accounting.usage_reported==invoked);
    CHECK(out.accounting.usage.input_tokens==(invoked?9:0)); CHECK(out.accounting.usage.output_tokens==(invoked?3:0));
    CHECK(out.accounting.usage.cache_read_tokens==(invoked?13:0)); CHECK(out.accounting.usage.cache_creation_tokens==(invoked?17:0));
    CHECK(out.accounting.usage.output_reasoning_tokens==(invoked?19:0));
    lubancode::agent::ModelUsageLedger ledger; CHECK(lubancode::app::RecordTitleRefinementCall(ledger,out)==invoked);
    if(invoked) { CHECK(ledger.by_role().at(lubancode::agent::ModelRole::Cheap).calls==1); }
    else { CHECK(ledger.by_role().empty()); CHECK(out.accounting.duration_ms==0); }
}
void ThrowExecution(bool standard) { if(standard) StandardFailure(); else throw 73; }
bool AwaitAtomic(const std::atomic<bool>& flag) {
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!flag.load() && std::chrono::steady_clock::now()<limit) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return flag.load();
}
struct ExecutionReleaseGuard { std::atomic<bool>& flag; ~ExecutionReleaseGuard(){flag.store(true);} };
}
TEST_CASE("Title execution: before-work exceptions settle repeatedly without Backend calls") {
    std::cout<<"[title-worker-execution-path] before-work\n";
    for(bool standard:{true,false}) {
        auto probe=std::make_shared<Probe>(); Refiner refiner;
        ExecutionHook hook([standard](testing::TitleWorkerPhase phase){if(phase==testing::TitleWorkerPhase::BeforeWork) ThrowExecution(standard);});
        for(std::uint64_t generation=1;generation<=10;++generation) {
            REQUIRE(refiner.Start(Inputs(probe,generation))); REQUIRE(Await(refiner)); CHECK(refiner.Busy());
            auto out=refiner.TakeFinished(); REQUIRE(out.has_value()); ExecutionFailure(*out,generation,false);
            CHECK_FALSE(refiner.TakeFinished().has_value()); CHECK_FALSE(refiner.Busy());
        }
        CHECK(probe->calls.load()==0); CHECK(probe->destroyed.load()==10);
    }
}
TEST_CASE("Title execution: exception after real refinement retains returned accounting") {
    std::cout<<"[title-worker-execution-path] after-refinement\n";
    for(bool standard:{true,false}) {
        auto probe=std::make_shared<Probe>(); Refiner refiner;
        ExecutionHook hook([standard](testing::TitleWorkerPhase phase){if(phase==testing::TitleWorkerPhase::AfterRefinement) ThrowExecution(standard);});
        REQUIRE(refiner.Start(Inputs(probe,41))); REQUIRE(Await(refiner)); auto out=refiner.TakeFinished();
        REQUIRE(out.has_value()); ExecutionFailure(*out,41,true); CHECK(probe->calls.load()==1); CHECK(probe->destroyed.load()==1);
    }
}
TEST_CASE("Title execution: publication exception clears prepared success and keeps usage") {
    std::cout<<"[title-worker-execution-path] before-publish\n";
    for(bool standard:{true,false}) {
        auto probe=std::make_shared<Probe>(); Refiner refiner;
        ExecutionHook hook([standard](testing::TitleWorkerPhase phase){if(phase==testing::TitleWorkerPhase::BeforePublish) ThrowExecution(standard);});
        REQUIRE(refiner.Start(Inputs(probe,42))); REQUIRE(Await(refiner)); auto out=refiner.TakeFinished();
        REQUIRE(out.has_value()); ExecutionFailure(*out,42,true); CHECK(probe->calls.load()==1); CHECK(probe->destroyed.load()==1);
    }
}
TEST_CASE("Title execution: pending failure keeps single flight and permits explicit restart") {
    std::cout<<"[title-worker-execution-path] restart\n";
    Refiner refiner; auto failed=std::make_shared<Probe>(); auto rejected=std::make_shared<Probe>();
    { ExecutionHook hook([](testing::TitleWorkerPhase phase){if(phase==testing::TitleWorkerPhase::BeforeWork) throw 73;});
      REQUIRE(refiner.Start(Inputs(failed,43))); REQUIRE(Await(refiner)); auto input=Inputs(rejected);
      CHECK_FALSE(refiner.Start(std::move(input))); CHECK(input.backend!=nullptr); }
    auto out=refiner.TakeFinished(); REQUIRE(out.has_value()); ExecutionFailure(*out,43,false);
    auto real=std::make_shared<Probe>(); REQUIRE(refiner.Start(Inputs(real,44))); REQUIRE(Await(refiner));
    auto success=refiner.TakeFinished(); REQUIRE(success.has_value()); CHECK(success->ok); CHECK(success->refinement_invoked);
    CHECK(success->generation==44); CHECK(real->calls.load()==1); CHECK(real->destroyed.load()==1);
    CHECK(failed->calls.load()==0); CHECK(failed->destroyed.load()==1); CHECK_FALSE(refiner.TakeFinished().has_value());
}
TEST_CASE("Title execution: independent frontend hook snapshots preserve both identities") {
    std::cout<<"[title-worker-execution-path] isolation\n";
    Refiner first,second; auto a=std::make_shared<Probe>(),b=std::make_shared<Probe>();
    { ExecutionHook hook([](testing::TitleWorkerPhase phase){if(phase==testing::TitleWorkerPhase::BeforeWork) StandardFailure();}); REQUIRE(first.Start(Inputs(a,45))); }
    { ExecutionHook hook([](testing::TitleWorkerPhase phase){if(phase==testing::TitleWorkerPhase::BeforeWork) throw 73;}); REQUIRE(second.Start(Inputs(b,46))); }
    REQUIRE(Await(first)); REQUIRE(Await(second)); auto one=first.TakeFinished(),two=second.TakeFinished();
    REQUIRE(one.has_value()); REQUIRE(two.has_value()); ExecutionFailure(*one,45,false); ExecutionFailure(*two,46,false);
    CHECK(a->calls.load()==0); CHECK(b->calls.load()==0); CHECK(a->destroyed.load()==1); CHECK(b->destroyed.load()==1);
}
TEST_CASE("Title execution: actual title account refuses failed title after real usage") {
    std::cout<<"[title-worker-execution-path] account\n";
    Scene scene; const auto local=scene.title; auto probe=std::make_shared<Probe>(); auto& refiner=scene.account->refiner();
    auto input=Inputs(probe,scene.account->generation()); input.trajectory=&*scene.ledger; input.provider="test"; input.trajectory_wire="wire";
    ExecutionHook hook([](testing::TitleWorkerPhase phase){if(phase==testing::TitleWorkerPhase::BeforePublish) StandardFailure();});
    REQUIRE(refiner.Start(std::move(input))); scene.account->NoteTitleGenerationStarted("cheap-title","test"); REQUIRE(Await(refiner));
    auto out=refiner.TakeFinished(); REQUIRE(out.has_value()); ExecutionFailure(*out,scene.account->generation(),true);
    const auto material=scene.JournalBytes(); CHECK(scene.account->AdoptRefined(*out)==lubancode::app::SessionTitleAccount::AdoptResult::Ignored);
    CHECK(scene.title==local); CHECK(scene.JournalBytes()==material); CHECK(probe->calls.load()==1); CHECK(probe->destroyed.load()==1);
    CHECK_FALSE(refiner.TakeFinished().has_value());
}
TEST_CASE("Title execution: late failed publication cannot append after actual scene retirement") {
    std::cout<<"[title-worker-execution-path] late-close\n";
    Scene scene; std::atomic<bool> entered{false},release{false},expired{false};
    Refiner refiner; auto probe=std::make_shared<Probe>(); ExecutionReleaseGuard guard{release};
    ExecutionHook hook([&](testing::TitleWorkerPhase phase){ if(phase!=testing::TitleWorkerPhase::AfterRefinement)return;
        entered.store(true); if(!AwaitAtomic(release)) expired.store(true); throw 73; });
    auto input=Inputs(probe,47); input.trajectory=&*scene.ledger; input.provider="test"; input.trajectory_wire="wire";
    REQUIRE(refiner.Start(std::move(input))); REQUIRE(AwaitAtomic(entered)); REQUIRE(scene.ledger->CloseSession("title_execution_done").error_code.empty());
    const auto closed=scene.JournalBytes(); release.store(true); REQUIRE(Await(refiner)); auto out=refiner.TakeFinished();
    REQUIRE(out.has_value()); ExecutionFailure(*out,47,true); CHECK_FALSE(expired.load()); CHECK(scene.JournalBytes()==closed);
    CHECK(probe->calls.load()==1); CHECK(probe->destroyed.load()==1); CHECK_FALSE(refiner.TakeFinished().has_value());
}
