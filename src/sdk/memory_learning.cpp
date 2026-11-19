#include "sdk/memory_learning.hpp"
#include "sdk/plan_write.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

#include "agent/memory_extraction.hpp"
#include "agent/memory_learning_gates.hpp"
#include "api/usage_json.hpp"
#include "memory/candidate_store.hpp"
#include "memory/project_commit.hpp"
#include "memory/project_memory.hpp"
#include "memory/topic_store.hpp"
#include "platform/atomic_write.hpp"
#include "platform/bounded_read.hpp"
#include "platform/owned_file_path.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/assembly/backend.hpp"
#include "runtime/session_service.hpp"
#include "sdk/adapters.hpp"
#include "sdk/usage_result.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/index.hpp"

#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
#include "sdk/memory_learning_test_hooks.hpp"
#endif

namespace lubancore::detail {
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
namespace testing {
namespace {
std::mutex learning_hooks_mutex;
LearningWorkerStartHookHandle learning_start_hook;
LearningWorkerExecutionHookHandle learning_execution_hook;
LearningCandidateStoreHookHandle learning_candidate_hook;
LearningCommitWriteHookHandle learning_commit_write_hook;
} // namespace

LearningWorkerStartHookHandle ReplaceLearningWorkerStartHook(
    LearningWorkerStartHookHandle replacement) noexcept {
    std::lock_guard lock(learning_hooks_mutex);
    learning_start_hook.swap(replacement);
    return replacement;
}
LearningWorkerStartHookHandle SnapshotLearningWorkerStartHook() noexcept {
    std::lock_guard lock(learning_hooks_mutex);
    return learning_start_hook;
}
LearningWorkerExecutionHookHandle ReplaceLearningWorkerExecutionHook(
    LearningWorkerExecutionHookHandle replacement) noexcept {
    std::lock_guard lock(learning_hooks_mutex);
    learning_execution_hook.swap(replacement);
    return replacement;
}
LearningWorkerExecutionHookHandle SnapshotLearningWorkerExecutionHook() noexcept {
    std::lock_guard lock(learning_hooks_mutex);
    return learning_execution_hook;
}
LearningCandidateStoreHookHandle ReplaceLearningCandidateStoreHook(
    LearningCandidateStoreHookHandle replacement) noexcept {
    std::lock_guard lock(learning_hooks_mutex);
    learning_candidate_hook.swap(replacement);
    return replacement;
}
LearningCandidateStoreHookHandle SnapshotLearningCandidateStoreHook() noexcept {
    std::lock_guard lock(learning_hooks_mutex);
    return learning_candidate_hook;
}
LearningCommitWriteHookHandle ReplaceLearningCommitWriteHook(
    LearningCommitWriteHookHandle replacement) noexcept {
    std::lock_guard lock(learning_hooks_mutex);
    learning_commit_write_hook.swap(replacement);
    return replacement;
}
LearningCommitWriteHookHandle SnapshotLearningCommitWriteHook() noexcept {
    std::lock_guard lock(learning_hooks_mutex);
    return learning_commit_write_hook;
}
} // namespace testing
#endif

namespace {
namespace fs = std::filesystem;
namespace mem = lubancode::memory;
namespace rt = lubancode::runtime;
namespace v3 = lubancode::trajectory::v3;
namespace extract = lubancode::agent::memory_extraction;
namespace gates = lubancode::agent::memory_learning_gates;
using Json = nlohmann::json;
using memory::v1::CandidateAcceptReport;
using memory::v1::LearningCandidate;
using memory::v1::LearningMode;
using memory::v1::LearningOptions;
using memory::v1::LearningReport;
using memory::v1::LearningSnapshot;
using memory::v1::LearningStage;
using memory::v1::SaveStageReceipt;

constexpr auto kPlanFile = "sdk-memory-learning-plan.json";
constexpr auto kReportsDir = "sdk-memory-learnings";
constexpr auto kAcceptsDir = "sdk-memory-accepts";
constexpr std::size_t kPlanCap = 4096;
constexpr std::size_t kReportCap = 524288;
constexpr std::size_t kTotalCap = 67108864;
constexpr std::size_t kDirEntryCap = 4096;

Error Fail(std::string code, std::string message = {}) {
    return {std::move(code), std::move(message)};
}

bool SafeText(const std::string& value) {
    return value.find('\0') == std::string::npos && lubancode::platform::IsValidUtf8(value);
}

bool SafeId(const std::string& value) {
    return !value.empty() && value.size() <= 200 &&
           value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") ==
               std::string::npos &&
           value != "." && value != "..";
}

bool Inside(const fs::path& path, const fs::path& root) {
    auto p = path.begin(), r = root.begin();
    for (; r != root.end(); ++r, ++p) {
        if (p == path.end() || *p != *r) return false;
    }
    return true;
}

bool SafeCandidateId(const std::string& value) {
    return SafeId(value) && value != "." && value != ".." &&
           value.find("..") == std::string::npos && value != "rejected";
}

const char* CommitStateName(mem::ProjectCommitState value) {
    switch (value) {
    case mem::ProjectCommitState::NotStarted: return "not_started";
    case mem::ProjectCommitState::Committed: return "committed";
    case mem::ProjectCommitState::Indeterminate: return "indeterminate";
    }
    return "indeterminate";
}

const char* CommitStageName(mem::ProjectCommitStage stage) {
    switch (stage) {
    case mem::ProjectCommitStage::Intent: return "intent";
    case mem::ProjectCommitStage::Snapshot: return "snapshot";
    case mem::ProjectCommitStage::Topic: return "topic";
    case mem::ProjectCommitStage::Cleanup: return "cleanup";
    case mem::ProjectCommitStage::Catalog: return "catalog";
    case mem::ProjectCommitStage::Index: return "index";
    case mem::ProjectCommitStage::Result: return "result";
    }
    return "invalid";
}

const char* CommitOutcomeName(lubancode::platform::WriteOutcome outcome) {
    switch (outcome) {
    case lubancode::platform::WriteOutcome::NotCommitted: return "not_committed";
    case lubancode::platform::WriteOutcome::CommittedDurabilityNotRequested: return "visible";
    case lubancode::platform::WriteOutcome::CommittedDurabilityUnconfirmed: return "unconfirmed";
    case lubancode::platform::WriteOutcome::CommittedDurable: return "durable";
    }
    return "invalid";
}

LearningCandidate CandidateToPublic(const mem::MemoryCandidate& c) {
    return {
        .id = c.id,
        .kind = mem::MemoryKindName(c.kind),
        .title = c.title,
        .summary = c.summary,
        .content = c.content,
        .keywords = c.keywords,
        .paths = c.paths,
        .confidence = c.confidence,
        .task_type = c.task_type,
        .created_at = c.created_at,
        .occurred_at = c.occurred_at,
    };
}

Json SaveRequestJson(const mem::SaveRequest& r) {
    Json evidence = Json::array();
    for (const auto& e : r.evidence) {
        evidence.push_back({{"path", e.path}, {"symbol", e.symbol}});
    }
    return {
        {"kind", mem::MemoryKindName(r.kind)},
        {"id", r.id},
        {"title", r.title},
        {"summary", r.summary},
        {"content", r.content},
        {"keywords", r.keywords},
        {"paths", r.paths},
        {"source_session", r.source_session},
        {"confidence", r.confidence},
        {"scope", {{"level", r.scope.level}, {"kind", r.scope.kind}, {"value", r.scope.value}}},
        {"evidence", std::move(evidence)},
        {"expires_at", r.expires_at},
        {"occurred_at", r.occurred_at},
    };
}

Json AcceptReportJson(const CandidateAcceptReport& r) {
    Json stages = Json::array();
    for (const auto& s : r.stages) {
        stages.push_back({{"stage", s.stage}, {"outcome", s.outcome}});
    }
    return {
        {"sessionId", r.session_id},
        {"candidateId", r.candidate_id},
        {"workspaceKey", r.workspace_key},
        {"learningPlanSha256", r.learning_plan_sha256},
        {"writePlanSha256", r.write_plan_sha256},
        {"commitKey", r.commit_key},
        {"sourceEventRef", r.source_event_ref},
        {"saveRequestSha256", r.save_request_sha256},
        {"requestSha256", r.request_sha256},
        {"state", r.state},
        {"memoryId", r.memory_id},
        {"memoryPath", r.memory_path},
        {"contentSha256", r.content_sha256},
        {"committedAt", r.committed_at},
        {"stages", std::move(stages)},
        {"duplicate", r.duplicate},
        {"errorCode", r.error_code},
        {"error", r.error},
    };
}

std::string CandidateSourceEventRef(
    const std::string& workspace_key,
    const std::string& session_id,
    const std::string& candidate_id) {
    return "workspace_key=" + workspace_key + "/session_id=" + session_id + "/candidate_id=" + candidate_id;
}

std::string CandidateCommitKey(
    const std::string& session_id,
    const std::string& candidate_id,
    const std::string& save_request_sha256) {
    const Json key{
        {"session", session_id},
        {"candidate", candidate_id},
        {"saveRequestSha256", save_request_sha256},
    };
    return "sdk-cand-" + lubancode::platform::Sha256Hex(key.dump());
}

const char* ModeName(LearningMode mode) {
    switch (mode) {
    case LearningMode::Off: return "off";
    case LearningMode::Review: return "review";
    case LearningMode::Auto: return "auto";
    }
    return "invalid";
}

std::optional<LearningMode> ParseModeName(const std::string& name) {
    if (name == "off") return LearningMode::Off;
    if (name == "review") return LearningMode::Review;
    if (name == "auto") return LearningMode::Auto;
    return std::nullopt;
}

const char* StageName(LearningStage stage) {
    switch (stage) {
    case LearningStage::NotAttempted: return "not_attempted";
    case LearningStage::Skipped: return "skipped";
    case LearningStage::BusyDropped: return "busy_dropped";
    case LearningStage::StartFailed: return "start_failed";
    case LearningStage::Running: return "running";
    case LearningStage::Failed: return "failed";
    case LearningStage::Cancelled: return "cancelled";
    case LearningStage::Completed: return "completed";
    case LearningStage::Indeterminate: return "indeterminate";
    }
    return "invalid";
}

std::optional<LearningStage> ParseStageName(const std::string& name) {
    if (name == "not_attempted") return LearningStage::NotAttempted;
    if (name == "skipped") return LearningStage::Skipped;
    if (name == "busy_dropped") return LearningStage::BusyDropped;
    if (name == "start_failed") return LearningStage::StartFailed;
    if (name == "running") return LearningStage::Running;
    if (name == "failed") return LearningStage::Failed;
    if (name == "cancelled") return LearningStage::Cancelled;
    if (name == "completed") return LearningStage::Completed;
    if (name == "indeterminate") return LearningStage::Indeterminate;
    return std::nullopt;
}

const char* WriteOutcomeName(lubancode::platform::WriteOutcome outcome) {
    switch (outcome) {
    case lubancode::platform::WriteOutcome::NotCommitted: return "NotCommitted";
    case lubancode::platform::WriteOutcome::CommittedDurabilityNotRequested:
        return "CommittedDurabilityNotRequested";
    case lubancode::platform::WriteOutcome::CommittedDurabilityUnconfirmed:
        return "CommittedDurabilityUnconfirmed";
    case lubancode::platform::WriteOutcome::CommittedDurable: return "CommittedDurable";
    }
    return "Unknown";
}

Result<std::optional<std::string>> ReadOwned(const fs::path& path, const fs::path& root, std::size_t cap) {
    if (!lubancode::platform::IsUnlinkedOwnedPath(root, path))
        return std::unexpected(Fail("sdk.memory_learning.path_escape"));
    const auto io_path = lubancode::platform::FileIoPath(path);
    std::error_code ec;
    const auto status = fs::symlink_status(io_path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        return std::optional<std::string>{};
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status) ||
        !lubancode::platform::RejectReparsePoint(io_path))
        return std::unexpected(Fail("sdk.memory_learning.invalid_file"));
    auto bytes = lubancode::platform::ReadBoundedRegularFile(io_path, cap);
    if (!bytes) return std::unexpected(Fail("sdk.memory_learning." + bytes.error()));
    if (!lubancode::platform::IsUnlinkedOwnedPath(root, path))
        return std::unexpected(Fail("sdk.memory_learning.path_escape"));
    if (!SafeText(*bytes)) return std::unexpected(Fail("sdk.memory_learning.invalid_text"));
    return std::optional<std::string>{std::move(*bytes)};
}

Result<void> Directory(const fs::path& path, const fs::path& root, bool create) {
    if (!lubancode::platform::IsUnlinkedOwnedPath(root, path))
        return std::unexpected(Fail("sdk.memory_learning.path_escape"));
    const auto io_path = lubancode::platform::FileIoPath(path);
    std::error_code ec;
    const auto status = fs::symlink_status(io_path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found)) {
        if (!create) return {};
        ec.clear();
        fs::create_directory(io_path, ec);
        if (ec) return std::unexpected(Fail("sdk.memory_learning.mkdir_failed"));
    } else if (ec || fs::is_symlink(status) || !fs::is_directory(status) ||
               !lubancode::platform::RejectReparsePoint(io_path)) {
        return std::unexpected(Fail("sdk.memory_learning.invalid_directory"));
    }
    if (!lubancode::platform::IsUnlinkedOwnedPath(root, path))
        return std::unexpected(Fail("sdk.memory_learning.path_escape"));
    return {};
}

const Json* Binding(const v3::MessageLine& m) {
    if (!m.system_meta || !m.system_meta->is_object()) return nullptr;
    const auto hosts = m.system_meta->find("hostBindings");
    if (hosts == m.system_meta->end() || !hosts->is_object()) return nullptr;
    const auto item = hosts->find("memoryLearning");
    return item == hosts->end() ? nullptr : &*item;
}

Json ReportJson(const LearningReport& r) {
    return {
        {"schemaVersion", 1},
        {"mode", ModeName(r.mode)},
        {"sessionId", r.session_id},
        {"operationId", r.operation_id},
        {"turnId", r.turn_id},
        {"learningTurnId", r.learning_turn_id},
        {"workspaceKey", r.workspace_key},
        {"planSha256", r.plan_sha256},
        {"stage", StageName(r.stage)},
        {"skipReason", r.skip_reason},
        {"taskType", r.task_type},
        {"model", r.model},
        {"candidateIds", r.candidate_ids},
        {"retrievalHints", r.retrieval_hints},
        {"errorCode", r.error_code},
        {"error", r.error},
        {"modelCalled", r.model_called},
        {"usage", detail::usage_result::Encode(r.usage)},
    };
}

LearningReport ParseReport(const Json& j) {
    if (!j.is_object() || j.size() != 18) throw std::runtime_error("report shape");
    LearningReport r;
    const auto parsed_mode = ParseModeName(j.at("mode").get<std::string>());
    const auto parsed_stage = ParseStageName(j.at("stage").get<std::string>());
    if (!parsed_mode || !parsed_stage) throw std::runtime_error("enum");
    r.mode = *parsed_mode;
    r.stage = *parsed_stage;
    r.session_id = j.at("sessionId").get<std::string>();
    r.operation_id = j.at("operationId").get<std::string>();
    r.turn_id = j.at("turnId").get<std::string>();
    r.learning_turn_id = j.at("learningTurnId").get<std::string>();
    r.workspace_key = j.at("workspaceKey").get<std::string>();
    r.plan_sha256 = j.at("planSha256").get<std::string>();
    r.skip_reason = j.at("skipReason").get<std::string>();
    r.task_type = j.at("taskType").get<std::string>();
    r.model = j.at("model").get<std::string>();
    r.candidate_ids = j.at("candidateIds").get<std::vector<std::string>>();
    r.retrieval_hints = j.at("retrievalHints").get<std::vector<std::string>>();
    r.error_code = j.at("errorCode").get<std::string>();
    r.error = j.at("error").get<std::string>();
    r.model_called = j.at("modelCalled").get<bool>();
    auto decoded_usage = detail::usage_result::Decode(j.at("usage"));
    if (!decoded_usage) throw std::runtime_error(std::string(decoded_usage.error()));
    r.usage = std::move(*decoded_usage);
    for (const auto* s : {&r.session_id, &r.operation_id, &r.turn_id, &r.learning_turn_id,
                          &r.workspace_key, &r.plan_sha256, &r.skip_reason, &r.task_type,
                          &r.model, &r.error_code, &r.error}) {
        if (!SafeText(*s)) throw std::runtime_error("utf8");
    }
    for (const auto& id : r.candidate_ids) {
        if (!SafeText(id) || id.empty()) throw std::runtime_error("candidate_id");
    }
    for (const auto& hint : r.retrieval_hints) {
        if (!SafeText(hint)) throw std::runtime_error("hint");
    }
    if (ReportJson(r) != j) throw std::runtime_error("canonical shape");
    return r;
}

std::expected<void, std::string> ValidateBypassUsageBinding(
    const LearningReport& report,
    const v3::V3Ledger& ledger) {
    if (ledger.session_id != report.session_id)
        return std::unexpected("learning usage session differs");
    std::set<std::string> seen;
    for (const auto& record : report.usage.attempts) {
        if (record.subordinate || record.cache_epoch != 0 ||
            record.source_session_id != ledger.session_id ||
            record.source_run_id != ledger.run_id ||
            record.turn_id.empty() ||
            (!report.learning_turn_id.empty() && record.turn_id != report.learning_turn_id) ||
            record.purpose != "memory_extract" ||
            record.trajectory_request_id.empty() ||
            !seen.insert(record.trajectory_request_id).second) {
            return std::unexpected("learning usage attempt owner differs");
        }
        const v3::EventLine* prepared = nullptr;
        for (const auto& event : ledger.events) {
            if (event.kind != v3::EventKindV3::ModelRequestPrepared ||
                event.request_id != record.trajectory_request_id)
                continue;
            if (prepared) return std::unexpected("learning usage request ambiguous");
            prepared = &event;
        }
        if (!prepared || prepared->session_id != ledger.session_id ||
            prepared->run_id != ledger.run_id ||
            prepared->turn_id != record.turn_id ||
            prepared->step_id != record.step_id ||
            !prepared->parent_turn_id.has_value() ||
            *prepared->parent_turn_id != report.turn_id ||
            !lubancode::api::usage_json::MemberStringEquals(prepared->payload, "purpose", "memory_extract") ||
            !lubancode::api::usage_json::MemberStringEquals(prepared->payload, "model", record.model)) {
            return std::unexpected("learning usage prepared event mismatch");
        }
        if (!v3::CheckPreparedAgainstChain(ledger, prepared->event_id).empty())
            return std::unexpected("learning usage prepared chain mismatch");
        const v3::EventLine* observed = nullptr;
        for (const auto& event : ledger.events) {
            if (event.kind != v3::EventKindV3::ModelUsageObserved ||
                event.request_id != record.trajectory_request_id)
                continue;
            if (observed) return std::unexpected("learning usage observation ambiguous");
            observed = &event;
        }
        const Json numbers = std::array<std::int64_t, 5>{
            record.usage.input_tokens, record.usage.output_tokens,
            record.usage.cache_read_tokens, record.usage.cache_creation_tokens,
            record.usage.output_reasoning_tokens};
        const Json response_id = record.provider_response_id.empty()
            ? Json(nullptr)
            : Json(record.provider_response_id);
        if (!observed || observed->seq <= prepared->seq ||
            observed->session_id != ledger.session_id ||
            observed->run_id != ledger.run_id ||
            observed->turn_id != record.turn_id ||
            observed->step_id != prepared->step_id ||
            observed->payload.value("numbers", Json()) != numbers ||
            observed->payload.value("providerResponseId", Json()) != response_id ||
            observed->payload.value("reportedByProvider", Json()) != record.reported_by_provider ||
            observed->payload.value("incomplete", Json()) != record.incomplete) {
            return std::unexpected("learning usage observation mismatch");
        }
        if (record.observation) {
            const lubancode::api::Usage selected{
                record.usage.input_tokens, record.usage.output_tokens,
                record.usage.cache_read_tokens, record.usage.cache_creation_tokens,
                record.usage.output_reasoning_tokens};
            const auto material = lubancode::api::usage_json::Encode(*record.observation, selected);
            if (!material || observed->payload.value("observation", Json()) != *material)
                return std::unexpected("learning usage observation material mismatch");
        } else if (observed->payload.contains("observation")) {
            return std::unexpected("learning usage observation material mismatch");
        }
        for (const auto& message : ledger.messages) {
            if (message.request_id != record.trajectory_request_id ||
                !lubancode::api::usage_json::MemberStringEquals(message.message, "role", "assistant"))
                continue;
            if (message.session_id != ledger.session_id ||
                message.run_id != ledger.run_id ||
                message.turn_id != record.turn_id)
                return std::unexpected("learning usage assistant message owner mismatch");
            if (const auto id = message.message.find("provider_response_id"); id != message.message.end()) {
                if (*id != response_id)
                    return std::unexpected("learning usage assistant response id mismatch");
            }
            if (record.reported_by_provider && message.usage && message.usage->is_object()) {
                const std::array<const char*, 5> keys{
                    "inputTokens", "outputTokens", "cacheReadTokens", "cacheWriteTokens", "reasoningTokens"};
                const std::array<std::int64_t, 5> expected{
                    record.usage.input_tokens, record.usage.output_tokens,
                    record.usage.cache_read_tokens, record.usage.cache_creation_tokens,
                    record.usage.output_reasoning_tokens};
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    if (message.usage->value(keys[i], Json()) != expected[i])
                        return std::unexpected("learning usage assistant message numbers mismatch");
                }
            }
        }
    }
    return {};
}

class LocalProjectCandidateStore final : public LearningCandidateStore {
public:
    LocalProjectCandidateStore(
        fs::path owned_root
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
        , testing::LearningCandidateStoreHookHandle hook
#endif
        )
        : owned_root_(std::move(owned_root))
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
        , hook_(std::move(hook))
#endif
    {}

    std::expected<std::string, std::string> Add(
        const fs::path& memory_dir,
        mem::MemoryCandidate candidate) override {
        std::lock_guard lock(mutex_);
        if (auto dir_ok = EnsureContainment(memory_dir, true); !dir_ok) {
            return std::unexpected(dir_ok.error());
        }
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
        if (hook_ && *hook_) {
            return (*hook_)(memory_dir, std::move(candidate));
        }
#endif
        return mem::candidates::AddCandidate(memory_dir, std::move(candidate));
    }

    std::expected<std::vector<mem::MemoryCandidate>, std::string> List(
        const fs::path& memory_dir) override {
        std::lock_guard lock(mutex_);
        auto dir_ok = EnsureContainment(memory_dir, false);
        if (!dir_ok) return std::unexpected(dir_ok.error());
        if (!*dir_ok) return std::vector<mem::MemoryCandidate>{};
        auto list = mem::candidates::ListCandidates(memory_dir);
        std::sort(list.begin(), list.end(), [](const mem::MemoryCandidate& a, const mem::MemoryCandidate& b) {
            if (a.created_at != b.created_at) return a.created_at < b.created_at;
            return a.id < b.id;
        });
        return list;
    }

    std::expected<std::optional<mem::MemoryCandidate>, std::string> Get(
        const fs::path& memory_dir,
        const std::string& candidate_id) override {
        std::lock_guard lock(mutex_);
        if (!SafeCandidateId(candidate_id)) return std::unexpected("invalid candidate_id");
        auto dir_ok = EnsureContainment(memory_dir, false);
        if (!dir_ok) return std::unexpected(dir_ok.error());
        if (!*dir_ok) return std::optional<mem::MemoryCandidate>{};
        return mem::candidates::GetCandidate(memory_dir, candidate_id);
    }

    std::expected<mem::MemoryCandidate, std::string> Edit(
        const fs::path& memory_dir,
        const std::string& candidate_id,
        const std::string& title,
        const std::string& content) override {
        std::lock_guard lock(mutex_);
        if (!SafeCandidateId(candidate_id)) return std::unexpected("invalid candidate_id");
        auto dir_ok = EnsureContainment(memory_dir, false);
        if (!dir_ok) return std::unexpected(dir_ok.error());
        if (!*dir_ok || !mem::candidates::GetCandidate(memory_dir, candidate_id).has_value()) {
            return std::unexpected("candidate_not_found");
        }
        auto edited = mem::candidates::EditCandidate(memory_dir, candidate_id, title, content);
        if (!edited) return std::unexpected(edited.error());
        auto updated = mem::candidates::GetCandidate(memory_dir, candidate_id);
        if (!updated.has_value()) return std::unexpected("candidate_not_found");
        return *updated;
    }

    std::expected<void, std::string> Reject(
        const fs::path& memory_dir,
        const std::string& candidate_id,
        const std::string& reason) override {
        std::lock_guard lock(mutex_);
        if (!SafeCandidateId(candidate_id)) return std::unexpected("invalid candidate_id");
        auto dir_ok = EnsureContainment(memory_dir, false);
        if (!dir_ok) return std::unexpected(dir_ok.error());
        if (!*dir_ok || !mem::candidates::GetCandidate(memory_dir, candidate_id).has_value()) {
            return std::unexpected("candidate_not_found");
        }
        return mem::candidates::RejectCandidate(memory_dir, candidate_id, reason);
    }

    std::expected<void, std::string> Remove(
        const fs::path& memory_dir,
        const std::string& candidate_id) override {
        std::lock_guard lock(mutex_);
        if (!SafeCandidateId(candidate_id)) return std::unexpected("invalid candidate_id");
        auto dir_ok = EnsureContainment(memory_dir, false);
        if (!dir_ok) return std::unexpected(dir_ok.error());
        if (!*dir_ok) return {};
        mem::candidates::RemoveCandidateFile(memory_dir, candidate_id);
        std::error_code ec;
        if (fs::exists(memory_dir / "memory-candidates" / (candidate_id + ".json"), ec)) {
            return std::unexpected("failed to remove accepted candidate file");
        }
        return {};
    }

private:
    std::expected<bool, std::string> EnsureContainment(
        const fs::path& memory_dir,
        bool create_if_missing) const {
        std::error_code ec;
        const auto root = fs::canonical(owned_root_ / "workspaces", ec);
        if (ec) return std::unexpected("owned workspaces root unavailable");
        auto status = fs::symlink_status(memory_dir, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found)) {
            if (!create_if_missing) {
                fs::path probe = memory_dir.parent_path();
                while (!probe.empty() && probe != probe.parent_path()) {
                    ec.clear();
                    const auto pstatus = fs::symlink_status(probe, ec);
                    if (ec == std::errc::no_such_file_or_directory ||
                        (!ec && pstatus.type() == fs::file_type::not_found)) {
                        probe = probe.parent_path();
                        continue;
                    }
                    if (ec || fs::is_symlink(pstatus) || !fs::is_directory(pstatus)) {
                        return std::unexpected("project memory directory ancestor is invalid or symlinked");
                    }
                    const auto preal = fs::canonical(probe, ec);
                    if (ec || !Inside(preal, root)) {
                        return std::unexpected("project memory directory escapes owned root");
                    }
                    break;
                }
                const auto weak = fs::weakly_canonical(memory_dir, ec);
                if (ec || !Inside(weak, root)) {
                    return std::unexpected("project memory directory escapes owned root");
                }
                return false;
            }
            ec.clear();
            fs::create_directories(memory_dir, ec);
            if (ec) return std::unexpected("cannot create project memory directory");
            status = fs::symlink_status(memory_dir, ec);
        }
        if (ec || fs::is_symlink(status) || !fs::is_directory(status)) {
            return std::unexpected("project memory directory is invalid or symlinked");
        }
        const auto real = fs::canonical(memory_dir, ec);
        if (ec || !Inside(real, root)) {
            return std::unexpected("project memory directory escapes owned root");
        }
        const fs::path candidates_dir = memory_dir / "memory-candidates";
        ec.clear();
        const auto cstatus = fs::symlink_status(candidates_dir, ec);
        if (ec != std::errc::no_such_file_or_directory && !(!ec && cstatus.type() == fs::file_type::not_found)) {
            if (ec || fs::is_symlink(cstatus) || !fs::is_directory(cstatus)) {
                return std::unexpected("candidate directory is invalid or symlinked");
            }
            const auto creal = fs::canonical(candidates_dir, ec);
            if (ec || !Inside(creal, real)) {
                return std::unexpected("candidate directory escapes project memory directory");
            }
        }
        return true;
    }

    mutable std::mutex mutex_;
    fs::path owned_root_;
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
    testing::LearningCandidateStoreHookHandle hook_;
#endif
};

class LearningRecorderProxy final : public lubancode::agent::LoopBoundaryRecorder {
public:
    explicit LearningRecorderProxy(std::unique_ptr<lubancode::agent::LoopBoundaryRecorder> inner)
        : inner_(std::move(inner)) {
        usage_.attempts_complete = true;
    }

    std::string_view UsageSourceSessionId() const override {
        return inner_ ? inner_->UsageSourceSessionId() : std::string_view{};
    }
    std::string_view UsageSourceRunId() const override {
        return inner_ ? inner_->UsageSourceRunId() : std::string_view{};
    }
    std::string RequestTurnId(const std::string& request_id) const override {
        return inner_ ? inner_->RequestTurnId(request_id) : std::string{};
    }
    std::string RequestStepId(const std::string& request_id) const override {
        return inner_ ? inner_->RequestStepId(request_id) : std::string{};
    }

    std::string OnRequestPrepared(const lubancode::api::Request& request,
                                  const lubancode::agent::RequestPreparedContext& ctx) override {
        if (!inner_) return {};
        auto request_id = inner_->OnRequestPrepared(request, ctx);
        if (!request_id.empty()) {
            prepared_request_id_ = request_id;
            prepared_model_ = request.model;
            prepared_turn_id_ = inner_->RequestTurnId(request_id);
            prepared_step_id_ = inner_->RequestStepId(request_id);
        }
        return request_id;
    }

    bool OnRequestSent(const std::string& request_id) override {
        if (!inner_) return false;
        const bool ok = inner_->OnRequestSent(request_id);
        if (ok) model_called_ = true;
        return ok;
    }

    std::optional<bool> OnUsageObservation(
        const std::string& request_id,
        const lubancode::api::Usage& usage,
        const ::lubancore::usage::v1::Observation* observation,
        bool reported,
        std::string_view response_id,
        bool incomplete) override {
        struct AttemptCtx {
            std::string_view trajectory_request_id;
            std::string_view provider_response_id;
            std::string_view model;
            std::string_view step_id;
            std::string_view turn_id;
            std::string_view purpose;
            int cache_epoch = 0;
            bool reported_by_provider = false;
            std::string_view source_session_id;
            std::string_view source_run_id;
        } ctx{
            request_id,
            response_id,
            prepared_model_,
            prepared_step_id_,
            prepared_turn_id_,
            "memory_extract",
            0,
            reported,
            UsageSourceSessionId(),
            UsageSourceRunId(),
        };
        try {
            detail::usage_result::CaptureAttempt(usage_, usage, observation, false, incomplete, ctx);
        } catch (...) {
            usage_.attempts_complete = false;
            throw;
        }
        if (!inner_) return std::nullopt;
        auto receipt = inner_->OnUsageObservation(request_id, usage, observation, reported, response_id, incomplete);
        if (!receipt.has_value() || !*receipt) {
            usage_.attempts_complete = false;
        }
        return receipt;
    }

    void OnUsageRecorded(const std::string& request_id, const lubancode::api::Usage& usage,
                         bool reported_by_provider, const std::string& provider_response_id,
                         int cache_epoch = 0, bool prefix_append_only = true,
                         bool cache_read_reported_by_provider = false,
                         bool cache_creation_reported_by_provider = false,
                         const std::string& usage_anomaly = std::string()) override {
        if (inner_) {
            inner_->OnUsageRecorded(request_id, usage, reported_by_provider, provider_response_id,
                cache_epoch, prefix_append_only, cache_read_reported_by_provider,
                cache_creation_reported_by_provider, usage_anomaly);
        }
    }

    bool OnOutputCompleted(const std::string& request_id, const lubancode::api::Message& assistant,
                           const std::string& stop_reason,
                           const std::string& provider_response_id) override {
        if (!inner_) return false;
        return inner_->OnOutputCompleted(request_id, assistant, stop_reason, provider_response_id);
    }

    void OnOutputFailed(const std::string& request_id, const std::string& reason) override {
        if (inner_) inner_->OnOutputFailed(request_id, reason);
    }

    void OnOutputCancelled(const std::string& request_id, lubancode::agent::OutputCancelSource source) override {
        if (inner_) inner_->OnOutputCancelled(request_id, source);
    }

    const OperationUsage& usage() const noexcept { return usage_; }
    bool model_called() const noexcept { return model_called_; }
    const std::string& learning_turn_id() const noexcept { return prepared_turn_id_; }

private:
    std::unique_ptr<lubancode::agent::LoopBoundaryRecorder> inner_;
    OperationUsage usage_;
    std::string prepared_request_id_;
    std::string prepared_model_;
    std::string prepared_turn_id_;
    std::string prepared_step_id_;
    bool model_called_ = false;
};

} // namespace

Result<std::shared_ptr<SessionMemoryLearning>> SessionMemoryLearning::Prepare(
    const std::optional<LearningOptions>& selection,
    std::unique_ptr<Backend>& learning_backend,
    const std::optional<Connection>& learning_connection,
    fs::path owned_root,
    lubancode::workspace::WorkspaceIdentity identity,
    std::string resume_id) {
    auto owner = std::shared_ptr<SessionMemoryLearning>(new SessionMemoryLearning);
    owner->owned_root_ = std::move(owned_root);
    owner->identity_ = std::move(identity);
    owner->resume_id_ = std::move(resume_id);
    owner->snapshot_.workspace_key = owner->identity_.workspace_key;
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
    owner->candidate_store_ = std::make_shared<LocalProjectCandidateStore>(
        owner->owned_root_, testing::SnapshotLearningCandidateStoreHook());
#else
    owner->candidate_store_ = std::make_shared<LocalProjectCandidateStore>(owner->owned_root_);
#endif

    const auto validate_options = [](const LearningOptions& opts) -> Result<void> {
        if (opts.mode == LearningMode::Auto) {
            return std::unexpected(Fail("sdk.memory_learning.auto_unsupported",
                "automatic direct memory commit is not enabled in Slice 1"));
        }
        if (opts.mode != LearningMode::Off && opts.mode != LearningMode::Review) {
            return std::unexpected(Fail("sdk.memory_learning.invalid_options"));
        }
        if (!SafeText(opts.model) || !SafeText(opts.reasoning_effort) ||
            opts.model.size() > 256 || opts.reasoning_effort.size() > 256 ||
            opts.timeout_secs <= 0 || opts.timeout_secs > 600 ||
            opts.max_candidates == 0 || opts.max_candidates > 3 ||
            opts.max_transcript_bytes < 256 || opts.max_transcript_bytes > 65536) {
            return std::unexpected(Fail("sdk.memory_learning.invalid_options"));
        }
        if (opts.mode == LearningMode::Review && opts.model.empty()) {
            return std::unexpected(Fail("sdk.memory_learning.invalid_options",
                "Review mode requires a nonempty extraction model"));
        }
        return {};
    };

    if (selection) {
        if (auto ok = validate_options(*selection); !ok) return std::unexpected(ok.error());
        owner->snapshot_.mode = selection->mode;
        owner->snapshot_.model = selection->model;
        owner->snapshot_.reasoning_effort = selection->reasoning_effort;
        owner->snapshot_.timeout_secs = selection->timeout_secs;
        owner->snapshot_.max_candidates = selection->max_candidates;
        owner->snapshot_.max_transcript_bytes = selection->max_transcript_bytes;
    }

    if (!owner->resume_id_.empty()) {
        auto dir = lubancode::workspace::index::ResolveDirByWorkspaceKey(
            owner->owned_root_ / "workspaces", owner->identity_.workspace_key);
        if (!dir) return std::unexpected(Fail("sdk.session.open_failed", "resume unavailable"));
        owner->expected_resume_dir_ = *dir / "sessions" / owner->resume_id_;
        auto bytes = ReadOwned(owner->expected_resume_dir_ / kPlanFile, owner->owned_root_, kPlanCap);
        if (!bytes) return std::unexpected(bytes.error());
        if (*bytes) {
            const auto plan = Json::parse(**bytes, nullptr, false);
            if (!plan.is_object() || !plan.contains("mode") || !plan["mode"].is_string())
                return std::unexpected(Fail("sdk.memory_learning.plan_invalid"));
            const auto parsed_mode = ParseModeName(plan["mode"].get<std::string>());
            if (!parsed_mode || *parsed_mode == LearningMode::Auto ||
                !plan.contains("model") || !plan["model"].is_string() ||
                !plan.contains("reasoningEffort") || !plan["reasoningEffort"].is_string() ||
                !plan.contains("timeoutSecs") || !plan["timeoutSecs"].is_number_integer() ||
                !plan.contains("maxCandidates") || !plan["maxCandidates"].is_number_unsigned() ||
                !plan.contains("maxTranscriptBytes") || !plan["maxTranscriptBytes"].is_number_unsigned()) {
                return std::unexpected(Fail("sdk.memory_learning.plan_invalid"));
            }
            LearningOptions saved;
            saved.mode = *parsed_mode;
            saved.model = plan["model"].get<std::string>();
            saved.reasoning_effort = plan["reasoningEffort"].get<std::string>();
            saved.timeout_secs = plan["timeoutSecs"].get<int>();
            saved.max_candidates = plan["maxCandidates"].get<std::size_t>();
            saved.max_transcript_bytes = plan["maxTranscriptBytes"].get<std::size_t>();
            if (!validate_options(saved))
                return std::unexpected(Fail("sdk.memory_learning.plan_invalid"));
            if (selection && *selection != saved)
                return std::unexpected(Fail("sdk.memory_learning.resume_mismatch"));
            owner->snapshot_.mode = saved.mode;
            owner->snapshot_.model = saved.model;
            owner->snapshot_.reasoning_effort = saved.reasoning_effort;
            owner->snapshot_.timeout_secs = saved.timeout_secs;
            owner->snapshot_.max_candidates = saved.max_candidates;
            owner->snapshot_.max_transcript_bytes = saved.max_transcript_bytes;
            if (plan != owner->Plan(owner->resume_id_) || plan.dump() != **bytes)
                return std::unexpected(Fail("sdk.memory_learning.plan_invalid"));
            owner->saved_plan_bytes_ = **bytes;
            owner->snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(**bytes);
        } else {
            if (selection)
                return std::unexpected(Fail("sdk.memory_learning.resume_mismatch",
                    "legacy session cannot gain a memory learning plan"));
            owner->legacy_ = true;
            owner->snapshot_.mode = LearningMode::Off;
        }
    }

    if (owner->snapshot_.mode == LearningMode::Off) {
        if (learning_backend != nullptr || learning_connection.has_value()) {
            return std::unexpected(Fail("sdk.memory_learning.unexpected_backend",
                "memory_learning backend/connection supplied while learning mode is Off"));
        }
        return owner;
    }

    const bool has_backend = learning_backend != nullptr;
    const bool has_connection = learning_connection.has_value();
    if (has_backend && has_connection) {
        return std::unexpected(Fail("sdk.memory_learning.backend_conflict",
            "provide exclusively one of memory_learning_backend or memory_learning_connection"));
    }
    if (!has_backend && !has_connection) {
        return std::unexpected(Fail("sdk.memory_learning.missing_backend",
            "Review mode requires memory_learning_backend or memory_learning_connection"));
    }

    if (has_backend) {
        owner->owned_backend_ = detail::OwnBackend(learning_backend);
        if (!owner->owned_backend_) {
            return std::unexpected(Fail("sdk.memory_learning.missing_backend"));
        }
        owner->wire_name_ = "sdk_custom";
        owner->provider_name_ = "sdk";
    } else {
        const auto& source = *learning_connection;
        if (source.base_url.empty() || !SafeText(source.base_url) ||
            !SafeText(source.api_key) ||
            source.connect_timeout_ms <= 0 || source.idle_timeout_seconds <= 0 ||
            source.request_timeout_seconds <= 0) {
            return std::unexpected(Fail("sdk.memory_learning.invalid_connection"));
        }
        lubancode::config::Config cfg;
        switch (source.wire) {
        case Wire::Anthropic:
            cfg.wire = lubancode::config::Wire::Anthropic;
            owner->wire_name_ = "anthropic";
            owner->provider_name_ = "anthropic";
            break;
        case Wire::ChatCompletions:
            cfg.wire = lubancode::config::Wire::ChatCompletions;
            owner->wire_name_ = "chat_completions";
            owner->provider_name_ = "openai_compatible";
            break;
        case Wire::Responses:
            cfg.wire = lubancode::config::Wire::Responses;
            owner->wire_name_ = "responses";
            owner->provider_name_ = "openai";
            break;
        case Wire::Gemini:
            cfg.wire = lubancode::config::Wire::GoogleGenerateContent;
            owner->wire_name_ = "gemini";
            owner->provider_name_ = "gemini";
            break;
        default:
            return std::unexpected(Fail("sdk.memory_learning.invalid_connection"));
        }
        cfg.base_url = source.base_url;
        cfg.auth_token = source.api_key;
        cfg.connect_timeout_ms = source.connect_timeout_ms;
        cfg.stream_idle_timeout_secs = source.idle_timeout_seconds;
        cfg.request_hard_timeout_secs = source.request_timeout_seconds;
        owner->backend_config_ = std::move(cfg);
    }

    return owner;
}

Json SessionMemoryLearning::Plan(const std::string& session) const {
    return {
        {"schemaVersion", 1},
        {"mode", ModeName(snapshot_.mode)},
        {"sessionId", session},
        {"workspaceKey", identity_.workspace_key},
        {"projectRoot", lubancode::workspace::NormalizeIdentityPathText(identity_.project_root)},
        {"model", snapshot_.model},
        {"reasoningEffort", snapshot_.reasoning_effort},
        {"timeoutSecs", snapshot_.timeout_secs},
        {"maxCandidates", snapshot_.max_candidates},
        {"maxTranscriptBytes", snapshot_.max_transcript_bytes},
        {"limits", {{"reportBytes", kReportCap}, {"reportTotalBytes", kTotalCap}, {"directoryEntries", kDirEntryCap}}},
    };
}

Result<void> SessionMemoryLearning::CheckBinding(const v3::V3Ledger& source) const {
    if (legacy_) {
        for (const auto& message : source.messages) {
            if (Binding(message)) return std::unexpected(Fail("sdk.memory_learning.plan_invalid"));
        }
        return {};
    }
    const Json expected{{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}};
    const auto matches = [&](const v3::MessageLine* m) {
        return m && Binding(*m) && *Binding(*m) == expected;
    };
    if (source.messages.empty() || !matches(&source.messages.front()) ||
        !matches(source.FindMessage(source.context.system_message_ref))) {
        return std::unexpected(Fail("sdk.memory_learning.plan_invalid"));
    }
    for (const auto& [revision, chain] : source.revision_chains) {
        (void)revision;
        if (!matches(source.FindMessage(chain.first)))
            return std::unexpected(Fail("sdk.memory_learning.plan_invalid"));
    }
    return {};
}

Result<void> SessionMemoryLearning::CheckSavedReports(const v3::V3Ledger& source) {
    const fs::path dir = session_dir_ / kReportsDir;
    if (!lubancode::platform::IsUnlinkedOwnedPath(session_dir_, dir)) {
        return std::unexpected(Fail("sdk.memory_learning.path_escape"));
    }
    const auto io_dir = lubancode::platform::FileIoPath(dir);
    std::error_code ec;
    const auto dir_status = fs::symlink_status(io_dir, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && dir_status.type() == fs::file_type::not_found))
        return {};
    if (ec || fs::is_symlink(dir_status) || !fs::is_directory(dir_status) ||
        !lubancode::platform::RejectReparsePoint(io_dir))
        return std::unexpected(Fail("sdk.memory_learning.report_invalid"));

    std::size_t entries = 0;
    std::size_t total_bytes = 0;
    for (fs::directory_iterator it(io_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (++entries > kDirEntryCap) return std::unexpected(Fail("sdk.memory_learning.report_limit"));
        const fs::path entry_path = dir / it->path().filename();
        if (entry_path.extension() == ".tmp") continue;
        std::error_code item_ec;
        const auto io_entry = lubancode::platform::FileIoPath(entry_path);
        const auto item_status = fs::symlink_status(io_entry, item_ec);
        if (item_ec || fs::is_symlink(item_status) || !fs::is_regular_file(item_status) ||
            !lubancode::platform::RejectReparsePoint(io_entry)) {
            return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
        }
        const auto name = lubancode::platform::PathToUtf8(entry_path.filename());
        if (name.size() <= 5 || name.substr(name.size() - 5) != ".json")
            return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
        const auto op_id = name.substr(0, name.size() - 5);
        if (!SafeId(op_id)) return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
        auto bytes = ReadOwned(entry_path, session_dir_, kReportCap);
        if (!bytes) return std::unexpected(bytes.error());
        if (!*bytes) return std::unexpected(Fail("sdk.memory_learning.report_unavailable"));
        total_bytes += (*bytes)->size();
        if (total_bytes > kTotalCap) return std::unexpected(Fail("sdk.memory_learning.report_limit"));
        auto decoded = DecodeReport(op_id, **bytes);
        if (!decoded) return std::unexpected(decoded.error());
        if (auto valid = ValidateReport(*decoded, &source, false); !valid) {
            return std::unexpected(valid.error());
        }
    }
    if (ec) return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
    return {};
}

std::expected<Json, std::string> SessionMemoryLearning::Open(const lubancode::trajectory::V3OpeningContext& context) {
    const auto fail = [](const Error& e) -> std::expected<Json, std::string> {
        return std::unexpected(e.code + ": " + e.message);
    };
    std::error_code ec;
    const auto dir = fs::canonical(context.session_dir, ec);
    const auto root = fs::canonical(owned_root_, ec);
    if (ec || !Inside(dir, root / "workspaces")) return std::unexpected("sdk.memory_learning.path_escape");
    session_dir_ = context.session_dir;
    snapshot_.session_id = context.session_id;
    memory_dir_ = dir.parent_path().parent_path() / "memory";
    snapshot_.memory_directory = lubancode::tools::PathToUtf8(memory_dir_);

    if (context.source) {
        if (context.session_id != resume_id_ || context.session_dir != expected_resume_dir_)
            return std::unexpected("sdk.memory_learning.resume_mismatch");
        auto bytes = ReadOwned(session_dir_ / kPlanFile, owned_root_, kPlanCap);
        if (!bytes) return fail(bytes.error());
        if (legacy_ ? bytes->has_value() : !bytes->has_value() || **bytes != saved_plan_bytes_)
            return std::unexpected("sdk.memory_learning.plan_invalid");
        if (auto valid = CheckBinding(*context.source); !valid) return fail(valid.error());
        if (auto valid = CheckSavedReports(*context.source); !valid) return fail(valid.error());
        if (auto valid = CheckSavedAcceptReports(); !valid) return fail(valid.error());
    } else {
        if (!resume_id_.empty()) return std::unexpected("sdk.memory_learning.resume_mismatch");
        auto prior = ReadOwned(session_dir_ / kPlanFile, owned_root_, kPlanCap);
        if (!prior) return fail(prior.error());
        if (*prior) return std::unexpected("sdk.memory_learning.plan_invalid");
        const auto bytes = Plan(context.session_id).dump();
        const auto saved = WriteFrozenPlan(session_dir_ / kPlanFile, bytes);
        if (!saved) {
            const auto& error = saved.error();
            return std::unexpected("sdk.memory_learning.plan_write_failed: " + Json{
                {"atomicCode", error.code},
                {"failureKind", error.failure_kind == lubancode::platform::WriteFailureKind::TransientReject
                    ? "TransientReject" : "Permanent"},
                {"outcome", WriteOutcomeName(error.outcome)},
                {"message", error.message}}.dump(-1, ' ', false, Json::error_handler_t::replace));
        }
        if (saved->outcome != lubancode::platform::WriteOutcome::CommittedDurable) {
            return std::unexpected("sdk.memory_learning.plan_write_failed: plan receipt was not CommittedDurable");
        }
        snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(bytes);
    }
    return legacy_
        ? Json::object()
        : Json{{"hostBindings", {{"memoryLearning", {{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}}}}}};
}

lubancode::trajectory::V3OpeningParticipant SessionMemoryLearning::OpeningParticipant() {
    return [owner = shared_from_this()](const auto& context) { return owner->Open(context); };
}

LearningSnapshot SessionMemoryLearning::Describe() const {
    return snapshot_;
}

LearningReport SessionMemoryLearning::EmptyReport(
    const std::string& operation_id,
    const std::string& turn_id,
    LearningStage stage) const {
    LearningReport report;
    report.mode = snapshot_.mode;
    report.session_id = snapshot_.session_id;
    report.operation_id = operation_id;
    report.turn_id = turn_id;
    report.workspace_key = snapshot_.workspace_key;
    report.plan_sha256 = snapshot_.plan_sha256;
    report.stage = stage;
    report.model = snapshot_.model;
    return report;
}

Result<void> SessionMemoryLearning::PersistReport(const LearningReport& report) const {
    if (!SafeId(report.operation_id)) return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
    if (auto valid = Directory(session_dir_ / kReportsDir, session_dir_, true); !valid) return valid;
    Json payload = ReportJson(report);
    payload["sha256"] = lubancode::platform::Sha256Hex(payload.dump());
    const auto bytes = payload.dump();
    if (bytes.size() > kReportCap) return std::unexpected(Fail("sdk.memory_learning.report_limit"));
    const auto path = session_dir_ / kReportsDir / (report.operation_id + ".json");
    auto existing = ReadOwned(path, session_dir_, kReportCap);
    if (!existing) return std::unexpected(existing.error());
    const auto saved = lubancode::platform::AtomicWriteFile(
        path, bytes, lubancode::platform::WriteDurability::ProcessCrashDurability);
    if (!saved || saved->outcome != lubancode::platform::WriteOutcome::CommittedDurable) {
        return std::unexpected(Fail("sdk.memory_learning.report_write_failed",
            saved ? "unconfirmed" : saved.error().message));
    }
    return {};
}

Result<std::optional<LearningReport>> SessionMemoryLearning::ReadReportFromDisk(
    const std::string& operation_id) const {
    if (!SafeId(operation_id)) return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
    if (auto valid = Directory(session_dir_ / kReportsDir, session_dir_, false); !valid)
        return std::unexpected(valid.error());
    auto bytes = ReadOwned(session_dir_ / kReportsDir / (operation_id + ".json"), session_dir_, kReportCap);
    if (!bytes) return std::unexpected(bytes.error());
    if (!*bytes) return std::optional<LearningReport>{};
    auto decoded = DecodeReport(operation_id, **bytes);
    if (!decoded) return std::unexpected(decoded.error());
    return std::optional<LearningReport>{std::move(*decoded)};
}

Result<LearningReport> SessionMemoryLearning::DecodeReport(
    const std::string& operation_id,
    const std::string& bytes) const {
    try {
        auto value = Json::parse(bytes);
        if (!value.is_object() || !value.contains("sha256") || !value["sha256"].is_string())
            return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
        const auto hash = value.at("sha256").get<std::string>();
        value.erase("sha256");
        if (hash != lubancode::platform::Sha256Hex(value.dump()))
            return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
        auto report = ParseReport(value);
        if (report.session_id != snapshot_.session_id ||
            report.operation_id != operation_id ||
            report.workspace_key != snapshot_.workspace_key ||
            report.plan_sha256 != snapshot_.plan_sha256 ||
            report.mode != snapshot_.mode) {
            return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
        }
        return report;
    } catch (const std::exception&) {
        return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
    }
}

Result<void> SessionMemoryLearning::ValidateReport(
    const LearningReport& report,
    const v3::V3Ledger* source,
    bool require_complete) const {
    if (report.session_id != snapshot_.session_id ||
        report.workspace_key != snapshot_.workspace_key ||
        report.plan_sha256 != snapshot_.plan_sha256 ||
        report.mode != snapshot_.mode) {
        return std::unexpected(Fail("sdk.memory_learning.report_invalid"));
    }
    if (require_complete && report.stage == LearningStage::Running) {
        return std::unexpected(Fail("sdk.memory_learning.report_invalid",
            "saved report remained in Running state"));
    }
    if (!report.usage.attempts.empty()) {
        if (!source) return std::unexpected(Fail("sdk.memory_learning.report_invalid",
            "verified ledger unavailable for learning usage"));
        const auto bound = ValidateBypassUsageBinding(report, *source);
        if (!bound) return std::unexpected(Fail("sdk.memory_learning.report_invalid", bound.error()));
    }
    return {};
}

void SessionMemoryLearning::ReapFinishedWorkerLocked() {
    if (active_slot_ && active_slot_->done.load()) {
        if (worker_.joinable()) worker_.join();
        active_slot_.reset();
    }
}

std::shared_ptr<Backend> SessionMemoryLearning::SnapshotBackend() const {
    std::lock_guard lock(backend_mutex_);
    return owned_backend_;
}

SessionMemoryLearning::PreparedDispatch SessionMemoryLearning::PrepareTurnDispatch(
    const TurnMaterials& materials,
    rt::TrajectorySessionLedger& ledger) {
    PreparedDispatch out;
    if (snapshot_.mode == LearningMode::Off) {
        return out;
    }
    if (!materials.turn_succeeded) {
        out.early_terminal = true;
        out.terminal_report = EmptyReport(materials.operation_id, materials.turn_id, LearningStage::NotAttempted);
        return out;
    }

    const auto make_skip = [&](std::string reason) {
        PreparedDispatch skip;
        skip.early_terminal = true;
        skip.terminal_report = EmptyReport(materials.operation_id, materials.turn_id, LearningStage::Skipped);
        skip.terminal_report.skip_reason = std::move(reason);
        return skip;
    };

    const bool has_new_history = std::any_of(
        materials.turn_messages.begin(), materials.turn_messages.end(),
        [](const lubancode::api::Message& m) { return m.role == lubancode::api::Role::Assistant; });
    if (!has_new_history) {
        return make_skip(gates::ExtractionSkipReasonName(gates::ExtractionSkipReason::NoNewHistory));
    }
    if (materials.turn_mutated) {
        return make_skip(gates::ExtractionSkipReasonName(gates::ExtractionSkipReason::AlreadyMutated));
    }

    const auto stats = gates::ComputeMeaningfulTextStats(materials.user_text);
    const auto signals = gates::EvaluateTurnDurableSignals(
        materials.user_text, materials.assistant_text, materials.has_tool_evidence);
    if (const auto reason = gates::EvaluateMustSkipTextGate(stats, materials.has_tool_evidence); reason.has_value()) {
        if (*reason != gates::ExtractionSkipReason::ShortText || signals.empty()) {
            return make_skip(gates::ExtractionSkipReasonName(*reason));
        }
    }
    if (signals.empty()) {
        return make_skip(gates::ExtractionSkipReasonName(gates::ExtractionSkipReason::NoDurableSignal));
    }

    std::string transcript = extract::BuildTurnTranscript(materials.turn_messages, snapshot_.max_transcript_bytes);
    if (transcript.empty()) {
        return make_skip(gates::ExtractionSkipReasonName(gates::ExtractionSkipReason::EmptyTranscript));
    }

    std::string task_type = extract::ClassifyTaskType(materials.user_text, materials.tool_names);
    std::string system_prompt = extract::BuildExtractionSystemPrompt(std::string{}, task_type);
    if (system_prompt.empty()) {
        return make_skip(gates::ExtractionSkipReasonName(gates::ExtractionSkipReason::PromptMissing));
    }

    {
        std::lock_guard wlock(worker_mutex_);
        ReapFinishedWorkerLocked();
        if (closing_.load()) {
            out.early_terminal = true;
            out.terminal_report = EmptyReport(materials.operation_id, materials.turn_id, LearningStage::Cancelled);
            out.terminal_report.task_type = std::move(task_type);
            out.terminal_report.error_code = "cancelled";
            out.terminal_report.error = "session closing before learning dispatch";
            return out;
        }
        if (active_slot_ != nullptr) {
            out.early_terminal = true;
            out.terminal_report = EmptyReport(materials.operation_id, materials.turn_id, LearningStage::BusyDropped);
            out.terminal_report.task_type = std::move(task_type);
            out.terminal_report.error_code = "in_flight_dropped";
            out.terminal_report.error = "previous memory learning operation still active";
            return out;
        }
    }

    rt::TrajectoryTurnBridge::Identity id{provider_name_, wire_name_, "host"};
    out.bypass = ledger.NewLeasedBypassRecorder(std::move(id), lubancode::accounting::RequestPurpose::MemoryExtract);
    out.task_type = std::move(task_type);
    out.system_prompt = std::move(system_prompt);
    out.transcript = std::move(transcript);
    out.ready = true;
    return out;
}

void SessionMemoryLearning::SettleOrStartOperation(
    const Operation& completed,
    PreparedDispatch&& dispatch,
    rt::TrajectorySessionLedger& ledger) {
    (void)ledger;
    if (snapshot_.mode == LearningMode::Off) {
        LearningReport report = EmptyReport(completed.operation_id, completed.turn_id, LearningStage::NotAttempted);
        std::lock_guard rlock(reports_mutex_);
        reports_.insert_or_assign(completed.operation_id, std::move(report));
        return;
    }

    const auto store_terminal = [&](LearningReport report) {
        if (!completed.turn_id.empty()) report.turn_id = completed.turn_id;
        auto saved = PersistReport(report);
        if (!saved) {
            report.stage = LearningStage::Indeterminate;
            report.error_code = saved.error().code;
            report.error = saved.error().message;
        }
        std::lock_guard rlock(reports_mutex_);
        reports_.insert_or_assign(completed.operation_id, std::move(report));
    };

    if (!completed.result_persisted || completed.state != OperationState::Succeeded || completed.turn_id.empty()) {
        dispatch.bypass.reset();
        store_terminal(EmptyReport(completed.operation_id, completed.turn_id, LearningStage::NotAttempted));
        return;
    }

    if (dispatch.early_terminal) {
        dispatch.bypass.reset();
        store_terminal(std::move(dispatch.terminal_report));
        return;
    }

    if (!dispatch.ready || !dispatch.bypass) {
        dispatch.bypass.reset();
        LearningReport report = EmptyReport(completed.operation_id, completed.turn_id, LearningStage::Failed);
        report.task_type = dispatch.task_type;
        report.error_code = "worker_start_failed";
        report.error = "memory learning dispatch could not acquire bypass recorder";
        store_terminal(std::move(report));
        return;
    }

    std::unique_ptr<lubancode::api::Backend> call_backend;
    if (auto backend = SnapshotBackend()) {
        call_backend = detail::AdaptBackend(std::move(backend));
    } else if (backend_config_) {
        call_backend = rt::assembly::BuildBackend(*backend_config_);
    }
    if (!call_backend) {
        dispatch.bypass.reset();
        LearningReport report = EmptyReport(completed.operation_id, completed.turn_id, LearningStage::StartFailed);
        report.task_type = dispatch.task_type;
        report.error_code = "worker_start_failed";
        report.error = "memory learning backend unavailable";
        store_terminal(std::move(report));
        return;
    }

    std::lock_guard wlock(worker_mutex_);
    ReapFinishedWorkerLocked();
    if (closing_.load()) {
        call_backend.reset();
        dispatch.bypass.reset();
        LearningReport report = EmptyReport(completed.operation_id, completed.turn_id, LearningStage::Cancelled);
        report.task_type = dispatch.task_type;
        report.error_code = "cancelled";
        report.error = "session closing before worker launch";
        store_terminal(std::move(report));
        return;
    }
    if (active_slot_ != nullptr) {
        call_backend.reset();
        dispatch.bypass.reset();
        LearningReport report = EmptyReport(completed.operation_id, completed.turn_id, LearningStage::BusyDropped);
        report.task_type = dispatch.task_type;
        report.error_code = "in_flight_dropped";
        report.error = "previous memory learning operation still active";
        store_terminal(std::move(report));
        return;
    }

    LearningReport running_report = EmptyReport(completed.operation_id, completed.turn_id, LearningStage::Running);
    running_report.task_type = dispatch.task_type;
    if (auto persisted = PersistReport(running_report); !persisted) {
        call_backend.reset();
        dispatch.bypass.reset();
        running_report.stage = LearningStage::StartFailed;
        running_report.error_code = persisted.error().code;
        running_report.error = persisted.error().message;
        std::lock_guard rlock(reports_mutex_);
        reports_.insert_or_assign(completed.operation_id, std::move(running_report));
        return;
    }
    {
        std::lock_guard rlock(reports_mutex_);
        reports_.insert_or_assign(completed.operation_id, running_report);
    }

    auto slot = std::make_shared<WorkerSlot>(completed.operation_id);
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
    auto start_hook = testing::SnapshotLearningWorkerStartHook();
    auto exec_hook = testing::SnapshotLearningWorkerExecutionHook();
#endif

    // Wrap unique resources in a shared holder so any failure before thread creation
    // lets the invoking thread deterministically clean up without dangling borrows.
    struct LaunchBox {
        std::unique_ptr<lubancode::api::Backend> backend;
        std::unique_ptr<lubancode::agent::LoopBoundaryRecorder> bypass;
    };
    auto box = std::make_shared<LaunchBox>(LaunchBox{std::move(call_backend), std::move(dispatch.bypass)});

    auto self = shared_from_this();
    LearningReport base_report = EmptyReport(completed.operation_id, completed.turn_id, LearningStage::Failed);
    base_report.task_type = dispatch.task_type;
    auto store = candidate_store_;
    const fs::path mem_dir = memory_dir_;
    const LearningSnapshot snap = snapshot_;
    std::string system_prompt = std::move(dispatch.system_prompt);
    std::string transcript = std::move(dispatch.transcript);
    std::string task_type = std::move(dispatch.task_type);

    auto worker_fn = [self, slot, box, store, mem_dir, snap,
                      base_report = std::move(base_report),
                      system_prompt = std::move(system_prompt),
                      transcript = std::move(transcript),
                      task_type = std::move(task_type)
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
                      , exec_hook
#endif
    ]() mutable {
        LearningReport report = std::move(base_report);
        auto proxy = std::make_unique<LearningRecorderProxy>(std::move(box->bypass));
        try {
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
            if (exec_hook && *exec_hook) (*exec_hook)(slot->operation_id);
#endif
            auto extracted = extract::RunMemoryExtraction(
                *box->backend, snap.model, system_prompt, transcript, snap.timeout_secs,
                snap.reasoning_effort, nullptr, proxy.get(), &slot->cancel);

            report.usage = proxy->usage();
            report.model_called = proxy->model_called();
            report.learning_turn_id = proxy->learning_turn_id();

            if (slot->cancel.load() || self->closing_.load()) {
                report.stage = LearningStage::Cancelled;
                report.error_code = "cancelled";
                report.error = extracted ? std::string("memory learning cancelled") : extracted.error().message;
            } else if (!extracted) {
                report.stage = LearningStage::Failed;
                report.error_code = extract::ExtractionErrorCodeName(extracted.error().code);
                report.error = extracted.error().message;
            } else {
                report.retrieval_hints = extracted->retrieval_terms;
                const std::size_t limit = std::min(extracted->candidates.size(), snap.max_candidates);
                bool store_failed = false;
                for (std::size_t i = 0; i < limit; ++i) {
                    if (slot->cancel.load() || self->closing_.load()) {
                        report.stage = LearningStage::Cancelled;
                        report.error_code = "cancelled";
                        report.error = "memory learning cancelled before candidate write";
                        store_failed = true;
                        break;
                    }
                    const auto& proposed = extracted->candidates[i];
                    const auto kind = mem::ParseMemoryKind(proposed.kind);
                    if (!kind.has_value()) continue;
                    mem::MemoryCandidate cand;
                    cand.kind = *kind;
                    cand.title = proposed.title;
                    cand.summary = proposed.summary;
                    cand.content = proposed.content;
                    cand.keywords = proposed.keywords;
                    cand.paths = proposed.paths;
                    cand.confidence = proposed.confidence;
                    cand.occurred_at = proposed.occurred_at;
                    cand.task_type = task_type;
                    auto added = store->Add(mem_dir, std::move(cand));
                    if (!added) {
                        report.stage = LearningStage::Failed;
                        report.error_code = "sdk.memory_learning.candidate_store_failed";
                        report.error = added.error();
                        store_failed = true;
                        break;
                    }
                    report.candidate_ids.push_back(*added);
                }
                if (!store_failed) {
                    report.stage = LearningStage::Completed;
                }
            }
        } catch (const std::exception& e) {
            report.usage = proxy->usage();
            report.model_called = proxy->model_called();
            report.learning_turn_id = proxy->learning_turn_id();
            report.stage = (slot->cancel.load() || self->closing_.load())
                ? LearningStage::Cancelled : LearningStage::Failed;
            report.error_code = report.stage == LearningStage::Cancelled ? "cancelled" : "worker_exception";
            report.error = e.what();
        } catch (...) {
            report.usage = proxy->usage();
            report.model_called = proxy->model_called();
            report.learning_turn_id = proxy->learning_turn_id();
            report.stage = (slot->cancel.load() || self->closing_.load())
                ? LearningStage::Cancelled : LearningStage::Failed;
            report.error_code = report.stage == LearningStage::Cancelled
                ? "cancelled" : "worker_unknown_exception";
            report.error = "unknown non-std exception in memory learning worker";
        }

        // Destroy proxy (retiring bypass recorder) and backend BEFORE setting done/persisting.
        proxy.reset();
        box->backend.reset();

        if (auto saved = self->PersistReport(report); !saved) {
            report.stage = LearningStage::Indeterminate;
            report.error_code = saved.error().code;
            report.error = saved.error().message;
        }
        {
            std::lock_guard rlock(self->reports_mutex_);
            self->reports_.insert_or_assign(slot->operation_id, std::move(report));
        }
        slot->done.store(true);
    };

    try {
#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
        if (start_hook && *start_hook) (*start_hook)();
#endif
        worker_ = std::thread(std::move(worker_fn));
        active_slot_ = std::move(slot);
    } catch (const std::exception& e) {
        box->bypass.reset();
        box->backend.reset();
        slot->done.store(true);
        LearningReport start_failure = EmptyReport(completed.operation_id, completed.turn_id, LearningStage::StartFailed);
        start_failure.task_type = task_type;
        start_failure.error_code = "worker_start_failed";
        start_failure.error = e.what();
        store_terminal(std::move(start_failure));
    } catch (...) {
        box->bypass.reset();
        box->backend.reset();
        slot->done.store(true);
        LearningReport start_failure = EmptyReport(completed.operation_id, completed.turn_id, LearningStage::StartFailed);
        start_failure.task_type = task_type;
        start_failure.error_code = "worker_start_failed";
        start_failure.error = "unknown error starting memory learning worker";
        store_terminal(std::move(start_failure));
    }
}

Result<void> SessionMemoryLearning::LoadOperationReport(
    const std::string& operation_id,
    const Operation& operation,
    const v3::V3Ledger* ledger,
    bool require_persisted_valid) {
    if (snapshot_.mode == LearningMode::Off || legacy_) {
        std::lock_guard rlock(reports_mutex_);
        reports_.insert_or_assign(operation_id,
            EmptyReport(operation_id, operation.turn_id, LearningStage::NotAttempted));
        return {};
    }

    auto disk = ReadReportFromDisk(operation_id);
    if (!disk) {
        if (require_persisted_valid) return std::unexpected(disk.error());
        std::lock_guard rlock(reports_mutex_);
        reports_.insert_or_assign(operation_id, Result<LearningReport>(std::unexpected(disk.error())));
        return {};
    }

    if (!*disk) {
        LearningReport missing = EmptyReport(operation_id, operation.turn_id,
            operation.state == OperationState::Succeeded ? LearningStage::Indeterminate : LearningStage::NotAttempted);
        if (missing.stage == LearningStage::Indeterminate) {
            missing.error_code = "sdk.memory_learning.report_unavailable";
            missing.error = "persisted learning report missing on resume";
        }
        std::lock_guard rlock(reports_mutex_);
        reports_.insert_or_assign(operation_id, std::move(missing));
        return {};
    }

    LearningReport report = std::move(**disk);
    if (!operation.turn_id.empty() && report.turn_id != operation.turn_id) {
        auto err = Fail("sdk.memory_learning.report_invalid", "operation turn differs");
        if (require_persisted_valid) return std::unexpected(err);
        std::lock_guard rlock(reports_mutex_);
        reports_.insert_or_assign(operation_id, Result<LearningReport>(std::unexpected(std::move(err))));
        return {};
    }

    if (auto valid = ValidateReport(report, ledger, false); !valid) {
        if (require_persisted_valid) return std::unexpected(valid.error());
        std::lock_guard rlock(reports_mutex_);
        reports_.insert_or_assign(operation_id, Result<LearningReport>(std::unexpected(valid.error())));
        return {};
    }

    if (report.stage == LearningStage::Running) {
        // Crash recovery: never replay models on resume; expose indeterminate gap.
        report.stage = LearningStage::Indeterminate;
        report.error_code = "sdk.memory_learning.interrupted";
        report.error = "memory learning was running when session terminated";
        report.usage.attempts_complete = false;
    }

    std::lock_guard rlock(reports_mutex_);
    reports_.insert_or_assign(operation_id, std::move(report));
    return {};
}

void SessionMemoryLearning::PopulateNotAttempted(
    const std::string& operation_id,
    const std::string& turn_id) {
    LearningReport report = EmptyReport(operation_id, turn_id, LearningStage::NotAttempted);
    if (snapshot_.mode != LearningMode::Off && !session_dir_.empty()) {
        if (auto saved = PersistReport(report); !saved) {
            report.stage = LearningStage::Indeterminate;
            report.error_code = saved.error().code;
            report.error = saved.error().message;
        }
    }
    std::lock_guard rlock(reports_mutex_);
    reports_.insert_or_assign(operation_id, std::move(report));
}

Result<LearningReport> SessionMemoryLearning::GetReport(const std::string& operation_id) const {
    std::lock_guard rlock(reports_mutex_);
    const auto it = reports_.find(operation_id);
    if (it == reports_.end()) {
        return std::unexpected(Fail("sdk.memory_learning.report_not_ready"));
    }
    return it->second;
}

void SessionMemoryLearning::CancelOperation(const std::string& operation_id) {
    std::lock_guard wlock(worker_mutex_);
    if (active_slot_ && active_slot_->operation_id == operation_id) {
        active_slot_->cancel.store(true);
    }
}

void SessionMemoryLearning::RequestClose() {
    closing_.store(true);
    std::lock_guard wlock(worker_mutex_);
    if (active_slot_) {
        active_slot_->cancel.store(true);
    }
}

void SessionMemoryLearning::JoinAndSettle() {
    std::lock_guard wlock(worker_mutex_);
    if (active_slot_) {
        active_slot_->cancel.store(true);
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    active_slot_.reset();
}

void SessionMemoryLearning::ReleaseBackend() {
    std::lock_guard lock(backend_mutex_);
    owned_backend_.reset();
}

SessionMemoryLearning::~SessionMemoryLearning() {
    RequestClose();
    JoinAndSettle();
}

Result<void> SessionMemoryLearning::PersistAcceptReport(const CandidateAcceptReport& report) const {
    if (!SafeId(report.commit_key)) return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
    if (auto valid = Directory(session_dir_ / kAcceptsDir, session_dir_, true); !valid) return valid;
    Json payload = AcceptReportJson(report);
    payload["sha256"] = lubancode::platform::Sha256Hex(payload.dump());
    const auto bytes = payload.dump();
    if (bytes.size() > kReportCap) return std::unexpected(Fail("sdk.memory_learning.accept_report_limit"));
    const auto path = session_dir_ / kAcceptsDir / (report.commit_key + ".json");
    auto existing = ReadOwned(path, session_dir_, kReportCap);
    if (!existing) return std::unexpected(existing.error());
    const auto saved = lubancode::platform::AtomicWriteFile(
        path, bytes, lubancode::platform::WriteDurability::ProcessCrashDurability);
    if (!saved || saved->outcome != lubancode::platform::WriteOutcome::CommittedDurable) {
        return std::unexpected(Fail("sdk.memory_learning.accept_report_write_failed",
            saved ? "unconfirmed" : saved.error().message));
    }
    return {};
}

Result<CandidateAcceptReport> SessionMemoryLearning::DecodeAcceptReport(
    const std::string& commit_key,
    const std::string& bytes) const {
    try {
        auto value = Json::parse(bytes);
        if (!value.is_object() || !value.contains("sha256") || !value["sha256"].is_string()) {
            return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
        }
        const auto hash = value.at("sha256").get<std::string>();
        value.erase("sha256");
        if (lubancode::platform::Sha256Hex(value.dump()) != hash) {
            return std::unexpected(Fail("sdk.memory_learning.accept_report_tampered"));
        }
        for (const auto* key : {"sessionId", "candidateId", "workspaceKey", "learningPlanSha256",
                "writePlanSha256", "commitKey", "sourceEventRef", "saveRequestSha256",
                "requestSha256", "state", "memoryId", "memoryPath", "contentSha256",
                "committedAt", "errorCode", "error"}) {
            if (!value.contains(key) || !value.at(key).is_string()) {
                return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
            }
        }
        if (!value.contains("duplicate") || !value.at("duplicate").is_boolean() ||
            !value.contains("stages") || !value.at("stages").is_array()) {
            return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
        }
        CandidateAcceptReport r;
        r.session_id = value.at("sessionId").get<std::string>();
        r.candidate_id = value.at("candidateId").get<std::string>();
        r.workspace_key = value.at("workspaceKey").get<std::string>();
        r.learning_plan_sha256 = value.at("learningPlanSha256").get<std::string>();
        r.write_plan_sha256 = value.at("writePlanSha256").get<std::string>();
        r.commit_key = value.at("commitKey").get<std::string>();
        r.source_event_ref = value.at("sourceEventRef").get<std::string>();
        r.save_request_sha256 = value.at("saveRequestSha256").get<std::string>();
        r.request_sha256 = value.at("requestSha256").get<std::string>();
        r.state = value.at("state").get<std::string>();
        r.memory_id = value.at("memoryId").get<std::string>();
        r.memory_path = value.at("memoryPath").get<std::string>();
        r.content_sha256 = value.at("contentSha256").get<std::string>();
        r.committed_at = value.at("committedAt").get<std::string>();
        r.duplicate = value.at("duplicate").get<bool>();
        r.error_code = value.at("errorCode").get<std::string>();
        r.error = value.at("error").get<std::string>();
        for (const auto& s : value.at("stages")) {
            if (!s.is_object() || !s.contains("stage") || !s.contains("outcome") ||
                !s.at("stage").is_string() || !s.at("outcome").is_string()) {
                return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
            }
            r.stages.push_back({s.at("stage").get<std::string>(), s.at("outcome").get<std::string>()});
        }
        if (r.commit_key != commit_key || AcceptReportJson(r) != value) {
            return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
        }
        return r;
    } catch (const std::exception&) {
        return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
    }
}

Result<void> SessionMemoryLearning::ValidateAcceptReport(const CandidateAcceptReport& report) const {
    if (legacy_ || snapshot_.mode == LearningMode::Off) {
        return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
    }
    if (!SafeId(report.session_id) || report.session_id != snapshot_.session_id ||
        !SafeCandidateId(report.candidate_id) || report.workspace_key != snapshot_.workspace_key ||
        report.learning_plan_sha256 != snapshot_.plan_sha256 || report.write_plan_sha256.empty() ||
        !SafeText(report.write_plan_sha256) || !SafeId(report.commit_key) ||
        report.source_event_ref != CandidateSourceEventRef(report.workspace_key, report.session_id, report.candidate_id) ||
        report.save_request_sha256.empty() || !SafeText(report.save_request_sha256) ||
        report.commit_key != CandidateCommitKey(report.session_id, report.candidate_id, report.save_request_sha256) ||
        !SafeText(report.request_sha256) || !SafeText(report.memory_id) || !SafeText(report.memory_path) ||
        !SafeText(report.content_sha256) || !SafeText(report.committed_at) ||
        !SafeText(report.error_code) || !SafeText(report.error)) {
        return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
    }
    if (report.state != "committed" && report.state != "not_started" && report.state != "indeterminate") {
        return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
    }
    for (const auto& stage : report.stages) {
        if (!SafeText(stage.stage) || !SafeText(stage.outcome)) {
            return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
        }
    }

    const fs::path lifecycle_root = session_dir_.parent_path().parent_path() / "lifecycle";
    if (auto valid = Directory(lifecycle_root, session_dir_.parent_path().parent_path(), false); !valid) {
        return valid;
    }
    const auto intent_bytes = ReadOwned(lifecycle_root / report.commit_key / "intent.json", lifecycle_root, kReportCap);
    if (!intent_bytes) return std::unexpected(intent_bytes.error());

    if (report.state == "committed") {
        if (!intent_bytes->has_value()) {
            return std::unexpected(Fail("sdk.memory_learning.accept_receipt_missing"));
        }
        const auto intent = Json::parse(**intent_bytes, nullptr, false);
        if (!intent.is_object() || !intent.contains("parameters") || !intent.at("parameters").is_object() ||
            !lubancode::api::usage_json::MemberStringEquals(intent, "session_id", report.session_id) ||
            !lubancode::api::usage_json::MemberStringEquals(intent, "workspace_key", report.workspace_key)) {
            return std::unexpected(Fail("sdk.memory_learning.accept_receipt_invalid"));
        }
        const auto& parameters = intent.at("parameters");
        if (!lubancode::api::usage_json::MemberStringEquals(parameters, "source_event_ref", report.source_event_ref)) {
            return std::unexpected(Fail("sdk.memory_learning.accept_receipt_invalid"));
        }
        const auto parsed_request = mem::store::ParseUpsertJob(parameters, true);
        if (!parsed_request ||
            lubancode::platform::Sha256Hex(SaveRequestJson(*parsed_request).dump()) != report.save_request_sha256) {
            return std::unexpected(Fail("sdk.memory_learning.accept_receipt_mismatch"));
        }
        const mem::ProjectCommitContext gate{
            identity_.project_root, memory_dir_, lifecycle_root,
            snapshot_.workspace_key, report.commit_key, report.session_id, report.source_event_ref};
        if (mem::ProjectCommitRequestSha256(gate, *parsed_request) != report.request_sha256) {
            return std::unexpected(Fail("sdk.memory_learning.accept_receipt_mismatch"));
        }
        const auto inspected = mem::InspectProjectCommitReceipt(gate, *parsed_request);
        if (inspected.state != mem::ProjectCommitState::Committed || !inspected.error_code.empty() ||
            inspected.memory_id != report.memory_id || inspected.memory_path != report.memory_path ||
            inspected.content_sha256 != report.content_sha256 || inspected.committed_at != report.committed_at ||
            !report.error.empty() || !report.error_code.empty() ||
            report.stages.size() != inspected.stages.size() + 1 ||
            report.stages.back() != SaveStageReceipt{"result", "durable"}) {
            return std::unexpected(Fail("sdk.memory_learning.accept_receipt_unconfirmed"));
        }
        for (std::size_t n = 0; n < inspected.stages.size(); ++n) {
            if (report.stages[n] !=
                SaveStageReceipt{CommitStageName(inspected.stages[n].stage), CommitOutcomeName(inspected.stages[n].outcome)}) {
                return std::unexpected(Fail("sdk.memory_learning.accept_receipt_unconfirmed"));
            }
        }
    } else if (report.state == "not_started") {
        if (report.error_code.empty() ||
            std::any_of(report.stages.begin(), report.stages.end(), [](const SaveStageReceipt& s) {
                return s.stage == "topic" && s.outcome != "not_committed";
            })) {
            return std::unexpected(Fail("sdk.memory_learning.accept_receipt_invalid"));
        }
        if (intent_bytes->has_value()) {
            const auto intent = Json::parse(**intent_bytes, nullptr, false);
            if (!intent.is_object() || !intent.contains("parameters") || !intent.at("parameters").is_object()) {
                return std::unexpected(Fail("sdk.memory_learning.accept_receipt_invalid"));
            }
            const auto parsed_request = mem::store::ParseUpsertJob(intent.at("parameters"), true);
            if (!parsed_request) return std::unexpected(Fail("sdk.memory_learning.accept_receipt_invalid"));
            const mem::ProjectCommitContext gate{
                identity_.project_root, memory_dir_, lifecycle_root,
                snapshot_.workspace_key, report.commit_key, report.session_id, report.source_event_ref};
            if (mem::InspectProjectCommitReceipt(gate, *parsed_request).state != mem::ProjectCommitState::NotStarted) {
                return std::unexpected(Fail("sdk.memory_learning.accept_receipt_unconfirmed"));
            }
        }
    } else if (report.error_code.empty()) {
        return std::unexpected(Fail("sdk.memory_learning.accept_receipt_invalid"));
    }

    const auto snapshot = std::find_if(report.stages.begin(), report.stages.end(), [](const SaveStageReceipt& s) {
        return s.stage == "snapshot" && s.outcome != "not_committed";
    });
    if (snapshot != report.stages.end() && !report.content_sha256.empty()) {
        const auto bytes = ReadOwned(lifecycle_root / report.commit_key / "topic.snapshot.md", lifecycle_root, kReportCap);
        if (!bytes || !*bytes || lubancode::platform::Sha256Hex(**bytes) != report.content_sha256) {
            return std::unexpected(Fail("sdk.memory_learning.accept_snapshot_mismatch"));
        }
    }
    return {};
}

Result<void> SessionMemoryLearning::CheckSavedAcceptReports() {
    const fs::path dir = session_dir_ / kAcceptsDir;
    if (!lubancode::platform::IsUnlinkedOwnedPath(session_dir_, dir)) {
        return std::unexpected(Fail("sdk.memory_learning.path_escape"));
    }
    const auto io_dir = lubancode::platform::FileIoPath(dir);
    std::error_code ec;
    const auto dir_status = fs::symlink_status(io_dir, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && dir_status.type() == fs::file_type::not_found)) {
        return {};
    }
    if (ec || fs::is_symlink(dir_status) || !fs::is_directory(dir_status) ||
        !lubancode::platform::RejectReparsePoint(io_dir)) {
        return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
    }

    std::vector<CandidateAcceptReport> loaded;
    std::size_t entries = 0;
    std::size_t total_bytes = 0;
    for (fs::directory_iterator it(io_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (++entries > kDirEntryCap) return std::unexpected(Fail("sdk.memory_learning.accept_report_limit"));
        const fs::path entry_path = dir / it->path().filename();
        if (entry_path.extension() == ".tmp") continue;
        std::error_code item_ec;
        const auto io_entry = lubancode::platform::FileIoPath(entry_path);
        const auto item_status = fs::symlink_status(io_entry, item_ec);
        if (item_ec || fs::is_symlink(item_status) || !fs::is_regular_file(item_status) ||
            !lubancode::platform::RejectReparsePoint(io_entry)) {
            return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
        }
        const auto name = lubancode::platform::PathToUtf8(entry_path.filename());
        if (name.size() <= 5 || name.substr(name.size() - 5) != ".json") {
            return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
        }
        const auto commit_key = name.substr(0, name.size() - 5);
        if (!SafeId(commit_key)) return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));
        auto bytes = ReadOwned(entry_path, session_dir_, kReportCap);
        if (!bytes) return std::unexpected(bytes.error());
        if (!*bytes) return std::unexpected(Fail("sdk.memory_learning.accept_report_unavailable"));
        total_bytes += (*bytes)->size();
        if (total_bytes > kTotalCap) return std::unexpected(Fail("sdk.memory_learning.accept_report_limit"));
        auto decoded = DecodeAcceptReport(commit_key, **bytes);
        if (!decoded) return std::unexpected(decoded.error());
        if (auto valid = ValidateAcceptReport(*decoded); !valid) return std::unexpected(valid.error());
        loaded.push_back(std::move(*decoded));
    }
    if (ec) return std::unexpected(Fail("sdk.memory_learning.accept_report_invalid"));

    std::sort(loaded.begin(), loaded.end(), [](const CandidateAcceptReport& a, const CandidateAcceptReport& b) {
        if (a.committed_at != b.committed_at) return a.committed_at < b.committed_at;
        return a.commit_key < b.commit_key;
    });

    std::lock_guard lock(accept_mutex_);
    accept_reports_.clear();
    accept_order_.clear();
    accept_indeterminate_ = false;
    for (auto& item : loaded) {
        if (item.state == "indeterminate") accept_indeterminate_ = true;
        accept_order_.push_back(item.commit_key);
        accept_reports_.insert_or_assign(item.commit_key, std::move(item));
    }
    return {};
}

Result<std::vector<LearningCandidate>> SessionMemoryLearning::ListCandidates() const {
    if (legacy_ || snapshot_.mode == LearningMode::Off) {
        return std::unexpected(Fail("sdk.memory_learning.disabled"));
    }
    if (!candidate_store_) return std::unexpected(Fail("sdk.memory_learning.candidate_store_unavailable"));
    auto listed = candidate_store_->List(memory_dir_);
    if (!listed) return std::unexpected(Fail("sdk.memory_learning.candidate_store_failed", listed.error()));
    std::vector<LearningCandidate> out;
    out.reserve(listed->size());
    for (const auto& candidate : *listed) {
        out.push_back(CandidateToPublic(candidate));
    }
    return out;
}

Result<LearningCandidate> SessionMemoryLearning::GetCandidate(const std::string& candidate_id) const {
    if (legacy_ || snapshot_.mode == LearningMode::Off) {
        return std::unexpected(Fail("sdk.memory_learning.disabled"));
    }
    if (!SafeCandidateId(candidate_id)) {
        return std::unexpected(Fail("sdk.memory_learning.invalid_input", "invalid candidate_id"));
    }
    if (!candidate_store_) return std::unexpected(Fail("sdk.memory_learning.candidate_store_unavailable"));
    auto found = candidate_store_->Get(memory_dir_, candidate_id);
    if (!found) return std::unexpected(Fail("sdk.memory_learning.candidate_store_failed", found.error()));
    if (!*found) return std::unexpected(Fail("sdk.memory_learning.candidate_not_found", candidate_id));
    return CandidateToPublic(**found);
}

Result<LearningCandidate> SessionMemoryLearning::EditCandidate(
    const std::string& candidate_id,
    const std::string& title,
    const std::string& content) {
    if (legacy_ || snapshot_.mode == LearningMode::Off) {
        return std::unexpected(Fail("sdk.memory_learning.disabled"));
    }
    if (!SafeCandidateId(candidate_id) || !SafeText(title) || !SafeText(content) ||
        (title.empty() && content.empty())) {
        return std::unexpected(Fail("sdk.memory_learning.invalid_input", "invalid candidate edit arguments"));
    }
    std::lock_guard lock(accept_mutex_);
    if (!candidate_store_) return std::unexpected(Fail("sdk.memory_learning.candidate_store_unavailable"));
    auto edited = candidate_store_->Edit(memory_dir_, candidate_id, title, content);
    if (!edited) {
        if (edited.error() == "candidate_not_found") {
            return std::unexpected(Fail("sdk.memory_learning.candidate_not_found", candidate_id));
        }
        return std::unexpected(Fail("sdk.memory_learning.invalid_input", edited.error()));
    }
    return CandidateToPublic(*edited);
}

Result<void> SessionMemoryLearning::RejectCandidate(
    const std::string& candidate_id,
    const std::string& reason) {
    if (legacy_ || snapshot_.mode == LearningMode::Off) {
        return std::unexpected(Fail("sdk.memory_learning.disabled"));
    }
    if (!SafeCandidateId(candidate_id) || !SafeText(reason)) {
        return std::unexpected(Fail("sdk.memory_learning.invalid_input", "invalid candidate reject arguments"));
    }
    std::lock_guard lock(accept_mutex_);
    if (!candidate_store_) return std::unexpected(Fail("sdk.memory_learning.candidate_store_unavailable"));
    auto rejected = candidate_store_->Reject(memory_dir_, candidate_id, reason);
    if (!rejected) {
        if (rejected.error() == "candidate_not_found") {
            return std::unexpected(Fail("sdk.memory_learning.candidate_not_found", candidate_id));
        }
        return std::unexpected(Fail("sdk.memory_learning.candidate_store_failed", rejected.error()));
    }
    return {};
}

Result<CandidateAcceptReport> SessionMemoryLearning::AcceptCandidate(
    const std::string& candidate_id,
    bool write_enabled,
    const std::string& write_plan_sha256,
    bool write_indeterminate) {
    if (legacy_ || snapshot_.mode == LearningMode::Off) {
        return std::unexpected(Fail("sdk.memory_learning.disabled"));
    }
    if (!write_enabled || write_plan_sha256.empty()) {
        return std::unexpected(Fail("sdk.memory_learning.write_disabled", "memory_write must be enabled to accept"));
    }
    if (!SafeCandidateId(candidate_id)) {
        return std::unexpected(Fail("sdk.memory_learning.invalid_input", "invalid candidate_id"));
    }

    std::lock_guard lock(accept_mutex_);
    if (accept_indeterminate_ || write_indeterminate) {
        return std::unexpected(Fail("sdk.memory_learning.stopped",
            "earlier project write or candidate accept entered indeterminate state"));
    }
    for (const auto& [existing_key, existing] : accept_reports_) {
        if (existing.candidate_id != candidate_id) continue;
        if (existing.state == "indeterminate") {
            return std::unexpected(Fail("sdk.memory_learning.stopped",
                "earlier accept for candidate entered indeterminate state: " + candidate_id));
        }
        if (existing.state == "committed") {
            return std::unexpected(Fail("sdk.memory_learning.already_committed", candidate_id));
        }
    }
    if (!candidate_store_) return std::unexpected(Fail("sdk.memory_learning.candidate_store_unavailable"));
    auto found = candidate_store_->Get(memory_dir_, candidate_id);
    if (!found) return std::unexpected(Fail("sdk.memory_learning.candidate_store_failed", found.error()));
    if (!*found) return std::unexpected(Fail("sdk.memory_learning.candidate_not_found", candidate_id));
    const auto& candidate = **found;

    if (candidate.confidence == "inferred") {
        return std::unexpected(Fail("sdk.memory_learning.candidate_inferred",
            "inferred candidate requires stronger evidence before accept"));
    }
    if (candidate.kind == mem::MemoryKind::Feedback && candidate.confidence != "user-stated") {
        return std::unexpected(Fail("sdk.memory_learning.candidate_invalid",
            "feedback candidate must have user-stated confidence"));
    }
    if (candidate.kind == mem::MemoryKind::Fact && candidate.paths.empty()) {
        return std::unexpected(Fail("sdk.memory_learning.candidate_missing_evidence",
            "fact candidate requires file evidence"));
    }

    mem::SaveRequest req;
    req.kind = candidate.kind;
    req.title = candidate.title;
    req.summary = candidate.summary;
    req.content = candidate.content;
    req.keywords = candidate.keywords;
    req.paths = candidate.paths;
    req.confidence = candidate.confidence;
    req.source_session = snapshot_.session_id;
    req.occurred_at = candidate.occurred_at;
    for (const auto& path : candidate.paths) {
        req.evidence.push_back(mem::MemoryEvidence{path, {}});
    }
    if (const auto valid_req = mem::store::ValidateSaveRequest(req); !valid_req) {
        return std::unexpected(Fail("sdk.memory_learning.candidate_invalid", valid_req.error()));
    }

    CandidateAcceptReport report;
    report.session_id = snapshot_.session_id;
    report.candidate_id = candidate_id;
    report.workspace_key = snapshot_.workspace_key;
    report.learning_plan_sha256 = snapshot_.plan_sha256;
    report.write_plan_sha256 = write_plan_sha256;
    report.source_event_ref = CandidateSourceEventRef(snapshot_.workspace_key, snapshot_.session_id, candidate_id);
    report.save_request_sha256 = lubancode::platform::Sha256Hex(SaveRequestJson(req).dump());
    report.commit_key = CandidateCommitKey(snapshot_.session_id, candidate_id, report.save_request_sha256);

    if (const auto it = accept_reports_.find(report.commit_key); it != accept_reports_.end()) {
        if (it->second.state == "committed") {
            return std::unexpected(Fail("sdk.memory_learning.already_committed", candidate_id));
        }
        if (it->second.state == "indeterminate") {
            return std::unexpected(Fail("sdk.memory_learning.stopped", candidate_id));
        }
    }

    const mem::ProjectCommitContext gate{
        identity_.project_root,
        memory_dir_,
        session_dir_.parent_path().parent_path() / "lifecycle",
        snapshot_.workspace_key,
        report.commit_key,
        snapshot_.session_id,
        report.source_event_ref,
    };
    report.request_sha256 = mem::ProjectCommitRequestSha256(gate, req);

#ifdef LUBANCORE_PRIVATE_OPENING_TEST_HOOKS
    const auto write_hook = testing::SnapshotLearningCommitWriteHook();
    const auto saved = write_hook
        ? mem::commit_testing::CommitProjectUpsert(
              gate,
              req,
              [&write_hook](
                  const fs::path& p,
                  std::string_view content,
                  lubancode::platform::WriteDurability durability) {
                  return (*write_hook)(p, content, durability);
              })
        : mem::CommitProjectUpsertWithCancellation(gate, req, [this] { return closing_.load(); });
#else
    const auto saved = mem::CommitProjectUpsertWithCancellation(gate, req, [this] { return closing_.load(); });
#endif

    report.state = CommitStateName(saved.state);
    report.memory_id = saved.memory_id;
    report.memory_path = saved.memory_path;
    report.content_sha256 = saved.content_sha256;
    report.committed_at = saved.committed_at;
    report.duplicate = saved.duplicate;
    report.error_code = saved.error_code;
    report.error = saved.error;
    for (const auto& stage : saved.stages) {
        report.stages.push_back({CommitStageName(stage.stage), CommitOutcomeName(stage.outcome)});
    }
    if (!saved.request_sha256.empty() && saved.request_sha256 != report.request_sha256) {
        report.state = "indeterminate";
        report.error_code = "sdk.memory_learning.request_mismatch";
        report.error = report.error_code;
    }

    const auto record_in_memory = [&] {
        if (!accept_reports_.contains(report.commit_key)) {
            accept_order_.push_back(report.commit_key);
        }
        accept_reports_.insert_or_assign(report.commit_key, report);
    };

    if (report.state == "committed") {
        auto persisted = PersistAcceptReport(report);
        if (!persisted) {
            accept_indeterminate_ = true;
            report.state = "indeterminate";
            report.error_code = persisted.error().code;
            report.error = persisted.error().message;
            record_in_memory();
            return std::unexpected(persisted.error());
        }
        auto removed = candidate_store_->Remove(memory_dir_, candidate_id);
        if (!removed) {
            accept_indeterminate_ = true;
            report.state = "indeterminate";
            report.error_code = "sdk.memory_learning.candidate_cleanup_failed";
            report.error = removed.error();
            record_in_memory();
            return std::unexpected(Fail(report.error_code, report.error));
        }
        record_in_memory();
        return report;
    }

    if (report.state == "indeterminate") {
        accept_indeterminate_ = true;
    }
    (void)PersistAcceptReport(report);
    record_in_memory();
    return report;
}

Result<std::vector<CandidateAcceptReport>> SessionMemoryLearning::GetAcceptedCandidates() const {
    std::lock_guard lock(accept_mutex_);
    std::vector<CandidateAcceptReport> out;
    out.reserve(accept_order_.size());
    for (const auto& commit_key : accept_order_) {
        if (const auto it = accept_reports_.find(commit_key); it != accept_reports_.end()) {
            out.push_back(it->second);
        }
    }
    return out;
}

bool SessionMemoryLearning::HasIndeterminateAccept() const {
    std::lock_guard lock(accept_mutex_);
    return accept_indeterminate_;
}

} // namespace lubancore::detail
