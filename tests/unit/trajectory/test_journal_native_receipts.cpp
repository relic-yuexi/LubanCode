#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

#include "trajectory/journal.hpp"

namespace {
namespace fs = std::filesystem;
namespace tr = lubancode::trajectory;
using Stage = tr::JournalNativeStage;
using AppendStatus = tr::JournalAppendStatus;
using CloseStatus = tr::JournalCloseReceipt::Status;
using Reason = tr::JournalBeforeIoReason;
using Durability = tr::Durability;

static_assert(std::is_trivially_copyable_v<tr::JournalNativeIoResult>);
static_assert(std::is_trivially_copyable_v<tr::JournalAppendReceipt>);
static_assert(std::is_trivially_copyable_v<tr::JournalCloseReceipt>);
static_assert(std::is_nothrow_move_constructible_v<tr::JournalWriter>);
static_assert(std::is_nothrow_move_assignable_v<tr::JournalWriter>);
static_assert(noexcept(std::declval<tr::JournalWriter&>().AppendLineDetailed({}, Durability::Buffered)));
static_assert(noexcept(std::declval<tr::JournalWriter&>().CloseDetailed()));

struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0};
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = fs::temp_directory_path() / ("journal-native-receipts-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++serial));
            if (fs::create_directory(candidate)) { root = candidate; break; }
        }
        REQUIRE_FALSE(root.empty());
    }
    ~Directory() { std::error_code ec; fs::remove_all(root, ec); }
};

std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    REQUIRE_FALSE(input.bad());
    return bytes;
}

struct ProbeState {
    std::array<tr::JournalNativeIoResult, 32> actual{};
    std::size_t count = 0;
    Stage inject = Stage::None;
    unsigned matching_boundaries = 0;
    unsigned live = 0, destroyed = 0;
    bool overflow = false;
};

class Probe final : public tr::JournalNativeIoProbe {
public:
    explicit Probe(std::shared_ptr<ProbeState> state) : state_(std::move(state)) { ++state_->live; }
    ~Probe() override { --state_->live; ++state_->destroyed; }
    bool After(const tr::JournalNativeIoResult& actual) noexcept override {
        if (state_->count < state_->actual.size()) state_->actual[state_->count++] = actual;
        else state_->overflow = true;
        if (actual.stage != state_->inject) return false;
        return ++state_->matching_boundaries == 1;
    }
private:
    std::shared_ptr<ProbeState> state_;
};

tr::JournalWriter Open(const fs::path& path, const std::shared_ptr<ProbeState>& state) {
    auto writer = tr::JournalWriter::OpenWithNativeIoProbe(
        path, tr::JournalWriter::OpenMode::CreateNew, std::make_shared<Probe>(state));
    const auto error = writer.has_value() ? std::string() : writer.error();
    REQUIRE_MESSAGE(writer.has_value(), error);
    return std::move(*writer);
}

void ActualSuccess(const tr::JournalNativeIoResult& actual) {
    REQUIRE(actual.attempted);
    CHECK(actual.succeeded);
    CHECK(actual.error_domain == tr::JournalNativeErrorDomain::None);
    CHECK(actual.native_error == 0);
    if (actual.stage == Stage::BodyWrite || actual.stage == Stage::NewlineWrite) {
        CHECK(actual.written_bytes == actual.requested_bytes);
    } else if (actual.stage == Stage::FileSync) {
#ifdef _WIN32
        CHECK(actual.native_return != 0);
#else
        CHECK(actual.native_return == 0);
#endif
    } else {
        CHECK(actual.native_return == 0);
    }
}

void NoIo(const tr::JournalAppendReceipt& receipt) {
    CHECK(receipt.status == AppendStatus::RejectedBeforeIO);
    CHECK_FALSE(receipt.body.attempted); CHECK_FALSE(receipt.newline.attempted);
    CHECK_FALSE(receipt.flush.attempted); CHECK_FALSE(receipt.file_sync.attempted);
    CHECK_FALSE(receipt.confirmed_durability); CHECK_FALSE(receipt.failure);
}

void SameNative(const tr::JournalNativeIoResult& actual, const tr::JournalNativeIoResult& first) {
    CHECK(actual.stage == first.stage); CHECK(actual.attempted == first.attempted);
    CHECK(actual.succeeded == first.succeeded); CHECK(actual.requested_bytes == first.requested_bytes);
    CHECK(actual.written_bytes == first.written_bytes); CHECK(actual.native_return == first.native_return);
    CHECK(actual.error_domain == first.error_domain); CHECK(actual.native_error == first.native_error);
    CHECK(actual.injected_unconfirmed == first.injected_unconfirmed);
}

void SameAppend(const tr::JournalAppendReceipt& actual, const tr::JournalAppendReceipt& first) {
    CHECK(actual.status == first.status); CHECK(actual.requested_durability == first.requested_durability);
    CHECK(actual.confirmed_durability == first.confirmed_durability); CHECK(actual.rejection == first.rejection);
    CHECK(actual.line_count == first.line_count);
    SameNative(actual.body, first.body); SameNative(actual.newline, first.newline);
    SameNative(actual.flush, first.flush); SameNative(actual.file_sync, first.file_sync);
    REQUIRE(actual.failure.has_value() == first.failure.has_value());
    if (actual.failure) SameNative(*actual.failure, *first.failure);
}

void SameClose(const tr::JournalCloseReceipt& actual, const tr::JournalCloseReceipt& first) {
    CHECK(actual.status == first.status); CHECK(actual.broken_before == first.broken_before);
    CHECK(actual.broken_after == first.broken_after); CHECK(actual.ok() == first.ok());
    REQUIRE(actual.native.has_value() == first.native.has_value());
    if (actual.native) SameNative(*actual.native, *first.native);
}

void Marker(const char* path) { std::cout << "[journal-native-receipt-path] " << path << std::endl; }
} // namespace

TEST_CASE("Journal detailed commit preserves actual durability and default bool bytes") {
    Directory directory;
    for (const auto durability : {Durability::Buffered, Durability::ProcessCrash, Durability::PowerLoss}) {
        const auto suffix = std::to_string(static_cast<int>(durability));
        const auto path = directory.root / ("detailed-" + suffix + ".jsonl");
        auto state = std::make_shared<ProbeState>();
        auto writer = Open(path, state);
        const std::string text = "{\"text\":\"owned\"}";
        const auto receipt = writer.AppendLineDetailed(text, durability);
        REQUIRE(receipt.status == AppendStatus::Committed);
        CHECK(receipt.requested_durability == durability);
        CHECK(receipt.confirmed_durability == (durability == Durability::PowerLoss
            ? Durability::PowerLoss : Durability::ProcessCrash));
        CHECK(receipt.rejection == Reason::None); CHECK_FALSE(receipt.failure);
        CHECK(receipt.line_count == 1); CHECK(writer.line_count() == 1); CHECK_FALSE(writer.broken());
        ActualSuccess(receipt.body); ActualSuccess(receipt.newline); ActualSuccess(receipt.flush);
        CHECK(receipt.body.requested_bytes == text.size()); CHECK(receipt.newline.requested_bytes == 1);
        CHECK_FALSE(receipt.body.injected_unconfirmed); CHECK_FALSE(receipt.newline.injected_unconfirmed);
        CHECK_FALSE(receipt.flush.injected_unconfirmed);
        REQUIRE(state->count == (durability == Durability::PowerLoss ? 4 : 3));
        CHECK(state->actual[0].stage == Stage::BodyWrite); CHECK(state->actual[1].stage == Stage::NewlineWrite);
        CHECK(state->actual[2].stage == Stage::Flush);
        if (durability == Durability::PowerLoss) {
            ActualSuccess(receipt.file_sync); CHECK_FALSE(receipt.file_sync.injected_unconfirmed);
            CHECK(state->actual[3].stage == Stage::FileSync);
        } else CHECK_FALSE(receipt.file_sync.attempted);
        CHECK(Bytes(path) == text + "\n");
        const auto closed = writer.CloseDetailed(); REQUIRE(closed.native); ActualSuccess(*closed.native);
        CHECK(closed.status == CloseStatus::Closed); CHECK(closed.ok()); CHECK_FALSE(closed.broken_before);
        CHECK_FALSE(writer.first_unconfirmed_append()); CHECK_FALSE(state->overflow);
        auto legacy = tr::JournalWriter::Open(directory.root / ("legacy-" + suffix + ".jsonl"),
                                              tr::JournalWriter::OpenMode::CreateNew);
        REQUIRE(legacy); CHECK(legacy->AppendLine(text, durability)); CHECK(legacy->line_count() == 1);
        CHECK(legacy->Close()); CHECK(Bytes(legacy->path()) == Bytes(path));
    }
    Marker("committed");
}

TEST_CASE("Journal before IO refusal keeps default close and zero native calls") {
    Directory directory; tr::JournalWriter empty;
    const auto unopened = empty.AppendLineDetailed("not-open", Durability::PowerLoss);
    NoIo(unopened); CHECK(unopened.rejection == Reason::Closed); CHECK(unopened.line_count == 0);
    const auto default_close = empty.CloseDetailed(); CHECK(default_close.status == CloseStatus::NoOpenHandle);
    CHECK_FALSE(default_close.native); CHECK(default_close.ok()); CHECK(empty.Close());
    SameClose(empty.CloseDetailed(), default_close);
    auto state = std::make_shared<ProbeState>(); auto writer = Open(directory.root / "empty.jsonl", state);
    const auto rejected = writer.AppendLineDetailed({}, Durability::Buffered);
    NoIo(rejected); CHECK(rejected.rejection == Reason::EmptyLine); CHECK_FALSE(writer.broken());
    CHECK_FALSE(writer.AppendLine({}, Durability::ProcessCrash)); CHECK(state->count == 0);
    CHECK(Bytes(writer.path()).empty()); CHECK_FALSE(writer.first_unconfirmed_append());
    CHECK(writer.Close()); REQUIRE(state->count == 1); CHECK(state->actual[0].stage == Stage::Close);
    const auto closed = writer.AppendLineDetailed("after-close", Durability::ProcessCrash);
    NoIo(closed); CHECK(closed.rejection == Reason::Closed); CHECK(state->count == 1);
    CHECK_FALSE(writer.AppendLine("after-close", Durability::PowerLoss)); CHECK(state->count == 1);
    Marker("before-io");
}

TEST_CASE("Journal controlled write boundary gaps preserve first native witnesses") {
    Directory directory;
    for (const auto stage : {Stage::BodyWrite, Stage::NewlineWrite}) for (int bool_entry = 0; bool_entry < 2; ++bool_entry) {
        auto state = std::make_shared<ProbeState>(); state->inject = stage;
        const auto path = directory.root / ("write-gap-" + std::to_string(static_cast<int>(stage)) +
                                           "-" + std::to_string(bool_entry) + ".jsonl");
        auto writer = Open(path, state);
        tr::JournalAppendReceipt first;
        if (bool_entry) {
            CHECK_FALSE(writer.AppendLine("original", Durability::PowerLoss));
            REQUIRE(writer.first_unconfirmed_append()); first = *writer.first_unconfirmed_append();
        } else first = writer.AppendLineDetailed("original", Durability::PowerLoss);
        REQUIRE(first.status == AppendStatus::Unconfirmed); REQUIRE(first.failure);
        CHECK(first.failure->stage == stage); CHECK(first.failure->injected_unconfirmed);
        // Real fwrite returned its actual full byte count. The independent
        // controlled uncertainty is not an alleged OS error or short write.
        ActualSuccess(*first.failure); ActualSuccess(first.body); ActualSuccess(first.newline);
        CHECK_FALSE(first.flush.attempted); CHECK_FALSE(first.file_sync.attempted);
        CHECK_FALSE(first.confirmed_durability); CHECK(first.line_count == 0);
        CHECK(writer.broken()); CHECK(writer.line_count() == 0);
        REQUIRE(state->count == 2); CHECK(state->matching_boundaries == 1);
        CHECK(state->actual[0].stage == Stage::BodyWrite); CHECK(state->actual[1].stage == Stage::NewlineWrite);
        CHECK_FALSE(state->actual[0].injected_unconfirmed); CHECK_FALSE(state->actual[1].injected_unconfirmed);
        const auto later = writer.AppendLineDetailed("never-retry", Durability::Buffered);
        NoIo(later); CHECK(later.rejection == Reason::Broken); CHECK(state->count == 2);
        CHECK_FALSE(writer.AppendLine("never-retry", Durability::ProcessCrash)); CHECK(state->count == 2);
        REQUIRE(writer.first_unconfirmed_append()); SameAppend(*writer.first_unconfirmed_append(), first);
        const auto closed = writer.CloseDetailed(); REQUIRE(closed.native); ActualSuccess(*closed.native);
        CHECK(closed.status == CloseStatus::Closed); CHECK(closed.broken_before); CHECK(closed.broken_after);
        CHECK_FALSE(closed.ok()); CHECK_FALSE(writer.Close()); CHECK(state->count == 3);
        CHECK(Bytes(path) == "original\n");
        SameAppend(*writer.first_unconfirmed_append(), first); CHECK_FALSE(state->overflow);
    }
    Marker("append-gap");
}

TEST_CASE("Journal controlled flush gaps retain only the earlier confirmation") {
    Directory directory;
    for (const auto stage : {Stage::Flush, Stage::FileSync}) {
        auto state = std::make_shared<ProbeState>(); state->inject = stage;
        auto writer = Open(directory.root / ("flush-gap-" + std::to_string(static_cast<int>(stage)) + ".jsonl"), state);
        const auto first = writer.AppendLineDetailed("flushed", Durability::PowerLoss);
        REQUIRE(first.status == AppendStatus::Unconfirmed); REQUIRE(first.failure);
        CHECK(first.failure->stage == stage); CHECK(first.failure->injected_unconfirmed);
        ActualSuccess(*first.failure); ActualSuccess(first.body); ActualSuccess(first.newline); ActualSuccess(first.flush);
        if (stage == Stage::Flush) {
            CHECK_FALSE(first.confirmed_durability); CHECK_FALSE(first.file_sync.attempted); CHECK(state->count == 3);
        } else {
            CHECK(first.confirmed_durability == Durability::ProcessCrash);
            ActualSuccess(first.file_sync); CHECK(state->count == 4);
        }
        CHECK(state->matching_boundaries == 1); CHECK(writer.broken()); CHECK(writer.line_count() == 0);
        CHECK(first.line_count == 0); CHECK(Bytes(writer.path()) == "flushed\n");
        const auto before = state->count;
        NoIo(writer.AppendLineDetailed("not-repeated", Durability::PowerLoss)); CHECK(state->count == before);
        REQUIRE(writer.first_unconfirmed_append()); SameAppend(*writer.first_unconfirmed_append(), first);
        CHECK_FALSE(writer.Close()); CHECK(state->count == before + 1);
        SameAppend(*writer.first_unconfirmed_append(), first); CHECK(Bytes(writer.path()) == "flushed\n");
        CHECK_FALSE(state->overflow);
    }
    Marker("flush-gap");
}

TEST_CASE("Journal first close result is separate from append and never repeated") {
    Directory directory;
    for (int inject = 0; inject < 2; ++inject) {
        auto state = std::make_shared<ProbeState>(); if (inject) state->inject = Stage::Close;
        const auto path = directory.root / ("close-" + std::to_string(inject) + ".jsonl");
        tr::JournalCloseReceipt first;
        {
            auto writer = Open(path, state);
            const auto appended = writer.AppendLineDetailed("committed-first", Durability::PowerLoss);
            REQUIRE(appended.status == AppendStatus::Committed); CHECK(appended.confirmed_durability == Durability::PowerLoss);
            CHECK_FALSE(writer.first_unconfirmed_append()); CHECK(state->count == 4);
            first = writer.CloseDetailed(); REQUIRE(first.native); ActualSuccess(*first.native);
            CHECK(first.native->injected_unconfirmed == (inject != 0)); CHECK_FALSE(first.broken_before);
            CHECK(first.broken_after == (inject != 0)); CHECK(first.ok() == (inject == 0));
            CHECK(first.status == (inject ? CloseStatus::Unconfirmed : CloseStatus::Closed));
            CHECK(state->count == 5); CHECK(state->matching_boundaries == static_cast<unsigned>(inject));
            CHECK(writer.Close() == (inject == 0)); SameClose(writer.CloseDetailed(), first); CHECK(state->count == 5);
            REQUIRE(writer.first_close_receipt()); SameClose(*writer.first_close_receipt(), first);
            NoIo(writer.AppendLineDetailed("closed-never-write", Durability::PowerLoss)); CHECK(state->count == 5);
            CHECK(writer.line_count() == 1); CHECK_FALSE(writer.first_unconfirmed_append());
            CHECK(appended.status == AppendStatus::Committed); CHECK(Bytes(path) == "committed-first\n");
        }
        CHECK(state->count == 5); CHECK(state->live == 0); CHECK(state->destroyed == 1); CHECK_FALSE(state->overflow);
        CHECK(Bytes(path) == "committed-first\n");
    }
    Marker("close");
}

TEST_CASE("Journal moves transfer witnesses and do not leak old target close failure") {
    Directory directory;
    auto source_state = std::make_shared<ProbeState>(); source_state->inject = Stage::NewlineWrite;
    {
        auto source = Open(directory.root / "moved-gap.jsonl", source_state);
        const auto first = source.AppendLineDetailed("source-gap", Durability::Buffered);
        REQUIRE(first.status == AppendStatus::Unconfirmed);
        tr::JournalWriter target(std::move(source));
        CHECK(source.line_count() == 0); CHECK_FALSE(source.broken()); CHECK_FALSE(source.first_unconfirmed_append());
        CHECK_FALSE(source.first_close_receipt()); REQUIRE(target.first_unconfirmed_append());
        SameAppend(*target.first_unconfirmed_append(), first);
        CHECK(source.Close()); CHECK(source_state->count == 2); CHECK_FALSE(target.Close()); CHECK(source_state->count == 3);
        CHECK(Bytes(target.path()) == "source-gap\n"); SameAppend(*target.first_unconfirmed_append(), first);
    }
    CHECK(source_state->count == 3); CHECK(source_state->live == 0); CHECK(source_state->destroyed == 1);
    auto old_state = std::make_shared<ProbeState>(); old_state->inject = Stage::Close;
    auto next_state = std::make_shared<ProbeState>();
    {
        auto target = Open(directory.root / "old-target.jsonl", old_state);
        auto source = Open(directory.root / "new-source.jsonl", next_state);
        CHECK(target.AppendLine("old", Durability::ProcessCrash));
        CHECK(source.AppendLine("new", Durability::ProcessCrash));
        target = std::move(source);
        REQUIRE(old_state->count == 4); CHECK(old_state->actual[3].stage == Stage::Close);
        ActualSuccess(old_state->actual[3]); CHECK(old_state->matching_boundaries == 1);
        CHECK(old_state->live == 0); CHECK(old_state->destroyed == 1);
        CHECK_FALSE(target.broken()); CHECK_FALSE(target.first_close_receipt()); CHECK_FALSE(target.first_unconfirmed_append());
        CHECK(target.line_count() == 1); CHECK_FALSE(source.first_close_receipt()); CHECK_FALSE(source.first_unconfirmed_append());
        CHECK(source.Close()); CHECK(next_state->count == 3);
        CHECK(target.Close()); CHECK(next_state->count == 4);
        CHECK(Bytes(directory.root / "old-target.jsonl") == "old\n"); CHECK(Bytes(target.path()) == "new\n");
    }
    CHECK(old_state->count == 4); CHECK(next_state->count == 4);
    CHECK(next_state->live == 0); CHECK(next_state->destroyed == 1);
    auto closed_state = std::make_shared<ProbeState>(); closed_state->inject = Stage::Close;
    {
        auto source = Open(directory.root / "closed-move.jsonl", closed_state);
        CHECK(source.AppendLine("closed", Durability::Buffered));
        const auto first = source.CloseDetailed(); REQUIRE(first.status == CloseStatus::Unconfirmed);
        tr::JournalWriter target(std::move(source));
        CHECK_FALSE(source.first_close_receipt()); CHECK_FALSE(source.broken()); REQUIRE(target.first_close_receipt());
        SameClose(*target.first_close_receipt(), first); SameClose(target.CloseDetailed(), first);
        CHECK_FALSE(target.Close()); CHECK(source.Close()); CHECK(closed_state->count == 4);
    }
    CHECK(closed_state->count == 4); CHECK(closed_state->live == 0); CHECK(closed_state->destroyed == 1);
    CHECK_FALSE(source_state->overflow); CHECK_FALSE(old_state->overflow);
    CHECK_FALSE(next_state->overflow); CHECK_FALSE(closed_state->overflow);
    Marker("move");
}
