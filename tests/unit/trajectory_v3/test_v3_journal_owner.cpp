#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "trajectory/journal_owner.hpp"
#include "trajectory/session_lock.hpp"
#include "trajectory/session_manager.hpp"
#include "workspace/identity.hpp"
#include "platform/paths.hpp"
#include "trajectory/session_recovery_view.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace {
namespace tr = lubancode::trajectory;
namespace v3 = tr::v3;
namespace fs = std::filesystem;
using Stage = tr::JournalNativeStage;
using NativeStatus = tr::JournalAppendStatus;
using Durability = tr::Durability;
static_assert(std::is_nothrow_move_constructible_v<tr::JournalOwner>);
static_assert(std::is_nothrow_move_assignable_v<tr::JournalOwner>);
static_assert(std::is_nothrow_move_constructible_v<tr::JournalOwner::AppendLease>);
static_assert(std::is_nothrow_move_constructible_v<tr::JournalOwnerUnconfirmed>);
struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        root = fs::temp_directory_path() / ("v3-journal-owner-" +
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
struct State {
    std::array<tr::JournalNativeIoResult, 96> observations{};
    std::size_t count = 0;
    Stage inject = Stage::None;
    unsigned closes = 0, live = 0, destroyed = 0;
    bool armed = false, overflow = false;
};
class Probe final : public tr::JournalNativeIoProbe {
public:
    explicit Probe(std::shared_ptr<State> state) : state_(std::move(state)) { ++state_->live; }
    ~Probe() override { --state_->live; ++state_->destroyed; }
    bool After(const tr::JournalNativeIoResult& actual) noexcept override {
        if (state_->count < state_->observations.size()) state_->observations[state_->count++] = actual;
        else state_->overflow = true;
        if (actual.stage == Stage::Close) ++state_->closes;
        if (!state_->armed || actual.stage != state_->inject) return false;
        state_->armed = false; return true;
    }
private:
    std::shared_ptr<State> state_;
};
v3::MessageDraft User(const std::string& text = "owner-user") {
    v3::MessageDraft draft; draft.turn_id = "turn-000001";
    draft.message = {{"role", "user"}, {"content", text}}; return draft;
}
void SameNative(const tr::JournalNativeIoResult& a, const tr::JournalNativeIoResult& b) {
    CHECK(a.stage == b.stage); CHECK(a.attempted == b.attempted); CHECK(a.succeeded == b.succeeded);
    CHECK(a.requested_bytes == b.requested_bytes); CHECK(a.written_bytes == b.written_bytes);
    CHECK(a.native_return == b.native_return); CHECK(a.error_domain == b.error_domain);
    CHECK(a.native_error == b.native_error); CHECK(a.injected_unconfirmed == b.injected_unconfirmed);
}
void Same(const tr::JournalOwnerUnconfirmed& a, const tr::JournalOwnerUnconfirmed& b) {
    CHECK(a.phase == b.phase); CHECK(a.identity.id == b.identity.id); CHECK(a.identity.seq == b.identity.seq);
    CHECK(a.identity.line_hash == b.identity.line_hash); CHECK(a.identity.canonical_bytes == b.identity.canonical_bytes);
    CHECK(a.native.status == b.native.status); CHECK(a.native.line_count == b.native.line_count);
    CHECK(a.native.requested_durability == b.native.requested_durability);
    CHECK(a.native.confirmed_durability == b.native.confirmed_durability); CHECK(a.native.rejection == b.native.rejection);
    SameNative(a.native.body, b.native.body); SameNative(a.native.newline, b.native.newline);
    SameNative(a.native.flush, b.native.flush); SameNative(a.native.file_sync, b.native.file_sync);
    REQUIRE(a.native.failure.has_value() == b.native.failure.has_value());
    if (a.native.failure) SameNative(*a.native.failure, *b.native.failure);
}
void Marker(const char* name) { std::cout << "[v3-journal-owner-path] " << name << std::endl; }
}

TEST_CASE("Journal owner: native append uncertainty freezes identity before another real call") {
    Directory directory;
    for (auto stage : {Stage::BodyWrite, Stage::NewlineWrite, Stage::Flush, Stage::FileSync}) {
        auto state = std::make_shared<State>();
        const auto path = directory.root / (std::to_string(static_cast<int>(stage)) + ".jsonl");
        auto owner = tr::JournalOwner::CreateNew(path, std::make_shared<Probe>(state)); REQUIRE(owner.has_value());
        state->inject = stage; state->armed = true;
        tr::JournalAppendReceipt mechanical;
        { auto lease = owner->AppendLine("{\"owner\":true}", Durability::PowerLoss, {"row-1", 7, std::string(64, 'a'), 14});
          mechanical = lease.native(); REQUIRE(mechanical.status == NativeStatus::Unconfirmed); }
        const auto first = owner->first_unconfirmed(); REQUIRE(first); REQUIRE(first->native.failure);
        CHECK(first->phase == tr::JournalOwnerUnconfirmed::Phase::NativeAppend);
        CHECK(first->identity.id == "row-1"); CHECK(first->identity.seq == 7);
        CHECK(first->identity.canonical_bytes == 14); CHECK(first->native.failure->stage == stage);
        CHECK(first->native.failure->succeeded); CHECK(first->native.failure->injected_unconfirmed);
        REQUIRE(owner->first_unconfirmed_append()); CHECK(owner->first_unconfirmed_append()->status == NativeStatus::Unconfirmed);
        CHECK_FALSE(owner->semantic_unconfirmed()); const auto calls = state->count;
        { auto lease = owner->AppendLine("{\"different\":true}", Durability::PowerLoss, {"other-row", 8, std::string(64, 'b'), 18});
          CHECK(lease.native().status == NativeStatus::RejectedBeforeIO); CHECK(lease.native().rejection == tr::JournalBeforeIoReason::Broken); }
        CHECK(state->count == calls); Same(*owner->first_unconfirmed(), *first);
        auto read = owner->Capture(); REQUIRE(read.has_value());
        // Body/newline uncertainty precedes fflush. Observe actual captured
        // bytes; never invent visibility or call an extra flush to make a test
        // green. Reading cannot upgrade the first native uncertainty.
        CHECK(read->bytes() == Bytes(path));
        if (stage == Stage::Flush || stage == Stage::FileSync) CHECK(read->bytes() == "{\"owner\":true}\n");
        REQUIRE(read->Close().has_value()); Same(*owner->first_unconfirmed(), *first);
        const auto closed = owner->CloseDetailed(); CHECK_FALSE(closed.ok());
        CHECK(Bytes(path) == "{\"owner\":true}\n"); // Actual successful fclose flushes its stdio buffer.
        REQUIRE(closed.native); CHECK(closed.native->succeeded); CHECK(closed.native->stage == Stage::Close);
        const auto count = state->count; const auto repeated = owner->CloseDetailed();
        CHECK(repeated.status == closed.status); CHECK(repeated.broken_after == closed.broken_after);
        REQUIRE(repeated.native); SameNative(*repeated.native, *closed.native);
        CHECK(state->count == count); CHECK(state->closes == 1); Same(*owner->first_unconfirmed(), *first); CHECK_FALSE(state->overflow);
    }
    Marker("native-first");
}

TEST_CASE("Journal owner: native committed and V3 completion unconfirmed remain separate facts") {
    Directory directory;
    for (unsigned kind = 0; kind < 3; ++kind) {
        auto state = std::make_shared<State>(); bool armed = false; unsigned calls = 0;
        v3::V3WriterOptions options; options.journal_native_io_probe = std::make_shared<Probe>(state);
        options.after_native_append = [&] { ++calls; if (armed) throw std::bad_alloc{}; };
        const auto path = directory.root / ("semantic-" + std::to_string(kind) + ".jsonl");
        auto writer = v3::V3Writer::Start(path, "owner-session", "run-1", "system", {}, std::move(options)); REQUIRE(writer.has_value());
        const auto seq = writer->next_seq(); const auto hash = writer->last_line_hash(); const auto revision = writer->context().revision;
        armed = true; v3::WriteReceipt receipt;
        if (kind == 0) receipt = writer->AppendMessage(User(), Durability::PowerLoss);
        else if (kind == 1) {
            v3::MessageDraft system; system.origin = v3::MessageOrigin::SessionRuntime;
            system.message = {{"role", "system"}, {"content", "next-system"}};
            system.system_meta = {{"cause", "initial"}, {"changeEventRef", nullptr}, {"systemChanged", false}, {"hostBindings", {{"owner", "fixture"}}}};
            receipt = writer->AppendMessage(std::move(system), Durability::PowerLoss);
        } else {
            v3::EventDraft event; event.kind = v3::EventKindV3::SessionEnded;
            event.payload = {{"reason", "exit"}, {"closeQuality", "clean"}};
            receipt = writer->AppendEvent(std::move(event), Durability::PowerLoss);
        }
        REQUIRE(receipt.status == v3::WriteReceipt::Status::IoFailed); CHECK(receipt.error_code == "v3writer.completion_unconfirmed");
        REQUIRE(receipt.journal_append); CHECK(receipt.journal_append->status == NativeStatus::Committed);
        CHECK_FALSE(receipt.journal_append->failure); CHECK(receipt.journal_append->file_sync.succeeded);
        CHECK_FALSE(receipt.id.empty()); CHECK(receipt.seq == seq); CHECK(receipt.line_hash.size() == 64);
        CHECK(writer->broken()); CHECK(writer->next_seq() == seq); CHECK(writer->last_line_hash() == hash);
        CHECK(writer->context().revision == revision); CHECK_FALSE(writer->first_unconfirmed_journal_append());
        const auto first = writer->first_unconfirmed_journal_completion(); REQUIRE(first);
        CHECK(first->phase == tr::JournalOwnerUnconfirmed::Phase::SemanticCompletion); CHECK(first->native.status == NativeStatus::Committed);
        CHECK(first->identity.id == receipt.id); CHECK(first->identity.seq == receipt.seq); CHECK(first->identity.line_hash == receipt.line_hash);
        const auto count = state->count; const auto observed_calls = calls;
        const auto later = writer->AppendMessage(User("must-not-replay"), Durability::PowerLoss);
        CHECK(later.status == v3::WriteReceipt::Status::IoFailed); CHECK_FALSE(later.journal_append);
        CHECK(state->count == count); CHECK(calls == observed_calls);
        auto captured = writer->CaptureJournal(); REQUIRE(captured.has_value());
        const auto ledger = v3::ReadV3LedgerCaptured(*captured); REQUIRE(ledger.has_value()); CHECK(ledger->lines == seq);
        REQUIRE(captured->Close().has_value()); REQUIRE(captured->Close().has_value());
        Same(*writer->first_unconfirmed_journal_completion(), *first); CHECK_FALSE(writer->first_unconfirmed_journal_append());
        const auto closed = writer->Close(); REQUIRE_FALSE(closed.has_value()); CHECK(closed.error() == "v3writer.completion_unconfirmed");
        const auto closed_count = state->count; const auto again = writer->Close(); REQUIRE_FALSE(again.has_value());
        CHECK(again.error() == closed.error()); CHECK(state->count == closed_count); CHECK(state->closes == 1);
        REQUIRE(state->observations[closed_count - 1].succeeded); CHECK(state->observations[closed_count - 1].stage == Stage::Close);
        Same(*writer->first_unconfirmed_journal_completion(), *first); CHECK_FALSE(writer->first_unconfirmed_journal_append());
        REQUIRE(v3::VerifyV3File(path).ok); CHECK_FALSE(state->overflow);
    }
    Marker("semantic-first");
}

TEST_CASE("Journal owner: dropped completion lease seals committed native write and Close is once") {
    Directory directory; const auto path = directory.root / "lease.jsonl"; auto state = std::make_shared<State>();
    auto owner = tr::JournalOwner::CreateNew(path, std::make_shared<Probe>(state)); REQUIRE(owner.has_value());
    { auto lease = owner->AppendLine("real-row", Durability::PowerLoss, {"lease-row", 1, std::string(64, 'c'), 8});
      REQUIRE(lease.native().status == NativeStatus::Committed); auto moved = std::move(lease);
      CHECK(moved.native().status == NativeStatus::Committed); }
    const auto first = owner->first_unconfirmed(); REQUIRE(first); CHECK(first->phase == tr::JournalOwnerUnconfirmed::Phase::SemanticCompletion);
    CHECK(first->native.status == NativeStatus::Committed); CHECK(owner->semantic_unconfirmed()); CHECK_FALSE(owner->first_unconfirmed_append());
    const auto count = state->count;
    { auto refused = owner->AppendLine("never", Durability::PowerLoss, {"new-identity", 2, std::string(64, 'd'), 5});
      CHECK(refused.native().rejection == tr::JournalBeforeIoReason::Broken); }
    CHECK(state->count == count); Same(*owner->first_unconfirmed(), *first);
    const auto closed = owner->CloseDetailed(); REQUIRE(closed.native); CHECK(closed.ok()); CHECK(closed.native->succeeded);
    const auto close_count = state->count; REQUIRE(owner->CloseDetailed().native); CHECK(state->count == close_count);
    CHECK(state->closes == 1); Same(*owner->first_unconfirmed(), *first); CHECK(Bytes(path) == "real-row\n");
    Marker("dropped-lease");
}

TEST_CASE("Journal owner: Close uncertainty caches actual first fclose while read material survives owner") {
    Directory directory; auto state = std::make_shared<State>(); tr::JournalReadHandle captured;
    const auto path = directory.root / "close.jsonl"; std::weak_ptr<Probe> weak;
    {
        auto probe = std::make_shared<Probe>(state); weak = probe;
        auto owner = tr::JournalOwner::CreateNew(path, std::move(probe)); REQUIRE(owner.has_value());
        { auto lease = owner->AppendLine("owned-close", Durability::PowerLoss, {"close-row", 1, std::string(64, 'e'), 11});
          REQUIRE(lease.native().status == NativeStatus::Committed); lease.Complete(); }
        auto read = owner->Capture(); REQUIRE(read.has_value()); captured = std::move(*read);
        state->inject = Stage::Close; state->armed = true;
        const auto first = owner->CloseDetailed(); REQUIRE(first.native); CHECK(first.status == tr::JournalCloseReceipt::Status::Unconfirmed);
        CHECK(first.native->succeeded); CHECK(first.native->injected_unconfirmed); const auto count = state->count;
        const auto second = owner->CloseDetailed(); REQUIRE(second.native); SameNative(*second.native, *first.native);
        CHECK(state->count == count); CHECK(state->closes == 1); CHECK_FALSE(owner->first_unconfirmed());
    }
    CHECK(weak.expired()); CHECK(state->live == 0); CHECK(state->destroyed == 1); CHECK(state->closes == 1);
    fs::rename(path, directory.root / "moved.jsonl"); CHECK(captured.bytes() == "owned-close\n");
    REQUIRE(captured.Close().has_value()); REQUIRE(captured.Close().has_value()); CHECK(captured.bytes() == "owned-close\n");
    Marker("closed-reader");
}

TEST_CASE("Journal owner: captured continuation refuses changed object prefix and EOF without repair") {
    Directory directory;
    for (unsigned mode = 0; mode < 5; ++mode) {
        const auto sid = "same-id-" + std::to_string(mode); const auto folder = directory.root / sid; REQUIRE(fs::create_directory(folder));
        const auto path = folder / (sid + ".jsonl");
        auto writer = v3::V3Writer::Start(path, sid, "original-run", "original-system"); REQUIRE(writer.has_value());
        REQUIRE(writer->Close().has_value());
        auto capture = tr::CaptureSessionRecovery(folder, "workspace", sid, {}); REQUIRE(capture.has_value());
        const auto original = capture->main_journal.bytes(); REQUIRE(capture->view.Find(tr::RecoveryKeyKind::MainV3));
        CHECK(capture->view.Find(tr::RecoveryKeyKind::MainV3)->bytes == original);
        if (mode == 0) fs::remove(path);
        if (mode == 1) { fs::rename(path, folder / "previous.jsonl"); Write(path, original); }
        if (mode == 2) Write(path, original.substr(0, original.size() - 1));
        if (mode == 3) Write(path, original + "\n");
        if (mode == 4) { auto changed = original; const auto position = changed.find("original-system"); REQUIRE(position != std::string::npos);
                        changed[position] = 'X'; Write(path, changed); }
        const auto before = mode == 0 ? std::string{} : Bytes(path);
        const auto owned = v3::ReadV3LedgerCaptured(capture->main_journal); REQUIRE(owned.has_value());
        CHECK(owned->session_id == sid); CHECK(owned->run_id == "original-run"); CHECK(owned->lines == 2);
        const auto continued = v3::V3Writer::ContinueCaptured(capture->main_journal); REQUIRE_FALSE(continued.has_value());
        if (mode == 0) CHECK_FALSE(fs::exists(path));
        else CHECK(Bytes(path) == before);
        if (mode == 1) CHECK(continued.error() == "recovery.object_changed");
        if (mode == 2 || mode == 4) CHECK(continued.error() == "recovery.prefix_changed");
        if (mode == 3) CHECK(continued.error() == "recovery.prefix_extended");
        REQUIRE(capture->main_journal.Close().has_value()); REQUIRE(capture->main_journal.Close().has_value());
        const auto closed = v3::V3Writer::ContinueCaptured(capture->main_journal); REQUIRE_FALSE(closed.has_value());
        CHECK(closed.error() == "recovery.anchor_closed");
    }
    Marker("capture-fences");
}

TEST_CASE("Journal owner: same captured File bytes restore original run sequence and physical prefix") {
    Directory directory; const auto folder = directory.root / "same-id"; REQUIRE(fs::create_directory(folder));
    const auto path = folder / "same-id.jsonl";
    auto original = v3::V3Writer::Start(path, "same-id", "same-run", "same-system"); REQUIRE(original.has_value());
    const auto first = original->AppendMessage(User("saved-history"), Durability::PowerLoss); REQUIRE(first.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(original->AdmitMessages({first.id}).status == v3::WriteReceipt::Status::Committed); REQUIRE(original->Close().has_value());
    Write(path, Bytes(path) + "\n");
    auto capture = tr::CaptureSessionRecovery(folder, "workspace", "same-id", {}); REQUIRE(capture.has_value());
    const auto prefix = capture->main_journal.bytes(); const auto source = v3::ReadV3LedgerCaptured(capture->main_journal); REQUIRE(source.has_value());
    auto continued = v3::V3Writer::ContinueCaptured(capture->main_journal); REQUIRE(continued.has_value());
    REQUIRE(capture->main_journal.Close().has_value()); CHECK(continued->run_id() == "same-run"); CHECK(continued->session_id() == "same-id");
    CHECK(continued->context().revision == source->context.revision); CHECK(continued->next_seq() == source->lines + 1);
    const auto next = continued->AppendMessage(User("new-history"), Durability::PowerLoss); REQUIRE(next.status == v3::WriteReceipt::Status::Committed);
    CHECK(next.seq == source->lines + 1); CHECK(next.line_hash != first.line_hash); REQUIRE(continued->Close().has_value());
    CHECK(Bytes(path).starts_with(prefix)); const auto ledger = v3::ReadV3Ledger(path); REQUIRE(ledger.has_value());
    CHECK(ledger->session_id == source->session_id); CHECK(ledger->run_id == source->run_id); CHECK(ledger->lines == source->lines + 1);
    REQUIRE(ledger->FindMessage(first.id)); CHECK(ledger->FindMessage(first.id)->line_hash == first.line_hash);
    REQUIRE(ledger->FindMessage(next.id)); CHECK(ledger->FindMessage(next.id)->seq == next.seq);
    CHECK(capture->main_journal.bytes() == prefix); const auto still_owned = v3::ReadV3LedgerCaptured(capture->main_journal);
    REQUIRE(still_owned.has_value()); CHECK(still_owned->lines == source->lines);
    Marker("same-id");
}

TEST_CASE("Journal owner: real SessionLock contains one capture and refuses EOF drift before owned continuation") {
    Directory directory; REQUIRE(fs::create_directory(directory.root / "project"));
    tr::SessionManagerOptions options; options.workspaces_root = directory.root / "workspaces";
    options.workspace_root = directory.root / "project";
    options.identity = lubancode::workspace::MakeFallbackIdentity(options.workspace_root);
    options.launch_cwd = lubancode::platform::PathToUtf8(options.workspace_root); options.v3_system_content = "locked-owner";
    std::string sid; fs::path file;
    { tr::SessionManager original(options); auto active = original.LaunchSession(); REQUIRE(active.has_value());
      sid = (*active)->session_id(); file = (*active)->directory.v3_stream_path();
      CHECK(tr::SessionLock::Inspect(file.parent_path()).has_value());
      tr::NullClearParticipant participant; REQUIRE(original.Close({}, &participant).error_code.empty()); }
    const auto prefix = Bytes(file); unsigned captures = 0, openings = 0; bool drift = true;
    options.recovery_factory = [&](const tr::SessionRecoveryView& reference) -> std::expected<tr::SessionRecoveryView, std::string> {
        ++captures; CHECK(tr::SessionLock::Inspect(file.parent_path()).has_value());
        REQUIRE(reference.Find(tr::RecoveryKeyKind::MainV3)); CHECK(reference.Find(tr::RecoveryKeyKind::MainV3)->bytes == prefix);
        if (drift) Write(file, prefix + "\n"); return reference;
    };
    options.v3_opening_participant = [&](const tr::V3OpeningContext& context) -> std::expected<nlohmann::json, std::string> {
        ++openings; CHECK(tr::SessionLock::Inspect(context.session_dir).has_value()); REQUIRE(context.source);
        REQUIRE(context.recovery_view); REQUIRE(context.recovery_view->Find(tr::RecoveryKeyKind::MainV3));
        CHECK(context.recovery_view->Find(tr::RecoveryKeyKind::MainV3)->bytes == prefix); return nlohmann::json::object();
    };
    tr::ResumeRequest request; request.source_session_id = sid;
    { tr::SessionManager rejected(options); const auto result = rejected.ResumeAsNew(request);
      CHECK(result.error_code == "resume.step5_failed"); CHECK(result.message.find("recovery.prefix_extended") != std::string::npos);
      CHECK(rejected.active() == nullptr); }
    CHECK(captures == 1); CHECK(openings == 1); CHECK(Bytes(file) == prefix + "\n");
    CHECK_FALSE(tr::SessionLock::Inspect(file.parent_path()).has_value());
    Write(file, prefix); drift = false;
    { tr::SessionManager restored(options); const auto result = restored.ResumeAsNew(request);
      REQUIRE_MESSAGE(result.error_code.empty(), result.message); REQUIRE(restored.active()); CHECK(restored.active()->session_id() == sid);
      CHECK(tr::SessionLock::Inspect(file.parent_path()).has_value()); CHECK(Bytes(file).starts_with(prefix));
      tr::NullClearParticipant participant; REQUIRE(restored.Close({}, &participant).error_code.empty()); }
    CHECK(captures == 2); CHECK(openings == 2); CHECK(Bytes(file).starts_with(prefix));
    CHECK_FALSE(tr::SessionLock::Inspect(file.parent_path()).has_value()); Marker("locked-opening");
}
