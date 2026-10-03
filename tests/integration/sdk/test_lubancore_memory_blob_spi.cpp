#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lubancore/core.hpp>
#include <lubancore/memory_blobs.hpp>
#include <nlohmann/json.hpp>

#include "memory/frontmatter.hpp"
#include "platform/sha256.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/reader.hpp"

namespace {
namespace sdk = lubancore;
namespace blob = sdk::memory_blobs::v1;
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
using namespace std::chrono_literals;
constexpr auto kNeedle = "SDKBLOBNEEDLE";
std::string Utf8(const fs::path& path) { return lubancode::tools::PathToUtf8(path); }
void WriteFile(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open()); out << bytes; out.close(); REQUIRE_FALSE(out.fail());
}
struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        for (unsigned i = 0; i < 64; ++i) {
            auto candidate = fs::temp_directory_path() / ("sdk-memory-blob-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) { root = std::move(candidate); break; }
            if (ec) throw std::runtime_error(ec.message());
        }
        if (root.empty()) throw std::runtime_error("cannot own a fresh fixture directory");
        try {
            for (const auto* name : {"cwd", "other-2", "other-3", "resources"}) fs::create_directories(root / name);
        } catch (...) { std::error_code ec; fs::remove_all(root, ec); throw; }
    }
    ~Directory() noexcept { std::error_code ec; fs::remove_all(root, ec); }
    sdk::RuntimeOptions Roots() const { return {Utf8(root / "data"), Utf8(root / "resources")}; }
};
enum class Fault { None, Scope, Hash, Bytes, Media, State, Durability, NoConfirmation,
    WeakConfirmation, Reject, Unknown, PublishThrow, BadRead, LongRead, ReadThrow, OpenError, OpenThrow, NullStore };
struct Entity { blob::Reference reference; std::string bytes; };
std::string Key(const blob::Scope& scope, const std::string& hash) {
    return scope.workspace_key + "/" + scope.session_id + "/" + hash;
}
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, Entity> entities;
    std::vector<blob::Scope> scopes;
    std::vector<blob::WriteRequest> writes;
    std::vector<sdk::ModelRequest> requests;
    std::atomic<unsigned> opens{0}, stores{0}, reads{0}, models{0}, store_destroyed{0}, provider_destroyed{0};
    Fault fault = Fault::None; // Set only before a call or after its operation has settled.
    std::string hold;
    bool entered = false, released = false;
    std::function<void(const char*)> on_phase;
    void Phase(const char* phase) {
        if (on_phase) on_phase(phase);
        std::unique_lock lock(mutex);
        if (hold != phase) return;
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return released; });
    }
    bool Await() { std::unique_lock lock(mutex); return cv.wait_for(lock, 10s, [&] { return entered; }); }
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
    Entity OnlyEntity() {
        std::lock_guard lock(mutex); REQUIRE(entities.size() == 1); return entities.begin()->second;
    }
    sdk::ModelRequest Request(std::size_t index) {
        std::lock_guard lock(mutex); REQUIRE(index < requests.size()); return requests[index];
    }
};
struct ReleaseOnExit {
    std::shared_ptr<State> state;
    ~ReleaseOnExit() { state->Release(); }
};
class HostStore final : public blob::Store {
public:
    HostStore(std::shared_ptr<State> state, blob::Scope scope) : state_(std::move(state)), scope_(std::move(scope)) {}
    ~HostStore() override { state_->Phase("store-destroy"); ++state_->store_destroyed; }
    blob::WriteReceipt Write(blob::WriteRequest request) override {
        ++state_->stores; state_->Phase("write");
        if (request.reference.scope != scope_)
            return {blob::CommitState::NotCommitted, request.reference, {}, {"fixture.scope", "foreign scope"}};
        const auto fault = state_->fault;
        auto reference = request.reference;
        {
            std::lock_guard lock(state_->mutex);
            state_->writes.push_back(request);
            if (fault != Fault::Reject) state_->entities[Key(scope_, reference.sha256)] = {reference, request.bytes};
        }
        if (fault == Fault::PublishThrow) throw std::runtime_error("published, then threw");
        if (fault == Fault::Reject) return {blob::CommitState::NotCommitted, reference, {}, {"fixture.rejected", "not published"}};
        if (fault == Fault::Unknown) return {blob::CommitState::Indeterminate, reference, {}, {"fixture.unknown", "confirmation unavailable"}};
        if (fault == Fault::Scope) reference.scope.session_id += "-foreign";
        if (fault == Fault::Hash) reference.sha256.assign(64, '0');
        if (fault == Fault::Bytes) ++reference.bytes;
        if (fault == Fault::Media) reference.media_type = "application/octet-stream";
        auto confirmation = std::optional<blob::Durability>{request.required_durability};
        if (fault == Fault::NoConfirmation) confirmation.reset();
        if (fault == Fault::WeakConfirmation) confirmation = blob::Durability::Buffered;
        if (fault == Fault::Durability) confirmation = static_cast<blob::Durability>(88);
        return {fault == Fault::State ? static_cast<blob::CommitState>(88) : blob::CommitState::Committed,
            std::move(reference), confirmation, {}};
    }
    sdk::Result<std::string> Read(blob::Reference reference, std::size_t cap) override {
        ++state_->reads; state_->Phase("read");
        if (state_->fault == Fault::ReadThrow) throw std::runtime_error("read failed");
        if (reference.scope != scope_ || reference.bytes > cap)
            return std::unexpected(sdk::Error{"fixture.read_identity", "foreign reference or cap"});
        std::lock_guard lock(state_->mutex);
        const auto found = state_->entities.find(Key(scope_, reference.sha256));
        if (found == state_->entities.end() || found->second.reference != reference)
            return std::unexpected(sdk::Error{"fixture.missing", "no such entity"});
        auto bytes = found->second.bytes;
        if (state_->fault == Fault::BadRead) bytes[0] = bytes[0] == 'z' ? 'x' : 'z';
        if (state_->fault == Fault::LongRead) bytes.append(cap + 1, 'z');
        return bytes;
    }
private:
    std::shared_ptr<State> state_;
    blob::Scope scope_;
};
class HostProvider final : public blob::Provider {
public:
    explicit HostProvider(std::shared_ptr<State> state) : state_(std::move(state)) {}
    ~HostProvider() override { state_->Phase("provider-destroy"); ++state_->provider_destroyed; }
    sdk::Result<std::unique_ptr<blob::Store>> Open(blob::Scope scope) override {
        ++state_->opens; state_->Phase("open");
        { std::lock_guard lock(state_->mutex); state_->scopes.push_back(scope); }
        if (state_->fault == Fault::OpenThrow) throw std::runtime_error("factory rejected");
        if (state_->fault == Fault::OpenError) return std::unexpected(sdk::Error{"fixture.open", "factory rejected"});
        if (state_->fault == Fault::NullStore) return std::unique_ptr<blob::Store>{};
        return std::unique_ptr<blob::Store>(std::make_unique<HostStore>(state_, std::move(scope)));
    }
private:
    std::shared_ptr<State> state_;
};
class Backend final : public sdk::Backend {
public:
    explicit Backend(std::shared_ptr<State> state) : state_(std::move(state)) {}
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest& request, sdk::Cancellation cancellation) override {
        ++state_->models;
        { std::lock_guard lock(state_->mutex); state_->requests.push_back(request); }
        state_->Phase("model");
        if (cancellation.requested()) return std::unexpected(sdk::Error{"fixture.cancelled", "cancelled"});
        return sdk::ModelReply{"blob-answer", {}, std::nullopt};
    }
private:
    std::shared_ptr<State> state_;
};
sdk::SessionOptions Options(const Directory& paths, const std::shared_ptr<State>& state, const fs::path& cwd = {}) {
    sdk::SessionOptions options;
    options.cwd = Utf8(cwd.empty() ? paths.root / "cwd" : cwd);
    options.model = "sdk-blob-model"; options.system_prompt = "Use admitted project context.";
    options.backend = std::make_unique<Backend>(state);
    options.memory = sdk::memory::v1::RecallOptions{8192, 1};
    options.memory_blob_provider = std::make_unique<HostProvider>(state);
    return options;
}
sdk::memory::v1::Snapshot Plan(const std::shared_ptr<sdk::Session>& session) {
    auto value = session->DescribeMemory(); REQUIRE(value.has_value()); return *value;
}
fs::path SessionDirectory(const std::shared_ptr<sdk::Session>& session) {
    return lubancode::tools::Utf8ToPath(Plan(session).memory_directory).parent_path() / "sessions" / session->id();
}
void Seed(const std::shared_ptr<sdk::Session>& session, const std::string& marker = "BLOB_ACTUAL_MARKER",
    const std::string& suffix = "one", std::size_t body_bytes = 1500) {
    lubancode::memory::MemoryEntry entry;
    entry.schema = 3; entry.id = "preference.sdk-blob-" + suffix; entry.name = entry.id;
    entry.title = std::string(kNeedle) + suffix; entry.summary = entry.title;
    entry.kind = lubancode::memory::MemoryKind::Preference; entry.status = "active";
    entry.confidence = "user-stated"; entry.keywords = {entry.title};
    WriteFile(lubancode::tools::Utf8ToPath(Plan(session).memory_directory) / "preferences" / (entry.id + ".md"),
        lubancode::memory::frontmatter::BuildTopicText(entry, Json::object(), entry.title + " " + marker + " " + std::string(body_bytes, 'x')));
}
std::pair<sdk::Receipt, sdk::Operation> Run(const std::shared_ptr<sdk::Session>& session,
    const std::string& key = "blob", const std::string& query = std::string(kNeedle) + "one") {
    auto receipt = session->Submit(key, query); REQUIRE(receipt.has_value());
    auto operation = session->WaitResult(receipt->operation_id, 30s); REQUIRE(operation.has_value());
    return {*receipt, *operation};
}
sdk::memory::v1::RecallReport Report(const std::shared_ptr<sdk::Session>& session, const sdk::Receipt& receipt) {
    auto value = session->GetMemoryRecall(receipt.operation_id); REQUIRE(value.has_value());
    CHECK(value->session_id == session->id()); CHECK(value->operation_id == receipt.operation_id);
    CHECK(value->workspace_key == Plan(session).workspace_key); return *value;
}
v3::V3Ledger Ledger(const std::shared_ptr<sdk::Session>& session) {
    auto ledger = v3::ReadV3Ledger(SessionDirectory(session) / (session->id() + ".jsonl"));
    REQUIRE(ledger.has_value()); return std::move(*ledger);
}
void CheckAdopted(const std::shared_ptr<sdk::Session>& session, const std::shared_ptr<State>& state,
    const sdk::Receipt& receipt, const sdk::Operation& operation, std::size_t request = 0) {
    INFO(operation.error); CHECK(operation.state == sdk::OperationState::Succeeded); CHECK(operation.result_persisted);
    const auto report = Report(session, receipt); REQUIRE(report.state == "admitted");
    REQUIRE(report.entries.size() == 1); CHECK(report.entries.front().selected);
    const auto entity = state->OnlyEntity(); CHECK(entity.bytes.size() > 512);
    { std::lock_guard lock(state->mutex); REQUIRE(state->writes.size() == 1);
      CHECK(state->writes.front().required_durability == blob::Durability::ProcessCrash);
      CHECK(state->writes.front().bytes == entity.bytes); }
    CHECK(entity.reference.scope.session_id == session->id());
    CHECK(entity.reference.scope.workspace_key == report.workspace_key);
    CHECK(entity.reference.bytes == entity.bytes.size()); CHECK(entity.reference.media_type == "text/plain");
    CHECK(entity.reference.sha256 == lubancode::platform::Sha256Hex(entity.bytes));
    auto ledger = Ledger(session); const auto* context = ledger.FindMessage(report.context_message_id); REQUIRE(context != nullptr);
    const auto text = context->message.at("content").get<std::string>();
    CHECK(text.find(entity.bytes) != std::string::npos); CHECK(text.size() == report.bytes);
    CHECK(lubancode::platform::Sha256Hex(text) == report.context_sha256);
    const auto actual = state->Request(request);
    CHECK(std::count_if(actual.messages.begin(), actual.messages.end(), [&](const auto& message) {
        return message.role == "user" && message.text == text;
    }) == 1);
    int facts = 0, prepared = 0;
    for (const auto& event : ledger.events) if (event.turn_id == operation.turn_id) {
        if (event.kind == v3::EventKindV3::MemoryRecallInjected) {
            ++facts;
            CHECK(event.payload.at("contentSha256").get<std::string>() == entity.reference.sha256);
            CHECK(event.payload.at("snapshotRef").get<std::string>() ==
                "artifacts/sha256/" + entity.reference.sha256.substr(0, 2) + "/" + entity.reference.sha256);
            CHECK_FALSE(event.payload.contains("snapshotInline"));
            CHECK(event.payload.at("contextMessageRef").get<std::string>() == report.context_message_id);
        }
        if (event.kind == v3::EventKindV3::ModelRequestPrepared) {
            ++prepared; CHECK(v3::CheckPreparedAgainstChain(ledger, event.event_id).empty());
            const auto& refs = event.payload.at("inputMessageRefs");
            CHECK(std::count(refs.begin(), refs.end(), Json(report.context_message_id)) == 1);
        }
    }
    CHECK(facts == 1); CHECK(prepared == 1);
    CHECK_FALSE(fs::exists(SessionDirectory(session) / "artifacts" / "sha256"));
}
void Marker(const char* path) { std::cout << "[sdk-memory-blob-path] " << path << std::endl; }
} // namespace

TEST_CASE("SDK Memory blob SPI: actual owned Store Read and model adoption") {
    Directory paths; auto state = std::make_shared<State>();
    auto runtime = sdk::Runtime::Create(paths.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(paths, state)); REQUIRE(session.has_value()); Seed(*session);
    const auto [receipt, operation] = Run(*session); CheckAdopted(*session, state, receipt, operation);
    CHECK(state->stores == 1); CHECK(state->reads >= 2); CHECK(state->models == 1);
    auto duplicate = (*session)->Submit("blob", std::string(kNeedle) + "one"); REQUIRE(duplicate.has_value());
    CHECK(duplicate->duplicate); CHECK(state->stores == 1); CHECK(state->models == 1);
    REQUIRE((*session)->Close().has_value()); CHECK(state->store_destroyed == 1); CHECK(state->provider_destroyed == 1);
    CHECK(Report(*session, receipt).context_sha256.size() == 64);
    auto disabled_state = std::make_shared<State>();
    auto disabled_options = Options(paths, disabled_state); disabled_options.memory.reset();
    auto disabled = (*runtime)->OpenSession(std::move(disabled_options)); REQUIRE(disabled.has_value());
    CHECK_FALSE(Plan(*disabled).enabled);
    auto write_plan = (*disabled)->DescribeMemoryWrite(); REQUIRE(write_plan.has_value()); CHECK_FALSE(write_plan->enabled);
    const auto [disabled_receipt, disabled_operation] = Run(*disabled);
    CHECK(disabled_operation.state == sdk::OperationState::Succeeded); CHECK(disabled_operation.result_persisted);
    CHECK(Report(*disabled, disabled_receipt).state == "disabled");
    CHECK(disabled_state->stores == 0); CHECK(disabled_state->reads == 0); CHECK(disabled_state->models == 1);
    const auto request = disabled_state->Request(0);
    CHECK(std::none_of(request.tools.begin(), request.tools.end(), [](const auto& tool) { return tool.name == "memory_save"; }));
    REQUIRE((*disabled)->Close().has_value());
    auto inline_state = std::make_shared<State>();
    auto inline_session = (*runtime)->OpenSession(Options(paths, inline_state)); REQUIRE(inline_session.has_value());
    Seed(*inline_session, "BLOB_INLINE_MARKER", "inline", 8);
    const auto [inline_receipt, inline_operation] = Run(*inline_session, "inline", std::string(kNeedle) + "inline");
    CHECK(inline_operation.state == sdk::OperationState::Succeeded); CHECK(inline_operation.result_persisted);
    const auto inline_report = Report(*inline_session, inline_receipt); REQUIRE(inline_report.state == "admitted");
    REQUIRE(inline_report.entries.size() == 1); CHECK(inline_report.entries.front().bytes <= 512);
    CHECK(inline_state->stores == 0); CHECK(inline_state->reads == 0); CHECK(inline_state->models == 1);
    const auto inline_ledger = Ledger(*inline_session);
    unsigned inline_facts = 0;
    for (const auto& event : inline_ledger.events) if (event.kind == v3::EventKindV3::MemoryRecallInjected) {
        ++inline_facts; CHECK(event.turn_id == inline_operation.turn_id);
        CHECK_FALSE(event.payload.contains("snapshotRef"));
        REQUIRE(event.payload.at("snapshotInline").is_string());
        CHECK(event.payload.at("snapshotInline").get<std::string>().find("BLOB_INLINE_MARKER") != std::string::npos);
    }
    CHECK(inline_facts == 1); REQUIRE((*inline_session)->Close().has_value());
    REQUIRE((*runtime)->Shutdown().has_value()); Marker("actual");
}

TEST_CASE("SDK Memory blob SPI: same ID resume verifies without historical Store") {
    Directory paths; auto state = std::make_shared<State>();
    auto runtime = sdk::Runtime::Create(paths.Roots()); REQUIRE(runtime.has_value());
    auto session = (*runtime)->OpenSession(Options(paths, state)); REQUIRE(session.has_value()); Seed(*session);
    const auto [receipt, operation] = Run(*session); CheckAdopted(*session, state, receipt, operation);
    const auto report = Report(*session, receipt); const auto id = (*session)->id();
    REQUIRE((*session)->Close().has_value()); const auto reads = state->reads.load();
    auto options = Options(paths, state); options.resume_session_id = id; options.memory.reset(); options.system_prompt.clear();
    auto resumed = (*runtime)->OpenSession(std::move(options)); REQUIRE(resumed.has_value());
    CHECK((*resumed)->id() == id); CHECK(state->stores == 1); CHECK(state->reads > reads);
    const auto restored = Report(*resumed, receipt);
    CHECK(restored.context_message_id == report.context_message_id); CHECK(restored.context_sha256 == report.context_sha256);
    const auto [next_receipt, next] = Run(*resumed, "next", "continue the existing context");
    CHECK(next.state == sdk::OperationState::Succeeded); CHECK(next.result_persisted); CHECK(state->stores == 1);
    const auto request = state->Request(1);
    CHECK(std::any_of(request.messages.begin(), request.messages.end(), [](const auto& message) {
        return message.text.find("BLOB_ACTUAL_MARKER") != std::string::npos;
    }));
    REQUIRE((*resumed)->Close().has_value()); CHECK(state->store_destroyed == 2); CHECK(state->provider_destroyed == 2);
    REQUIRE((*runtime)->Shutdown().has_value()); Marker("resume");
}

TEST_CASE("SDK Memory blob SPI: receipt identity durability unknown and wrong Read stop adoption") {
    for (const auto fault : {Fault::Scope, Fault::Hash, Fault::Bytes, Fault::Media, Fault::State, Fault::Durability,
        Fault::NoConfirmation, Fault::WeakConfirmation, Fault::Reject, Fault::Unknown, Fault::PublishThrow,
        Fault::BadRead, Fault::LongRead, Fault::ReadThrow}) {
        INFO(static_cast<int>(fault)); Directory paths; auto state = std::make_shared<State>(); state->fault = fault;
        auto runtime = sdk::Runtime::Create(paths.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(paths, state)); REQUIRE(session.has_value()); Seed(*session);
        const auto [receipt, operation] = Run(*session);
        INFO(operation.error); CHECK(operation.state == sdk::OperationState::Indeterminate); CHECK_FALSE(operation.result_persisted);
        CHECK(state->models == 0); CHECK(state->stores == 1);
        const auto report = Report(*session, receipt); CHECK(report.state == "failed");
        CHECK(report.error.find("memory.recall.snapshot_failed") != std::string::npos); CHECK(report.context_message_id.empty());
        const auto expected = fault == Fault::NoConfirmation || fault == Fault::WeakConfirmation ? "cas.durability_unconfirmed" :
            fault == Fault::Reject ? "fixture.rejected" : fault == Fault::Unknown ? "fixture.unknown" :
            fault == Fault::PublishThrow || fault == Fault::ReadThrow ? "cas.provider_exception" :
            fault == Fault::BadRead || fault == Fault::LongRead ? "cas.read_mismatch" : "cas.receipt_mismatch";
        CHECK(report.error.find(expected) != std::string::npos);
        auto ledger = Ledger(*session);
        CHECK(std::none_of(ledger.events.begin(), ledger.events.end(), [&](const auto& event) {
            return event.turn_id == operation.turn_id && (event.kind == v3::EventKindV3::MemoryRecallInjected ||
                event.kind == v3::EventKindV3::ModelRequestPrepared);
        }));
        CHECK_FALSE(fs::exists(SessionDirectory(*session) / "artifacts" / "sha256"));
        // Unknown publication remains visible in the host store; no retry/rollback.
        if (fault == Fault::PublishThrow) CHECK(state->OnlyEntity().bytes.size() > 512);
        (void)(*session)->Close(); (void)(*runtime)->Shutdown();
        CHECK(state->stores == 1); CHECK(state->store_destroyed == 1); CHECK(state->provider_destroyed == 1);
    }
    Marker("receipt");
}

TEST_CASE("SDK Memory blob SPI: four actual Sessions isolate same and different projects") {
    Directory paths; std::array<std::shared_ptr<State>, 4> states;
    for (auto& state : states) { state = std::make_shared<State>(); state->hold = "model"; }
    auto runtime = sdk::Runtime::Create(paths.Roots()); REQUIRE(runtime.has_value());
    std::array<std::shared_ptr<sdk::Session>, 4> sessions;
    const std::array<fs::path, 4> cwds{paths.root / "cwd", paths.root / "cwd", paths.root / "other-2", paths.root / "other-3"};
    std::array<sdk::Receipt, 4> receipts;
    // Release all live callbacks before Session destruction on any failed check.
    struct ReleaseAll { std::array<std::shared_ptr<State>, 4>& states; ~ReleaseAll() { for (const auto& s : states) s->Release(); } } release{states};
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        auto session = (*runtime)->OpenSession(Options(paths, states[i], cwds[i])); REQUIRE(session.has_value()); sessions[i] = *session;
        Seed(sessions[i], "BLOB_SCENE_" + std::to_string(i), std::to_string(i));
    }
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        auto value = sessions[i]->Submit("scene", std::string(kNeedle) + std::to_string(i)); REQUIRE(value.has_value()); receipts[i] = *value;
    }
    for (const auto& state : states) REQUIRE(state->Await());
    for (const auto& state : states) { CHECK(state->models == 1); CHECK(state->store_destroyed == 0); state->Release(); }
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        auto operation = sessions[i]->WaitResult(receipts[i].operation_id, 30s); REQUIRE(operation.has_value());
        CheckAdopted(sessions[i], states[i], receipts[i], *operation);
        const auto entity = states[i]->OnlyEntity();
        CHECK(entity.bytes.find("BLOB_SCENE_" + std::to_string(i)) != std::string::npos);
        for (std::size_t other = 0; other < sessions.size(); ++other) if (i != other) {
            CHECK(entity.bytes.find("BLOB_SCENE_" + std::to_string(other)) == std::string::npos);
            CHECK(entity.reference.scope != states[other]->OnlyEntity().reference.scope);
        }
    }
    CHECK(Plan(sessions[0]).workspace_key == Plan(sessions[1]).workspace_key);
    CHECK(Plan(sessions[0]).workspace_key != Plan(sessions[2]).workspace_key);
    REQUIRE(sessions[0]->Close().has_value()); CHECK(states[0]->provider_destroyed == 1); CHECK(states[1]->provider_destroyed == 0);
    REQUIRE((*runtime)->Shutdown().has_value());
    for (const auto& state : states) { CHECK(state->store_destroyed == 1); CHECK(state->provider_destroyed == 1); }
    Marker("isolation");
}

TEST_CASE("SDK Memory blob SPI: Close waits for the actual borrowed Store and Read") {
    for (const auto* held : {"write", "read"}) {
        Directory paths; auto state = std::make_shared<State>(); state->hold = held;
        auto runtime = sdk::Runtime::Create(paths.Roots()); REQUIRE(runtime.has_value());
        auto session = (*runtime)->OpenSession(Options(paths, state)); REQUIRE(session.has_value()); Seed(*session);
        std::future<sdk::Result<void>> closer;
        ReleaseOnExit release{state};
        auto submitted = (*session)->Submit("held", std::string(kNeedle) + "one"); REQUIRE(submitted.has_value());
        REQUIRE(state->Await());
        closer = std::async(std::launch::async, [session = *session] { return session->Close(); });
        CHECK(closer.wait_for(20ms) == std::future_status::timeout);
        CHECK(state->store_destroyed == 0); CHECK(state->provider_destroyed == 0); CHECK(state->models == 0);
        state->Release(); REQUIRE(closer.wait_for(10s) == std::future_status::ready); REQUIRE(closer.get().has_value());
        CHECK(state->store_destroyed == 1); CHECK(state->provider_destroyed == 1); CHECK(state->stores == 1);
        auto operation = (*session)->ReadOperation(submitted->operation_id); REQUIRE(operation.has_value());
        CHECK(operation->state == sdk::OperationState::Cancelled); CHECK(operation->result_persisted);
        CHECK(state->models == 0); REQUIRE((*runtime)->Shutdown().has_value());
    }
    Marker("drain");
}

TEST_CASE("SDK Memory blob SPI: opening failures retire captures and lifecycle reentry is rejected") {
    for (const auto fault : {Fault::OpenError, Fault::OpenThrow, Fault::NullStore}) {
        Directory paths; auto state = std::make_shared<State>(); state->fault = fault;
        auto runtime = sdk::Runtime::Create(paths.Roots()); REQUIRE(runtime.has_value());
        auto failed = (*runtime)->OpenSession(Options(paths, state)); REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().message.find(fault == Fault::OpenError ? "fixture.open" : "cas.open_failed") != std::string::npos);
        CHECK(state->opens == 1); CHECK(state->stores == 0); CHECK(state->models == 0); CHECK(state->provider_destroyed == 1);
        REQUIRE((*runtime)->Shutdown().has_value());
    }
    {
        Directory paths; auto state = std::make_shared<State>(); state->hold = "open"; state->fault = Fault::OpenError;
        auto runtime = sdk::Runtime::Create(paths.Roots()); REQUIRE(runtime.has_value());
        std::future<sdk::Result<std::shared_ptr<sdk::Session>>> opener;
        std::future<sdk::Result<void>> closer;
        ReleaseOnExit release{state};
        opener = std::async(std::launch::async, [&paths, state, owner = runtime->get()] {
            return owner->OpenSession(Options(paths, state));
        });
        REQUIRE(state->Await());
        closer = std::async(std::launch::async, [owner = runtime->get()] { return owner->Shutdown(); });
        CHECK(closer.wait_for(20ms) == std::future_status::timeout);
        CHECK(state->provider_destroyed == 0); CHECK(state->stores == 0); CHECK(state->models == 0);
        state->Release(); REQUIRE(opener.wait_for(10s) == std::future_status::ready); REQUIRE_FALSE(opener.get().has_value());
        REQUIRE(closer.wait_for(10s) == std::future_status::ready); REQUIRE(closer.get().has_value());
        CHECK(state->opens == 1); CHECK(state->provider_destroyed == 1); CHECK(state->store_destroyed == 0);
    }
    {
        Directory paths; auto state = std::make_shared<State>(); auto peer_state = std::make_shared<State>();
        std::atomic<unsigned> checks{0}, rejected{0};
        auto runtime = sdk::Runtime::Create(paths.Roots()); REQUIRE(runtime.has_value());
        auto peer_options = Options(paths, peer_state); peer_options.memory.reset(); peer_options.memory_blob_provider.reset();
        auto peer = (*runtime)->OpenSession(std::move(peer_options)); REQUIRE(peer.has_value());
        std::weak_ptr<sdk::Session> weak_peer = *peer;
        state->on_phase = [owner = runtime->get(), weak_peer, &checks, &rejected](const char*) {
            auto target = weak_peer.lock(); if (!target) return;
            ++checks;
            const auto shutdown = owner->Shutdown(); const auto close = target->Close();
            const auto wait = target->WaitResult("absent", 1ms); const auto open = owner->OpenSession({});
            if (!shutdown && !close && !wait && !open && shutdown.error().code == "sdk.lifecycle.reentrant" &&
                close.error().code == "sdk.lifecycle.reentrant" && wait.error().code == "sdk.lifecycle.reentrant" &&
                open.error().code == "sdk.lifecycle.reentrant") ++rejected;
        };
        auto session = (*runtime)->OpenSession(Options(paths, state)); REQUIRE(session.has_value()); Seed(*session);
        const auto [receipt, operation] = Run(*session); CheckAdopted(*session, state, receipt, operation);
        const auto id = (*session)->id(); REQUIRE((*session)->Close().has_value());
        CHECK(checks >= 6); CHECK(rejected == checks);
        // The factory succeeds, then the actual locked opening rejects an old
        // fragment's wrong bytes. Both Store and provider must retire on failure.
        state->fault = Fault::BadRead;
        auto options = Options(paths, state); options.resume_session_id = id; options.memory.reset(); options.system_prompt.clear();
        auto failed = (*runtime)->OpenSession(std::move(options)); REQUIRE_FALSE(failed.has_value());
        CHECK(state->stores == 1); CHECK(state->models == 1); CHECK(state->store_destroyed == 2); CHECK(state->provider_destroyed == 2);
        CHECK(rejected == checks); REQUIRE((*peer)->Close().has_value()); REQUIRE((*runtime)->Shutdown().has_value());
    }
    Marker("opening");
}
