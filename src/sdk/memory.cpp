#include "sdk/memory.hpp"
#include "trajectory/session_recovery_view.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>

#include "memory/recall_engine.hpp"
#include "memory/internal.hpp"
#include "memory/topic_store.hpp"
#include "platform/atomic_write.hpp"
#include "platform/bounded_read.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/session_service.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/cas_store.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/index.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
constexpr auto kPlanFile = "sdk-memory-plan.json";
constexpr std::size_t kPlanBytes = 4096, kReportBytes = 512 * 1024;
Error Fail(std::string code, std::string message = {}) { return {std::move(code), std::move(message)}; }
bool Inside(const fs::path& path, const fs::path& root) {
    auto p = path.begin(), r = root.begin();
    for (; r != root.end(); ++r, ++p) if (p == path.end() || *p != *r) return false;
    return true;
}
bool SafeText(const std::string& value) {
    return value.find('\0') == std::string::npos && lubancode::platform::IsValidUtf8(value);
}
bool SafeId(const std::string& value) {
    return !value.empty() && value.size() <= 200 &&
           value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
Result<void> ReportDirectory(const fs::path& session_dir, bool create) {
    const auto path = session_dir / "sdk-memory-recalls";
    std::error_code ec;
    auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found)) {
        if (!create) return {};
        ec.clear();
        fs::create_directory(path, ec);
        if (ec) return std::unexpected(Fail("sdk.memory.report_write_failed"));
        status = fs::symlink_status(path, ec);
    }
    if (ec || fs::is_symlink(status) || !fs::is_directory(status))
        return std::unexpected(Fail("sdk.memory.invalid_file"));
    const auto real = fs::canonical(path, ec);
    if (ec) return std::unexpected(Fail("sdk.memory.invalid_file"));
    const auto root = fs::canonical(session_dir, ec);
    if (ec || real != root / "sdk-memory-recalls") return std::unexpected(Fail("sdk.memory.path_escape"));
    return {};
}
Result<std::optional<std::string>> ReadOwned(const fs::path& path, const fs::path& owned, std::size_t cap) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        return std::optional<std::string>{};
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        return std::unexpected(Fail("sdk.memory.invalid_file"));
    const auto real = fs::canonical(path, ec);
    if (ec) return std::unexpected(Fail("sdk.memory.invalid_file"));
    const auto root = fs::canonical(owned, ec);
    if (ec || !Inside(real, root)) return std::unexpected(Fail("sdk.memory.path_escape"));
    auto bytes = lubancode::platform::ReadBoundedRegularFile(real, cap);
    if (!bytes) return std::unexpected(Fail("sdk.memory." + bytes.error()));
    if (!SafeText(*bytes))
        return std::unexpected(Fail("sdk.memory.invalid_text"));
    return std::optional<std::string>{std::move(*bytes)};
}
const Json* Binding(const v3::MessageLine& message) {
    if (!message.system_meta || !message.system_meta->is_object()) return nullptr;
    const auto hosts = message.system_meta->find("hostBindings");
    if (hosts == message.system_meta->end() || !hosts->is_object()) return nullptr;
    const auto binding = hosts->find("memory");
    return binding == hosts->end() ? nullptr : &*binding;
}
Json ReportJson(const memory::v1::RecallReport& report) {
    Json entries = Json::array();
    for (const auto& entry : report.entries)
        entries.push_back({{"id", entry.id}, {"score", entry.score}, {"selected", entry.selected},
            {"stale", entry.stale}, {"expired", entry.expired}, {"scopeBlocked", entry.scope_blocked},
            {"budgetDropped", entry.budget_dropped}, {"belowThreshold", entry.below_threshold},
            {"weak", entry.weak}, {"reason", entry.reason}, {"bytes", entry.bytes}});
    return {{"schemaVersion", 1}, {"enabled", report.enabled}, {"sessionId", report.session_id},
        {"operationId", report.operation_id}, {"turnId", report.turn_id}, {"workspaceKey", report.workspace_key},
        {"planSha256", report.plan_sha256}, {"state", report.state}, {"error", report.error},
        {"contextMessageId", report.context_message_id}, {"contextSha256", report.context_sha256},
        {"bytes", report.bytes}, {"entries", std::move(entries)}};
}
class Collector final : public lubancode::memory::MemoryAccounting {
public:
    std::vector<lubancode::memory::InjectedMemoryRecord> records;
    std::expected<void, std::string> RecordRecallInjection(const lubancode::memory::InjectedMemoryRecord& record) override {
        records.push_back(record);
        return {};
    }
    std::string RecordSaveRequested(const lubancode::memory::SaveLedgerNote&) override { return {}; }
    std::string current_session_id() const override { return {}; }
};
} // namespace

Result<std::shared_ptr<SessionMemory>> SessionMemory::Prepare(
    const std::optional<memory::v1::RecallOptions>& selection, fs::path owned_root,
    lubancode::workspace::WorkspaceIdentity identity, std::string resume_id, fs::path cwd) {
    auto result = std::shared_ptr<SessionMemory>(new SessionMemory);
    result->owned_root_ = std::move(owned_root);
    result->identity_ = std::move(identity);
    result->cwd_ = std::move(cwd);
    result->resume_id_ = std::move(resume_id);
    result->snapshot_.workspace_key = result->identity_.workspace_key;
    result->snapshot_.enabled = selection.has_value();
    if (selection) result->snapshot_.options = *selection;
    if (!result->resume_id_.empty()) {
        auto directory = lubancode::workspace::index::ResolveDirByWorkspaceKey(
            result->owned_root_ / "workspaces", result->identity_.workspace_key);
        if (!directory) return std::unexpected(Fail("sdk.session.open_failed", "resume session is unavailable"));
        result->expected_resume_dir_ = *directory / "sessions" / result->resume_id_;
        auto bytes = ReadOwned(result->expected_resume_dir_ / kPlanFile, result->owned_root_, kPlanBytes);
        if (!bytes) return std::unexpected(bytes.error());
        if (*bytes) {
            auto plan = Json::parse(**bytes, nullptr, false);
            if (!plan.is_object() || plan.value("schemaVersion", Json()) != 1 ||
                plan.value("sessionId", Json()) != result->resume_id_ ||
                plan.value("workspaceKey", Json()) != result->identity_.workspace_key ||
                plan.value("enabled", Json()) != true || !plan.contains("maxBytes") || !plan["maxBytes"].is_number_unsigned() ||
                !plan.contains("maxResults") || !plan["maxResults"].is_number_unsigned() || plan.dump() != **bytes)
                return std::unexpected(Fail("sdk.memory.plan_invalid"));
            memory::v1::RecallOptions saved{plan["maxBytes"].get<std::size_t>(), plan["maxResults"].get<std::size_t>()};
            if (selection && *selection != saved) return std::unexpected(Fail("sdk.memory.resume_mismatch"));
            result->snapshot_.enabled = true;
            result->snapshot_.options = saved;
            result->saved_plan_bytes_ = **bytes;
            result->snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(**bytes);
            if (plan != result->Plan(result->resume_id_)) return std::unexpected(Fail("sdk.memory.plan_invalid"));
        } else {
            if (selection) return std::unexpected(Fail("sdk.memory.resume_mismatch", "legacy session has no Memory plan"));
            result->legacy_ = true;
        }
    } else if (!selection) result->legacy_ = true;
    const auto& options = result->snapshot_.options;
    if (options.max_bytes == 0 || options.max_bytes > 65536 || options.max_results == 0 || options.max_results > 32)
        return std::unexpected(Fail("sdk.memory.invalid_options"));
    return result;
}
Json SessionMemory::Plan(const std::string& session_id) const {
    return {{"schemaVersion", 1}, {"enabled", true}, {"sessionId", session_id},
        {"workspaceKey", identity_.workspace_key}, {"maxBytes", snapshot_.options.max_bytes},
        {"maxResults", snapshot_.options.max_results},
        {"readLimits", {{"catalogBytes", 4194304}, {"topicBytes", 16384}, {"topicTotalBytes", 25165824}, {"contextBytes", 131072},
            {"evidenceBytes", 16777216}, {"evidenceTotalBytes", 67108864}, {"entries", 1024}, {"directoryEntries", 4096},
            {"reportBytes", 524288}, {"reportEntries", 4096}, {"reportTotalBytes", 67108864}}}};
}
Result<void> SessionMemory::CheckBinding(const v3::V3Ledger& source) const {
    if (legacy_) {
        for (const auto& message : source.messages)
            if (Binding(message)) return std::unexpected(Fail("sdk.memory.plan_invalid", "journal binds a missing plan"));
        return {};
    }
    const Json expected{{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}};
    const auto matches = [&](const v3::MessageLine* message) {
        const auto* binding = message ? Binding(*message) : nullptr;
        return binding && *binding == expected;
    };
    if (source.messages.empty() || !matches(&source.messages.front()))
        return std::unexpected(Fail("sdk.memory.plan_invalid", "initial Memory binding differs"));
    for (const auto& [revision, chain] : source.revision_chains) {
        (void)revision;
        if (!matches(source.FindMessage(chain.first)))
            return std::unexpected(Fail("sdk.memory.plan_invalid", "adopted Memory binding differs"));
    }
    if (!matches(source.FindMessage(source.context.system_message_ref)))
        return std::unexpected(Fail("sdk.memory.plan_invalid", "effective Memory binding differs"));
    return {};
}
std::expected<Json, std::string> SessionMemory::Open(const lubancode::trajectory::V3OpeningContext& context) {
    const auto fail = [](const Error& error) -> std::expected<Json, std::string> {
        return std::unexpected(error.code + ": " + error.message);
    };
    std::error_code ec;
    const auto directory = fs::canonical(context.session_dir, ec);
    if (ec) return std::unexpected("sdk.memory.open_failed: invalid session directory");
    const auto root = fs::canonical(owned_root_, ec);
    if (ec || !Inside(directory, root / "workspaces")) return std::unexpected("sdk.memory.path_escape");
    if (!context.memory_capability || context.memory_capability->scope() !=
        lubancode::trajectory::CasScope{identity_.workspace_key, context.session_id})
        return std::unexpected("sdk.memory.scope_mismatch");
    memory_capability_ = context.memory_capability;
    session_dir_ = context.session_dir;
    snapshot_.session_id = context.session_id;
    snapshot_.memory_directory = lubancode::tools::PathToUtf8(directory.parent_path().parent_path() / "memory");
    if (context.source) {
        if (context.session_id != resume_id_ || context.session_dir != expected_resume_dir_)
            return std::unexpected("sdk.memory.resume_mismatch");
        // Legacy/off sources adopt only the locked V3 binding. No path read
        // reuses the lock-preflight plan as adopted Memory facts.
        if (snapshot_.enabled) {
            if (!context.recovery_view || context.recovery_view->session_id != context.session_id ||
                context.recovery_view->workspace_key != identity_.workspace_key)
                return std::unexpected("sdk.memory.recovery_view_missing");
            const auto* plan = context.recovery_view->Find(lubancode::trajectory::RecoveryKeyKind::MemoryPlan);
            if (!plan || plan->state != lubancode::trajectory::RecoveryReadState::Value || plan->bytes != saved_plan_bytes_)
                return std::unexpected("sdk.memory.plan_invalid: plan changed before locked adoption");
        }
        auto binding = CheckBinding(*context.source);
        if (!binding) return fail(binding.error());
        if (snapshot_.enabled) {
            auto reports = CheckSavedReports(*context.source, *context.recovery_view);
            if (!reports) return fail(reports.error());
        }
    } else if (!resume_id_.empty()) return std::unexpected("sdk.memory.resume_mismatch");
    else if (snapshot_.enabled) {
        auto prior = ReadOwned(context.session_dir / kPlanFile, owned_root_, kPlanBytes);
        if (!prior) return fail(prior.error());
        if (*prior) return std::unexpected("sdk.memory.plan_invalid: new plan already exists");
        const auto bytes = Plan(context.session_id).dump();
        auto written = lubancode::platform::AtomicWriteFile(context.session_dir / kPlanFile, bytes,
            lubancode::platform::WriteDurability::ProcessCrashDurability);
        if (!written) return std::unexpected("sdk.memory.plan_write_failed: " + written.error().message);
        snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(bytes);
    }
    return snapshot_.enabled ? Json{{"hostBindings", {{"memory", {{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}}}}}}
                             : Json::object();
}
lubancode::trajectory::V3OpeningParticipant SessionMemory::OpeningParticipant() {
    return [owner = shared_from_this()](const auto& context) { return owner->Open(context); };
}
memory::v1::RecallReport SessionMemory::EmptyReport(const std::string& operation_id, const std::string& turn_id) const {
    memory::v1::RecallReport report;
    report.enabled = snapshot_.enabled;
    report.session_id = snapshot_.session_id;
    report.operation_id = operation_id;
    report.turn_id = turn_id;
    report.workspace_key = snapshot_.workspace_key;
    report.plan_sha256 = snapshot_.plan_sha256;
    report.state = snapshot_.enabled ? "no_match" : "disabled";
    return report;
}
Result<SessionMemory::Recall> SessionMemory::BuildRecall(const std::string& query,
    const std::string& operation_id, const std::string& turn_id) const {
    Recall result;
    result.report = EmptyReport(operation_id, turn_id);
    if (!snapshot_.enabled) return result;
    if (!SafeText(query)) return std::unexpected(Fail("sdk.memory.invalid_query"));
    const auto memory_dir = lubancode::tools::Utf8ToPath(snapshot_.memory_directory);
    std::error_code ec;
    const auto status = fs::symlink_status(memory_dir, ec);
    if (ec != std::errc::no_such_file_or_directory && status.type() != fs::file_type::not_found) {
        const auto real = fs::canonical(memory_dir, ec);
        if (ec) return std::unexpected(Fail("sdk.memory.path_escape"));
        const auto workspace_root = fs::canonical(session_dir_.parent_path().parent_path(), ec);
        if (ec || !Inside(real, workspace_root))
            return std::unexpected(Fail("sdk.memory.path_escape"));
    }
    auto read_view = lubancode::memory::store::ReadProjectRecallSnapshot(memory_dir, identity_.project_root);
    if (!read_view) return std::unexpected(Fail("sdk.memory.read_failed", read_view.error()));
    lubancode::memory::ProjectIdentity identity;
    identity.project_root = identity_.project_root;
    identity.identity_root = identity_.identity_root;
    identity.workspace_key = identity_.workspace_key;
    identity.workspace_dir = session_dir_.parent_path().parent_path();
    identity.display_name = identity_.display_name;
    identity.git = identity_.git();
    lubancode::memory::Options options;
    options.global_allowed = options.enabled = true;
    options.user_enabled = false;
    options.learn = options.learn_ceiling = lubancode::memory::LearnMode::Off;
    options.max_retrieval_bytes = snapshot_.options.max_bytes;
    options.max_results = snapshot_.options.max_results;
    Collector collector;
    lubancode::memory::RecallTrace trace;
    result.context = lubancode::memory::recall::BuildTurnContext(options, identity, memory_dir, {}, {},
        &collector, query, cwd_, lubancode::memory::QueryOrigin::User, false, {}, turn_id, &*read_view, &trace);
    result.records = std::move(collector.records);
    for (const auto& entry : trace.entries)
        result.report.entries.push_back({entry.id, entry.score, entry.injected, entry.stale_blocked,
            entry.expired, entry.scope_blocked, entry.budget_dropped, entry.below_threshold,
            entry.weak, entry.drop_reason, entry.bytes});
    result.report.bytes = result.context.size();
    if (!SafeText(result.context)) return std::unexpected(Fail("sdk.memory.invalid_text"));
    if (result.context.size() > 131072 || ReportJson(result.report).dump().size() + 256 > kReportBytes)
        return std::unexpected(Fail("sdk.memory.limit_exceeded"));
    if (!result.context.empty()) result.report.context_sha256 = lubancode::platform::Sha256Hex(result.context);
    return result;
}
Result<void> SessionMemory::PersistReport(const memory::v1::RecallReport& report) const {
    if (!snapshot_.enabled) return {};
    if (!SafeId(report.operation_id)) return std::unexpected(Fail("sdk.memory.report_invalid"));
    auto directory = ReportDirectory(session_dir_, true);
    if (!directory) return directory;
    const auto payload = ReportJson(report);
    Json envelope{{"payload", payload}, {"sha256", lubancode::platform::Sha256Hex(payload.dump())}};
    const auto bytes = envelope.dump();
    if (bytes.size() > kReportBytes) return std::unexpected(Fail("sdk.memory.report_limit"));
    auto written = lubancode::platform::AtomicWriteFile(session_dir_ / "sdk-memory-recalls" / (report.operation_id + ".json"),
        bytes, lubancode::platform::WriteDurability::ProcessCrashDurability);
    if (!written) return std::unexpected(Fail("sdk.memory.report_write_failed", written.error().message));
    return {};
}
Result<memory::v1::RecallReport> SessionMemory::ReadReport(const std::string& operation_id,
    std::size_t* actual_bytes, std::size_t byte_limit) const {
    if (!SafeId(operation_id)) return std::unexpected(Fail("sdk.memory.report_invalid"));
    auto directory = ReportDirectory(session_dir_, false);
    if (!directory) return std::unexpected(directory.error());
    auto bytes = ReadOwned(session_dir_ / "sdk-memory-recalls" / (operation_id + ".json"), owned_root_, std::min(kReportBytes, byte_limit));
    if (!bytes) return std::unexpected(bytes.error());
    if (!*bytes) return std::unexpected(Fail("sdk.memory.report_unavailable"));
    if (actual_bytes) *actual_bytes = (**bytes).size();
    return ParseReportBytes(operation_id, **bytes);
}
Result<memory::v1::RecallReport> SessionMemory::ParseReportBytes(
    const std::string& operation_id, const std::string& bytes) const {
    try {
        const auto envelope = Json::parse(bytes);
        const auto& payload = envelope.at("payload");
        if (!payload.is_object() || envelope.at("sha256") != lubancode::platform::Sha256Hex(payload.dump()) ||
            payload.at("schemaVersion") != 1 || payload.at("sessionId") != snapshot_.session_id ||
            payload.at("operationId") != operation_id || payload.at("workspaceKey") != snapshot_.workspace_key ||
            payload.at("planSha256") != snapshot_.plan_sha256 || !payload.at("entries").is_array() ||
            payload.at("entries").size() > 1024)
            return std::unexpected(Fail("sdk.memory.report_invalid"));
        auto report = EmptyReport(operation_id, payload.at("turnId").get<std::string>());
        report.state = payload.at("state").get<std::string>();
        if (report.state != "not_attempted" && report.state != "no_match" && report.state != "admitted" && report.state != "failed")
            return std::unexpected(Fail("sdk.memory.report_invalid"));
        report.error = payload.at("error").get<std::string>();
        report.context_message_id = payload.at("contextMessageId").get<std::string>();
        report.context_sha256 = payload.at("contextSha256").get<std::string>();
        report.bytes = payload.at("bytes").get<std::size_t>();
        for (const auto& entry : payload.at("entries"))
            report.entries.push_back({entry.at("id").get<std::string>(), entry.at("score").get<int>(),
                entry.at("selected").get<bool>(), entry.at("stale").get<bool>(), entry.at("expired").get<bool>(),
                entry.at("scopeBlocked").get<bool>(), entry.at("budgetDropped").get<bool>(), entry.at("belowThreshold").get<bool>(),
                entry.at("weak").get<bool>(), entry.at("reason").get<std::string>(), entry.at("bytes").get<std::size_t>()});
        if (ReportJson(report) != payload || envelope.size() != 2 || !SafeText(report.error) ||
            !SafeText(report.turn_id) || !SafeText(report.context_message_id) || !SafeText(report.context_sha256))
            return std::unexpected(Fail("sdk.memory.report_invalid"));
        return report;
    } catch (const Json::exception&) { return std::unexpected(Fail("sdk.memory.report_invalid")); }
}

Result<void> SessionMemory::ValidateReport(const memory::v1::RecallReport& report, const v3::V3Ledger& source) const {
    const auto bad = [] { return std::unexpected(Fail("sdk.memory.report_invalid", "report differs from verified input")); };
    if (!snapshot_.enabled) return report.enabled || report.state != "disabled" ? Result<void>(bad()) : Result<void>{};
    if (!report.enabled || report.session_id != snapshot_.session_id || source.session_id != snapshot_.session_id ||
        report.workspace_key != snapshot_.workspace_key || report.plan_sha256 != snapshot_.plan_sha256 ||
        !SafeId(report.operation_id) || report.bytes > 131072 || report.entries.size() > 1024) return bad();
    std::map<std::string, std::size_t> selected;
    std::size_t selected_bytes = 0;
    for (const auto& entry : report.entries) {
        if (!lubancode::memory::IsValidId(entry.id) || !SafeText(entry.id) || !SafeText(entry.reason)) return bad();
        if (entry.selected) {
            if (!entry.bytes || !selected.emplace(entry.id, entry.bytes).second ||
                entry.bytes > snapshot_.options.max_bytes - selected_bytes || entry.stale || entry.expired ||
                entry.scope_blocked || entry.budget_dropped || entry.below_threshold) return bad();
            selected_bytes += entry.bytes;
        }
    }
    if (selected.size() > snapshot_.options.max_results) return bad();
    bool has_recall_fact = false, has_context_input = false;
    for (const auto& event : source.events)
        if (event.kind == v3::EventKindV3::MemoryRecallInjected && event.turn_id == report.turn_id)
            has_recall_fact = true;
    for (const auto& message : source.messages) {
        if (message.turn_id != report.turn_id || message.origin != v3::MessageOrigin::ContextRuntime ||
            message.display != v3::DisplayMode::Hidden || message.message.value("role", Json()) != "user") continue;
        for (const auto& event : source.events) {
            if (event.kind != v3::EventKindV3::ContextInputApplied) continue;
            const auto refs = event.payload.find("addedMessageRefs");
            if (refs != event.payload.end() && refs->is_array() &&
                std::find(refs->begin(), refs->end(), Json(message.message_id)) != refs->end()) has_context_input = true;
        }
    }
    if (report.state == "not_attempted" || report.state == "no_match") {
        if (has_recall_fact || has_context_input || !report.context_message_id.empty() || !report.context_sha256.empty() || report.bytes ||
            std::any_of(report.entries.begin(), report.entries.end(), [](const auto& entry) { return entry.selected; })) return bad();
        return {};
    }
    if (report.state != "failed" && report.state != "admitted") return bad();
    if (report.context_message_id.empty())
        return report.state == "failed" && !report.error.empty() && !has_recall_fact && !has_context_input ? Result<void>{} : Result<void>(bad());
    const auto* message = source.FindMessage(report.context_message_id);
    if (!message || message->session_id != report.session_id || message->turn_id != report.turn_id ||
        message->origin != v3::MessageOrigin::ContextRuntime || message->display != v3::DisplayMode::Hidden ||
        message->message.value("role", Json()) != "user" || !message->message.contains("content") ||
        !message->message["content"].is_string()) return bad();
    const auto& text = message->message["content"].get_ref<const std::string&>();
    if (text.empty() || text.size() != report.bytes || lubancode::platform::Sha256Hex(text) != report.context_sha256) return bad();
    std::size_t adopted = 0;
    for (const auto& event : source.events) {
        if (event.kind != v3::EventKindV3::ContextInputApplied) continue;
        const auto refs = event.payload.find("addedMessageRefs");
        if (refs != event.payload.end() && refs->is_array())
            adopted += std::count(refs->begin(), refs->end(), Json(report.context_message_id));
    }
    if (adopted != 1) return bad();
    std::set<std::string> verified;
    for (const auto& event : source.events) {
        if (event.kind != v3::EventKindV3::MemoryRecallInjected ||
            event.payload.value("contextMessageRef", Json()) != report.context_message_id) continue;
        const auto& payload = event.payload;
        if (event.session_id != report.session_id || event.turn_id != report.turn_id ||
            payload.value("memoryLevel", Json()) != "project" || !payload.contains("memoryId") ||
            !payload["memoryId"].is_string() || payload.contains("snapshotInline") == payload.contains("snapshotRef") ||
            !payload.contains("contentSha256") || !payload["contentSha256"].is_string()) return bad();
        const auto id = payload["memoryId"].get<std::string>();
        if (!selected.contains(id) || !verified.insert(id).second) return bad();
        const auto hash = payload["contentSha256"].get<std::string>();
        std::string fragment;
        if (payload.contains("snapshotInline")) {
            if (!payload["snapshotInline"].is_string()) return bad();
            fragment = payload["snapshotInline"].get<std::string>();
            if (fragment.size() > 512) return bad();
        } else {
            if (!payload["snapshotRef"].is_string() || hash.size() != 64 ||
                hash.find_first_not_of("0123456789abcdef") != std::string::npos) return bad();
            const auto relative = "artifacts/sha256/" + hash.substr(0, 2) + "/" + hash;
            if (payload["snapshotRef"] != relative) return bad();
            if (!memory_capability_) return bad();
            const lubancode::trajectory::CasReference reference{
                {report.workspace_key, report.session_id}, hash,
                static_cast<std::uint64_t>(selected.at(id)), "text/plain"};
            auto bytes = memory_capability_->Read(reference, selected.at(id));
            if (!bytes || bytes->size() <= 512) return bad();
            fragment = std::move(*bytes);
        }
        if (fragment.size() != selected.at(id) ||
            payload.value("injectedBytes", Json()) != selected.at(id) ||
            payload.value("contentSha256", Json()) != lubancode::platform::Sha256Hex(fragment) ||
            text.find(fragment) == std::string::npos) return bad();
    }
    if (report.state == "admitted" && selected.size() != verified.size()) return bad();
    return {};
}

Result<void> SessionMemory::CheckSavedReports(const v3::V3Ledger& source, const lubancode::trajectory::SessionRecoveryView& view) const {
    if (!snapshot_.enabled) return {};
    std::map<std::string, std::string> final_turns, final_statuses;
    std::set<std::string> complete_finals;
    std::set<std::string> accepted, dispatched, bound_turns;
    const auto* operations = view.Find(lubancode::trajectory::RecoveryKeyKind::Operations);
    if (!operations) return std::unexpected(Fail("sdk.memory.recovery_view_missing"));
    auto facts = lubancode::runtime::SessionService::ReadOperationFactsOwned(operations->bytes);
    if (!facts) return std::unexpected(Fail("sdk.memory.report_invalid", facts.error()));
    for (const auto& fact : *facts) {
        if (!SafeId(fact.operation_id)) return std::unexpected(Fail("sdk.memory.report_invalid"));
        if (fact.kind == "operation.accepted") accepted.insert(fact.operation_id);
        if (fact.kind == "operation.dispatched") dispatched.insert(fact.operation_id);
        if (fact.kind != "operation.final") continue;
        if (!final_turns.emplace(fact.operation_id, fact.turn_id).second ||
            (!fact.turn_id.empty() && !bound_turns.insert(fact.turn_id).second))
            return std::unexpected(Fail("sdk.memory.report_invalid", "operation turn is not unique"));
        final_statuses.emplace(fact.operation_id, fact.execution_status);
        if (fact.execution_status != "interrupted") {
            const auto* result_bytes = view.Find(lubancode::trajectory::RecoveryKeyKind::SdkResult, fact.operation_id);
            if (result_bytes && result_bytes->state == lubancode::trajectory::RecoveryReadState::Value) {
                const auto result = Json::parse(result_bytes->bytes, nullptr, false);
                if (result.is_object() && result.value("operationId", Json()) == fact.operation_id &&
                    result.value("turnId", Json()) == fact.turn_id && result.value("complete", Json()) == true)
                    complete_finals.insert(fact.operation_id);
            }
        }
    }
    for (const auto& entry : view.results.entries) {
        if (!entry.temporary && !dispatched.contains(lubancode::platform::PathToUtf8(fs::u8path(entry.name).stem())))
            return std::unexpected(Fail("sdk.memory.report_invalid", "SDK result has no preceding operation dispatch"));
    }
    if (!view.reports.present) {
        if (!complete_finals.empty()) return std::unexpected(Fail("sdk.memory.report_unavailable"));
        return {};
    }
    std::set<std::string> reports, report_turns;
    std::size_t used = 0, directory_entries = 0;
    for (const auto& entry : view.reports.entries) {
        if (++directory_entries > 4096) return std::unexpected(Fail("sdk.memory.report_limit"));
        const auto file = fs::u8path(entry.name);
        if (entry.temporary) continue;
        const auto id = lubancode::platform::PathToUtf8(file.stem());
        if (reports.size() >= 4096 || file.extension() != ".json" || !SafeId(id) || !accepted.contains(id) || !reports.insert(id).second)
            return std::unexpected(Fail("sdk.memory.report_invalid"));
        const auto* bytes = view.Find(lubancode::trajectory::RecoveryKeyKind::MemoryRecall, id);
        if (!bytes || bytes->state != lubancode::trajectory::RecoveryReadState::Value)
            return std::unexpected(Fail("sdk.memory.report_unavailable"));
        if (bytes->bytes.size() > 67108864 - used) return std::unexpected(Fail("sdk.memory.report_limit"));
        used += bytes->bytes.size();
        auto report = ParseReportBytes(id, bytes->bytes);
        if (!report) {
            if (report.error().code.starts_with("sdk.memory.read.") || report.error().code == "sdk.memory.invalid_text")
                return std::unexpected(report.error());
            if (complete_finals.contains(id)) return std::unexpected(report.error());
            continue;
        }
        if (!report->turn_id.empty() && !report_turns.insert(report->turn_id).second)
            return std::unexpected(Fail("sdk.memory.report_invalid", "report turn belongs to multiple operations"));
        if (const auto final = final_turns.find(id); complete_finals.contains(id) && final != final_turns.end() && final->second != report->turn_id)
            return std::unexpected(Fail("sdk.memory.report_invalid", "operation turn differs"));
        if (complete_finals.contains(id) && final_statuses.at(id) == "success" &&
            report->state != "no_match" && report->state != "admitted")
            return std::unexpected(Fail("sdk.memory.report_invalid", "successful operation has no complete recall result"));
        auto valid = ValidateReport(*report, source);
        if (!valid && complete_finals.contains(id)) return valid;
    }
    for (const auto& [id, turn] : final_turns) {
        (void)turn;
        if (complete_finals.contains(id) && !reports.contains(id)) return std::unexpected(Fail("sdk.memory.report_unavailable"));
    }
    return {};
}

} // namespace lubancore::detail
