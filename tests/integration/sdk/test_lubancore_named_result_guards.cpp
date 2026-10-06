#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lubancore/named_results.hpp>
#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "runtime/session_service.hpp"
#include "sdk/named_results.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/named_result_opening.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/session_manager.hpp"
#include "trajectory/replay.hpp"
#include "trajectory/v3/result_store.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace sdk = lubancore;
namespace blob = sdk::named_results::v1;
namespace traj = lubancode::trajectory;
namespace v3 = traj::v3;
namespace platform = lubancode::platform;
namespace rt = lubancode::runtime;
using Json = nlohmann::json;
using Knowledge = v3::ResultStore::PersistedResult::Knowledge;

void Mark(const char* path) { std::cout << "[sdk-named-result-guards-path] " << path << '\n'; }
struct Directory {
    fs::path root, project;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("named-guards-" + std::to_string(platform::CurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root / "project"); root = fs::canonical(root); project = root / "project";
    }
    ~Directory() { std::error_code ignored; fs::remove_all(platform::FileIoPath(root), ignored); }
};
void Write(const fs::path& path, const std::string& value) {
    std::ofstream file(platform::FileIoPath(path), std::ios::binary | std::ios::trunc); REQUIRE(file.is_open());
    file.write(value.data(), static_cast<std::streamsize>(value.size())); file.close(); REQUIRE_FALSE(file.fail());
}
std::string Read(const fs::path& path) {
    std::ifstream file(platform::FileIoPath(path), std::ios::binary); REQUIRE(file.is_open());
    std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}; REQUIRE_FALSE(file.bad()); return bytes;
}
struct Control {
    fs::path root;
    unsigned opens = 0, writes = 0;
    unsigned reject_write = 0;
    bool giant_token = false, duplicate_name = false, giant_receipt = false;
    int state = static_cast<int>(blob::CommitState::Committed);
    int durability = static_cast<int>(blob::Durability::ProcessCrash);
    std::vector<platform::ImmutableWriteReceipt> native;
};
class Store final : public blob::Store {
public:
    Store(std::shared_ptr<Control> control, blob::OpenRequest request) : control_(std::move(control)), request_(std::move(request)) {
        fs::create_directories(control_->root);
    }
    blob::WriteReceipt PublishNew(blob::WriteRequest request) override {
        REQUIRE(request.reference.scope == request_.scope); REQUIRE(request.reference.binding_id == request_.binding_id);
        blob::WriteReceipt result; result.reference = request.reference; result.request_key = request.request_key;
        if (++control_->writes == control_->reject_write) { result.error = {"guard.zero_publication", {}}; return result; }
        const auto actual = platform::CreateImmutableFileDetailed(control_->root / request.reference.logical_name, request.bytes);
        control_->native.push_back(actual);
        result.state = actual.outcome == platform::WriteOutcome::NotCommitted ? blob::CommitState::NotCommitted : blob::CommitState::Committed;
        if (actual.outcome == platform::WriteOutcome::CommittedDurable) result.confirmed_durability = blob::Durability::ProcessCrash;
        result.error = {actual.error_code, actual.message};
        if (control_->giant_receipt) {
            result.state = static_cast<blob::CommitState>(control_->state);
            result.confirmed_durability = static_cast<blob::Durability>(control_->durability);
            result.error = {"guard.oversized_claim", std::string(8193, 'E')};
        }
        return result;
    }
    sdk::Result<std::string> Read(blob::Reference reference, std::size_t cap) override {
        REQUIRE(reference.scope == request_.scope); REQUIRE(reference.binding_id == request_.binding_id);
        if (reference.bytes > cap) return std::unexpected(sdk::Error{"guard.limit", {}});
        return ::Read(control_->root / reference.logical_name);
    }
    sdk::Result<blob::NameSnapshot> SnapshotNames(blob::ListLimits limits) override {
        blob::NameSnapshot result{request_.scope, request_.binding_id, "actual-native-inventory", true, {}};
        for (const auto& entry : fs::directory_iterator(control_->root)) result.names.push_back(platform::PathToUtf8(entry.path().filename()));
        REQUIRE(result.names.size() <= limits.entries);
        if (control_->giant_token) result.token.assign(201, 'T');
        if (control_->duplicate_name) { REQUIRE_FALSE(result.names.empty()); result.names.push_back(result.names.front()); }
        return result;
    }
private:
    std::shared_ptr<Control> control_;
    blob::OpenRequest request_;
};
class Provider final : public blob::Provider {
public:
    explicit Provider(std::shared_ptr<Control> value) : control_(std::move(value)) {}
    sdk::Result<std::unique_ptr<blob::Store>> Open(blob::OpenRequest request) override {
        ++control_->opens; return std::unique_ptr<blob::Store>(std::make_unique<Store>(control_, std::move(request)));
    }
private:
    std::shared_ptr<Control> control_;
};
std::shared_ptr<traj::NamedResultFactory> Factory(const std::shared_ptr<Control>& control) {
    blob::Options options{"actual-native-guard-v1", std::make_unique<Provider>(control)};
    auto factory = sdk::detail::MakeNamedResultFactory(std::move(options));
    REQUIRE_MESSAGE(factory.has_value(), (factory ? std::string() : factory.error().code)); return *factory;
}
std::unique_ptr<rt::SessionService> Open(const Directory& directory, const std::shared_ptr<traj::NamedResultFactory>& factory = {},
    const std::string& resume = {}, bool participant = true) {
    rt::SessionLaunchRequest request;
    request.cwd_utf8 = platform::PathToUtf8(directory.project);
    request.workspace_identity = lubancode::workspace::MakeFallbackIdentity(directory.project);
    request.workspaces_root = directory.root / "state" / "workspaces";
    request.wire_name = "chat"; request.v3_system_content = "actual named storage guard";
    request.named_result_factory = factory;
    if (participant) request.v3_opening_participant = [](const traj::V3OpeningContext&) -> std::expected<Json, std::string> { return Json::object(); };
    if (!resume.empty()) { request.resume_at_launch = true; request.require_v3_resume = true; request.resume_source_session_id = resume; }
    return std::make_unique<rt::SessionService>(std::move(request));
}
std::shared_ptr<traj::NamedResultCapability> Capability(rt::SessionService& service) {
    REQUIRE_MESSAGE(service.runtime() != nullptr, service.launch_error());
    auto cap = service.trajectory()->named_result_capability(); REQUIRE(cap != nullptr);
    const auto owner = traj::SessionLock::Inspect(service.trajectory()->session_dir()); REQUIRE(owner);
    REQUIRE(owner->pid == platform::CurrentProcessId()); return cap;
}
v3::ResultStore::PersistRequest Material(std::string text = "actual named native bytes") {
    v3::ResultStore::PersistRequest request;
    request.tool_call_id = "storage-material"; request.result_kind = "text"; request.attempt = 1;
    v3::ResultStore::ChannelOutput output;
    output.channel = "combined"; output.data = std::move(text); output.output_bytes = output.data.size();
    request.outputs.push_back(std::move(output)); return request;
}
struct FlushFailure {
    explicit FlushFailure(bool directory) : directory_(directory) {
        if (directory_) platform::SetDirectoryFlushFailureForTest(true); else platform::SetFileFlushFailureForTest(true);
    }
    ~FlushFailure() { if (directory_) platform::SetDirectoryFlushFailureForTest(false); else platform::SetFileFlushFailureForTest(false); }
    bool directory_;
};
std::string MutateEnvelope(const std::string& source, bool role) {
    std::vector<Json> rows; std::istringstream input(source); std::string line;
    while (std::getline(input, line)) rows.push_back(Json::parse(line));
    REQUIRE_FALSE(rows.empty()); unsigned changed = 0;
    if (role) { rows.front()["message"]["role"] = "user"; ++changed; }
    else for (auto& row : rows) if (row.value("kind", "") == "session.started") { row["sessionId"] = "foreign-named-owner"; ++changed; }
    REQUIRE(changed == 1);
    std::string previous(v3::kGenesisHash), output;
    for (auto& row : rows) {
        row.erase("prevHash"); row.erase("lineHash");
        auto body = traj::CanonicalJsonDump(row); REQUIRE(body);
        const auto hash = v3::ComputeLineHash(previous, *body);
        row["prevHash"] = previous; row["lineHash"] = hash;
        auto rendered = traj::CanonicalJsonDump(row); REQUIRE(rendered); output += *rendered + '\n'; previous = hash;
    }
    return output;
}
} // namespace

TEST_CASE("named result guards: a real partial material seals every facade before releasing its owner") {
    Directory directory; auto control = std::make_shared<Control>(); control->root = directory.root / "external";
    auto service = Open(directory, Factory(control)); auto cap = Capability(*service);
    auto captures = v3::ResultStore::Open(cap, "capture-"); auto results = v3::ResultStore::Open(cap); REQUIRE(captures); REQUIRE(results);
    control->reject_write = 2; const auto partial = captures->Persist(Material());
    REQUIRE_FALSE(partial.ok); REQUIRE(partial.publication); REQUIRE(partial.publication->knowledge == Knowledge::Indeterminate);
    REQUIRE(control->writes == 2); REQUIRE(control->native.size() == 1);
    REQUIRE(control->native.front().outcome == platform::WriteOutcome::CommittedDurable);
    const auto first = cap->FirstUnconfirmedPublication(); REQUIRE(first); REQUIRE(*first == *partial.publication);
    const auto refused = results->Persist(Material("different formal material")); REQUIRE_FALSE(refused.ok);
    REQUIRE(refused.publication); REQUIRE(*refused.publication == *first); REQUIRE(control->writes == 2);
    REQUIRE_FALSE(v3::ResultStore::Open(cap, "job-admission-"));
    const auto& ref = partial.publication->files.front().receipt->reference;
    auto read = cap->Read("artifacts/" + ref.logical_name, ref.sha256, ref.bytes, ref.media_type, 4096); REQUIRE(read);
    REQUIRE(*read == "actual named native bytes"); REQUIRE(*cap->FirstUnconfirmedPublication() == *first);
    (void)service->Close("partial_guard"); REQUIRE(control->writes == 2); Mark("shared-partial");
}

TEST_CASE("named result guards: File preserves actual native uncertainty and known before-publish failure") {
    for (const bool directory_failure : {false, true}) {
        Directory directory; auto service = Open(directory); auto cap = Capability(*service);
        auto store = v3::ResultStore::Open(cap); REQUIRE(store);
        v3::ResultStore::PersistedResult failure;
        { FlushFailure fault(directory_failure); failure = store->Persist(Material()); }
        REQUIRE_FALSE(failure.ok); REQUIRE(failure.publication); REQUIRE(failure.publication->files.size() == 1);
        const auto& file = failure.publication->files.front(); REQUIRE(file.native);
        const auto native = *file.native;
        REQUIRE(native.body.attempted); REQUIRE(native.body.written_bytes == std::string("actual named native bytes").size());
        if (directory_failure) {
            REQUIRE(native.outcome == platform::WriteOutcome::CommittedDurabilityUnconfirmed);
            REQUIRE(native.file_sync.succeeded); REQUIRE(native.publish.succeeded); REQUIRE(native.parent_sync.injected_failure);
            REQUIRE(failure.publication->knowledge == Knowledge::Indeterminate);
            REQUIRE(*cap->FirstUnconfirmedPublication() == *failure.publication); REQUIRE_FALSE(v3::ResultStore::Open(cap));
            const auto& ref = file.receipt->reference;
            REQUIRE(cap->Read("artifacts/" + ref.logical_name, ref.sha256, ref.bytes, ref.media_type, 4096));
            REQUIRE(cap->FirstUnconfirmedPublication()->files.front().native == native);
        } else {
            REQUIRE(native.outcome == platform::WriteOutcome::NotCommitted); REQUIRE(native.file_sync.injected_failure);
            REQUIRE_FALSE(native.publish.attempted); REQUIRE_FALSE(cap->FirstUnconfirmedPublication());
            const auto next = store->Persist(Material("next real bytes")); REQUIRE(next.ok); REQUIRE(next.result_id == "res-000001");
        }
        (void)service->Close("native_receipt_guard");
    }
    Mark("file-receipts");
}

TEST_CASE("named result guards: File opens lazily preserves prefix skips caps and later collisions") {
    Directory directory; const auto session = directory.root / "standalone"; fs::create_directory(session);
    traj::SessionLockOwner owner{platform::CurrentProcessId(), traj::CurrentProcessStartToken(), 1};
    auto lock = traj::SessionLock::Acquire(session, owner); REQUIRE(lock); REQUIRE(lock->holds());
    const auto identity = lubancode::workspace::MakeFallbackIdentity(directory.project);
    auto lease = traj::OpenNamedResultCapability({identity.workspace_key, "standalone"}, session); REQUIRE(lease);
    REQUIRE_FALSE(fs::exists(session / "artifacts"));
    fs::create_directory(session / "artifacts");
#ifdef _WIN32
    Write(session / "artifacts" / "unrelated-note", "unrelated");
#else
    Write(session / "artifacts" / "unrelated:note", "unrelated");
#endif
    Write(session / "artifacts" / "res-000001.combined.txt.tmp", "actual orphan");
    auto limited = v3::ResultStore::Open(lease->share(), "res-", 1); REQUIRE_FALSE(limited);
    REQUIRE(limited.error() == "result directory entry limit exceeded");
    auto store = v3::ResultStore::Open(lease->share(), "res-", 2); REQUIRE(store);
    fs::create_directory(session / "artifacts" / "res-000002.json"); // after Open, before Persist
    const auto collision = store->Persist(Material()); REQUIRE_FALSE(collision.ok); REQUIRE(collision.publication);
    REQUIRE(collision.publication->knowledge == Knowledge::Indeterminate);
    REQUIRE(Read(session / "artifacts" / "res-000002.combined.txt") == "actual named native bytes");
    REQUIRE(fs::is_directory(session / "artifacts" / "res-000002.json"));
    lease->CloseWrites();
    const auto overflow = directory.root / "overflow"; fs::create_directories(overflow / "artifacts");
    Write(overflow / "artifacts" / "res-999999999999999999999999999999.json", "occupied");
    CHECK_THROWS_AS(v3::ResultStore::Open(overflow), std::out_of_range);
    fs::remove(overflow / "artifacts" / "res-999999999999999999999999999999.json");
    auto after = v3::ResultStore::Open(overflow); REQUIRE(after); REQUIRE(after->Persist(Material()).ok);
    Mark("file-compatibility");
}

TEST_CASE("named result guards: generic locked opening rejects real foreign owner and non-system source before Provider") {
    for (const bool role : {false, true}) {
        Directory directory; auto control = std::make_shared<Control>(); control->root = directory.root / "external";
        auto factory = Factory(control); auto service = Open(directory, factory); auto cap = Capability(*service);
        const auto id = cap->scope().session_id; const auto path = service->trajectory()->session_dir() / (id + ".jsonl");
        (void)service->Close("seed_envelope"); service.reset();
        const auto original = Read(path); const auto changed = MutateEnvelope(original, role); Write(path, changed);
        if (!role) REQUIRE(v3::ReadV3Ledger(path)); // real verified file reaches the generic owner check
        const auto opens = control->opens; auto refused = Open(directory, factory, id);
        REQUIRE(refused->runtime() == nullptr); REQUIRE(control->opens == opens); REQUIRE(Read(path) == changed);
        if (!role) REQUIRE(refused->launch_error().find("named_result.initial_owner_mismatch") != std::string::npos);
        refused.reset(); Write(path, original);
        auto missing = Open(directory, {}, id, false); REQUIRE(missing->runtime() == nullptr);
        REQUIRE(missing->launch_error().find("named_result.provider_required") != std::string::npos);
        REQUIRE(Read(path) == original); REQUIRE(control->opens == opens);
    }
    Mark("opening-envelope");
}

TEST_CASE("named result guards: original illegal public enum scalars survive bounded first-unknown fingerprints") {
    Directory directory; auto control = std::make_shared<Control>(); control->root = directory.root / "external";
    control->giant_receipt = true; auto factory = Factory(control);
    std::string id; std::vector<std::string> fingerprints; std::optional<traj::CasScope> scope;
    for (unsigned variant = 0; variant < 4; ++variant) {
        control->state = variant < 2 ? 123 + static_cast<int>(variant) : static_cast<int>(blob::CommitState::Committed);
        control->durability = variant < 2 ? static_cast<int>(blob::Durability::ProcessCrash) : 123 + static_cast<int>(variant - 2);
        auto service = Open(directory, factory, id); auto cap = Capability(*service);
        if (!scope) scope = cap->scope(); REQUIRE(cap->scope() == *scope); id = cap->scope().session_id;
        auto store = v3::ResultStore::Open(cap); REQUIRE(store);
        const auto failed = store->PersistListingDetailed("fixed-claim.txt", "identical actual bytes");
        REQUIRE_FALSE(failed.ok); REQUIRE(failed.publication.knowledge == Knowledge::Indeterminate);
        REQUIRE(failed.publication.files.front().receipt); const auto& receipt = *failed.publication.files.front().receipt;
        REQUIRE(receipt.oversize_claim); const auto& claim = *receipt.oversize_claim;
        REQUIRE(claim.state == control->state); REQUIRE(claim.durability == control->durability);
        REQUIRE(claim.field_bytes[8] == 8193); REQUIRE(claim.fingerprint_complete); REQUIRE(claim.sha256.size() == 64);
        REQUIRE(receipt.error.message.size() <= 4096); fingerprints.push_back(claim.sha256);
        const auto writes = control->writes; const auto first = *cap->FirstUnconfirmedPublication();
        REQUIRE_FALSE(store->PersistListingDetailed("different-name.txt", "must not write").ok);
        REQUIRE(control->writes == writes); REQUIRE(*cap->FirstUnconfirmedPublication() == first);
        REQUIRE(Read(control->root / "fixed-claim.txt") == "identical actual bytes");
        (void)service->Close("close_oversized_claim");
    }
    REQUIRE(fingerprints[0] != fingerprints[1]); REQUIRE(fingerprints[2] != fingerprints[3]);
    Mark("claim-bounds");
}

TEST_CASE("named result guards: pre-callback rejection keeps owner usable and actual Close retires write lease") {
    Directory directory; auto control = std::make_shared<Control>(); control->root = directory.root / "external";
    auto service = Open(directory, Factory(control)); auto cap = Capability(*service);
    auto store = v3::ResultStore::Open(cap); REQUIRE(store);
    auto invalid = Material(); invalid.outputs.front().channel = "../escape";
    const auto refusal = store->Persist(invalid); REQUIRE_FALSE(refusal.ok); REQUIRE(refusal.publication);
    REQUIRE(refusal.publication->knowledge == Knowledge::NotCommitted); REQUIRE(control->writes == 0);
    REQUIRE_FALSE(refusal.publication->files.front().called); REQUIRE_FALSE(cap->FirstUnconfirmedPublication());
    const auto actual = store->Persist(Material()); REQUIRE(actual.ok); REQUIRE(actual.result_id == "res-000001");
    const auto ref = actual.publication->files.front().receipt->reference; const auto writes = control->writes;
    (void)service->Close("retire_actual_owner"); service.reset();
    REQUIRE_FALSE(store->Persist(Material("late write")).ok); REQUIRE(control->writes == writes);
    auto saved = cap->Read("artifacts/" + ref.logical_name, ref.sha256, ref.bytes, ref.media_type, 4096); REQUIRE(saved);
    REQUIRE(*saved == "actual named native bytes"); Mark("owner-retirement");
}

TEST_CASE("named result guards: actual complete inventory cannot carry oversized token or duplicated occupied name") {
    for (const bool duplicate : {false, true}) {
        Directory directory; auto control = std::make_shared<Control>(); control->root = directory.root / "external";
        fs::create_directories(control->root); Write(control->root / "occupied.txt", "real preexisting object");
        control->duplicate_name = duplicate; control->giant_token = !duplicate;
        auto service = Open(directory, Factory(control)); REQUIRE(service->runtime() == nullptr);
        REQUIRE(service->launch_error().find("named_result.snapshot_invalid") != std::string::npos);
        REQUIRE(control->opens == 1); REQUIRE(control->writes == 0);
        REQUIRE(Read(control->root / "occupied.txt") == "real preexisting object");
    }
    Mark("inventory");
}

TEST_CASE("named result guards: an actual attached ancestor cannot open an external same-ID storage domain") {
    Directory directory;
    auto source = Open(directory); (void)Capability(*source);
    const auto source_path = source->trajectory()->v3_main_writer()->path();
    (void)source->Close("ancestor_source"); source.reset();
    const auto ledger = v3::ReadV3Ledger(source_path); REQUIRE(ledger);
    const auto last = ledger->LastEntry(); REQUIRE(last);
    const auto source_id = last->is_message ? ledger->messages[last->index].message_id : ledger->events[last->index].event_id;
    const auto source_hash = last->is_message ? ledger->messages[last->index].line_hash : ledger->events[last->index].line_hash;
    traj::ReplayState replay;
    replay.session_id = ledger->session_id; replay.run_id = ledger->run_id;
    replay.effective_conversation = traj::EffectiveConversationFromV3(*ledger, v3::ProjectModelContext(*ledger));
    auto control = std::make_shared<Control>(); control->root = directory.root / "external"; auto factory = Factory(control);
    auto target = Open(directory, factory); const auto cap = Capability(*target); const auto id = cap->scope().session_id;
    v3::EventDraft attached; attached.kind = v3::EventKindV3::ResumeSourceAttached;
    attached.payload = {{"sourceRef", {{"sessionId", ledger->session_id}, {"runId", ledger->run_id},
        {"seq", last->seq}, {"id", source_id}, {"hash", source_hash}}},
        {"replayVersion", "v3-context-chain-2"}, {"importedStateHash", traj::ComputeReplayStateHash(replay)}};
    const auto written = target->trajectory()->v3_main_writer()->AppendEvent(std::move(attached), traj::Durability::PowerLoss);
    REQUIRE(written.status == v3::WriteReceipt::Status::Committed);
    const auto path = target->trajectory()->v3_main_writer()->path();
    (void)target->Close("ancestor_target"); target.reset();
    REQUIRE(v3::ReadV3Ledger(path)); const auto bytes = Read(path); const auto opens = control->opens;
    auto refused = Open(directory, factory, id); REQUIRE(refused->runtime() == nullptr);
    REQUIRE(refused->launch_error().find("named_result.cross_session_unsupported") != std::string::npos);
    REQUIRE(control->opens == opens); REQUIRE(control->writes == 0); REQUIRE(Read(path) == bytes);
    Mark("ancestor");
}
