#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "memory/project_commit.hpp"
#include "memory/project_memory.hpp"
#include "platform/atomic_write.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"

namespace {
namespace fs = std::filesystem;
namespace memory = lubancode::memory;
namespace platform = lubancode::platform;
using Receipt = memory::ProjectCommitReceipt;
using State = memory::ProjectCommitState;
using namespace std::chrono_literals;

void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(platform::FileIoPath(path), std::ios::binary | std::ios::trunc); REQUIRE(file.is_open());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); file.close(); REQUIRE_FALSE(file.fail());
}
std::string Read(const fs::path& path) {
    std::ifstream file(platform::FileIoPath(path), std::ios::binary); REQUIRE(file.is_open());
    const std::string bytes((std::istreambuf_iterator<char>(file)), {}); REQUIRE_FALSE(file.bad()); return bytes;
}
struct Fixture {
    fs::path root;
    memory::ProjectCommitContext context;
    Fixture() {
        static std::atomic<unsigned> number{0};
        root = fs::temp_directory_path() / ("memory-handoff-" +
            std::to_string(platform::CurrentProcessId()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++number));
        REQUIRE(fs::create_directory(root)); root = fs::canonical(root);
        context.project_root = root / "repo";
        context.memory_directory = root / "workspace" / "memory";
        context.lifecycle_root = root / "workspace" / "lifecycle";
        context.workspace_key = "handoff-project";
        REQUIRE(fs::create_directory(context.project_root)); REQUIRE(fs::create_directory(root / "workspace"));
    }
    ~Fixture() {
        std::error_code error; fs::remove_all(platform::FileIoPath(root), error);
    }
    memory::ProjectCommitContext Context(const std::string& tag) const {
        auto value = context; value.operation_id = "save-" + tag; value.session_id = "session-" + tag;
        value.source_event_ref = "workspace_key=handoff-project/session_id=" + value.session_id +
            "/run_id=run-1/event_id=event-" + tag;
        return value;
    }
    fs::path Intent(const std::string& tag) const { return context.lifecycle_root / ("save-" + tag) / "intent.json"; }
    fs::path Lock() const { return context.memory_directory / ".state" / "memory.lock"; }
};
memory::SaveRequest Request(const std::string& tag) {
    memory::SaveRequest request;
    request.kind = memory::MemoryKind::Fact; request.id = "fact." + tag; request.title = "Handoff " + tag;
    request.content = "actual content for " + tag; request.source_session = "session-" + tag;
    return request;
}
struct Gate {
    std::mutex mutex; std::condition_variable cv;
    bool entered = false, released = false;
    void Enter() { std::unique_lock lock(mutex); entered = true; cv.notify_all(); cv.wait(lock, [&] { return released; }); }
    bool Await() { std::unique_lock lock(mutex); return cv.wait_for(lock, 10s, [&] { return entered; }); }
    void Release() { { std::lock_guard lock(mutex); released = true; } cv.notify_all(); }
};
struct Signal {
    std::mutex mutex; std::condition_variable cv; bool seen = false;
    void Set() { { std::lock_guard lock(mutex); seen = true; } cv.notify_all(); }
    bool Await() { std::unique_lock lock(mutex); return cv.wait_for(lock, 10s, [&] { return seen; }); }
};
// Futures come from packaged_task, not async: assertion unwinding reaches this
// owner, which releases every controlled writer before joining all threads.
struct Threads {
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<Gate>> gates;
    std::shared_ptr<Gate> NewGate() { auto value = std::make_shared<Gate>(); gates.push_back(value); return value; }
    std::future<Receipt> Start(std::function<Receipt()> body) {
        std::packaged_task<Receipt()> task(std::move(body)); auto future = task.get_future();
        threads.emplace_back(std::move(task)); return future;
    }
    void Join() { for (auto& gate : gates) gate->Release(); for (auto& thread : threads) if (thread.joinable()) thread.join(); }
    ~Threads() { Join(); }
};
Receipt Finished(std::future<Receipt>& future) {
    REQUIRE(future.wait_for(20s) == std::future_status::ready); return future.get();
}
std::future<Receipt> Hold(Threads& threads, const memory::ProjectCommitContext& context,
                          const memory::SaveRequest& request, const std::shared_ptr<Gate>& gate) {
    return threads.Start([context, request, gate] {
        return memory::commit_testing::CommitProjectUpsert(context, request,
            [gate](const auto& path, auto bytes, auto durability) {
                auto actual = platform::AtomicWriteFile(path, bytes, durability);
                if (path.filename() == "intent.json") gate->Enter();
                return actual;
            });
    });
}
void Committed(const Receipt& receipt, const Fixture& fixture, const std::string& tag) {
    REQUIRE_MESSAGE(receipt.state == State::Committed, (receipt.error_code + ": " + receipt.error));
    CHECK(receipt.operation_id == "save-" + tag); CHECK(receipt.memory_id == "fact." + tag);
    REQUIRE_FALSE(receipt.memory_path.empty());
    CHECK(Read(fixture.context.memory_directory / platform::Utf8ToPath(receipt.memory_path)).find("actual content for " + tag) != std::string::npos);
    CHECK(Read(fixture.context.memory_directory / ".state" / "catalog.json").find("fact." + tag) != std::string::npos);
    CHECK(Read(fixture.context.memory_directory / "index.md").find(receipt.memory_path) != std::string::npos);
}
void Unstarted(const Receipt& receipt, const Fixture& fixture, const std::string& tag, const std::string& code) {
    CHECK(receipt.state == State::NotStarted); CHECK(receipt.error_code == code); CHECK(receipt.stages.empty());
    CHECK_FALSE(fs::exists(fixture.Intent(tag)));
    if (!receipt.memory_path.empty())
        CHECK_FALSE(fs::exists(fixture.context.memory_directory / platform::Utf8ToPath(receipt.memory_path)));
}
void Mark(const char* path) { std::fprintf(stderr, "[memory-project-handoff-path] %s\n", path); std::fflush(stderr); }
} // namespace

TEST_CASE("project handoff: same directory commits cross a real held writer") {
    Fixture fixture; Threads threads; auto gate = threads.NewGate();
    auto first = Hold(threads, fixture.Context("first"), Request("first"), gate); REQUIRE(gate->Await());
    REQUIRE(fs::is_regular_file(fixture.Intent("first"))); const auto owner = Read(fixture.Lock() / "owner");
    auto entered = std::make_shared<Signal>();
    auto second = threads.Start([context = fixture.Context("second"), request = Request("second"), entered] {
        return memory::CommitProjectUpsertWithCancellation(context, request, [entered] { entered->Set(); return false; });
    });
    REQUIRE(entered->Await()); CHECK(second.wait_for(0ms) == std::future_status::timeout);
    CHECK_FALSE(fs::exists(fixture.Intent("second"))); CHECK(Read(fixture.Lock() / "owner") == owner);
    gate->Release(); const auto a = Finished(first), b = Finished(second); threads.Join();
    Committed(a, fixture, "first"); Committed(b, fixture, "second");
    CHECK_FALSE(fs::exists(fixture.Lock()));
    CHECK(memory::InspectProjectCommitReceipt(fixture.Context("first"), Request("first")).state == State::Committed);
    CHECK(memory::ConfirmProjectCommitReceipt(fixture.context.lifecycle_root, "save-second").state == State::Committed);
    Mark("same-directory");
}

TEST_CASE("project handoff: actual directory aliases cannot publish competing slots") {
    Fixture fixture; auto alias = fixture.Context("alias");
#ifdef _WIN32
    auto spelling = platform::PathToUtf8(fixture.root / "workspace");
    for (char& ch : spelling) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    const auto alternate = platform::Utf8ToPath(spelling);
#else
    const auto alternate = fixture.root / "workspace-alias";
    fs::create_directory_symlink(fixture.root / "workspace", alternate);
#endif
    REQUIRE(fs::equivalent(alternate, fixture.root / "workspace"));
    alias.memory_directory = alternate / "memory"; alias.lifecycle_root = alternate / "lifecycle";
    Threads threads; auto gate = threads.NewGate();
    auto first = Hold(threads, fixture.Context("original"), Request("original"), gate); REQUIRE(gate->Await());
    auto entered = std::make_shared<Signal>();
    auto other = threads.Start([alias, entered] {
        return memory::CommitProjectUpsertWithCancellation(alias, Request("alias"), [entered] { entered->Set(); return false; });
    });
    REQUIRE(entered->Await()); CHECK(other.wait_for(0ms) == std::future_status::timeout);
    CHECK_FALSE(fs::exists(fixture.Intent("alias"))); gate->Release();
    const auto a = Finished(first), b = Finished(other); threads.Join();
    Committed(a, fixture, "original"); Committed(b, fixture, "alias"); CHECK_FALSE(fs::exists(fixture.Lock()));
    Mark("aliases");
}

TEST_CASE("project handoff: cancelled queued ticket retires without invoking disk mutation") {
    Fixture fixture; Threads threads; auto gate = threads.NewGate();
    auto first = Hold(threads, fixture.Context("holder"), Request("holder"), gate); REQUIRE(gate->Await());
    auto entered = std::make_shared<Signal>(); auto stop = std::make_shared<std::atomic<bool>>(false);
    auto waiter = threads.Start([context = fixture.Context("cancelled"), entered, stop] {
        return memory::CommitProjectUpsertWithCancellation(context, Request("cancelled"), [entered, stop] {
            entered->Set(); return stop->load();
        });
    });
    REQUIRE(entered->Await()); const auto owner = Read(fixture.Lock() / "owner");
    stop->store(true); const auto cancelled = Finished(waiter);
    Unstarted(cancelled, fixture, "cancelled", "memory.commit.cancelled");
    CHECK(Read(fixture.Lock() / "owner") == owner); CHECK(first.wait_for(0ms) == std::future_status::timeout);
    auto successor_entered = std::make_shared<Signal>();
    auto successor = threads.Start([context = fixture.Context("successor"), successor_entered] {
        return memory::CommitProjectUpsertWithCancellation(context, Request("successor"), [successor_entered] {
            successor_entered->Set(); return false;
        });
    });
    REQUIRE(successor_entered->Await()); CHECK_FALSE(fs::exists(fixture.Intent("successor")));
    gate->Release(); const auto a = Finished(first), b = Finished(successor); threads.Join();
    Committed(a, fixture, "holder"); Committed(b, fixture, "successor");
    Committed(memory::CommitProjectUpsert(fixture.Context("after-retirement"), Request("after-retirement")), fixture, "after-retirement");
    CHECK_FALSE(fs::exists(fixture.Lock())); Mark("waiting-cancel");
}

TEST_CASE("project handoff: another project commits while the first writer remains held") {
    Fixture first_project, second_project; Threads threads; auto gate = threads.NewGate();
    auto held = Hold(threads, first_project.Context("held"), Request("held"), gate); REQUIRE(gate->Await());
    auto independent = threads.Start([context = second_project.Context("independent")] {
        return memory::CommitProjectUpsert(context, Request("independent"));
    });
    const auto b = Finished(independent); Committed(b, second_project, "independent");
    CHECK(held.wait_for(0ms) == std::future_status::timeout); REQUIRE(fs::is_directory(first_project.Lock()));
    CHECK_FALSE(fs::exists(second_project.Lock()));
    gate->Release(); const auto a = Finished(held); threads.Join(); Committed(a, first_project, "held");
    Mark("independent");
}

TEST_CASE("project handoff: exceptions and held or queued reentry cannot strand the next ticket") {
    Fixture fixture, nested_project; Threads threads; auto gate = threads.NewGate();
    auto held_reentry = std::make_shared<std::optional<Receipt>>();
    auto nested = std::make_shared<std::optional<Receipt>>();
    auto first = threads.Start([context = fixture.Context("throwing"), inner = fixture.Context("held-reentry"),
                               other = nested_project.Context("nested"), held_reentry, nested, gate] {
        return memory::commit_testing::CommitProjectUpsert(context, Request("throwing"),
            [inner, other, held_reentry, nested, gate](const auto&, auto, auto)
                -> std::expected<platform::AtomicWriteReceipt, platform::AtomicWriteError> {
                *held_reentry = memory::CommitProjectUpsert(inner, Request("held-reentry"));
                *nested = memory::CommitProjectUpsert(other, Request("nested"));
                gate->Enter();
                throw std::runtime_error("controlled writer failure before its first AtomicWrite");
            });
    });
    REQUIRE(gate->Await()); REQUIRE(held_reentry->has_value()); REQUIRE(nested->has_value());
    Unstarted(**held_reentry, fixture, "held-reentry", "memory.commit.reentrant");
    Committed(**nested, nested_project, "nested");
    auto queued_reentry = std::make_shared<std::optional<Receipt>>();
    auto waiter = threads.Start([context = fixture.Context("predicate-throw"), inner = fixture.Context("queued-reentry"), queued_reentry] {
        return memory::CommitProjectUpsertWithCancellation(context, Request("predicate-throw"), [inner, queued_reentry]() -> bool {
            *queued_reentry = memory::CommitProjectUpsert(inner, Request("queued-reentry"));
            throw std::runtime_error("controlled waiting predicate failure");
        });
    });
    const auto cancelled = Finished(waiter); REQUIRE(queued_reentry->has_value());
    Unstarted(**queued_reentry, fixture, "queued-reentry", "memory.commit.reentrant");
    Unstarted(cancelled, fixture, "predicate-throw", "memory.commit.invalid_or_failed");
    auto entered = std::make_shared<Signal>();
    auto next = threads.Start([context = fixture.Context("after-exception"), entered] {
        return memory::CommitProjectUpsertWithCancellation(context, Request("after-exception"), [entered] { entered->Set(); return false; });
    });
    REQUIRE(entered->Await()); CHECK_FALSE(fs::exists(fixture.Intent("after-exception")));
    gate->Release(); const auto failed = Finished(first), succeeded = Finished(next); threads.Join();
    Unstarted(failed, fixture, "throwing", "memory.commit.invalid_or_failed");
    Committed(succeeded, fixture, "after-exception"); CHECK_FALSE(fs::exists(fixture.Lock()));
    Mark("retirement");
}

TEST_CASE("project handoff: local queue does not steal external live or broken OwnerLock") {
    {
        Fixture fixture; memory::OwnerLock holder;
        REQUIRE(memory::OwnerLock::TryAcquire(fixture.Lock(), &holder).status == memory::OwnerLock::Status::Acquired);
        const auto owner = Read(fixture.Lock() / "owner");
        const auto refused = memory::CommitProjectUpsert(fixture.Context("external"), Request("external"));
        Unstarted(refused, fixture, "external", "memory.commit.lock_refused");
        CHECK(holder.holds()); CHECK(memory::OwnerLock::HolderAlive(fixture.Lock())); CHECK(Read(fixture.Lock() / "owner") == owner);
        holder.Release(); Committed(memory::CommitProjectUpsert(fixture.Context("released"), Request("released")), fixture, "released");
    }
    {
        Fixture fixture; fs::create_directories(fixture.Lock()); Write(fixture.Lock() / "owner", "{broken-owner");
        const auto refused = memory::CommitProjectUpsert(fixture.Context("broken"), Request("broken"));
        Unstarted(refused, fixture, "broken", "memory.commit.lock_refused");
        CHECK(Read(fixture.Lock() / "owner") == "{broken-owner"); CHECK(fs::is_directory(fixture.Lock()));
    }
    Mark("external-lock");
}
