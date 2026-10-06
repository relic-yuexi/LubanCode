#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <lubancore/core.hpp>
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "sdk/prepare_journal.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/session_recovery_view.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancore_consumer {
std::string JournalOwnerCase(const std::string&, const std::filesystem::path&);
}
namespace {
namespace fs = std::filesystem;
namespace tr = lubancode::trajectory;
namespace v3 = tr::v3;
namespace sdk = lubancore;
namespace blob = sdk::memory_blobs::v1;
using namespace std::chrono_literals;
struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0}; root = fs::temp_directory_path() / ("sdk-journal-native-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        REQUIRE(fs::create_directory(root));
    }
    ~Directory() { std::error_code ec; fs::remove_all(root, ec); }
};
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc); REQUIRE(output.is_open());
    output << bytes; output.close(); REQUIRE_FALSE(output.fail());
}
void CheckPrepareCapture(const fs::path& base) {
    const auto folder = base / "prepare-capture"; fs::create_directory(folder);
    const auto path = folder / "prepare-source.jsonl";
    auto writer = v3::V3Writer::Start(path, "prepare-source", "original-run", "PREPARE_SYSTEM"); REQUIRE(writer);
    REQUIRE(writer->Close());
    sdk::detail::SessionPrepareJournal prepared("prepare-workspace", "prepare-source");
    REQUIRE(prepared.capture_attempts() == 0); REQUIRE_FALSE(prepared.expectation());
    const auto first = prepared.Read(path); REQUIRE(first); REQUIRE(prepared.capture_attempts() == 1);
    const auto expected = prepared.expectation(); REQUIRE(expected);
    const auto saved_bytes = Bytes(path); REQUIRE(expected->bytes == saved_bytes.size());
    const auto moved = folder / "moved.jsonl"; fs::rename(path, moved);
    const auto cached = prepared.Read(path); REQUIRE(cached); REQUIRE((*cached)->lines == (*first)->lines);
    REQUIRE((*cached)->session_id == (*first)->session_id); REQUIRE((*cached)->run_id == (*first)->run_id);
    REQUIRE(cached->get() == first->get());
    REQUIRE(prepared.capture_attempts() == 1); REQUIRE_FALSE(prepared.Read(moved));
    fs::rename(moved, path);
    auto lock = tr::SessionLock::Acquire(folder, {lubancode::platform::CurrentProcessId(), tr::CurrentProcessStartToken(), 1});
    REQUIRE(lock); REQUIRE(lock->holds());
    tr::RecoveryCaptureRequest request; request.expected_main = expected;
    auto locked = tr::CaptureSessionRecovery(folder, "prepare-workspace", "prepare-source", request); REQUIRE(locked);
    const auto* actual = locked->view.Find(tr::RecoveryKeyKind::MainV3); REQUIRE(actual); REQUIRE(actual->bytes == saved_bytes);
    REQUIRE(locked->main_journal.Close());
    for (const auto field : {"workspace", "session", "stream", "bytes", "hash"}) {
        auto wrong = request;
        if (std::string(field) == "workspace") wrong.expected_main->workspace_key = "foreign";
        if (std::string(field) == "session") wrong.expected_main->session_id = "foreign";
        if (std::string(field) == "stream") wrong.expected_main->stream = "child";
        if (std::string(field) == "bytes") ++wrong.expected_main->bytes;
        if (std::string(field) == "hash") wrong.expected_main->sha256.assign(64, '0');
        unsigned invoked = 0;
        const auto refused = tr::CaptureSessionRecovery(folder, "prepare-workspace", "prepare-source", wrong,
            [&](const tr::SessionRecoveryView& reference) -> std::expected<tr::SessionRecoveryView, std::string> {
                ++invoked; return reference;
            });
        REQUIRE_FALSE(refused); REQUIRE(refused.error() == "recovery.prepare_source_changed"); REQUIRE(invoked == 0);
    }
    auto continuation = v3::V3Writer::Continue(path); REQUIRE(continuation);
    v3::MessageDraft extra; extra.turn_id = continuation->NewTurnId();
    extra.message = {{"role", "user"}, {"content", "ACTUAL_PREPARE_DRIFT"}};
    REQUIRE(continuation->AppendMessage(std::move(extra)).status == v3::WriteReceipt::Status::Committed);
    REQUIRE(continuation->Close());
    unsigned factories = 0;
    const auto drift = tr::CaptureSessionRecovery(folder, "prepare-workspace", "prepare-source", request,
        [&](const tr::SessionRecoveryView& reference) -> std::expected<tr::SessionRecoveryView, std::string> {
            ++factories; return reference;
        });
    REQUIRE_FALSE(drift); REQUIRE(drift.error() == "recovery.prepare_source_changed"); REQUIRE(factories == 0);
    const auto old_value = prepared.Read(path); REQUIRE(old_value);
    REQUIRE((*old_value)->lines == (*first)->lines); REQUIRE(prepared.capture_attempts() == 1);
    const auto changed_bytes = Bytes(path); Write(path, "not a V3 journal\n");
    sdk::detail::SessionPrepareJournal bad("prepare-workspace", "prepare-source");
    const auto invalid = bad.Read(path); REQUIRE_FALSE(invalid); REQUIRE_FALSE(bad.expectation());
    Write(path, changed_bytes); const auto still_invalid = bad.Read(path); REQUIRE_FALSE(still_invalid);
    REQUIRE(still_invalid.error() == invalid.error()); REQUIRE(bad.capture_attempts() == 1);
    sdk::detail::SessionPrepareJournal fresh("prepare-workspace", "prepare-source");
    const auto reread = fresh.Read(path); REQUIRE(reread); REQUIRE((*reread)->lines == (*first)->lines + 1);
    const auto foreign_path = folder / "foreign-source.jsonl";
    auto foreign_writer = v3::V3Writer::Start(foreign_path, "foreign-source", "foreign-run", "FOREIGN_SYSTEM");
    REQUIRE(foreign_writer); REQUIRE(foreign_writer->Close()); Write(path, Bytes(foreign_path));
    sdk::detail::SessionPrepareJournal foreign("prepare-workspace", "prepare-source");
    const auto foreign_view = foreign.Read(path); REQUIRE(foreign_view);
    REQUIRE((*foreign_view)->session_id == "foreign-source"); REQUIRE(foreign.expectation());
    REQUIRE(foreign.expectation()->session_id == "prepare-source");
    request.expected_main = foreign.expectation();
    auto unchanged_foreign = tr::CaptureSessionRecovery(folder, "prepare-workspace", "prepare-source", request);
    REQUIRE(unchanged_foreign); // Same bytes must not become source_changed merely for their foreign ID.
    const auto foreign_ledger = v3::ReadV3LedgerCaptured(unchanged_foreign->main_journal); REQUIRE(foreign_ledger);
    REQUIRE(foreign_ledger->session_id == "foreign-source"); REQUIRE(unchanged_foreign->main_journal.Close());
    Write(path, changed_bytes);
}
struct OpeningState {
    std::atomic<unsigned> models{0}, backends{0}, opens{0}, stores{0}, providers{0}, io{0}, extension_factories{0};
    fs::path source;
    bool drift = false, drift_committed = false;
};
class PrepareBackend final : public sdk::Backend {
public:
    explicit PrepareBackend(std::shared_ptr<OpeningState> state) : state_(std::move(state)) { ++state_->backends; }
    ~PrepareBackend() override { --state_->backends; }
    sdk::Result<sdk::ModelReply> Generate(const sdk::ModelRequest&, sdk::Cancellation) override {
        ++state_->models; return sdk::ModelReply{"PREPARE_COMPLETE", {}, {}};
    }
private: std::shared_ptr<OpeningState> state_;
};
class UnusedMemory final : public blob::Store {
public:
    explicit UnusedMemory(std::shared_ptr<OpeningState> state) : state_(std::move(state)) { ++state_->stores; }
    ~UnusedMemory() override { --state_->stores; }
    blob::WriteReceipt Write(blob::WriteRequest request) override {
        ++state_->io; return {blob::CommitState::NotCommitted, std::move(request.reference), {}, {"fixture.unused_memory", {}}};
    }
    sdk::Result<std::string> Read(blob::Reference, std::size_t) override {
        ++state_->io; return std::unexpected(sdk::Error{"fixture.unused_memory", {}});
    }
private: std::shared_ptr<OpeningState> state_;
};
class OpeningMemory final : public blob::Provider {
public:
    explicit OpeningMemory(std::shared_ptr<OpeningState> state) : state_(std::move(state)) { ++state_->providers; }
    ~OpeningMemory() override { --state_->providers; }
    sdk::Result<std::unique_ptr<blob::Store>> Open(blob::Scope scope) override {
        ++state_->opens;
        if (state_->drift) {
            REQUIRE(scope.session_id == state_->source.stem().string());
            REQUIRE(tr::SessionLock::Inspect(state_->source.parent_path()));
            auto writer = v3::V3Writer::Continue(state_->source); REQUIRE(writer);
            v3::MessageDraft extra; extra.turn_id = writer->NewTurnId();
            extra.message = {{"role", "user"}, {"content", "ACTUAL_LOCKED_OPENING_DRIFT"}};
            REQUIRE(writer->AppendMessage(std::move(extra)).status == v3::WriteReceipt::Status::Committed);
            REQUIRE(writer->Close()); state_->drift_committed = true;
        }
        return std::unique_ptr<blob::Store>(std::make_unique<UnusedMemory>(state_));
    }
private: std::shared_ptr<OpeningState> state_;
};
void CheckSdkPreparePriorityAndDrift(const fs::path& base) {
    const auto root = base / "prepare-sdk";
    fs::create_directories(root / "project"); fs::create_directories(root / "resources");
    const auto utf8 = [](const fs::path& p) { return lubancode::platform::PathToUtf8(p); };
    auto runtime = sdk::Runtime::Create({utf8(root / "state"), utf8(root / "resources")}); REQUIRE(runtime);
    auto state = std::make_shared<OpeningState>();
    const auto options = [&](const std::string& sid, bool provider) {
        sdk::SessionOptions value; value.cwd = utf8(root / "project"); value.model = "prepare-fixture";
        value.backend = std::make_unique<PrepareBackend>(state); value.resume_session_id = sid;
        value.max_steps_per_turn = 4;
        if (provider) value.memory_blob_provider = std::make_unique<OpeningMemory>(state);
        return value;
    };
    auto opened = (*runtime)->OpenSession(options({}, false)); REQUIRE(opened);
    auto accepted = (*opened)->Submit("prepare-first", "first"); REQUIRE(accepted);
    auto completed = (*opened)->WaitResult(accepted->operation_id, 20s); REQUIRE(completed);
    REQUIRE(completed->state == sdk::OperationState::Succeeded); REQUIRE((*opened)->Close());
    const auto sid = (*opened)->id();
    for (const auto& entry : fs::recursive_directory_iterator(root / "state"))
        if (entry.is_regular_file() && entry.path().filename() == sid + ".jsonl") state->source = entry.path();
    REQUIRE_FALSE(state->source.empty()); const auto directory = state->source.parent_path();
    const auto source_bytes = Bytes(state->source);
    const auto jobs_path = directory / "sdk-command-jobs-plan.json";
    const auto child_path = directory / "sdk-subagent-plan.json";
    const auto skills_path = directory / "sdk-skills-plan.json";
    const auto lua_path = directory / "sdk-lua-plan.json";
    const auto jobs = Bytes(jobs_path), child = Bytes(child_path), skills = Bytes(skills_path), lua = Bytes(lua_path);
    const auto refused = [&](const char* code) {
        const auto result = (*runtime)->OpenSession(options(sid, true)); REQUIRE_FALSE(result);
        REQUIRE(result.error().code == code); REQUIRE(state->models == 1); REQUIRE(state->opens == 0);
        REQUIRE(state->backends == 0); REQUIRE(state->providers == 0); REQUIRE(state->stores == 0);
    };
    Write(state->source, "bad soft-probe source\n"); Write(jobs_path, "{}");
    refused("sdk.job.plan_invalid"); // Action's old no-Action probe remains soft.
    auto strict_options = options(sid, true);
    sdk::extensions::v1::Registration action;
    action.manifest.id = "prepare-action"; action.manifest.version = "1";
    sdk::extensions::v1::HandlerDefinition handler;
    handler.name = "prepare-action-handler"; handler.definition_hash = "prepare-action-v1";
    handler.point = sdk::extensions::v1::Point::PreAction;
    action.manifest.handlers.push_back(std::move(handler));
    action.factory = [state](const sdk::extensions::v1::SessionContext&) -> sdk::Result<std::unique_ptr<sdk::extensions::v1::Instance>> {
        ++state->extension_factories;
        return std::unexpected(sdk::Error{"fixture.unexpected_factory", {}});
    };
    strict_options.extensions.push_back(std::move(action));
    const auto strict = (*runtime)->OpenSession(std::move(strict_options)); REQUIRE_FALSE(strict);
    REQUIRE(strict.error().code == "sdk.action.plan_invalid"); REQUIRE(state->extension_factories == 0);
    REQUIRE(state->opens == 0); REQUIRE(state->backends == 0); REQUIRE(state->providers == 0);
    Write(state->source, source_bytes); Write(jobs_path, jobs);
    const auto foreign_path = root / "foreign-main.jsonl";
    auto foreign_writer = v3::V3Writer::Start(foreign_path, "foreign-session", "foreign-run", "FOREIGN_SYSTEM");
    REQUIRE(foreign_writer); REQUIRE(foreign_writer->Close());
    Write(state->source, Bytes(foreign_path)); REQUIRE(v3::ReadV3Ledger(state->source));
    const auto held_jobs = root / "held-command-jobs-plan.json"; fs::rename(jobs_path, held_jobs);
    // No-Action and legacy no-Job validation stay soft; the existing Subagent
    // scope check, not the preflight witness, rejects this valid foreign stream.
    refused("sdk.subagent.plan_invalid");
    fs::rename(held_jobs, jobs_path); Write(state->source, source_bytes);
    Write(child_path, "{}"); Write(skills_path, "{}"); refused("sdk.subagent.plan_invalid");
    Write(child_path, child); Write(lua_path, "{}"); refused("sdk.skill.plan_invalid");
    Write(skills_path, skills); refused("sdk.lua.plan_invalid"); Write(lua_path, lua);
    const auto absent = (*runtime)->OpenSession(options("missing-prepare-session", true)); REQUIRE_FALSE(absent);
    REQUIRE(absent.error().code == "sdk.session.open_failed"); REQUIRE(state->opens == 0);
    state->drift = true;
    const auto drift = (*runtime)->OpenSession(options(sid, true)); REQUIRE_FALSE(drift);
    REQUIRE(drift.error().code == "sdk.session.open_failed");
    REQUIRE(drift.error().message.find("recovery.prepare_source_changed") != std::string::npos);
    REQUIRE(state->drift_committed); REQUIRE(state->opens == 1); REQUIRE(state->models == 1);
    REQUIRE(state->backends == 0); REQUIRE(state->providers == 0); REQUIRE(state->stores == 0); REQUIRE(state->io == 0);
    REQUIRE_FALSE(tr::SessionLock::Inspect(directory));
    const auto altered = v3::ReadV3Ledger(state->source); REQUIRE(altered);
    REQUIRE(Bytes(state->source).starts_with(source_bytes)); REQUIRE(Bytes(state->source) != source_bytes);
    state->drift = false;
    auto restored = (*runtime)->OpenSession(options(sid, true)); REQUIRE(restored);
    accepted = (*restored)->Submit("prepare-second", "second"); REQUIRE(accepted);
    completed = (*restored)->WaitResult(accepted->operation_id, 20s); REQUIRE(completed);
    REQUIRE(completed->state == sdk::OperationState::Succeeded); REQUIRE(completed->result_persisted);
    REQUIRE(state->models == 2); REQUIRE((*restored)->Close()); REQUIRE((*runtime)->Shutdown());
    REQUIRE(state->backends == 0); REQUIRE(state->providers == 0); REQUIRE(state->stores == 0); REQUIRE(state->io == 0);
}
}
TEST_CASE("SDK Journal owner guards: actual installed-host path preserves canonical run chain and old prefix") {
    Directory directory; const auto path = fs::u8path(lubancore_consumer::JournalOwnerCase("close-read", directory.root));
    REQUIRE(fs::is_regular_file(path)); CHECK_FALSE(tr::SessionLock::Inspect(path.parent_path()).has_value());
    const auto ledger = v3::ReadV3Ledger(path); REQUIRE_MESSAGE(ledger.has_value(), (ledger ? "" : ledger.error()));
    const auto state = path.parent_path().parent_path().parent_path().parent_path().parent_path();
    const auto receipt_file = state.parent_path() / "journal-owner-host-receipt.txt";
    std::ifstream input(receipt_file, std::ios::binary); REQUIRE(input.is_open());
    std::map<std::string, std::string> receipt; std::string line;
    while (std::getline(input, line)) { const auto position = line.find('='); REQUIRE(position != std::string::npos);
        REQUIRE(receipt.emplace(line.substr(0, position), line.substr(position + 1)).second); }
    CHECK(receipt.at("session_id") == ledger->session_id); CHECK(receipt.at("model_calls") == "4"); CHECK(receipt.at("tool_calls") == "2");
    const auto bytes = Bytes(path); const auto before = Bytes(state.parent_path() / "journal-owner-before.jsonl");
    const auto count = std::stoull(receipt.at("prefix_bytes"));
    CHECK(before.size() == count); REQUIRE(count < bytes.size()); CHECK(bytes.size() == std::stoull(receipt.at("after_bytes")));
    CHECK(bytes.starts_with(before));
    const auto prefix_lines = tr::RecoveryStreamLines(before, std::nullopt, true); REQUIRE(prefix_lines.has_value());
    const auto prefix = v3::ReadV3LedgerOwned(path, *prefix_lines); REQUIRE(prefix.has_value()); CHECK(prefix->lines < ledger->lines);
    CHECK(prefix->session_id == ledger->session_id); CHECK(prefix->run_id == ledger->run_id);
    unsigned started = 0, ended = 0, prepared = 0; std::uint64_t expected_seq = 1;
    for (const auto& entry : ledger->timeline) {
        CHECK(entry.seq == expected_seq++);
        if (entry.is_message) {
            const auto& message = ledger->messages.at(entry.index); CHECK(message.session_id == prefix->session_id); CHECK(message.run_id == prefix->run_id);
            if (message.seq <= prefix->lines) { const auto* old = prefix->FindMessage(message.message_id); REQUIRE(old); CHECK(old->line_hash == message.line_hash); }
        } else {
            const auto& event = ledger->events.at(entry.index); CHECK(event.session_id == prefix->session_id); CHECK(event.run_id == prefix->run_id);
            if (event.seq <= prefix->lines) { const auto* old = prefix->FindEvent(event.event_id); REQUIRE(old); CHECK(old->line_hash == event.line_hash); }
            started += event.kind == v3::EventKindV3::SessionStarted; ended += event.kind == v3::EventKindV3::SessionEnded;
            if (event.kind == v3::EventKindV3::ModelRequestPrepared) { ++prepared; CHECK(v3::CheckPreparedAgainstChain(*ledger, event.event_id).empty()); }
        }
    }
    CHECK(started == 1); CHECK(ended == 2); CHECK(prepared == 4); CHECK(expected_seq == ledger->lines + 1);
    CheckPrepareCapture(directory.root);
    CheckSdkPreparePriorityAndDrift(directory.root);
    std::cout << "[sdk-journal-owner-guard] actual-public-source" << std::endl;
}
