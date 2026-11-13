#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/writer.hpp"

namespace {
namespace fs = std::filesystem;
namespace tr = lubancode::trajectory;
namespace v3 = tr::v3;
using Stage = tr::JournalNativeStage;
using Durability = tr::Durability;
using Status = v3::WriteReceipt::Status;

static_assert(std::is_trivially_copyable_v<tr::JournalAppendReceipt>);
static_assert(std::is_nothrow_move_constructible_v<v3::V3Writer>);
static_assert(std::is_nothrow_move_assignable_v<v3::V3Writer>);

class Clock final : public v3::V3Clock {
public:
    std::int64_t WallMs() const override { return 1759468800000LL; }
};
struct Directory {
    fs::path root;
    Clock clock;
    Directory() {
        static std::atomic<unsigned> serial{0};
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = fs::temp_directory_path() / ("v3-journal-receipts-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++serial));
            if (fs::create_directory(candidate)) { root = candidate; break; }
        }
        REQUIRE_FALSE(root.empty());
    }
    ~Directory() { std::error_code ec; fs::remove_all(root, ec); }
};
struct ProbeState {
    std::array<tr::JournalNativeIoResult, 128> actual{};
    std::size_t count = 0;
    unsigned closes = 0, live = 0, destroyed = 0, injected = 0;
    Stage inject = Stage::None;
    bool armed = false, overflow = false;
};
class Probe final : public tr::JournalNativeIoProbe {
public:
    explicit Probe(std::shared_ptr<ProbeState> state) : state_(std::move(state)) { ++state_->live; }
    ~Probe() override { --state_->live; ++state_->destroyed; }
    bool After(const tr::JournalNativeIoResult& actual) noexcept override {
        if (state_->count < state_->actual.size()) state_->actual[state_->count++] = actual;
        else state_->overflow = true;
        if (actual.stage == Stage::Close) ++state_->closes;
        if (!state_->armed || actual.stage != state_->inject) return false;
        state_->armed = false;
        ++state_->injected;
        return true;
    }
private:
    std::shared_ptr<ProbeState> state_;
};
v3::V3Writer Start(Directory& directory, const fs::path& path,
                   const std::shared_ptr<ProbeState>& state, v3::V3WriterOptions options = {}) {
    options.journal_native_io_probe = std::make_shared<Probe>(state);
    auto writer = v3::V3Writer::Start(path, "20260910-120000-AAAAAA", "run-000001", "system",
        nlohmann::json::object(), std::move(options), &directory.clock);
    const auto start_error = writer.has_value() ? std::string{} : writer.error();
    REQUIRE_MESSAGE(writer.has_value(), start_error);
    REQUIRE(state->count == 8); // Two actual PowerLoss startup rows, four boundaries each.
    CHECK_FALSE(writer->first_unconfirmed_journal_append());
    return std::move(*writer);
}
v3::MessageDraft User(std::string content = "actual user") {
    v3::MessageDraft draft;
    draft.turn_id = "turn-000001";
    draft.origin = v3::MessageOrigin::Human;
    draft.message = {{"role", "user"}, {"content", std::move(content)}};
    return draft;
}
v3::EventDraft Ended() {
    v3::EventDraft draft;
    draft.kind = v3::EventKindV3::SessionEnded;
    draft.payload = {{"reason", "exit"}, {"closeQuality", "clean"}};
    return draft;
}
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    REQUIRE_FALSE(input.bad());
    return bytes;
}
void ActualSuccess(const tr::JournalNativeIoResult& actual) {
    REQUIRE(actual.attempted);
    CHECK(actual.succeeded);
    CHECK(actual.error_domain == tr::JournalNativeErrorDomain::None);
    CHECK(actual.native_error == 0);
    if (actual.stage == Stage::BodyWrite || actual.stage == Stage::NewlineWrite)
        CHECK(actual.written_bytes == actual.requested_bytes);
    else if (actual.stage == Stage::FileSync) {
#ifdef _WIN32
        CHECK(actual.native_return != 0);
#else
        CHECK(actual.native_return == 0);
#endif
    } else CHECK(actual.native_return == 0);
}
void SameNative(const tr::JournalNativeIoResult& left, const tr::JournalNativeIoResult& right) {
    CHECK(left.stage == right.stage); CHECK(left.attempted == right.attempted);
    CHECK(left.succeeded == right.succeeded); CHECK(left.requested_bytes == right.requested_bytes);
    CHECK(left.written_bytes == right.written_bytes); CHECK(left.native_return == right.native_return);
    CHECK(left.error_domain == right.error_domain); CHECK(left.native_error == right.native_error);
    CHECK(left.injected_unconfirmed == right.injected_unconfirmed);
}
void SameAppend(const tr::JournalAppendReceipt& left, const tr::JournalAppendReceipt& right) {
    CHECK(left.status == right.status); CHECK(left.requested_durability == right.requested_durability);
    CHECK(left.confirmed_durability == right.confirmed_durability); CHECK(left.rejection == right.rejection);
    CHECK(left.line_count == right.line_count);
    SameNative(left.body, right.body); SameNative(left.newline, right.newline);
    SameNative(left.flush, right.flush); SameNative(left.file_sync, right.file_sync);
    REQUIRE(left.failure.has_value() == right.failure.has_value());
    if (left.failure) SameNative(*left.failure, *right.failure);
}
void BeforeIo(const v3::WriteReceipt& receipt) {
    CHECK_FALSE(receipt.journal_append);
    CHECK(receipt.id.empty()); CHECK(receipt.seq == 0); CHECK(receipt.line_hash.empty());
}
void Committed(const v3::WriteReceipt& receipt, Durability durability, std::uint64_t sequence) {
    REQUIRE(receipt.status == Status::Committed); REQUIRE(receipt.journal_append);
    CHECK_FALSE(receipt.id.empty()); CHECK(receipt.seq == sequence); CHECK(receipt.line_hash.size() == 64);
    const auto& mechanical = *receipt.journal_append;
    CHECK(mechanical.status == tr::JournalAppendStatus::Committed);
    CHECK(mechanical.requested_durability == durability);
    // The original Buffered path also does a real fflush, so it confirms ProcessCrash.
    CHECK(mechanical.confirmed_durability ==
        (durability == Durability::PowerLoss ? Durability::PowerLoss : Durability::ProcessCrash));
    CHECK(mechanical.line_count == sequence); CHECK_FALSE(mechanical.failure);
    ActualSuccess(mechanical.body); ActualSuccess(mechanical.newline); ActualSuccess(mechanical.flush);
    CHECK_FALSE(mechanical.body.injected_unconfirmed); CHECK_FALSE(mechanical.newline.injected_unconfirmed);
    CHECK_FALSE(mechanical.flush.injected_unconfirmed);
    if (durability == Durability::PowerLoss) ActualSuccess(mechanical.file_sync);
    else CHECK_FALSE(mechanical.file_sync.attempted);
}
tr::JournalAppendReceipt Gap(const v3::WriteReceipt& receipt, Stage stage) {
    REQUIRE(receipt.status == Status::Rejected); CHECK(receipt.error_code == "v3writer.io_failed");
    CHECK(receipt.id.empty()); CHECK(receipt.seq == 0); CHECK(receipt.line_hash.empty());
    REQUIRE(receipt.journal_append); const auto copy = *receipt.journal_append;
    REQUIRE(copy.status == tr::JournalAppendStatus::Unconfirmed); REQUIRE(copy.failure);
    CHECK(copy.failure->stage == stage); CHECK(copy.failure->injected_unconfirmed);
    ActualSuccess(*copy.failure); ActualSuccess(copy.body); ActualSuccess(copy.newline);
    CHECK(copy.line_count == 2); CHECK(copy.requested_durability == Durability::PowerLoss);
    return copy;
}
void Marker(std::string_view name) { std::cout << "[v3-journal-witness-path] " << name << std::endl; }
} // namespace

TEST_CASE("V3 Journal committed witnesses retain actual durability and canonical chain") {
    Directory directory;
    for (const auto durability : {Durability::Buffered, Durability::ProcessCrash, Durability::PowerLoss}) {
        const auto path = directory.root / ("committed-" + std::to_string(static_cast<int>(durability)) + ".jsonl");
        auto state = std::make_shared<ProbeState>();
        auto writer = Start(directory, path, state);
        const auto message = writer.AppendMessage(User(), durability);
        Committed(message, durability, 3); CHECK(writer.HasMessageId(message.id));
        const auto admitted = writer.AdmitMessages({message.id}, durability);
        Committed(admitted, durability, 4);
        CHECK(writer.context().revision == 2); REQUIRE(writer.context().chain.size() == 2);
        CHECK(writer.context().chain.back().message_ref == message.id);
        const auto ended = writer.AppendEvent(Ended(), durability);
        Committed(ended, durability, 5);
        CHECK(writer.next_seq() == 6); CHECK(writer.last_line_hash() == ended.line_hash);
        CHECK_FALSE(writer.first_unconfirmed_journal_append()); REQUIRE(writer.Close().has_value());
        const auto verified = v3::VerifyV3File(path); REQUIRE(verified.ok); CHECK(verified.lines == 5);
        const auto ledger = v3::ReadV3Ledger(path); REQUIRE(ledger.has_value()); CHECK(ledger->context.revision == 2);
        const auto* saved = ledger->FindMessage(message.id); REQUIRE(saved);
        CHECK(saved->seq == message.seq); CHECK(saved->line_hash == message.line_hash);
        CHECK(saved->session_id == writer.session_id()); CHECK(saved->run_id == writer.run_id());
        CHECK(state->closes == 1); CHECK_FALSE(state->overflow);
    }
    const auto default_path = directory.root / "default.jsonl";
    auto plain = v3::V3Writer::Start(default_path, "20260910-120000-AAAAAA", "run-000001", "system",
        nlohmann::json::object(), {}, &directory.clock);
    REQUIRE(plain.has_value());
    const auto plain_message = plain->AppendMessage(User(), Durability::PowerLoss);
    Committed(plain_message, Durability::PowerLoss, 3);
    Committed(plain->AdmitMessages({plain_message.id}), Durability::PowerLoss, 4);
    Committed(plain->AppendEvent(Ended(), Durability::PowerLoss), Durability::PowerLoss, 5);
    REQUIRE(plain->Close().has_value()); REQUIRE(plain->Close().has_value());
    CHECK(Bytes(default_path) == Bytes(directory.root /
        ("committed-" + std::to_string(static_cast<int>(Durability::PowerLoss)) + ".jsonl")));
    Marker("committed");
}

TEST_CASE("V3 Journal pre-IO failures have no mechanical witness or hidden retry") {
    Directory directory;
    auto state = std::make_shared<ProbeState>();
    bool inject = false;
    v3::V3WriterOptions options;
    options.inject_io_failure = [&]() -> std::optional<std::string> {
        return inject ? std::optional<std::string>("controlled") : std::nullopt;
    };
    auto writer = Start(directory, directory.root / "before.jsonl", state, std::move(options));
    const auto before = Bytes(writer.path()); const auto seq = writer.next_seq();
    const auto hash = writer.last_line_hash(); const auto count = state->count;
    auto invalid = User(); invalid.message["role"] = "invalid-role";
    const auto schema = writer.AppendMessage(std::move(invalid), Durability::PowerLoss);
    CHECK(schema.status == Status::Rejected); CHECK(schema.error_code == "schema3.bad_role"); BeforeIo(schema);
    const auto canonical = writer.AppendMessage(User(std::string(1, static_cast<char>(0xff))), Durability::PowerLoss);
    CHECK(canonical.status == Status::Rejected); CHECK(canonical.error_code == "v3writer.canonical_failed"); BeforeIo(canonical);
    CHECK_FALSE(writer.broken()); CHECK(state->count == count); CHECK(Bytes(writer.path()) == before);
    inject = true;
    const auto injected = writer.AppendMessage(User(), Durability::PowerLoss);
    CHECK(injected.status == Status::Rejected); CHECK(injected.error_code == "v3writer.injected"); BeforeIo(injected);
    CHECK(writer.broken()); CHECK_FALSE(writer.first_unconfirmed_journal_append());
    inject = false;
    const auto later = writer.AppendEvent(Ended(), Durability::PowerLoss);
    CHECK(later.status == Status::IoFailed); CHECK(later.error_code == "v3writer.broken"); BeforeIo(later);
    CHECK(writer.next_seq() == seq); CHECK(writer.last_line_hash() == hash);
    CHECK(state->count == count); CHECK(Bytes(writer.path()) == before); REQUIRE(writer.Close().has_value());

    auto closed_state = std::make_shared<ProbeState>();
    auto closed = Start(directory, directory.root / "closed.jsonl", closed_state);
    REQUIRE(closed.Close().has_value()); const auto closed_count = closed_state->count;
    const auto refused = closed.AppendMessage(User(), Durability::PowerLoss);
    CHECK(refused.status == Status::Rejected); CHECK(refused.error_code == "v3writer.closed"); BeforeIo(refused);
    CHECK(closed_state->count == closed_count); CHECK_FALSE(closed.first_unconfirmed_journal_append());

    v3::V3WriterOptions resumed_options;
    resumed_options.journal_native_io_probe = std::make_shared<Probe>(closed_state);
    const auto absent = directory.root / "does-not-exist.jsonl";
    const auto rejected = v3::V3Writer::Continue(absent, resumed_options);
    REQUIRE_FALSE(rejected.has_value()); CHECK(rejected.error() == "v3writer.native_probe_resume_unsupported");
    CHECK_FALSE(fs::exists(absent));
    auto capture = tr::JournalFileAnchor::ReadExisting(closed.path(), std::nullopt);
    REQUIRE(capture.has_value());
    const auto rejected_prefix = v3::V3Writer::ContinueOwnedPrefix(absent, "invalid-prefix", *capture->anchor, resumed_options);
    REQUIRE_FALSE(rejected_prefix.has_value()); CHECK(rejected_prefix.error() == "v3writer.native_probe_resume_unsupported");
    CHECK_FALSE(fs::exists(absent)); CHECK(closed_state->count == closed_count);
    REQUIRE(capture->anchor->Close().has_value());
    Marker("before-io");
}

TEST_CASE("V3 Journal body and newline gaps keep the first actual append witness") {
    Directory directory;
    for (const auto stage : {Stage::BodyWrite, Stage::NewlineWrite}) {
        auto state = std::make_shared<ProbeState>();
        auto writer = Start(directory, directory.root / ("append-" + std::to_string(static_cast<int>(stage)) + ".jsonl"), state);
        const auto seq = writer.next_seq(); const auto hash = writer.last_line_hash(); const auto count = state->count;
        state->inject = stage; state->armed = true;
        const auto first = Gap(writer.AppendMessage(User(), Durability::PowerLoss), stage);
        CHECK_FALSE(first.flush.attempted); CHECK_FALSE(first.file_sync.attempted); CHECK_FALSE(first.confirmed_durability);
        REQUIRE(writer.first_unconfirmed_journal_append()); SameAppend(*writer.first_unconfirmed_journal_append(), first);
        CHECK(writer.broken()); CHECK(writer.next_seq() == seq); CHECK(writer.last_line_hash() == hash);
        CHECK(writer.context().revision == 1); CHECK(state->count == count + 2); CHECK(state->injected == 1);
        for (std::size_t index = count; index < state->count; ++index) CHECK_FALSE(state->actual[index].injected_unconfirmed);
        const auto after = state->count;
        const auto refused = writer.AppendEvent(Ended(), Durability::PowerLoss);
        CHECK(refused.status == Status::IoFailed); CHECK(refused.error_code == "v3writer.broken"); BeforeIo(refused);
        CHECK(state->count == after);
        REQUIRE_FALSE(writer.Close().has_value()); CHECK(state->closes == 1);
        REQUIRE(writer.first_unconfirmed_journal_append()); SameAppend(*writer.first_unconfirmed_journal_append(), first);
        CHECK(v3::VerifyV3File(writer.path()).ok); CHECK_FALSE(state->overflow);
    }
    Marker("append-gap");
}

TEST_CASE("V3 Journal flush and sync gaps preserve native success and earlier confirmation") {
    Directory directory;
    for (const auto stage : {Stage::Flush, Stage::FileSync}) {
        auto state = std::make_shared<ProbeState>();
        auto writer = Start(directory, directory.root / ("flush-" + std::to_string(static_cast<int>(stage)) + ".jsonl"), state);
        const auto seq = writer.next_seq(); const auto hash = writer.last_line_hash(); const auto count = state->count;
        state->inject = stage; state->armed = true;
        const auto first = Gap(writer.AppendMessage(User(), Durability::PowerLoss), stage);
        ActualSuccess(first.flush);
        if (stage == Stage::Flush) {
            CHECK_FALSE(first.confirmed_durability); CHECK_FALSE(first.file_sync.attempted);
            CHECK(state->count == count + 3);
        } else {
            CHECK(first.confirmed_durability == Durability::ProcessCrash); ActualSuccess(first.file_sync);
            CHECK(state->count == count + 4);
        }
        CHECK(writer.next_seq() == seq); CHECK(writer.last_line_hash() == hash); CHECK(state->injected == 1);
        const auto verified = v3::VerifyV3File(writer.path()); REQUIRE(verified.ok); CHECK(verified.lines == 3);
        REQUIRE(writer.first_unconfirmed_journal_append()); SameAppend(*writer.first_unconfirmed_journal_append(), first);
        REQUIRE_FALSE(writer.Close().has_value()); CHECK(state->closes == 1);
        REQUIRE(writer.first_unconfirmed_journal_append()); SameAppend(*writer.first_unconfirmed_journal_append(), first);
        CHECK_FALSE(state->overflow);
    }
    Marker("flush-gap");
}

TEST_CASE("V3 Journal cached uncertainty and first checked Close never reverse their result") {
    Directory directory;
    auto state = std::make_shared<ProbeState>();
    auto writer = Start(directory, directory.root / "cache.jsonl", state);
    state->inject = Stage::FileSync; state->armed = true;
    const auto owned = Gap(writer.AppendMessage(User(), Durability::PowerLoss), Stage::FileSync);
    const auto first_close = writer.Close(); REQUIRE_FALSE(first_close.has_value());
    const auto after_close = state->count;
    const auto repeated = writer.Close(); REQUIRE_FALSE(repeated.has_value()); CHECK(repeated.error() == first_close.error());
    writer.CloseFile(); CHECK(state->count == after_close); CHECK(state->closes == 1);
    REQUIRE(v3::VerifyV3File(writer.path()).ok);
    const auto refused = writer.AppendMessage(User("never retry"), Durability::PowerLoss);
    CHECK(refused.status == Status::IoFailed); BeforeIo(refused); CHECK(state->count == after_close);
    REQUIRE(writer.first_unconfirmed_journal_append()); SameAppend(*writer.first_unconfirmed_journal_append(), owned);

    // Both directions use the real first native fclose. A callback deliberately
    // changes its next answer; the second Close must not call it or the native IO.
    for (const bool first_fails : {false, true}) {
        auto closing_state = std::make_shared<ProbeState>(); unsigned calls = 0;
        v3::V3WriterOptions options;
        options.inject_close_failure = [&]() -> std::optional<std::string> {
            ++calls;
            if ((calls == 1) == first_fails) return "controlled.first-close";
            return std::nullopt;
        };
        auto closing = Start(directory, directory.root / (first_fails ? "first-fails.jsonl" : "first-succeeds.jsonl"),
            closing_state, std::move(options));
        const auto first = closing.Close(); REQUIRE(first.has_value() == !first_fails);
        const auto count = closing_state->count;
        const auto again = closing.Close(); CHECK(again.has_value() == first.has_value());
        if (first_fails) { REQUIRE_FALSE(again.has_value()); CHECK(first.error() == "controlled.first-close"); CHECK(again.error() == first.error()); }
        closing.CloseFile(); CHECK(calls == 1); CHECK(closing_state->closes == 1); CHECK(closing_state->count == count);
        CHECK(closing.closed()); CHECK(closing.broken() == first_fails);
        CHECK_FALSE(closing.first_unconfirmed_journal_append());
        REQUIRE(count > 0); ActualSuccess(closing_state->actual[count - 1]);
        CHECK(closing_state->actual[count - 1].stage == Stage::Close); CHECK(v3::VerifyV3File(closing.path()).ok);
    }
    Marker("cache");
}

TEST_CASE("V3 Journal witnesses survive moves and owned-prefix recovery starts a fresh live cache") {
    Directory directory;
    auto source_state = std::make_shared<ProbeState>(); auto target_state = std::make_shared<ProbeState>();
    std::optional<tr::JournalAppendReceipt> owned;
    {
        auto source = Start(directory, directory.root / "source.jsonl", source_state);
        source_state->inject = Stage::Flush; source_state->armed = true;
        owned = Gap(source.AppendMessage(User(), Durability::PowerLoss), Stage::Flush);
        auto moved = std::move(source);
        CHECK_FALSE(source.first_unconfirmed_journal_append()); REQUIRE(source.Close().has_value());
        REQUIRE(moved.first_unconfirmed_journal_append()); SameAppend(*moved.first_unconfirmed_journal_append(), *owned);
        auto target = Start(directory, directory.root / "target.jsonl", target_state);
        target = std::move(moved); // Retires the other actual file before transferring ownership.
        CHECK_FALSE(moved.first_unconfirmed_journal_append()); REQUIRE(moved.Close().has_value());
        CHECK(target_state->closes == 1); CHECK(target_state->live == 0); CHECK(target_state->destroyed == 1);
        REQUIRE_FALSE(target.Close().has_value()); REQUIRE_FALSE(target.Close().has_value()); CHECK(source_state->closes == 1);
        REQUIRE(target.first_unconfirmed_journal_append()); SameAppend(*target.first_unconfirmed_journal_append(), *owned);
    }
    REQUIRE(owned); CHECK(owned->status == tr::JournalAppendStatus::Unconfirmed);
    REQUIRE(owned->failure); CHECK(owned->failure->injected_unconfirmed); ActualSuccess(*owned->failure);
    CHECK(source_state->live == 0); CHECK(source_state->destroyed == 1); CHECK(source_state->closes == 1);
    CHECK(v3::VerifyV3File(directory.root / "source.jsonl").ok);

    const auto path = directory.root / "resume.jsonl";
    auto resumed_state = std::make_shared<ProbeState>();
    auto original = Start(directory, path, resumed_state);
    const auto message = original.AppendMessage(User("saved"), Durability::PowerLoss); Committed(message, Durability::PowerLoss, 3);
    REQUIRE(original.Close().has_value()); const auto seq = original.next_seq(); const auto hash = original.last_line_hash();
    auto captured = tr::JournalFileAnchor::ReadExisting(path, std::nullopt); REQUIRE(captured.has_value());
    auto continued = v3::V3Writer::ContinueOwnedPrefix(path, captured->bytes, *captured->anchor, {}, &directory.clock);
    const auto continue_error = continued.has_value() ? std::string{} : continued.error();
    REQUIRE_MESSAGE(continued.has_value(), continue_error);
    REQUIRE(captured->anchor->Close().has_value()); CHECK_FALSE(continued->first_unconfirmed_journal_append());
    CHECK(continued->session_id() == original.session_id()); CHECK(continued->run_id() == original.run_id());
    CHECK(continued->next_seq() == seq); CHECK(continued->last_line_hash() == hash);
    const auto next = continued->AppendMessage(User("new live"), Durability::PowerLoss);
    REQUIRE(next.status == Status::Committed); REQUIRE(next.journal_append);
    CHECK(next.seq == seq); CHECK_FALSE(continued->first_unconfirmed_journal_append()); REQUIRE(continued->Close().has_value());
    CHECK(v3::VerifyV3File(path).ok); CHECK(owned->status == tr::JournalAppendStatus::Unconfirmed);
    v3::V3Writer empty; CHECK_FALSE(empty.first_unconfirmed_journal_append()); REQUIRE(empty.Close().has_value());
    CHECK_FALSE(source_state->overflow); CHECK_FALSE(target_state->overflow); CHECK_FALSE(resumed_state->overflow);
    Marker("lifetime");
}
