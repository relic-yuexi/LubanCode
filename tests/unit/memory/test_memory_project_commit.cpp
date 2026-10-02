#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "memory/frontmatter.hpp"
#include "memory/internal.hpp"
#include "memory/project_commit.hpp"
#include "memory/project_memory.hpp"
#include "memory/topic_store.hpp"

using namespace lubancode;
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using State = memory::ProjectCommitState;
using Stage = memory::ProjectCommitStage;
using Outcome = platform::WriteOutcome;

void Write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    REQUIRE(out.good());
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(in.bad());
    return text;
}
struct Rig {
    fs::path root;
    memory::ProjectCommitContext context;
    memory::SaveRequest request;
    explicit Rig(const std::string& name) {
        static std::atomic<unsigned> sequence{0};
        root = fs::temp_directory_path() / ("lubancode-project-commit-" + name + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
               std::to_string(++sequence));
        context.project_root = root / "repo";
        context.memory_directory = root / "home" / "workspaces" / "project-key" / "memory";
        context.lifecycle_root = context.memory_directory.parent_path() / "lifecycle";
        context.workspace_key = "project-key";
        context.operation_id = "save-1";
        context.session_id = "s-1";
        context.source_event_ref = "workspace_key=project-key/session_id=s-1/run_id=r-1/event_id=evt-1";
        fs::create_directories(context.project_root);
        fs::create_directories(context.memory_directory.parent_path());
        Write(context.project_root / "build.txt", "build evidence v1\n");
        request.kind = memory::MemoryKind::Fact;
        request.id = "fact.build";
        request.title = "Build instruction";
        request.content = "Build the project using build.txt.";
        request.paths = {"build.txt"};
        request.source_session = context.source_event_ref;
    }
    ~Rig() { std::error_code ec; fs::remove_all(root, ec); }
    fs::path Op(const std::string& name) const { return context.lifecycle_root / name; }
    fs::path Op() const { return Op(context.operation_id); }
    fs::path Topic() const { return context.memory_directory / "facts" / "build.md"; }
};
std::expected<platform::AtomicWriteReceipt, platform::AtomicWriteError> Reject() {
    return std::unexpected(platform::AtomicWriteError{"atomic.tmp_write_failed", "injected before replacement", Outcome::NotCommitted});
}
struct FlushGuard {
    explicit FlushGuard(bool directory) : directory_(directory) {
        if (directory_) platform::SetDirectoryFlushFailureForTest(true);
        else platform::SetFileFlushFailureForTest(true);
    }
    ~FlushGuard() {
        if (directory_) platform::SetDirectoryFlushFailureForTest(false);
        else platform::SetFileFlushFailureForTest(false);
    }
    bool directory_;
};
void Rewrite(const fs::path& path, Json value) {
    value.erase("sha256");
    value["sha256"] = hooks::Sha256Hex(value.dump());
    Write(path, value.dump(2) + "\n");
}
bool Has(const memory::ProjectCommitReceipt& receipt, Stage stage, Outcome outcome) {
    return std::any_of(receipt.stages.begin(), receipt.stages.end(), [&](const auto& item) {
        return item.stage == stage && item.outcome == outcome;
    });
}
}  // namespace

TEST_CASE("project commit: typed create confirms every file and preserves CLI outcome") {
    Rig rig("create");
    const auto result = memory::CommitProjectUpsert(rig.context, rig.request);
    INFO(result.error_code, ": ", result.error);
    REQUIRE(result.state == State::Committed);
    CHECK_FALSE(result.duplicate);
    CHECK(result.memory_id == rig.request.id);
    CHECK(result.memory_path == "facts/build.md");
    CHECK(result.request_sha256.size() == 64);
    CHECK(result.content_sha256 == hooks::Sha256Hex(Read(rig.Topic())));
    CHECK(Read(rig.Op() / "topic.snapshot.md") == Read(rig.Topic()));
    for (const auto stage : {Stage::Intent, Stage::Snapshot, Stage::Topic, Stage::Catalog, Stage::Index, Stage::Result})
        CHECK(Has(result, stage, Outcome::CommittedDurable));
    const auto saved = Json::parse(Read(rig.Op() / "result.json"));
    CHECK(saved.at("outcome").at("memory_id").get<std::string>() == rig.request.id);
    CHECK(saved.at("outcome").at("committed_at").get<std::string>() == result.committed_at);
    const auto intent = Json::parse(Read(rig.Op() / "intent.json"));
    CHECK(intent.at("parameters").at("source_event_ref").get<std::string>() == rig.context.source_event_ref);
    CHECK(intent.at("parameters").at("session_id").get<std::string>() == rig.context.session_id);
    const auto view = memory::store::ReadProjectRecallSnapshot(rig.context.memory_directory, rig.context.project_root);
    REQUIRE(view.has_value());
    REQUIRE(view->entries.size() == 1);
    CHECK(view->entries[0].public_entry.id == result.memory_id);
    const auto catalog = Json::parse(Read(rig.context.memory_directory / ".state" / "catalog.json"));
    CHECK(catalog.at("entries")[0].at("paths").get<std::vector<std::string>>() == view->entries[0].public_entry.paths);
    CHECK(catalog.at("entries")[0].at("evidence")[0].at("path").get<std::string>() == "build.txt");
    CHECK(Read(rig.context.memory_directory / "index.md").find("facts/build.md") != std::string::npos);
    const auto cli_receipt = memory::ReadMemorySaveReceipt(rig.context.lifecycle_root,
                                                         rig.context.operation_id);
    CHECK(cli_receipt.committed);
    CHECK(cli_receipt.error.empty());
}

TEST_CASE("project commit: update keeps creation history and confirms old-name removal") {
    Rig rig("update");
    rig.request.occurred_at = "2025-01-02";
    REQUIRE(memory::CommitProjectUpsert(rig.context, rig.request).state == State::Committed);
    const auto first = memory::frontmatter::Parse(Read(rig.Topic()), true);
    REQUIRE(first.has_value());
    fs::rename(rig.Topic(), rig.Topic().parent_path() / "legacy-build.md");
    REQUIRE(memory::RebuildMemoryIndex(rig.context.memory_directory).has_value());
    rig.context.operation_id = "save-2";
    rig.request.content = "Updated build text.";
    rig.request.occurred_at.clear();
    rig.request.source_session = "workspace_key=project-key/session_id=s-2/run_id=r-2/event_id=evt-2";
    const auto updated = memory::CommitProjectUpsert(rig.context, rig.request);
    INFO(updated.error);
    REQUIRE(updated.state == State::Committed);
    CHECK(Has(updated, Stage::Cleanup, Outcome::CommittedDurable));
    CHECK_FALSE(fs::exists(rig.Topic().parent_path() / "legacy-build.md"));
    CHECK(fs::is_regular_file(rig.Topic().parent_path() / ".memory-commit-cleanup.json"));
    const auto parsed = memory::frontmatter::Parse(Read(rig.Topic()), true);
    REQUIRE(parsed.has_value());
    CHECK(parsed->entry.created_at == first->entry.created_at);
    CHECK(parsed->entry.occurred_at == first->entry.occurred_at);
    CHECK(parsed->entry.source_sessions.size() == 2);
    CHECK(parsed->body.find("2025-01-02") != std::string::npos);
    // A checksummed downgrade cannot erase cleanup or a visible topic stage.
    const auto path = rig.Op() / "result.json";
    const auto original = Json::parse(Read(path));
    for (int variant = 0; variant != 5; ++variant) {
        auto changed = original;
        if (variant == 0) std::erase_if(changed["stages"].get_ref<Json::array_t&>(), [](const auto& item) {
            return item.at("stage").template get<int>() == static_cast<int>(Stage::Cleanup);
        });
        if (variant == 1) changed["commit_state"] = "not_started";
        if (variant == 2) changed["commit_state"] = "indeterminate";
        if (variant == 3) changed["stages"][0]["outcome"] = static_cast<int>(Outcome::NotCommitted);
        if (variant == 4) changed["prepared"]["previous_file"] = "../escape.md";
        if (variant == 4) {
            auto intent = Json::parse(Read(rig.Op() / "intent.json"));
            intent["prepared"]["previous_file"] = "../escape.md";
            Rewrite(rig.Op() / "intent.json", std::move(intent));
        }
        Rewrite(path, std::move(changed));
        const auto rejected = memory::CommitProjectUpsert(rig.context, rig.request);
        CHECK(rejected.state == State::Indeterminate);
        CHECK(rejected.error_code == "memory.commit.invalid_record");
    }
}

TEST_CASE("project commit: duplicate uses immutable snapshot after newer topic update") {
    Rig rig("duplicate");
    const auto first_request = rig.request;
    const auto first_context = rig.context;
    const auto first = memory::CommitProjectUpsert(first_context, first_request);
    REQUIRE(first.state == State::Committed);
    const auto first_result = Read(rig.Op() / "result.json");
    const auto first_snapshot = Read(rig.Op() / "topic.snapshot.md");
    rig.context.operation_id = "save-2";
    rig.request.content = "Second accepted topic version.";
    REQUIRE(memory::CommitProjectUpsert(rig.context, rig.request).state == State::Committed);
    const auto current = Read(rig.Topic());
    Write(rig.context.project_root / "build.txt", "evidence changed after commit\n");
    int writes = 0;
    const auto duplicate = memory::commit_testing::CommitProjectUpsert(first_context, first_request,
        [&](const auto& path, auto bytes, auto durability) {
            ++writes;
            CHECK(path == rig.Op("save-1") / "result.json");
            return platform::AtomicWriteFile(path, bytes, durability);
        });
    REQUIRE(duplicate.state == State::Committed);
    CHECK(duplicate.duplicate);
    CHECK(writes == 1);
    CHECK(duplicate.content_sha256 == first.content_sha256);
    CHECK(duplicate.request_sha256 == first.request_sha256);
    CHECK(Read(rig.Topic()) == current);
    CHECK(Read(rig.Op("save-1") / "result.json") == first_result);
    CHECK(Read(rig.Op("save-1") / "topic.snapshot.md") == first_snapshot);
}

TEST_CASE("project commit: changed request and corrupt or legacy receipts never replay") {
    Rig rig("invalid-receipt");
    REQUIRE(memory::CommitProjectUpsert(rig.context, rig.request).state == State::Committed);
    const auto topic = Read(rig.Topic());
    SUBCASE("same key changed content conflicts") {
        rig.request.content = "Different request under the same key.";
        const auto result = memory::CommitProjectUpsert(rig.context, rig.request);
        CHECK(result.state == State::Indeterminate);
        CHECK(result.error_code == "memory.commit.request_conflict");
    }
    SUBCASE("broken result stays an error") {
        Write(rig.Op() / "result.json", "{broken");
        CHECK(memory::CommitProjectUpsert(rig.context, rig.request).error_code == "memory.commit.invalid_record");
    }
    SUBCASE("old schema1 intent has no request binding") {
        Write(rig.Op() / "intent.json", Json{{"schema_version", 1}, {"operation_id", rig.context.operation_id}}.dump());
        CHECK(memory::CommitProjectUpsert(rig.context, rig.request).error_code == "memory.commit.legacy_receipt");
    }
    SUBCASE("snapshot corruption rejects cached committed result") {
        Write(rig.Op() / "topic.snapshot.md", "changed immutable snapshot");
        CHECK(memory::CommitProjectUpsert(rig.context, rig.request).error_code == "memory.commit.snapshot_invalid");
    }
    CHECK(Read(rig.Topic()) == topic);
}

TEST_CASE("project commit: intent-only and post-topic crash windows forbid replay") {
    for (bool after_topic : {false, true}) {
        Rig rig(after_topic ? "crash-after-topic" : "crash-before-topic");
        const auto interrupted = memory::commit_testing::CommitProjectUpsert(rig.context, rig.request,
            [&](const auto& path, auto bytes, auto durability) -> std::expected<platform::AtomicWriteReceipt, platform::AtomicWriteError> {
                if (path.filename() == (after_topic ? "result.json" : "topic.snapshot.md")) throw std::runtime_error("simulated exit window");
                return platform::AtomicWriteFile(path, bytes, durability);
            });
        CHECK(interrupted.state == State::Indeterminate);
        REQUIRE(fs::is_regular_file(rig.Op() / "intent.json"));
        CHECK_FALSE(fs::exists(rig.Op() / "result.json"));
        CHECK(fs::exists(rig.Topic()) == after_topic);
        int replay_writes = 0;
        const auto replay = memory::commit_testing::CommitProjectUpsert(rig.context, rig.request,
            [&](const auto& path, auto bytes, auto durability) {
                ++replay_writes; return platform::AtomicWriteFile(path, bytes, durability);
            });
        CHECK(replay.state == State::Indeterminate);
        CHECK(replay.error_code == "memory.commit.intent_unresolved");
        CHECK(replay_writes == 0);
    }
}

TEST_CASE("project commit: file-flush failure preserves NotCommitted topic stage") {
    Rig rig("file-flush");
    const auto failed = memory::commit_testing::CommitProjectUpsert(rig.context, rig.request,
        [&](const auto& path, auto bytes, auto durability) {
            if (path == rig.Topic()) { FlushGuard guard(false); return platform::AtomicWriteFile(path, bytes, durability); }
            return platform::AtomicWriteFile(path, bytes, durability);
        });
    CHECK(failed.state == State::NotStarted);
    CHECK(Has(failed, Stage::Topic, Outcome::NotCommitted));
    CHECK(Has(failed, Stage::Result, Outcome::CommittedDurable));
    CHECK_FALSE(fs::exists(rig.Topic()));
    const auto duplicate = memory::CommitProjectUpsert(rig.context, rig.request);
    CHECK(duplicate.state == State::NotStarted);
    CHECK(duplicate.duplicate);
    CHECK_FALSE(fs::exists(rig.Topic()));
}

TEST_CASE("project commit: topic directory-flush failure remains visible and indeterminate") {
    Rig rig("topic-flush");
    const auto failed = memory::commit_testing::CommitProjectUpsert(rig.context, rig.request,
        [&](const auto& path, auto bytes, auto durability) {
            if (path == rig.Topic()) { FlushGuard guard(true); return platform::AtomicWriteFile(path, bytes, durability); }
            return platform::AtomicWriteFile(path, bytes, durability);
        });
    CHECK(failed.state == State::Indeterminate);
    CHECK(Has(failed, Stage::Topic, Outcome::CommittedDurabilityUnconfirmed));
    REQUIRE(fs::is_regular_file(rig.Topic()));
    const auto topic = Read(rig.Topic());
    const auto duplicate = memory::CommitProjectUpsert(rig.context, rig.request);
    CHECK(duplicate.state == State::Indeterminate);
    CHECK(duplicate.duplicate);
    CHECK(Read(rig.Topic()) == topic);
}

TEST_CASE("project commit: result directory-flush failure needs receipt-only confirmation") {
    Rig rig("result-flush");
    const auto failed = memory::commit_testing::CommitProjectUpsert(rig.context, rig.request,
        [&](const auto& path, auto bytes, auto durability) {
            if (path.filename() == "result.json") { FlushGuard guard(true); return platform::AtomicWriteFile(path, bytes, durability); }
            return platform::AtomicWriteFile(path, bytes, durability);
        });
    CHECK(failed.state == State::Indeterminate);
    CHECK(Has(failed, Stage::Result, Outcome::CommittedDurabilityUnconfirmed));
    const auto topic = Read(rig.Topic()), original = Read(rig.Op() / "result.json");
    const auto confirmed = memory::ConfirmProjectCommitReceipt(rig.context.lifecycle_root, rig.context.operation_id);
    REQUIRE(confirmed.state == State::Committed);
    CHECK(confirmed.duplicate);
    CHECK(Read(rig.Topic()) == topic);
    CHECK(Read(rig.Op() / "result.json") == original);
    CHECK(memory::ConfirmProjectCommitReceipt(rig.context.lifecycle_root, "not-present").state == State::Indeterminate);
    CHECK_FALSE(fs::exists(rig.Op("not-present")));
}

TEST_CASE("project commit: derived write failure preserves changed topic without replay") {
    Rig rig("derived-failure");
    const auto failed = memory::commit_testing::CommitProjectUpsert(rig.context, rig.request,
        [&](const auto& path, auto bytes, auto durability) {
            if (path.filename() == "catalog.json") return Reject();
            return platform::AtomicWriteFile(path, bytes, durability);
        });
    CHECK(failed.state == State::Indeterminate);
    CHECK(Has(failed, Stage::Catalog, Outcome::NotCommitted));
    CHECK(Has(failed, Stage::Result, Outcome::CommittedDurable));
    REQUIRE(fs::is_regular_file(rig.Topic()));
    const auto topic = Read(rig.Topic());
    CHECK(memory::CommitProjectUpsert(rig.context, rig.request).state == State::Indeterminate);
    CHECK(Read(rig.Topic()) == topic);
    CHECK_FALSE(fs::exists(rig.context.memory_directory / "index.md"));
}

TEST_CASE("project commit: cancellation and live project lock retain ownership") {
    Rig rig("cancel-lock");
    SUBCASE("pre-cancel does not create intent") {
        std::stop_source stop; stop.request_stop();
        const auto cancelled = memory::CommitProjectUpsert(rig.context, rig.request, stop.get_token());
        CHECK(cancelled.state == State::NotStarted);
        CHECK(cancelled.error_code == "memory.commit.cancelled");
        CHECK_FALSE(fs::exists(rig.Op() / "intent.json"));
    }
    SUBCASE("existing atomic flag cancels without a monitor thread") {
        std::atomic<bool> cancelled{true};
        const auto result = memory::CommitProjectUpsertWithCancellation(rig.context, rig.request,
            [&cancelled] { return cancelled.load(std::memory_order_acquire); });
        CHECK(result.state == State::NotStarted);
        CHECK(result.error_code == "memory.commit.cancelled");
        CHECK_FALSE(fs::exists(rig.Op() / "intent.json"));
    }
    SUBCASE("cancellation after snapshot persists honest no-topic result") {
        std::stop_source stop;
        const auto cancelled = memory::commit_testing::CommitProjectUpsert(rig.context, rig.request,
            [&](const auto& path, auto bytes, auto durability) {
                auto written = platform::AtomicWriteFile(path, bytes, durability);
                if (path.filename() == "topic.snapshot.md") stop.request_stop();
                return written;
            }, stop.get_token());
        CHECK(cancelled.state == State::NotStarted);
        CHECK(cancelled.error_code == "memory.commit.cancelled");
        CHECK_FALSE(fs::exists(rig.Topic()));
        CHECK(memory::CommitProjectUpsert(rig.context, rig.request).state == State::NotStarted);
    }
    SUBCASE("live holder remains owned while cancellation stops bounded wait") {
        memory::OwnerLock holder;
        const auto path = rig.context.memory_directory / ".state" / "memory.lock";
        REQUIRE(memory::OwnerLock::TryAcquire(path, &holder).status == memory::OwnerLock::Status::Acquired);
        std::stop_source stop;
        std::jthread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); stop.request_stop(); });
        const auto cancelled = memory::CommitProjectUpsert(rig.context, rig.request, stop.get_token());
        CHECK(cancelled.state == State::NotStarted);
        CHECK(cancelled.error_code == "memory.commit.cancelled");
        CHECK(holder.holds());
        CHECK(memory::OwnerLock::HolderAlive(path));
        CHECK_FALSE(fs::exists(rig.Op() / "intent.json"));
    }
}

TEST_CASE("project commit: strict project inputs reject malformed paths and bounded evidence") {
    Rig rig("strict-input");
    SUBCASE("user scope stays outside project gate") { rig.request.scope.level = "user"; rig.request.scope.kind = "user"; }
    SUBCASE("NUL is rejected before any intent") { rig.request.content.push_back('\0'); }
    SUBCASE("invalid UTF8 is rejected before any intent") { rig.request.title.push_back(static_cast<char>(0xff)); }
    SUBCASE("nonregular evidence is not an absent fingerprint") { fs::create_directory(rig.context.project_root / "blocked"); rig.request.paths = {"blocked"}; }
    SUBCASE("oversized evidence is rejected before intent") { Write(rig.context.project_root / "large", std::string(16 * 1024 * 1024 + 1, 'x')); rig.request.paths = {"large"}; }
    SUBCASE("bad catalog is not silently repaired") { Write(rig.context.memory_directory / ".state" / "catalog.json", "{broken"); }
    const auto rejected = memory::CommitProjectUpsert(rig.context, rig.request);
    CHECK(rejected.state == State::NotStarted);
    CHECK_FALSE(rejected.error_code.empty());
    CHECK_FALSE(fs::exists(rig.Op() / "intent.json"));
    CHECK_FALSE(fs::exists(rig.Topic()));
}

TEST_CASE("project commit: CLI worker shares project gate and retains other job receipts") {
    Rig rig("cli-worker");
    const auto home = rig.root / "home";
    const auto pending = home / "memory-jobs" / "pending";
    auto job = Json{{"schema", 1}, {"operation", "upsert"}, {"workspace_key", rig.context.workspace_key},
                    {"project_root", memory::PathUtf8(rig.context.project_root)},
                    {"memory_dir", memory::PathUtf8(rig.context.memory_directory)}, {"kind", "fact"},
                    {"id", rig.request.id}, {"title", rig.request.title}, {"content", rig.request.content},
                    {"paths", Json::array({"build.txt"})}};
    Write(pending / "10000-1.json", job.dump());
    const auto first = memory::RunPendingMemoryJobs(home);
    REQUIRE(first.has_value());
    CHECK(*first == 1);
    const auto result_path = rig.Op("memsave-10000-1") / "result.json";
    const auto result_text = Read(result_path);
    const auto result = Json::parse(result_text);
    CHECK(result.at("commit_schema").get<int>() == 1);
    CHECK(result.at("commit_state").get<std::string>() == "committed");
    const auto topic_text = Read(rig.Topic());
    Write(pending / "10000-1.json", job.dump());
    const auto duplicate = memory::RunPendingMemoryJobs(home);
    REQUIRE(duplicate.has_value());
    CHECK(*duplicate == 1);
    CHECK(Read(result_path) == result_text);
    CHECK(Read(rig.Topic()) == topic_text);
    // A source request cannot be silently replaced under the same queue key.
    job["content"] = "Changed same job key.";
    Write(pending / "10000-1.json", job.dump());
    const auto conflict = memory::RunPendingMemoryJobs(home);
    REQUIRE(conflict.has_value());
    CHECK(*conflict == 0);
    CHECK(fs::is_regular_file(home / "memory-jobs" / "failed" / "10000-1.json"));
    CHECK(Read(home / "memory-jobs" / "failed" / "10000-1.json.error.txt").find("memory.commit.indeterminate") != std::string::npos);
    CHECK(Read(rig.Topic()) == topic_text);
    job["operation"] = "rebuild";
    Write(pending / "10000-2.json", job.dump());
    REQUIRE(memory::RunPendingMemoryJobs(home).has_value());
    const auto rebuilt = Json::parse(Read(rig.Op("memsave-10000-2") / "result.json"));
    CHECK_FALSE(rebuilt.contains("commit_schema"));
    CHECK(rebuilt.at("outcome").contains("committed_at"));
    job["operation"] = "upsert";
    job["kind"] = "preference"; job["id"] = "preference.build";
    job["memory_dir"] = memory::PathUtf8(home / "memory" / "user");
    job["scope"] = {{"level", "user"}, {"kind", "user"}, {"value", ""}};
    job["paths"] = Json::array();
    Write(pending / "10000-3.json", job.dump());
    const auto user = memory::RunPendingMemoryJobs(home);
    REQUIRE(user.has_value());
    CHECK(*user == 1);
    const auto user_result = Json::parse(Read(home / "memory" / "user" / ".state" / "lifecycle" / "memsave-10000-3" / "result.json"));
    CHECK_FALSE(user_result.contains("commit_schema"));
    CHECK(user_result.at("outcome").contains("committed_at"));
}
