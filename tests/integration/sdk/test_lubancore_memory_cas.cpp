#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <lubancore/core.hpp>
#include <lubancore/memory.hpp>
#include "memory/frontmatter.hpp"
#include "platform/sha256.hpp"
#include "runtime/memory_ledger_bridge.hpp"
#include "runtime/session_service.hpp"
#include "sdk/memory.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/cas_store.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace traj = lubancode::trajectory;
namespace rt = lubancode::runtime;
namespace mem = lubancode::memory;
namespace sdk = lubancore;
using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr auto kNeedle = "MEMORYCASNEEDLE";
constexpr auto kId = "preference.cas-fixture";
std::string Utf8(const fs::path& path) { return lubancode::tools::PathToUtf8(path); }
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open()); stream << bytes; stream.close(); REQUIRE_FALSE(stream.fail());
}
std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary); REQUIRE(stream.is_open());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("sdk-memory-cas-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "cwd"); fs::create_directories(root / "resources");
        root = fs::canonical(root);
    }
    ~Directory() { std::error_code ignored; fs::remove_all(root, ignored); }
};
std::string Body(const std::string& marker) {
    std::string text = std::string(kNeedle) + " " + marker + "\n";
    while (text.size() < 2200) text += marker + " project preference remains local.\n";
    return text;
}
void Seed(const fs::path& directory, const std::string& marker = "CAS_OWN_MARKER") {
    mem::MemoryEntry entry;
    entry.schema = 3; entry.id = kId; entry.name = kId; entry.title = kNeedle;
    entry.summary = kNeedle; entry.kind = mem::MemoryKind::Preference;
    entry.status = "active"; entry.confidence = "user-stated"; entry.keywords = {kNeedle};
    Write(directory / "preferences" / (std::string(kId) + ".md"),
        mem::frontmatter::BuildTopicText(entry, Json::object(), Body(marker)));
}
struct FakeStore final : traj::CasStore {
    traj::CasScope scope;
    std::map<std::string, std::string> entities;
    unsigned stores = 0, reads = 0;
    bool reject = false, unconfirmed = false, wrong_reference = false, bad_read = false, throw_write = false;
    std::size_t last_cap = 0;
    explicit FakeStore(traj::CasScope value) : scope(std::move(value)) {}
    traj::CasWriteReceipt Store(const traj::CasWriteRequest& request) override {
        ++stores;
        if (throw_write) throw std::runtime_error("provider exception");
        if (reject) return {traj::CasCommitState::NotCommitted, request.reference, std::nullopt, {"fake.reject", {}}};
        entities[request.reference.sha256] = std::string(request.bytes);
        auto reference = request.reference;
        if (wrong_reference) reference.scope.session_id += "-foreign";
        return {traj::CasCommitState::Committed, reference,
            unconfirmed ? traj::CasDurability::Buffered : request.required, {}};
    }
    std::expected<std::string, traj::CasError> Read(const traj::CasReference& reference, std::size_t cap) override {
        ++reads; last_cap = cap;
        if (reference.scope != scope) return std::unexpected(traj::CasError{"fake.scope", {}});
        const auto entity = entities.find(reference.sha256);
        if (entity == entities.end()) return std::unexpected(traj::CasError{"fake.missing", {}});
        return bad_read ? entity->second + "FOREIGN" : entity->second;
    }
};
struct Factory final : traj::MemoryCapabilityFactory {
    std::map<std::string, std::shared_ptr<FakeStore>> stores;
    std::vector<std::string> calls;
    bool reject = false;
    std::function<void(FakeStore&)> configure;
    std::expected<std::shared_ptr<traj::CasStore>, traj::CasError> Open(const traj::CasScope& scope) override {
        calls.push_back(scope.workspace_key + "/" + scope.session_id);
        if (reject) return std::unexpected(traj::CasError{"fake.open_failed", {}});
        auto& store = stores[calls.back()];
        if (!store) { store = std::make_shared<FakeStore>(scope); if (configure) configure(*store); }
        return std::shared_ptr<traj::CasStore>(store);
    }
    std::shared_ptr<FakeStore> At(const traj::CasScope& scope) {
        return stores.at(scope.workspace_key + "/" + scope.session_id);
    }
};

// The fake is injected into the real lock owner before the private SDK Memory
// opening gate. This host exercises the SDK component and genuine V3 adoption;
// full public Session/Backend execution is separately tested with default File.
struct MemoryHost {
    std::shared_ptr<sdk::detail::SessionMemory> memory;
    std::unique_ptr<rt::SessionService> service;
    lubancode::workspace::WorkspaceIdentity identity;
    unsigned gates = 0;
    MemoryHost(Directory& directory, std::shared_ptr<Factory> factory, std::string resume = {}, fs::path cwd = {}) {
        if (cwd.empty()) cwd = directory.root / "cwd";
        fs::create_directories(cwd);
        auto resolved = lubancode::workspace::ResolveWorkspaceIdentity(cwd, directory.root / "data");
        REQUIRE(resolved.has_value()); identity = *resolved;
        auto prepared = sdk::detail::SessionMemory::Prepare(sdk::memory::v1::RecallOptions{},
            directory.root / "data", identity, resume, cwd);
        REQUIRE_MESSAGE(prepared.has_value(), (prepared ? std::string() : prepared.error().code));
        memory = *prepared;
        rt::SessionLaunchRequest launch;
        launch.cwd_utf8 = Utf8(cwd); launch.workspace_identity = identity;
        launch.workspaces_root = directory.root / "data" / "workspaces";
        launch.v3_system_content = "CAS component host";
        launch.memory_capability_factory = factory;
        auto opening = memory->OpeningParticipant();
        launch.v3_opening_participant = [this, factory, opening = std::move(opening)](const traj::V3OpeningContext& context) {
            ++gates;
            REQUIRE(context.memory_capability != nullptr);
            const traj::CasScope expected_scope{identity.workspace_key, context.session_id};
            CHECK(context.memory_capability->scope() == expected_scope);
            if (factory) CHECK_FALSE(factory->calls.empty());
            // The true owner already holds the native session lock at this gate.
            traj::SessionLockOwner contender;
            contender.pid = 1;
            auto second = traj::SessionLock::Acquire(context.session_dir, contender);
            CHECK_FALSE(second.has_value());
            return opening(context);
        };
        if (!resume.empty()) {
            launch.resume_at_launch = true; launch.require_v3_resume = true;
            launch.resume_source_session_id = std::move(resume);
        }
        service = std::make_unique<rt::SessionService>(std::move(launch));
    }
    rt::TrajectorySessionLedger& Ledger() { REQUIRE(service->trajectory() != nullptr); return *service->trajectory(); }
    fs::path SessionDirectory() { return Ledger().session_dir(); }
    sdk::memory::v1::RecallReport Admit() {
        rt::SessionService::InputRequest input; input.client_operation_id = "cas-input"; input.text = kNeedle;
        const auto accepted = service->SubmitInput(input); REQUIRE(accepted.accepted);
        const auto pending = service->PopPendingInput(); REQUIRE(pending.status == rt::SessionService::PendingPop::Status::Ok);
        const auto turn = Ledger().v3_main_writer()->NewTurnId();
        auto bridge = Ledger().NewTurnBridge({"cas-fixture", "cas-fixture", "terminal"});
        bridge->BeginTurn(turn, "external_user");
        lubancode::api::Message user; user.role = lubancode::api::Role::User;
        user.content.push_back(lubancode::api::TextBlock{pending.input.text}); bridge->RecordInput(user);
        auto recalled = memory->BuildRecall(pending.input.text, pending.input.operation_id, turn);
        REQUIRE(recalled.has_value()); REQUIRE(recalled->records.size() == 1);
        REQUIRE(recalled->records.front().content.size() > 512);
        rt::MemoryLedgerBridge accounting(Ledger());
        auto adopted = accounting.AdmitRecallContext(recalled->context, recalled->records, turn);
        REQUIRE_MESSAGE(adopted.has_value(), (adopted ? std::string() : adopted.error()));
        REQUIRE(adopted->error.empty());
        auto report = recalled->report;
        report.state = "admitted"; report.context_message_id = adopted->message_id;
        report.context_sha256 = lubancode::platform::Sha256Hex(recalled->context); report.bytes = recalled->context.size();
        REQUIRE(memory->PersistReport(report).has_value());
        bridge->EndTurn(true, false, "success");
        const auto source = traj::v3::ReadV3Ledger(Ledger().v3_main_writer()->path()); REQUIRE(source.has_value());
        REQUIRE(memory->ValidateReport(report, *source).has_value());
        return report;
    }
    void Close() { const auto closed = service->Close("exit"); INFO(closed.message); REQUIRE(closed.error_code.empty()); }
};
class Backend final : public sdk::Backend {
public:
    unsigned calls = 0;
    std::string observed;
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation) override {
        ++calls;
        for (const auto& message : request.messages) observed += message.text + "\n";
        return sdk::ModelReply{"CAS full session answer", {}, sdk::Usage{4, 3}};
    }
};
} // namespace

TEST_CASE("memory CAS File confirms ProcessCrash, deduplicates, and bounds verified reads") {
    Directory directory;
    auto opened = traj::OpenMemoryCapability({"project-a", "session-a"}, directory.root / "artifacts"); REQUIRE(opened.has_value());
    const auto cap = opened->share();
    const auto first = cap->Store(Body("FILE_MARKER"), "text/plain"); REQUIRE(first.Confirms(traj::CasDurability::ProcessCrash));
    const auto path = directory.root / fs::path(traj::MemoryCapability::LogicalReference(first.reference.sha256));
    REQUIRE(fs::is_regular_file(path)); const auto stamp = fs::last_write_time(path);
    const auto second = cap->Store(Body("FILE_MARKER"), "text/plain"); REQUIRE(second.Confirms(traj::CasDurability::ProcessCrash));
    CHECK(second.reference == first.reference); CHECK(fs::last_write_time(path) == stamp);
    auto exact = cap->Read(first.reference, Body("FILE_MARKER").size()); REQUIRE(exact.has_value()); CHECK(*exact == Body("FILE_MARKER"));
    CHECK_FALSE(cap->Read(first.reference, Body("FILE_MARKER").size() - 1).has_value());
    auto wrong_size = first.reference; ++wrong_size.bytes;
    CHECK_FALSE(cap->Read(wrong_size, wrong_size.bytes).has_value());
    Write(path, Body("FILE_MARKER") + std::string(10000, 'x'));
    CHECK_FALSE(cap->Read(first.reference, first.reference.bytes).has_value());
    auto foreign = first.reference; foreign.scope.session_id = "session-b";
    CHECK_FALSE(cap->Read(foreign, foreign.bytes).has_value());
}

TEST_CASE("memory CAS File never overwrites an existing corrupt content address") {
    Directory directory;
    auto opened = traj::OpenMemoryCapability({"project-a", "session-a"}, directory.root / "artifacts"); REQUIRE(opened.has_value());
    const auto receipt = opened->share()->Store(Body("OLD_MARKER"), "text/plain"); REQUIRE(receipt.Confirms(traj::CasDurability::ProcessCrash));
    const auto path = directory.root / fs::path(traj::MemoryCapability::LogicalReference(receipt.reference.sha256));
    Write(path, "CORRUPT_ENTITY"); const auto old = Read(path);
    const auto refused = opened->share()->Store(Body("OLD_MARKER"), "text/plain");
    CHECK(refused.state == traj::CasCommitState::NotCommitted); CHECK(refused.error.code == "cas.existing_corrupt");
    CHECK(Read(path) == old); CHECK_FALSE(opened->share()->Read(receipt.reference, receipt.reference.bytes).has_value());
    fs::remove(path); fs::create_directory(path);
    CHECK_FALSE(opened->share()->Read(receipt.reference, receipt.reference.bytes).has_value());
    CHECK(opened->share()->Store(Body("OLD_MARKER"), "text/plain").error.code == "cas.existing_corrupt");
#ifndef _WIN32
    fs::remove(path); REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
    CHECK_FALSE(opened->share()->Read(receipt.reference, receipt.reference.bytes).has_value());
    CHECK(opened->share()->Store(Body("OLD_MARKER"), "text/plain").error.code == "cas.existing_corrupt");
    Directory external;
    fs::remove_all(directory.root / "artifacts");
    fs::create_directory_symlink(external.root, directory.root / "artifacts");
    CHECK(opened->share()->Store(Body("OLD_MARKER"), "text/plain").error.code == "cas.path_escape");
    CHECK_FALSE(opened->share()->Read(receipt.reference, receipt.reference.bytes).has_value());
    CHECK_FALSE(fs::exists(external.root / "sha256"));
#endif
}

TEST_CASE("memory CAS native close rejection precedes publication and confirmation failure retains the entity") {
    Directory directory;
    const traj::CasScope scope{"project-a", "session-a"};
    for (const auto boundary : {traj::FileCasBoundary::AfterNativeClose, traj::FileCasBoundary::AfterPublish}) {
        const auto root = directory.root / (boundary == traj::FileCasBoundary::AfterNativeClose ? "pre" : "post") / "artifacts";
        fs::create_directories(root.parent_path());
        auto store = traj::MakeFileCasStore(scope, root, [boundary](auto actual) -> std::optional<std::string> {
            return actual == boundary ? std::optional<std::string>("test-boundary") : std::nullopt;
        });
        traj::MemoryCapability cap(scope, store);
        const auto receipt = cap.Store(Body("BOUNDARY_MARKER"), "text/plain", traj::CasDurability::PowerLoss);
        CHECK_FALSE(receipt.Confirms(traj::CasDurability::PowerLoss));
        const auto path = root.parent_path() / fs::path(traj::MemoryCapability::LogicalReference(receipt.reference.sha256));
        if (boundary == traj::FileCasBoundary::AfterNativeClose) {
            CHECK(receipt.state == traj::CasCommitState::NotCommitted); CHECK_FALSE(fs::exists(path));
        } else {
            CHECK(receipt.state == traj::CasCommitState::Committed); CHECK(receipt.confirmed_durability == traj::CasDurability::ProcessCrash);
            CHECK(Read(path) == Body("BOUNDARY_MARKER")); REQUIRE(cap.Read(receipt.reference, receipt.reference.bytes).has_value());
        }
    }
}

TEST_CASE("memory CAS factory receipts and returned bytes cannot claim a foreign or unconfirmed value") {
    Directory directory;
    auto factory = std::make_shared<Factory>();
    auto opened = traj::OpenMemoryCapability({"project-a", "session-a"}, directory.root / "unused", factory); REQUIRE(opened.has_value());
    auto cap = opened->share(); auto store = factory->At(cap->scope());
    store->unconfirmed = true;
    const auto unconfirmed = cap->Store(Body("FAKE_MARKER"), "text/plain");
    CHECK(unconfirmed.state == traj::CasCommitState::Committed); CHECK_FALSE(unconfirmed.Confirms(traj::CasDurability::ProcessCrash));
    CHECK(unconfirmed.error.code == "cas.durability_unconfirmed");
    store->unconfirmed = false; store->wrong_reference = true;
    const auto wrong = cap->Store(Body("FAKE_MARKER"), "text/plain"); CHECK(wrong.state == traj::CasCommitState::Indeterminate);
    CHECK(wrong.error.code == "cas.receipt_mismatch");
    store->wrong_reference = false;
    const auto good = cap->Store(Body("FAKE_MARKER"), "text/plain"); REQUIRE(good.Confirms(traj::CasDurability::ProcessCrash));
    store->bad_read = true; CHECK_FALSE(cap->Read(good.reference, good.reference.bytes).has_value());
    CHECK(store->last_cap == good.reference.bytes); store->bad_read = false;
    store->throw_write = true; CHECK(cap->Store(Body("THROW_MARKER"), "text/plain").state == traj::CasCommitState::Indeterminate);
    CHECK_FALSE(fs::exists(directory.root / "unused"));
}

TEST_CASE("memory CAS lease destruction seals borrowed writes while immutable reads remain bounded") {
    Directory directory;
    auto factory = std::make_shared<Factory>(); std::shared_ptr<traj::MemoryCapability> borrowed;
    traj::CasReference reference;
    {
        auto owned = traj::OpenMemoryCapability({"project-a", "session-a"}, directory.root / "unused", factory); REQUIRE(owned.has_value());
        borrowed = owned->share(); const auto receipt = borrowed->Store(Body("LEASE_MARKER"), "text/plain");
        REQUIRE(receipt.Confirms(traj::CasDurability::ProcessCrash)); reference = receipt.reference;
    }
    auto store = factory->At(reference.scope); const auto before = store->stores;
    const auto refused = borrowed->Store("late write", "text/plain"); CHECK(refused.state == traj::CasCommitState::NotCommitted);
    CHECK(refused.error.code == "cas.owner_closed"); CHECK(store->stores == before);
    auto read = borrowed->Read(reference, reference.bytes); REQUIRE(read.has_value()); CHECK(*read == Body("LEASE_MARKER"));
}

TEST_CASE("memory CAS fake is actually consumed by SDK recall reports and same-ID locked recovery without File entities") {
    Directory directory; auto factory = std::make_shared<Factory>();
    MemoryHost host(directory, factory); INFO(host.service->launch_error()); REQUIRE(host.service->runtime() != nullptr);
    CHECK(host.gates == 1); Seed(lubancode::tools::Utf8ToPath(host.memory->Describe().memory_directory));
    const auto report = host.Admit(); const auto sid = host.Ledger().session_id(); const auto path = host.SessionDirectory();
    auto capability = host.Ledger().memory_capability(); auto store = factory->At(capability->scope());
    REQUIRE(store->stores == 1); REQUIRE(store->reads >= 2); REQUIRE(store->entities.size() == 1);
    CHECK_FALSE(fs::exists(path / "artifacts" / "sha256"));
    // The legacy CLI dispatch accounting consumer uses the same owned provider.
    // Spawn a real child stream, freeze actual ProjectMemory selection, then
    // cancel that opened-but-unexecuted child rather than inventing success.
    auto child = host.Ledger().SpawnSubagent("call-cas-dispatch", "CAS dispatch accounting");
    REQUIRE(child.has_value());
    rt::MemoryLedgerBridge accounting(host.Ledger());
    mem::ProjectIdentity project_identity;
    project_identity.project_root = host.identity.project_root;
    project_identity.identity_root = host.identity.identity_root;
    project_identity.workspace_key = host.identity.workspace_key;
    project_identity.workspace_dir = path.parent_path().parent_path();
    project_identity.display_name = host.identity.display_name;
    project_identity.git = host.identity.git();
    mem::Options selection;
    selection.global_allowed = selection.enabled = true; selection.user_enabled = false;
    selection.learn = selection.learn_ceiling = mem::LearnMode::Off;
    mem::ProjectMemory cli_memory(project_identity, directory.root / "data", selection);
    cli_memory.set_accounting(&accounting);
    const auto before_dispatch = store->stores;
    const auto frozen = cli_memory.BuildTurnContextForDispatch(kNeedle, directory.root / "cwd", (*child)->run_id());
    REQUIRE(frozen.find("CAS_OWN_MARKER") != std::string::npos);
    CHECK(store->stores == before_dispatch + 1); CHECK_FALSE(fs::exists(path / "artifacts" / "sha256"));
    const auto parent = traj::v3::ReadV3Ledger(host.Ledger().v3_main_writer()->path()); REQUIRE(parent.has_value());
    CHECK(std::count_if(parent->events.begin(), parent->events.end(), [&](const auto& event) {
        return event.kind == traj::v3::EventKindV3::MemoryRecallInjected &&
            event.payload.value("targetRunId", std::string()) == (*child)->run_id() &&
            event.payload.contains("snapshotRef") && !event.payload.contains("contextMessageRef");
    }) == 1);
    const auto cancelled = (*child)->Finish(rt::SubagentExecutionOutcome::Cancelled, "fixture-dispatch-not-run");
    REQUIRE(cancelled.durable()); child->reset();
    auto saved = host.memory->ReadReport(report.operation_id); REQUIRE(saved.has_value()); CHECK(saved->context_sha256 == report.context_sha256);
    host.Close(); const auto reads = store->reads; const auto writes = store->stores;
    CHECK(capability->Store("late write", "text/plain").error.code == "cas.owner_closed");
    MemoryHost resumed(directory, factory, sid); INFO(resumed.service->launch_error()); REQUIRE(resumed.service->runtime() != nullptr);
    CHECK(resumed.Ledger().session_id() == sid); CHECK(resumed.gates == 1);
    CHECK(store->reads > reads); CHECK(store->stores == writes); CHECK_FALSE(fs::exists(path / "artifacts" / "sha256"));
    auto recovered = resumed.memory->ReadReport(report.operation_id); REQUIRE(recovered.has_value());
    auto ledger = traj::v3::ReadV3Ledger(resumed.Ledger().v3_main_writer()->path()); REQUIRE(ledger.has_value());
    REQUIRE(resumed.memory->ValidateReport(*recovered, *ledger).has_value()); resumed.Close();
}

TEST_CASE("memory CAS refusal or failed verified read cannot publish new context, recall fact, or model request") {
    for (const auto mode : {0, 1, 2}) {
        Directory directory; auto factory = std::make_shared<Factory>();
        factory->configure = [mode](FakeStore& store) { store.reject = mode == 0; store.unconfirmed = mode == 1; store.bad_read = mode == 2; };
        MemoryHost host(directory, factory); REQUIRE(host.service->runtime() != nullptr);
        Seed(lubancode::tools::Utf8ToPath(host.memory->Describe().memory_directory));
        const auto turn = host.Ledger().v3_main_writer()->NewTurnId();
        auto bridge = host.Ledger().NewTurnBridge({"cas-fixture", "cas-fixture", "terminal"}); bridge->BeginTurn(turn, "external_user");
        auto recalled = host.memory->BuildRecall(kNeedle, "op-1", turn); REQUIRE(recalled.has_value()); REQUIRE(recalled->records.size() == 1);
        rt::MemoryLedgerBridge accounting(host.Ledger());
        auto admitted = accounting.AdmitRecallContext(recalled->context, recalled->records, turn); CHECK_FALSE(admitted.has_value());
        const auto ledger = traj::v3::ReadV3Ledger(host.Ledger().v3_main_writer()->path()); REQUIRE(ledger.has_value());
        CHECK(std::none_of(ledger->messages.begin(), ledger->messages.end(), [&](const auto& message) {
            return message.turn_id == turn && message.origin == traj::v3::MessageOrigin::ContextRuntime;
        }));
        CHECK(std::none_of(ledger->events.begin(), ledger->events.end(), [&](const auto& event) {
            return event.turn_id == turn && (event.kind == traj::v3::EventKindV3::MemoryRecallInjected ||
                event.kind == traj::v3::EventKindV3::ModelRequestPrepared);
        }));
        bridge->EndTurn(false, false, "cas-not-admitted"); bridge.reset(); host.Close();
    }
}

TEST_CASE("memory CAS two same-project and two different-project sessions keep factory scope and closure isolated") {
    Directory directory; auto factory = std::make_shared<Factory>();
    std::vector<std::unique_ptr<MemoryHost>> hosts;
    const auto cwd_before = fs::current_path();
    for (const auto& cwd : {directory.root / "cwd", directory.root / "cwd", directory.root / "project-b", directory.root / "project-c"})
        hosts.push_back(std::make_unique<MemoryHost>(directory, factory, std::string(), cwd));
    std::vector<traj::CasReference> references;
    for (std::size_t index = 0; index < hosts.size(); ++index) {
        REQUIRE(hosts[index]->service->runtime() != nullptr);
        const auto receipt = hosts[index]->Ledger().memory_capability()->Store(Body("ISOLATION_" + std::to_string(index)), "text/plain");
        REQUIRE(receipt.Confirms(traj::CasDurability::ProcessCrash)); references.push_back(receipt.reference);
    }
    REQUIRE(factory->stores.size() == 4);
    CHECK(references[0].scope.workspace_key == references[1].scope.workspace_key);
    CHECK(references[0].scope.session_id != references[1].scope.session_id);
    for (std::size_t index = 0; index < hosts.size(); ++index) {
        auto cap = hosts[index]->Ledger().memory_capability();
        auto own = cap->Read(references[index], references[index].bytes); REQUIRE(own.has_value()); CHECK(*own == Body("ISOLATION_" + std::to_string(index)));
        for (std::size_t foreign = 0; foreign < hosts.size(); ++foreign)
            if (foreign != index) CHECK_FALSE(cap->Read(references[foreign], references[foreign].bytes).has_value());
    }
    hosts[0]->Close();
    CHECK(hosts[0]->Ledger().memory_capability()->Store("late", "text/plain").error.code == "cas.owner_closed");
    for (std::size_t index = 1; index < hosts.size(); ++index) {
        CHECK(hosts[index]->Ledger().memory_capability()->Store("still live", "text/plain").Confirms(traj::CasDurability::ProcessCrash)); hosts[index]->Close();
    }
    CHECK(fs::current_path() == cwd_before);
}

TEST_CASE("memory CAS factory opening rejection does not invoke the SDK gate or publish a V3 stream") {
    Directory directory; auto factory = std::make_shared<Factory>(); factory->reject = true;
    MemoryHost host(directory, factory); CHECK(host.service->runtime() == nullptr); CHECK(host.gates == 0);
    CHECK(host.service->launch_error().find("fake.open_failed") != std::string::npos);
    REQUIRE(factory->calls.size() == 1);
    unsigned streams = 0;
    for (const auto& entry : fs::recursive_directory_iterator(directory.root / "data" / "workspaces"))
        if (entry.is_regular_file() && entry.path().extension() == ".jsonl" && entry.path().parent_path().parent_path().filename() == "sessions") ++streams;
    CHECK(streams == 0);
}

TEST_CASE("memory CAS default File runs a complete public SDK turn, saved report, and resumed turn") {
    Directory directory;
    auto runtime = sdk::Runtime::Create({Utf8(directory.root / "data"), Utf8(directory.root / "resources")}); REQUIRE(runtime.has_value());
    sdk::SessionOptions options; options.cwd = Utf8(directory.root / "cwd"); options.model = "cas-model";
    options.memory = sdk::memory::v1::RecallOptions{};
    auto first_backend = std::make_unique<Backend>(); auto* first = first_backend.get(); options.backend = std::move(first_backend);
    auto session = (*runtime)->OpenSession(std::move(options)); REQUIRE(session.has_value());
    auto snapshot = (*session)->DescribeMemory(); REQUIRE(snapshot.has_value()); Seed(lubancode::tools::Utf8ToPath(snapshot->memory_directory));
    const auto receipt = (*session)->Submit("cas-full", kNeedle); REQUIRE(receipt.has_value());
    auto result = (*session)->WaitResult(receipt->operation_id, 30s); REQUIRE(result.has_value()); INFO(result->error);
    CHECK(result->state == sdk::OperationState::Succeeded); CHECK(result->result_persisted); CHECK(first->calls == 1);
    CHECK(first->observed.find("CAS_OWN_MARKER") != std::string::npos);
    auto report = (*session)->GetMemoryRecall(receipt->operation_id); REQUIRE(report.has_value()); CHECK(report->state == "admitted");
    const auto sid = (*session)->id(); const auto path = lubancode::tools::Utf8ToPath(snapshot->memory_directory).parent_path() / "sessions" / sid;
    REQUIRE(fs::exists(path / "artifacts" / "sha256"));
    REQUIRE((*session)->Close().has_value());
    sdk::SessionOptions restore; restore.cwd = Utf8(directory.root / "cwd"); restore.model = "cas-model"; restore.resume_session_id = sid;
    auto next_backend = std::make_unique<Backend>(); auto* next = next_backend.get(); restore.backend = std::move(next_backend);
    auto resumed = (*runtime)->OpenSession(std::move(restore)); REQUIRE(resumed.has_value());
    auto old = (*resumed)->GetMemoryRecall(receipt->operation_id); REQUIRE(old.has_value()); CHECK(old->context_sha256 == report->context_sha256);
    const auto second = (*resumed)->Submit("cas-next", "continue"); REQUIRE(second.has_value());
    const auto done = (*resumed)->WaitResult(second->operation_id, 30s); REQUIRE(done.has_value()); INFO(done->error);
    CHECK(done->state == sdk::OperationState::Succeeded); CHECK(done->result_persisted); CHECK(next->calls == 1);
    CHECK(next->observed.find("CAS_OWN_MARKER") != std::string::npos); REQUIRE((*resumed)->Close().has_value());
}
