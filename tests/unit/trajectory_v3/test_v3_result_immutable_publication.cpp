#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "trajectory/v3/result_store.hpp"

namespace {
namespace fs = std::filesystem;
namespace platform = lubancode::platform;
using Store = lubancode::trajectory::v3::ResultStore;
using Knowledge = Store::PersistedResult::Knowledge;
using Outcome = platform::WriteOutcome;
using Durability = platform::WriteDurability;

struct Fixture {
    fs::path root;
    Fixture() {
        static std::atomic<unsigned> next{0};
        root = fs::temp_directory_path() / ("immutable-publication-" + std::to_string(platform::CurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++next));
        REQUIRE(fs::create_directory(platform::FileIoPath(root))); root = fs::canonical(root);
    }
    ~Fixture() {
        platform::SetFileFlushFailureForTest(false); platform::SetDirectoryFlushFailureForTest(false);
        std::error_code error; fs::remove_all(platform::FileIoPath(root), error);
    }
};
std::string Read(const fs::path& path) {
    std::ifstream file(platform::FileIoPath(path), std::ios::binary); REQUIRE(file.is_open());
    const std::string bytes((std::istreambuf_iterator<char>(file)), {}); REQUIRE_FALSE(file.bad()); return bytes;
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(platform::FileIoPath(path), std::ios::binary | std::ios::trunc); REQUIRE(file.is_open());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); file.close(); REQUIRE_FALSE(file.fail());
}
std::map<std::string, std::string> Snapshot(const fs::path& directory) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::directory_iterator(platform::FileIoPath(directory))) {
        REQUIRE(entry.is_regular_file()); result.emplace(platform::PathToUtf8(entry.path().filename()), Read(entry.path()));
    }
    return result;
}
void NoTemporary(const fs::path& directory) {
    for (const auto& entry : fs::directory_iterator(platform::FileIoPath(directory)))
        CHECK(entry.path().extension() != ".tmp");
}
void ClosedFile(const platform::ImmutableWriteReceipt& receipt) {
    REQUIRE(receipt.temp_open.attempted); REQUIRE(receipt.temp_open.succeeded);
    REQUIRE(receipt.file_close.attempted); REQUIRE(receipt.file_close.succeeded);
    REQUIRE(receipt.file_close.result); CHECK(*receipt.file_close.result == 0);
    CHECK_FALSE(receipt.ancestor_chain_confirmed);
}
void Durable(const platform::ImmutableWriteReceipt& receipt, std::size_t size) {
    REQUIRE_MESSAGE(receipt.ok(), (receipt.error_code + ": " + receipt.message));
    CHECK(receipt.requested == Durability::ProcessCrashDurability); CHECK(receipt.outcome == Outcome::CommittedDurable);
    ClosedFile(receipt);
    REQUIRE(receipt.body.attempted); CHECK(receipt.body.succeeded); CHECK(receipt.body.requested_bytes == size);
    CHECK(receipt.body.written_bytes == size); CHECK(receipt.flush.attempted); CHECK(receipt.flush.succeeded);
    CHECK(receipt.file_sync.attempted); CHECK(receipt.file_sync.succeeded); CHECK_FALSE(receipt.file_sync.injected_failure);
    CHECK(receipt.publish.attempted); CHECK(receipt.publish.succeeded); REQUIRE(receipt.publish.result);
    CHECK(receipt.parent_open.attempted); CHECK(receipt.parent_open.succeeded);
    CHECK(receipt.parent_sync.attempted); CHECK(receipt.parent_sync.succeeded); CHECK_FALSE(receipt.parent_sync.injected_failure);
    CHECK(receipt.parent_close.attempted); CHECK(receipt.parent_close.succeeded);
    CHECK(fs::equivalent(receipt.confirmed_parent, receipt.target.parent_path()));
    CHECK_FALSE(fs::exists(platform::FileIoPath(receipt.temporary)));
#ifdef _WIN32
    CHECK_FALSE(receipt.cleanup.attempted); // Move consumed the temporary name.
#else
    CHECK(receipt.cleanup.attempted); CHECK(receipt.cleanup.succeeded); // link retained our temporary name until unlink.
#endif
}
Store::PersistRequest Request(std::string bytes = "actual result body") {
    Store::PersistRequest request;
    request.result_kind = "text"; request.tool_call_id = "action-000001";
    request.attempt = 1; request.execution_event_ref = "evt-000004";
    request.capture_limits = {{"max_output_bytes", 4096}};
    request.preview_policy = {{"maxPreviewBytes", 4096}};
    Store::ChannelOutput channel; channel.channel = "combined"; channel.data = std::move(bytes);
    channel.output_bytes = channel.data.size(); request.outputs.push_back(std::move(channel));
    return request;
}
void Mark(const char* path) { std::fprintf(stderr, "[result-immutable-publication-path] %s\n", path); std::fflush(stderr); }
} // namespace

TEST_CASE("immutable result publication: actual visibility and durable native stages stay distinct") {
    Fixture fixture;
    const auto visible = platform::CreateImmutableFileDetailed(fixture.root / "visible", "visible bytes", Durability::AtomicVisibility);
    REQUIRE(visible.ok()); CHECK(visible.outcome == Outcome::CommittedDurabilityNotRequested); ClosedFile(visible);
    CHECK(visible.body.written_bytes == 13); CHECK(visible.flush.succeeded); CHECK_FALSE(visible.file_sync.attempted);
    CHECK(visible.publish.succeeded); CHECK_FALSE(visible.parent_open.attempted); CHECK(visible.confirmed_parent.empty());
    CHECK(Read(fixture.root / "visible") == "visible bytes");
    const auto durable = platform::CreateImmutableFileDetailed(fixture.root / "durable", "durable bytes");
    Durable(durable, 13); CHECK(Read(durable.target) == "durable bytes"); NoTemporary(fixture.root);
    const auto missing = platform::CreateImmutableFileDetailed(fixture.root / "missing" / "parent" / "file", "no mkdir promise");
    CHECK(missing.outcome == Outcome::NotCommitted); CHECK_FALSE(missing.temp_open.attempted);
    CHECK_FALSE(fs::exists(fixture.root / "missing")); Mark("actual");
}

TEST_CASE("immutable result publication: concurrent native publishers never replace the winner") {
    Fixture fixture;
    struct RacingWriters {
        std::mutex mutex; std::condition_variable cv; bool go = false;
        std::vector<std::thread> threads;
        void Join() {
            { std::lock_guard lock(mutex); go = true; } cv.notify_all();
            for (auto& thread : threads) if (thread.joinable()) thread.join();
        }
        ~RacingWriters() { Join(); }
    };
    std::array<platform::ImmutableWriteReceipt, 2> receipts;
    std::array<std::exception_ptr, 2> exceptions;
    const std::array<std::string, 2> bodies = {"FIRST complete body", "SECOND complete body"};
    const auto target = fixture.root / "winner";
    RacingWriters race; // Releases/joins before every captured local is destroyed.
    for (std::size_t index = 0; index < receipts.size(); ++index) race.threads.emplace_back([&, index] {
        { std::unique_lock lock(race.mutex); race.cv.wait(lock, [&] { return race.go; }); }
        try { receipts[index] = platform::CreateImmutableFileDetailed(target, bodies[index]); }
        catch (...) { exceptions[index] = std::current_exception(); }
    });
    race.Join(); REQUIRE_FALSE(exceptions[0]); REQUIRE_FALSE(exceptions[1]);
    REQUIRE(receipts[0].ok() != receipts[1].ok());
    const std::size_t winner = receipts[0].ok() ? 0 : 1, loser = 1 - winner;
    Durable(receipts[winner], bodies[winner].size()); ClosedFile(receipts[loser]);
    CHECK(receipts[loser].outcome == Outcome::NotCommitted); CHECK(receipts[loser].publish.attempted);
    CHECK_FALSE(receipts[loser].publish.succeeded); CHECK(receipts[loser].publish.native_error != 0);
    CHECK(receipts[0].temporary != receipts[1].temporary); CHECK(Read(target) == bodies[winner]);
    const auto previous = Read(target);
    const auto repeated = platform::CreateImmutableFileDetailed(target, "must not overwrite");
    CHECK_FALSE(repeated.ok()); CHECK(repeated.outcome == Outcome::NotCommitted); CHECK(Read(target) == previous);
    NoTemporary(fixture.root); Mark("race");
}

TEST_CASE("immutable result publication: exclusive temporary open preserves an existing temporary object") {
    Fixture fixture;
    const auto target = fixture.root / "exclusive";
    const auto first = platform::CreateImmutableFileDetailed(target, "original"); Durable(first, 8);
    // The public receipt identifies the actual previous temporary name. The
    // contract's next PID/sequence name is occupied deliberately, using plain
    // file I/O so no platform publication consumes that sequence in between.
    const auto name = platform::PathToUtf8(first.temporary.filename());
    const auto dash = name.rfind('-'), suffix = name.rfind(".tmp");
    REQUIRE(dash != std::string::npos); REQUIRE(suffix > dash);
    const auto sequence = std::stoull(name.substr(dash + 1, suffix - dash - 1));
    const auto occupied = fixture.root / platform::Utf8ToPath(name.substr(0, dash + 1) + std::to_string(sequence + 1) + ".tmp");
    Write(occupied, "foreign temporary sentinel");
    const auto refused = platform::CreateImmutableFileDetailed(target, "must not truncate temporary");
    CHECK(refused.temporary == occupied); CHECK_FALSE(refused.ok()); CHECK(refused.outcome == Outcome::NotCommitted);
    CHECK(refused.temp_open.attempted); CHECK_FALSE(refused.temp_open.succeeded); CHECK_FALSE(refused.body.attempted);
    CHECK_FALSE(refused.publish.attempted); CHECK_FALSE(refused.cleanup.attempted);
    CHECK(Read(occupied) == "foreign temporary sentinel"); CHECK(Read(target) == "original");
    Mark("exclusive-temp");
}

TEST_CASE("immutable result publication: controlled file-sync refusal stays before publication") {
    Fixture fixture; auto store = Store::Open(fixture.root); REQUIRE(store);
    platform::SetFileFlushFailureForTest(true);
    const auto failed = store->Persist(Request());
    platform::SetFileFlushFailureForTest(false);
    CHECK_FALSE(failed.ok); REQUIRE(failed.publication); CHECK(failed.publication->knowledge == Knowledge::NotCommitted);
    REQUIRE(failed.publication->files.size() == 1); REQUIRE(failed.publication->files[0].native);
    const auto& native = *failed.publication->files[0].native;
    ClosedFile(native); CHECK(native.outcome == Outcome::NotCommitted); CHECK(native.body.succeeded);
    CHECK(native.file_sync.injected_failure); CHECK_FALSE(native.file_sync.attempted);
    CHECK_FALSE(native.publish.attempted); CHECK(native.cleanup.succeeded);
    CHECK(failed.publication->error_code == "immutable.file_flush_failed");
    CHECK_FALSE(store->FirstUnconfirmedPublication()); CHECK(Snapshot(fixture.root / "artifacts").empty());
    const auto next = store->Persist(Request()); REQUIRE(next.ok); CHECK(next.result_id == "res-000001");
    NoTemporary(fixture.root / "artifacts"); Mark("file-failure");
}

TEST_CASE("immutable result publication: directory uncertainty is cached without readback upgrade") {
    Fixture fixture; auto store = Store::Open(fixture.root); REQUIRE(store);
    platform::SetDirectoryFlushFailureForTest(true);
    const auto unknown = store->Persist(Request("visible but unconfirmed"));
    platform::SetDirectoryFlushFailureForTest(false);
    CHECK_FALSE(unknown.ok); REQUIRE(unknown.publication); CHECK(unknown.publication->knowledge == Knowledge::Indeterminate);
    REQUIRE(unknown.publication->files.size() == 1); REQUIRE(unknown.publication->files[0].native);
    const auto& native = *unknown.publication->files[0].native;
    ClosedFile(native); CHECK(native.file_sync.succeeded); CHECK(native.publish.succeeded);
    CHECK(native.outcome == Outcome::CommittedDurabilityUnconfirmed); CHECK(native.parent_sync.injected_failure);
    CHECK_FALSE(native.parent_sync.attempted); CHECK(native.confirmed_parent.empty());
    CHECK(Read(native.target) == "visible but unconfirmed");
    const auto before = Snapshot(fixture.root / "artifacts");
    const auto repeated = store->Persist(Request("different bytes cannot retry this attempt"));
    CHECK_FALSE(repeated.ok); REQUIRE(repeated.publication); CHECK(*repeated.publication == *unknown.publication);
    REQUIRE(store->FirstUnconfirmedPublication()); CHECK(*store->FirstUnconfirmedPublication() == *unknown.publication);
    CHECK(Snapshot(fixture.root / "artifacts") == before);
    auto reopened = Store::Open(fixture.root); REQUIRE(reopened);
    const auto next = reopened->Persist(Request("fresh logical result")); REQUIRE(next.ok); CHECK(next.result_id == "res-000002");
    CHECK(Read(native.target) == "visible but unconfirmed");
    CHECK(unknown.publication->files[0].native->outcome == Outcome::CommittedDurabilityUnconfirmed);
    NoTemporary(fixture.root / "artifacts"); Mark("directory-unknown");
}

TEST_CASE("immutable result publication: real ResultStore channels metadata and legacy listing remain compatible") {
    Fixture fixture; auto store = Store::Open(fixture.root); REQUIRE(store);
    const auto request = Request(); const auto result = store->Persist(request);
    REQUIRE(result.ok); CHECK(result.result_id == "res-000001"); REQUIRE(result.publication);
    CHECK(result.publication->knowledge == Knowledge::Committed); CHECK(result.publication->error.empty());
    REQUIRE(result.publication->files.size() == 2); REQUIRE(result.result_ref.size() == 2);
    for (const auto& file : result.publication->files) {
        REQUIRE(file.called); REQUIRE(file.native);
        Durable(*file.native, Read(file.native->target).size());
    }
    CHECK(Read(fixture.root / "artifacts" / "res-000001.combined.txt") == request.outputs[0].data);
    CHECK(result.result_ref[0].at("artifactId") == "res-000001");
    const auto listing = store->PersistListing("res-000001.index.txt", "real listing"); REQUIRE(listing);
    CHECK(*listing == "artifacts/res-000001.index.txt");
    const auto conflict = store->PersistListing("res-000001.index.txt", "cannot overwrite"); CHECK_FALSE(conflict);
    CHECK_FALSE(store->FirstUnconfirmedPublication()); CHECK(Read(fixture.root / *listing) == "real listing");
    const auto next = store->Persist(Request("next result")); REQUIRE(next.ok); CHECK(next.result_id == "res-000002");
    NoTemporary(fixture.root / "artifacts"); Mark("store");
}

TEST_CASE("immutable result publication: a later metadata conflict preserves partial publications and first error") {
    Fixture fixture; auto store = Store::Open(fixture.root); REQUIRE(store);
    Write(fixture.root / "artifacts" / "res-000001.json", "occupied metadata");
    const auto partial = store->Persist(Request("actual earlier channel"));
    CHECK_FALSE(partial.ok); REQUIRE(partial.publication); CHECK(partial.publication->knowledge == Knowledge::Indeterminate);
    REQUIRE(partial.publication->files.size() == 2);
    REQUIRE(partial.publication->files[0].native); REQUIRE(partial.publication->files[1].native);
    Durable(*partial.publication->files[0].native, 22);
    CHECK(partial.publication->files[1].native->outcome == Outcome::NotCommitted);
    CHECK_FALSE(partial.publication->files[1].native->publish.succeeded);
    CHECK(Read(fixture.root / "artifacts" / "res-000001.json") == "occupied metadata");
    CHECK(Read(fixture.root / "artifacts" / "res-000001.combined.txt") == "actual earlier channel");
    const auto before = Snapshot(fixture.root / "artifacts");
    const auto blocked = store->PersistListingDetailed("unrelated.txt", "no new write after partial publication");
    CHECK_FALSE(blocked.ok); CHECK(blocked.publication == *partial.publication);
    CHECK(Snapshot(fixture.root / "artifacts") == before);
    auto reopened = Store::Open(fixture.root); REQUIRE(reopened);
    const auto next = reopened->Persist(Request()); REQUIRE(next.ok); CHECK(next.result_id == "res-000002");
    CHECK(Read(fixture.root / "artifacts" / "res-000001.json") == "occupied metadata");
    NoTemporary(fixture.root / "artifacts"); Mark("partial");
}

TEST_CASE("immutable result publication: listing reports native uncertainty and preserves its published bytes") {
    Fixture fixture; auto store = Store::Open(fixture.root); REQUIRE(store);
    platform::SetDirectoryFlushFailureForTest(true);
    const auto unknown = store->PersistListingDetailed("listing.txt", "actual listing bytes");
    platform::SetDirectoryFlushFailureForTest(false);
    CHECK_FALSE(unknown.ok); CHECK(unknown.publication.knowledge == Knowledge::Indeterminate);
    REQUIRE(unknown.publication.files.size() == 1); REQUIRE(unknown.publication.files[0].native);
    CHECK(unknown.publication.files[0].native->outcome == Outcome::CommittedDurabilityUnconfirmed);
    CHECK(Read(fixture.root / "artifacts" / "listing.txt") == "actual listing bytes");
    const auto before = Snapshot(fixture.root / "artifacts");
    const auto legacy = store->PersistListing("other.txt", "blocked"); REQUIRE_FALSE(legacy);
    CHECK(legacy.error() == unknown.publication.error); CHECK(Snapshot(fixture.root / "artifacts") == before);
    auto reopened = Store::Open(fixture.root); REQUIRE(reopened);
    const auto conflict = reopened->PersistListingDetailed("listing.txt", "do not overwrite after reopen");
    CHECK_FALSE(conflict.ok); CHECK(conflict.publication.knowledge == Knowledge::NotCommitted);
    CHECK(Read(fixture.root / "artifacts" / "listing.txt") == "actual listing bytes");
    const auto fresh = reopened->PersistListingDetailed("next.txt", "fresh"); REQUIRE(fresh.ok);
    REQUIRE(fresh.publication.files[0].native); Durable(*fresh.publication.files[0].native, 5);
    NoTemporary(fixture.root / "artifacts"); Mark("listing");
}
