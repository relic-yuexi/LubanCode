#include "memory/project_commit.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "hooks/hash.hpp"
#include "memory/frontmatter.hpp"
#include "memory/internal.hpp"
#include "memory/topic_store.hpp"
#include "platform/bounded_read.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::memory {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Stage = ProjectCommitStage;
using State = ProjectCommitState;
using Outcome = platform::WriteOutcome;
constexpr auto kDurability = platform::WriteDurability::ProcessCrashDurability;
constexpr std::size_t kRecordBytes = 128 * 1024, kTopicBytes = 16 * 1024;

bool SafeText(const std::string& text, std::size_t cap) {
    return text.size() <= cap && text.find('\0') == std::string::npos && platform::IsValidUtf8(text);
}
bool SafeKey(const std::string& key) {
    return !key.empty() && key.size() <= 128 && key != "." && key != ".." &&
           std::all_of(key.begin(), key.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
           });
}
bool Absent(const fs::path& path, std::error_code& ec) {
    const auto status = fs::symlink_status(path, ec);
    return ec == std::errc::no_such_file_or_directory ||
           (!ec && status.type() == fs::file_type::not_found);
}

// Local handoff supplements, never replaces, the on-disk OwnerLock. Weak slots
// retain neither Sessions nor cancellation/write callbacks. Separate copies of
// this implementation (e.g. another DSO) still synchronize via OwnerLock.
struct ProjectHandoffTarget { fs::path memory, workspace; };
struct ProjectHandoffWaiter { std::thread::id thread = std::this_thread::get_id(); };
struct ProjectHandoffSlot {
    explicit ProjectHandoffSlot(ProjectHandoffTarget value) : target(std::move(value)) {}
    const ProjectHandoffTarget target;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<ProjectHandoffWaiter>> waiting;
    std::thread::id holder;
    bool held = false;
};
struct ProjectHandoffRegistry {
    std::mutex mutex;
    std::vector<std::weak_ptr<ProjectHandoffSlot>> slots;
    // A snapshot pins its epoch, so address reuse cannot pass the publication
    // check. Pruning dead weak slots needs no epoch: none can have live waiters.
    std::shared_ptr<const unsigned char> epoch = std::make_shared<const unsigned char>(0);
};

std::expected<ProjectHandoffTarget, std::string> HandoffTarget(const fs::path& memory,
                                                             const fs::path& workspace) {
    std::error_code error;
    if (Absent(memory, error)) return ProjectHandoffTarget{workspace / "memory", workspace};
    if (error || !fs::is_directory(fs::symlink_status(memory, error)) || error)
        return std::unexpected("memory.commit.invalid_directory");
    auto real = fs::canonical(memory, error);
    if (error || !IsWithin(real, workspace)) return std::unexpected("memory.commit.path_escape");
    return ProjectHandoffTarget{std::move(real), workspace};
}

std::expected<bool, std::string> SameHandoffTarget(const ProjectHandoffTarget& a,
                                                 const ProjectHandoffTarget& b) {
    if (a.memory == b.memory) return true;
    const auto present = [](const fs::path& path) -> std::expected<bool, std::string> {
        std::error_code status_error;
        const auto status = fs::symlink_status(path, status_error);
        if (status_error == std::errc::no_such_file_or_directory ||
            (!status_error && status.type() == fs::file_type::not_found)) return false;
        if (status_error || !fs::is_directory(status))
            return std::unexpected("memory.commit.handoff_identity_failed");
        return true;
    };
    const auto a_present = present(a.memory), b_present = present(b.memory);
    if (!a_present || !b_present)
        return std::unexpected("memory.commit.handoff_identity_failed");
    std::error_code error;
    // equivalent() requires existing paths; libc++ need not report ENOENT for
    // an absent leaf. Detect absence through status, never an arbitrary error.
    if (*a_present && *b_present) {
        const bool same = fs::equivalent(a.memory, b.memory, error);
        if (error) return std::unexpected("memory.commit.handoff_identity_failed");
        return same;
    }
    // Before mkdir, canonical parents are the actual existing identity. This
    // also joins Windows case/short-path aliases without guessing a case fold.
    if (a.memory.filename() != "memory" || b.memory.filename() != "memory")
        return std::unexpected("memory.commit.handoff_identity_failed");
    const bool parents = fs::equivalent(a.workspace, b.workspace, error);
    if (error) return std::unexpected("memory.commit.handoff_identity_failed");
    return parents;
}

std::expected<std::shared_ptr<ProjectHandoffSlot>, std::string> HandoffSlot(ProjectHandoffTarget target) {
    static ProjectHandoffRegistry registry;
    for (;;) {
        std::vector<std::shared_ptr<ProjectHandoffSlot>> snapshot;
        std::shared_ptr<const unsigned char> epoch;
        {
            std::lock_guard lock(registry.mutex);
            std::erase_if(registry.slots, [](const auto& weak) { return weak.expired(); });
            snapshot.reserve(registry.slots.size());
            for (const auto& weak : registry.slots) if (auto live = weak.lock()) snapshot.push_back(std::move(live));
            epoch = registry.epoch;
        }
        for (const auto& slot : snapshot) {
            auto same = SameHandoffTarget(target, slot->target); // filesystem outside registry lock
            if (!same) return std::unexpected(same.error());
            if (*same) return slot;
        }
        auto candidate = std::make_shared<ProjectHandoffSlot>(target);
        auto next_epoch = std::make_shared<const unsigned char>(0);
        {
            std::lock_guard lock(registry.mutex);
            if (registry.epoch != epoch) continue; // another publisher may have used an equivalent spelling
            registry.slots.push_back(candidate);
            registry.epoch = std::move(next_epoch);
        }
        return candidate;
    }
}

class ProjectHandoffLease final {
public:
    ProjectHandoffLease(ProjectHandoffLease&& other) noexcept
        : slot_(std::move(other.slot_)), waiter_(std::move(other.waiter_)), held_(std::exchange(other.held_, false)) {}
    ProjectHandoffLease(const ProjectHandoffLease&) = delete;
    ProjectHandoffLease& operator=(const ProjectHandoffLease&) = delete;
    ~ProjectHandoffLease() {
        if (!slot_) return;
        {
            std::lock_guard lock(slot_->mutex);
            if (held_) { slot_->held = false; slot_->holder = {}; }
            else std::erase(slot_->waiting, waiter_);
        }
        slot_->changed.notify_all();
    }
    static std::expected<ProjectHandoffLease, std::string> Acquire(
        ProjectHandoffTarget target, const ProjectCommitCancellation& cancelled) {
        auto found = HandoffSlot(std::move(target));
        if (!found) return std::unexpected(found.error());
        auto slot = std::move(*found);
        auto waiter = std::make_shared<ProjectHandoffWaiter>();
        {
            std::lock_guard lock(slot->mutex);
            if ((slot->held && slot->holder == waiter->thread) ||
                std::any_of(slot->waiting.begin(), slot->waiting.end(), [&](const auto& item) { return item->thread == waiter->thread; }))
                return std::unexpected("memory.commit.reentrant");
            slot->waiting.push_back(waiter);
        }
        ProjectHandoffLease lease(std::move(slot), std::move(waiter));
        for (;;) {
            // The queued ticket already exists when arbitrary cancellation code
            // runs, so a same-thread reentry is refused instead of waiting on us.
            if (cancelled && cancelled()) return std::unexpected("memory.commit.cancelled");
            std::unique_lock lock(lease.slot_->mutex);
            if (!lease.slot_->held && lease.slot_->waiting.front() == lease.waiter_) {
                lease.slot_->waiting.pop_front();
                lease.slot_->held = true; lease.slot_->holder = lease.waiter_->thread; lease.held_ = true;
                return lease;
            }
            lease.slot_->changed.wait_for(lock, std::chrono::milliseconds(10));
        }
    }
private:
    ProjectHandoffLease(std::shared_ptr<ProjectHandoffSlot> slot, std::shared_ptr<ProjectHandoffWaiter> waiter) noexcept
        : slot_(std::move(slot)), waiter_(std::move(waiter)) {}
    std::shared_ptr<ProjectHandoffSlot> slot_;
    std::shared_ptr<ProjectHandoffWaiter> waiter_;
    bool held_ = false;
};

std::expected<void, std::string> Directory(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    if (Absent(path, ec)) {
        ec.clear();
        fs::create_directories(path, ec);
        if (ec) return std::unexpected("memory.commit.mkdir_failed");
    } else if (ec) {
        return std::unexpected("memory.commit.invalid_directory");
    }
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::is_directory(status)) return std::unexpected("memory.commit.invalid_directory");
    const auto real = fs::canonical(path, ec);
    if (ec || !IsWithin(real, root)) return std::unexpected("memory.commit.path_escape");
    return {};
}
std::expected<std::optional<std::string>, std::string> ReadOptional(const fs::path& path,
                                                                  std::size_t cap) {
    std::error_code ec;
    if (Absent(path, ec)) return std::optional<std::string>{};
    if (ec) return std::unexpected("memory.commit.read_failed");
    auto bytes = platform::ReadBoundedRegularFile(path, cap);
    if (!bytes) return std::unexpected("memory.commit." + bytes.error());
    if (!SafeText(*bytes, cap)) return std::unexpected("memory.commit.invalid_text");
    return std::optional<std::string>{std::move(*bytes)};
}
std::expected<void, std::string> WriteTarget(const fs::path& path) {
    std::error_code ec;
    if (Absent(path, ec)) return {};
    const auto status = fs::symlink_status(path, ec);
    if (ec || !fs::is_regular_file(status)) return std::unexpected("memory.commit.invalid_write_target");
    return {};
}
Json SaveIdentity(const ProjectCommitContext& context, const SaveRequest& request) {
    Json evidence = Json::array();
    for (const auto& item : request.evidence) evidence.push_back({{"path", item.path}, {"symbol", item.symbol}});
    return {{"project_root", PathUtf8(AbsoluteNormal(context.project_root))},
            {"memory_dir", PathUtf8(AbsoluteNormal(context.memory_directory))},
            {"lifecycle_root", PathUtf8(AbsoluteNormal(context.lifecycle_root))},
            {"workspace_key", context.workspace_key}, {"session_id", context.session_id},
            {"source_event_ref", context.source_event_ref}, {"job_operation", "upsert"},
            {"kind", MemoryKindName(request.kind)}, {"id", request.id}, {"title", request.title},
            {"summary", request.summary}, {"content", request.content}, {"keywords", request.keywords},
            {"paths", request.paths}, {"source_session", request.source_session},
            {"confidence", request.confidence}, {"expires_at", request.expires_at},
            {"occurred_at", request.occurred_at},
            {"scope", {{"level", request.scope.level}, {"kind", request.scope.kind}, {"value", request.scope.value}}},
            {"evidence", std::move(evidence)}};
}
std::string Seal(Json value) {
    value["sha256"] = hooks::Sha256Hex(value.dump());
    return value.dump(2) + "\n";
}
std::expected<Json, std::string> OpenRecord(const std::string& text) {
    auto value = Json::parse(text, nullptr, false);
    if (value.is_object() && !value.contains("commit_schema") && value.value("schema_version", 0) == 1)
        return std::unexpected("memory.commit.legacy_receipt");
    if (!value.is_object() || !value.contains("sha256") || !value["sha256"].is_string())
        return std::unexpected("memory.commit.invalid_record");
    const auto checksum = value["sha256"].get<std::string>();
    value.erase("sha256");
    if (checksum != hooks::Sha256Hex(value.dump())) return std::unexpected("memory.commit.invalid_record");
    if (!value.contains("commit_schema") || !value["commit_schema"].is_number_integer() || value["commit_schema"] != 1 ||
        !value.contains("schema_version") || !value["schema_version"].is_number_integer() || value["schema_version"] != 1)
        return std::unexpected("memory.commit.invalid_record");
    return value;
}
std::int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
const char* StateName(State state) {
    switch (state) {
        case State::NotStarted: return "not_started";
        case State::Committed: return "committed";
        case State::Indeterminate: return "indeterminate";
    }
    return "indeterminate";
}
Json StageValues(const ProjectCommitReceipt& receipt) {
    auto stages = Json::array();
    for (const auto& item : receipt.stages)
        stages.push_back({{"stage", static_cast<int>(item.stage)}, {"outcome", static_cast<int>(item.outcome)}});
    return stages;
}
bool IdentityMatches(const Json& value, const ProjectCommitReceipt& receipt) {
    return value.value("schema_version", 0) == 1 &&
           value.value("operation_id", std::string()) == receipt.operation_id &&
           value.value("workspace_key", std::string()) == receipt.workspace_key &&
           value.value("request_sha256", std::string()) == receipt.request_sha256;
}
std::expected<Json, std::string> Fingerprints(const SaveRequest& request, const fs::path& project) {
    auto fingerprints = Json::object();
    std::set<std::string> paths(request.paths.begin(), request.paths.end());
    for (const auto& item : request.evidence) paths.insert(item.path);
    if (paths.size() > 24) return std::unexpected("memory.commit.evidence_limit");
    constexpr std::size_t kSingle = 16 * 1024 * 1024, kTotal = 64 * 1024 * 1024;
    std::size_t used = 0;
    for (const auto& relative : paths) {
        const auto path = project / Utf8Path(relative);
        std::error_code ec;
        bool missing = false;
        auto prefix = project;
        for (const auto& component : Utf8Path(relative)) {
            prefix /= component;
            if (Absent(prefix, ec)) { missing = true; break; }
            const auto resolved = fs::canonical(prefix, ec);
            if (ec || !IsWithin(resolved, project)) return std::unexpected("memory.commit.evidence_path");
            if (prefix != path && !fs::is_directory(resolved)) return std::unexpected("memory.commit.evidence_path");
        }
        if (missing) continue;  // Preserve absent evidence, not broken links.
        const auto real = fs::canonical(path, ec);
        if (ec || !IsWithin(real, project)) return std::unexpected("memory.commit.evidence_path");
        auto bytes = platform::ReadBoundedRegularFile(real, std::min(kSingle, kTotal - used));
        if (!bytes) return std::unexpected("memory.commit." + bytes.error());
        used += bytes->size();
        std::ostringstream hash;
        hash << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << StableHash(*bytes);
        fingerprints[relative] = hash.str();
    }
    return fingerprints;
}

ProjectCommitReceipt Run(const ProjectCommitContext& context, const SaveRequest& request,
                         const commit_testing::WriteFile& write, const ProjectCommitCancellation& cancelled,
                         bool existing_only = false, bool inspect_only = false) {
    ProjectCommitReceipt receipt;
    receipt.operation_id = context.operation_id;
    receipt.workspace_key = context.workspace_key;
    bool topic_attempted = false;
    bool intent_visible = false;
    const auto fail = [&](std::string code, std::string detail = {}) {
        receipt.error_code = std::move(code);
        receipt.error = detail.empty() ? receipt.error_code : std::move(detail);
    };
    try {
        if (!SafeKey(context.operation_id) || !SafeText(context.workspace_key, 256) || context.workspace_key.empty() ||
            !SafeText(context.session_id, 2048) || !SafeText(context.source_event_ref, 4096) ||
            request.scope.level != "project" ||
            (request.kind != MemoryKind::Fact && request.kind != MemoryKind::Preference && request.kind != MemoryKind::Feedback)) {
            fail("memory.commit.invalid_request"); return receipt;
        }
        if (auto valid = store::ValidateSaveRequest(request); !valid) {
            fail("memory.commit.invalid_request", valid.error()); return receipt;
        }
        if (!SafeText(request.id + request.title + request.summary + request.content + request.source_session +
                      request.confidence + request.expires_at + request.occurred_at + request.scope.kind + request.scope.value, 16 * 1024)) {
            fail("memory.commit.invalid_text"); return receipt;
        }
        for (const auto& item : request.keywords) if (!SafeText(item, 256)) { fail("memory.commit.invalid_text"); return receipt; }
        for (const auto& item : request.paths) if (!SafeText(item, 1024)) { fail("memory.commit.invalid_text"); return receipt; }
        for (const auto& item : request.evidence)
            if (!SafeText(item.path, 1024) || !SafeText(item.symbol, 1024)) { fail("memory.commit.invalid_text"); return receipt; }
        const auto memory = AbsoluteNormal(context.memory_directory);
        const auto lifecycle = AbsoluteNormal(context.lifecycle_root);
        if (context.memory_directory.empty() || context.lifecycle_root.empty() || context.project_root.empty() ||
            memory.filename() != "memory" || lifecycle != memory.parent_path() / "lifecycle") {
            fail("memory.commit.invalid_target"); return receipt;
        }
        std::error_code ec;
        const auto workspace = fs::canonical(memory.parent_path(), ec);
        if (ec || !fs::is_directory(workspace)) { fail("memory.commit.invalid_target"); return receipt; }
        const auto project = fs::canonical(context.project_root, ec);
        if (ec || !fs::is_directory(project)) { fail("memory.commit.invalid_project"); return receipt; }
        const auto identity = SaveIdentity(context, request);
        if (!SafeText(identity.dump(), 64 * 1024)) { fail("memory.commit.invalid_text"); return receipt; }
        receipt.request_sha256 = hooks::Sha256Hex(identity.dump());
        auto target = HandoffTarget(memory, workspace);
        if (!target) { fail(target.error()); return receipt; }
        // Declared before OwnerLock: its actual on-disk Release finishes before
        // the next local ticket may enter TryAcquire. All mutations stay inside.
        auto handoff = ProjectHandoffLease::Acquire(std::move(*target), cancelled);
        if (!handoff) { fail(handoff.error()); return receipt; }
        for (const auto& directory : {memory, memory / ".state", lifecycle}) {
            if (inspect_only) {
                const auto status = fs::symlink_status(directory, ec);
                if (ec || !fs::is_directory(status)) { fail("memory.commit.receipt_missing"); return receipt; }
                const auto real = fs::canonical(directory, ec);
                if (ec || !IsWithin(real, workspace)) { fail("memory.commit.path_escape"); return receipt; }
            } else if (auto valid = Directory(directory, workspace); !valid) { fail(valid.error()); return receipt; }
        }
        OwnerLock lock;
        for (int attempt = 0; attempt != 20; ++attempt) {
            if (cancelled && cancelled()) { fail("memory.commit.cancelled"); return receipt; }
            const auto acquired = OwnerLock::TryAcquire(memory / ".state" / "memory.lock", &lock);
            if (acquired.status == OwnerLock::Status::Acquired) break;
            if (acquired.status != OwnerLock::Status::HeldByLiveHolder || attempt == 19) {
                fail("memory.commit.lock_refused", ProjectLockRefusal(acquired, "项目记忆")); return receipt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const auto operation_dir = lifecycle / Utf8Path(context.operation_id);
        if (inspect_only) {
            const auto status = fs::symlink_status(operation_dir, ec);
            if (ec || !fs::is_directory(status)) { fail("memory.commit.receipt_missing"); return receipt; }
            const auto real = fs::canonical(operation_dir, ec);
            if (ec || !IsWithin(real, workspace)) { fail("memory.commit.path_escape"); return receipt; }
        } else if (auto valid = Directory(operation_dir, workspace); !valid) { fail(valid.error()); return receipt; }
        const auto intent_path = operation_dir / "intent.json", result_path = operation_dir / "result.json";
        const auto snapshot_path = operation_dir / "topic.snapshot.md";
        const auto saved_intent = ReadOptional(intent_path, kRecordBytes);
        const auto saved_result = ReadOptional(result_path, kRecordBytes);
        if (!saved_intent || !saved_result) {
            receipt.state = State::Indeterminate;
            fail(!saved_intent ? saved_intent.error() : saved_result.error()); return receipt;
        }
        if (saved_intent->has_value() || saved_result->has_value()) {
            receipt.state = State::Indeterminate;
            intent_visible = true;  // Even a malformed old record forbids replay.
            if (!saved_intent->has_value()) { fail("memory.commit.orphan_result"); return receipt; }
            const auto intent = OpenRecord(**saved_intent);
            if (!intent) { fail(intent.error()); return receipt; }
            if (intent->at("operation") != "memory_save" || intent->at("session_id") != context.session_id ||
                !intent->at("requested_at_ms").is_number_integer() || intent->at("requested_at_ms").get<std::int64_t>() < 0) {
                fail("memory.commit.invalid_record"); return receipt;
            }
            if (!IdentityMatches(*intent, receipt) || intent->at("parameters") != identity) {
                fail("memory.commit.request_conflict"); return receipt;
            }
            if (!saved_result->has_value()) { fail("memory.commit.intent_unresolved"); return receipt; }
            const auto result = OpenRecord(**saved_result);
            if (!result) { fail(result.error()); return receipt; }
            if (result->at("status") != "completed" || !result->at("completed_at_ms").is_number_integer() ||
                result->at("completed_at_ms").get<std::int64_t>() < 0) {
                fail("memory.commit.invalid_record"); return receipt;
            }
            if (!IdentityMatches(*result, receipt) || result->at("prepared") != intent->at("prepared")) {
                fail("memory.commit.invalid_record"); return receipt;
            }
            const auto& prepared = result->at("prepared");
            receipt.memory_id = prepared.at("memory_id").get<std::string>();
            receipt.memory_path = prepared.at("memory_path").get<std::string>();
            receipt.content_sha256 = prepared.at("content_sha256").get<std::string>();
            receipt.committed_at = prepared.at("committed_at").get<std::string>();
            const auto previous_file = prepared.at("previous_file").get<std::string>();
            const auto expected_name = store::PrepareUpsert(request, {}, Json::object(), "");
            if (receipt.memory_id != expected_name.entry.public_entry.id ||
                receipt.memory_path != expected_name.entry.public_entry.file ||
                !SafeText(previous_file, 1024) || (!previous_file.empty() && !IsSafeRelativePath(previous_file)) ||
                !prepared.at("bytes").is_number_unsigned() || prepared.at("bytes").get<std::size_t>() > kTopicBytes ||
                !SafeText(receipt.committed_at, 64) || !LooksLikeMemoryDate(receipt.committed_at) ||
                receipt.content_sha256.size() != 64 ||
                !std::all_of(receipt.content_sha256.begin(), receipt.content_sha256.end(), [](unsigned char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                })) { fail("memory.commit.invalid_record"); return receipt; }
            const auto state = result->at("commit_state").get<std::string>();
            if (state != "committed" && state != "not_started" && state != "indeterminate") {
                fail("memory.commit.invalid_record"); return receipt;
            }
            if (!result->at("stages").is_array() || result->at("stages").size() > 6) {
                fail("memory.commit.invalid_record"); return receipt;
            }
            int previous = -1;
            for (const auto& item : result->at("stages")) {
                if (!item.is_object() || !item.contains("stage") || !item.contains("outcome") ||
                    !item["stage"].is_number_integer() || !item["outcome"].is_number_integer()) {
                    fail("memory.commit.invalid_record"); return receipt;
                }
                const auto stage = item.at("stage").get<std::int64_t>(), outcome = item.at("outcome").get<std::int64_t>();
                if (stage <= previous || stage < 0 || stage >= static_cast<int>(Stage::Result) ||
                    outcome < 0 || outcome > static_cast<int>(Outcome::CommittedDurable) ||
                    (state == "committed" && outcome != static_cast<int>(Outcome::CommittedDurable))) {
                    fail("memory.commit.invalid_record"); return receipt;
                }
                previous = static_cast<int>(stage);
                receipt.stages.push_back({static_cast<Stage>(stage), static_cast<Outcome>(outcome)});
            }
            if (receipt.stages.empty() || receipt.stages.front().stage != Stage::Intent ||
                receipt.stages.front().outcome != Outcome::CommittedDurable) {
                fail("memory.commit.invalid_record"); return receipt;
            }
            std::vector<Stage> expected_stages{Stage::Intent, Stage::Snapshot, Stage::Topic};
            if (!previous_file.empty() && previous_file != receipt.memory_path) expected_stages.push_back(Stage::Cleanup);
            expected_stages.push_back(Stage::Catalog); expected_stages.push_back(Stage::Index);
            for (std::size_t i = 0; i != receipt.stages.size(); ++i) {
                if (i >= expected_stages.size() || receipt.stages[i].stage != expected_stages[i] ||
                    (receipt.stages[i].outcome != Outcome::CommittedDurable && i + 1 != receipt.stages.size())) {
                    fail("memory.commit.invalid_record"); return receipt;
                }
            }
            const auto visible_topic = std::any_of(receipt.stages.begin(), receipt.stages.end(), [](const auto& item) {
                return item.stage == Stage::Topic && item.outcome != Outcome::NotCommitted;
            });
            const bool complete = receipt.stages.size() == expected_stages.size() &&
                std::all_of(receipt.stages.begin(), receipt.stages.end(), [](const auto& item) { return item.outcome == Outcome::CommittedDurable; });
            const auto derived_state = complete ? "committed" : visible_topic ? "indeterminate" : "not_started";
            if (state != derived_state) { fail("memory.commit.invalid_record"); return receipt; }
            if (state == "committed") {
                for (auto needed : {Stage::Intent, Stage::Snapshot, Stage::Topic, Stage::Catalog, Stage::Index})
                    if (std::none_of(receipt.stages.begin(), receipt.stages.end(), [&](const auto& item) { return item.stage == needed; })) {
                        fail("memory.commit.invalid_record"); return receipt;
                    }
                if (!previous_file.empty() && previous_file != receipt.memory_path &&
                    std::none_of(receipt.stages.begin(), receipt.stages.end(), [](const auto& item) { return item.stage == Stage::Cleanup; })) {
                    fail("memory.commit.invalid_record"); return receipt;
                }
                const auto snapshot = ReadOptional(snapshot_path, kTopicBytes);
                if (!snapshot || !snapshot->has_value() || hooks::Sha256Hex(**snapshot) != receipt.content_sha256 ||
                    prepared.at("bytes").get<std::size_t>() != (**snapshot).size() ||
                    result->at("outcome").at("content_sha256") != receipt.content_sha256 ||
                    result->at("outcome").at("memory_id") != receipt.memory_id ||
                    result->at("outcome").at("memory_path") != receipt.memory_path ||
                    result->at("outcome").at("committed_at") != receipt.committed_at) {
                    fail("memory.commit.snapshot_invalid"); return receipt;
                }
            } else {
                receipt.error_code = result->at("outcome").at("stable_error_code").get<std::string>();
                receipt.error = result->at("outcome").at("error").get<std::string>();
                if (receipt.error_code.empty() || !SafeText(receipt.error_code, 256) || !SafeText(receipt.error, 8192) ||
                    result->at("outcome").contains("committed_at")) {
                    fail("memory.commit.invalid_record"); return receipt;
                }
            }
            // A visible result from a previous uncertain directory flush must
            // confirm durability now. This reflushes only the same receipt bytes,
            // never repeats a topic mutation or replaces the original outcome.
            if (inspect_only) {
                receipt.state = state == "committed" ? State::Committed : state == "not_started" ? State::NotStarted : State::Indeterminate;
                receipt.duplicate = true;
                return receipt;
            }
            const auto confirmed = write(result_path, **saved_result, kDurability);
            receipt.stages.push_back({Stage::Result, confirmed ? confirmed->outcome : confirmed.error().outcome});
            if (!confirmed || confirmed->outcome != Outcome::CommittedDurable) {
                fail("memory.commit.result_unconfirmed", confirmed ? "result durability not confirmed" : confirmed.error().message); return receipt;
            }
            receipt.state = state == "committed" ? State::Committed : state == "not_started" ? State::NotStarted : State::Indeterminate;
            receipt.duplicate = true;
            return receipt;
        }
        if (existing_only) { fail("memory.commit.receipt_missing"); return receipt; }
        const auto orphan_snapshot = ReadOptional(snapshot_path, kTopicBytes);
        if (!orphan_snapshot || orphan_snapshot->has_value()) {
            receipt.state = State::Indeterminate;
            fail(orphan_snapshot ? "memory.commit.orphan_snapshot" : orphan_snapshot.error()); return receipt;
        }
        if (cancelled && cancelled()) { fail("memory.commit.cancelled"); return receipt; }
        auto snapshot = store::ReadProjectRecallSnapshot(memory, project, true, false);
        if (!snapshot) { fail("memory.commit.prepare_failed", snapshot.error()); return receipt; }
        auto fingerprints = Fingerprints(request, project);
        if (!fingerprints) { fail(fingerprints.error()); return receipt; }
        auto prepared = store::PrepareUpsert(request, snapshot->entries, std::move(*fingerprints), NowIsoUtc());
        const auto& entry = prepared.entry.public_entry;
        receipt.memory_id = entry.id; receipt.memory_path = entry.file;
        receipt.content_sha256 = hooks::Sha256Hex(prepared.topic_text); receipt.committed_at = entry.updated_at;
        if (!SafeText(prepared.topic_text, kTopicBytes) || !IsValidId(entry.id) || !IsSafeRelativePath(entry.file) ||
            !frontmatter::Parse(prepared.topic_text, true)) {
            fail("memory.commit.topic_limit"); return receipt;
        }
        const auto topic = memory / Utf8Path(entry.file);
        if (auto valid = Directory(topic.parent_path(), workspace); !valid) { fail(valid.error()); return receipt; }
        const auto existing_topic = WriteTarget(topic);
        if (!existing_topic) { fail(existing_topic.error()); return receipt; }
        for (const auto& existing : snapshot->entries) {
            if (existing.public_entry.file == entry.file && existing.public_entry.id != entry.id) {
                fail("memory.commit.topic_conflict"); return receipt;
            }
        }
        if (!prepared.previous_file.empty() && prepared.previous_file != entry.file) {
            const auto old_parent = (memory / Utf8Path(prepared.previous_file)).parent_path();
            if (auto valid = Directory(old_parent, workspace); !valid) { fail(valid.error()); return receipt; }
            const auto barrier = WriteTarget(old_parent / ".memory-commit-cleanup.json");
            if (!barrier) { fail(barrier.error()); return receipt; }
        }
        auto entries = std::move(snapshot->entries);
        std::erase_if(entries, [&](const auto& item) { return item.public_entry.id == entry.id; });
        if (entries.size() >= 1024) { fail("memory.commit.topic_limit"); return receipt; }
        entries.push_back(prepared.entry);
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.public_entry.kind != b.public_entry.kind ? a.public_entry.kind < b.public_entry.kind
                                                            : a.public_entry.id < b.public_entry.id;
        });
        const auto derived = store::PrepareMemoryIndex(entries, false, receipt.committed_at);
        if (derived.catalog.size() > 4 * 1024 * 1024 || derived.index.size() > 4 * 1024 * 1024) {
            fail("memory.commit.index_limit"); return receipt;
        }
        for (const auto& path : {memory / ".state" / "catalog.json", memory / "index.md"}) {
            const auto existing = WriteTarget(path);
            if (!existing) { fail(existing.error()); return receipt; }
        }
        const Json material{{"memory_id", receipt.memory_id}, {"memory_path", receipt.memory_path},
                            {"content_sha256", receipt.content_sha256}, {"committed_at", receipt.committed_at},
                            {"bytes", prepared.topic_text.size()}, {"previous_file", prepared.previous_file}};
        const Json intent{{"schema_version", 1}, {"commit_schema", 1}, {"operation", "memory_save"},
                          {"operation_id", receipt.operation_id}, {"workspace_key", receipt.workspace_key},
                          {"session_id", context.session_id}, {"requested_at_ms", NowMillis()},
                          {"request_sha256", receipt.request_sha256}, {"parameters", identity}, {"prepared", material}};
        const auto save = [&](Stage stage, const fs::path& path, std::string_view bytes) {
            if (stage == Stage::Topic) topic_attempted = true;
            auto result = write(path, bytes, kDurability);
            const auto outcome = result ? result->outcome : result.error().outcome;
            receipt.stages.push_back({stage, outcome});
            if (stage == Stage::Intent) intent_visible = outcome != Outcome::NotCommitted;
            if (stage == Stage::Topic && outcome == Outcome::NotCommitted) topic_attempted = false;
            if (!result || outcome != Outcome::CommittedDurable) {
                fail("memory.commit.write_failed", result ? "requested durability was not confirmed" : result.error().code + ": " + result.error().message);
                return false;
            }
            return true;
        };
        if (cancelled && cancelled()) { fail("memory.commit.cancelled"); return receipt; }
        if (!save(Stage::Intent, intent_path, Seal(intent))) {
            receipt.state = intent_visible ? State::Indeterminate : State::NotStarted;
            return receipt;
        }
        bool successful = false;
        if (cancelled && cancelled()) fail("memory.commit.cancelled");
        else if (save(Stage::Snapshot, snapshot_path, prepared.topic_text) &&
                 !(cancelled && cancelled()) && save(Stage::Topic, topic, prepared.topic_text)) {
            bool cleanup_ok = true;
            if (cancelled && cancelled()) { fail("memory.commit.cancelled"); cleanup_ok = false; }
            else if (!prepared.previous_file.empty() && prepared.previous_file != entry.file) {
                const auto old = memory / Utf8Path(prepared.previous_file);
                ec.clear();
                if (!fs::remove(old, ec) || ec) { fail("memory.commit.cleanup_failed", ec.message()); cleanup_ok = false; }
                else {
                    // Flushing this same directory after unlink confirms the
                    // old-name directory change using the shared platform writer.
                    cleanup_ok = save(Stage::Cleanup, old.parent_path() / ".memory-commit-cleanup.json",
                                      Json{{"operation_id", receipt.operation_id}, {"removed", prepared.previous_file}}.dump() + "\n");
                }
            }
            if (cleanup_ok && !(cancelled && cancelled()) && save(Stage::Catalog, memory / ".state" / "catalog.json", derived.catalog) &&
                !(cancelled && cancelled()) && save(Stage::Index, memory / "index.md", derived.index)) successful = true;
        }
        if (!successful && receipt.error_code.empty()) fail("memory.commit.cancelled");
        receipt.state = successful ? State::Committed : topic_attempted ? State::Indeterminate : State::NotStarted;
        Json outcome;
        if (successful) {
            outcome = {{"memory_id", receipt.memory_id}, {"memory_version", receipt.committed_at},
                       {"content_sha256", receipt.content_sha256}, {"memory_path", receipt.memory_path},
                       {"committed_at", receipt.committed_at}};
        } else {
            outcome = {{"stable_error_code", receipt.error_code}, {"error", receipt.error}, {"retryable", false}};
        }
        const Json result{{"schema_version", 1}, {"commit_schema", 1}, {"operation_id", receipt.operation_id},
                          {"workspace_key", receipt.workspace_key}, {"status", "completed"}, {"completed_at_ms", NowMillis()},
                          {"request_sha256", receipt.request_sha256}, {"commit_state", StateName(receipt.state)},
                          {"prepared", material}, {"stages", StageValues(receipt)}, {"outcome", outcome}};
        if (!save(Stage::Result, result_path, Seal(result))) receipt.state = State::Indeterminate;
        return receipt;
    } catch (const std::exception& error) {
        receipt.state = topic_attempted || intent_visible ? State::Indeterminate : State::NotStarted;
        fail("memory.commit.invalid_or_failed", error.what());
        return receipt;
    }
}
}  // namespace

ProjectCommitReceipt CommitProjectUpsert(const ProjectCommitContext& context, const SaveRequest& request,
                                        std::stop_token stop) {
    return Run(context, request, platform::AtomicWriteFile, [stop] { return stop.stop_requested(); });
}
ProjectCommitReceipt CommitProjectUpsertWithCancellation(const ProjectCommitContext& context,
                                                        const SaveRequest& request,
                                                        ProjectCommitCancellation cancelled) {
    return Run(context, request, platform::AtomicWriteFile, cancelled);
}
ProjectCommitReceipt ConfirmProjectCommitReceipt(const fs::path& lifecycle_root,
                                                const std::string& operation_id) {
    ProjectCommitReceipt invalid;
    invalid.state = State::Indeterminate;
    invalid.operation_id = operation_id;
    invalid.error_code = "memory.commit.invalid_record";
    invalid.error = invalid.error_code;
    if (!SafeKey(operation_id)) return invalid;
    try {
        const auto bytes = ReadOptional(lifecycle_root / Utf8Path(operation_id) / "intent.json", kRecordBytes);
        if (!bytes || !bytes->has_value()) return invalid;
        const auto intent = OpenRecord(**bytes);
        if (!intent) { invalid.error_code = intent.error(); invalid.error = intent.error(); return invalid; }
        const auto& parameters = intent->at("parameters");
        auto request = store::ParseUpsertJob(parameters, true);
        if (!request) return invalid;
        ProjectCommitContext context;
        context.project_root = Utf8Path(parameters.at("project_root").get<std::string>());
        context.memory_directory = AbsoluteNormal(lifecycle_root).parent_path() / "memory";
        context.lifecycle_root = lifecycle_root;
        context.workspace_key = intent->at("workspace_key").get<std::string>();
        context.operation_id = operation_id;
        context.session_id = parameters.at("session_id").get<std::string>();
        context.source_event_ref = parameters.at("source_event_ref").get<std::string>();
        return Run(context, *request, platform::AtomicWriteFile, {}, true);
    } catch (const std::exception&) {
        return invalid;
    }
}
ProjectCommitReceipt InspectProjectCommitReceipt(const ProjectCommitContext& context, const SaveRequest& request) {
    return Run(context, request, platform::AtomicWriteFile, {}, true, true);
}
std::string ProjectCommitRequestSha256(const ProjectCommitContext& context, const SaveRequest& request) {
    return hooks::Sha256Hex(SaveIdentity(context, request).dump());
}
namespace commit_testing {
ProjectCommitReceipt CommitProjectUpsert(const ProjectCommitContext& context, const SaveRequest& request,
                                        WriteFile write, std::stop_token stop) {
    return Run(context, request, write, [stop] { return stop.stop_requested(); });
}
}  // namespace commit_testing
}  // namespace lubancode::memory
