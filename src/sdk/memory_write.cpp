#include "sdk/memory_write.hpp"
#include "sdk/plan_write.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

#include "memory/project_commit.hpp"
#include "memory/topic_store.hpp"
#include "platform/atomic_write.hpp"
#include "platform/bounded_read.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/memory_ledger_bridge.hpp"
#include "runtime/session_service.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/index.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
namespace mem = lubancode::memory;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
using Report = memory::v1::SaveReport;
constexpr auto kPlanFile = "sdk-memory-write-plan.json";
constexpr std::size_t kPlanCap = 4096, kReportCap = 524288, kTotalCap = 67108864, kCallCap = 64;
Error Fail(std::string code, std::string message = {}) { return {std::move(code), std::move(message)}; }
const char* WriteOutcomeName(lubancode::platform::WriteOutcome outcome) {
    switch (outcome) {
    case lubancode::platform::WriteOutcome::NotCommitted: return "NotCommitted";
    case lubancode::platform::WriteOutcome::CommittedDurabilityNotRequested: return "CommittedDurabilityNotRequested";
    case lubancode::platform::WriteOutcome::CommittedDurabilityUnconfirmed: return "CommittedDurabilityUnconfirmed";
    case lubancode::platform::WriteOutcome::CommittedDurable: return "CommittedDurable";
    }
    return "Unknown";
}
bool Safe(const std::string& s) { return s.find('\0') == std::string::npos && lubancode::platform::IsValidUtf8(s); }
bool SafeJson(const Json& j) {
    if (j.is_string()) return Safe(j.get<std::string>());
    if (j.is_array() || j.is_object()) for (const auto& value : j) if (!SafeJson(value)) return false;
    return true;
}
bool SafeId(const std::string& s) {
    return !s.empty() && s.size() <= 200 && s.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == std::string::npos && s != "." && s != "..";
}
bool Inside(const fs::path& p, const fs::path& r) {
    auto a = p.begin();
    for (auto b = r.begin(); b != r.end(); ++a, ++b) if (a == p.end() || *a != *b) return false;
    return true;
}
Result<std::optional<std::string>> Read(const fs::path& path, const fs::path& root, std::size_t cap) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        return std::optional<std::string>{};
    if (ec || !fs::is_regular_file(status)) return std::unexpected(Fail("sdk.memory_write.invalid_file"));
    const auto real = fs::canonical(path, ec), owned = fs::canonical(root, ec);
    if (ec || !Inside(real, owned)) return std::unexpected(Fail("sdk.memory_write.path_escape"));
    auto bytes = lubancode::platform::ReadBoundedRegularFile(real, cap);
    if (!bytes) return std::unexpected(Fail("sdk.memory_write." + bytes.error()));
    if (!Safe(*bytes)) return std::unexpected(Fail("sdk.memory_write.invalid_text"));
    return std::optional<std::string>{std::move(*bytes)};
}
Result<void> Directory(const fs::path& path, const fs::path& root, bool create) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found)) {
        if (!create) return {};
        ec.clear(); fs::create_directory(path, ec);
        if (ec) return std::unexpected(Fail("sdk.memory_write.mkdir_failed"));
    } else if (ec || !fs::is_directory(status)) return std::unexpected(Fail("sdk.memory_write.invalid_directory"));
    const auto real = fs::canonical(path, ec), owned = fs::canonical(root, ec);
    if (ec || !Inside(real, owned)) return std::unexpected(Fail("sdk.memory_write.path_escape"));
    return {};
}
Json RequestJson(const mem::SaveRequest& r) {
    Json evidence = Json::array();
    for (const auto& e : r.evidence) evidence.push_back({{"path", e.path}, {"symbol", e.symbol}});
    return {{"kind", mem::MemoryKindName(r.kind)}, {"id", r.id}, {"title", r.title},
        {"summary", r.summary}, {"content", r.content}, {"keywords", r.keywords}, {"paths", r.paths},
        {"source_session", r.source_session}, {"confidence", r.confidence},
        {"scope", {{"level", r.scope.level}, {"kind", r.scope.kind}, {"value", r.scope.value}}},
        {"evidence", std::move(evidence)}, {"expires_at", r.expires_at}, {"occurred_at", r.occurred_at}};
}
std::string CommitKey(const lubancode::tools::ToolInvocationIdentity& i) {
    const Json key{{"session", i.session_id}, {"operation", i.operation_id}, {"turn", i.turn_id},
        {"action", i.action_id}, {"attempt", i.attempt}};
    return "sdk-save-" + lubancode::platform::Sha256Hex(key.dump());
}
const char* State(mem::ProjectCommitState value) {
    switch (value) {
    case mem::ProjectCommitState::NotStarted: return "not_started";
    case mem::ProjectCommitState::Committed: return "committed";
    case mem::ProjectCommitState::Indeterminate: return "indeterminate";
    } return "indeterminate";
}
const char* Stage(mem::ProjectCommitStage s) {
    switch (s) {
    case mem::ProjectCommitStage::Intent: return "intent";
    case mem::ProjectCommitStage::Snapshot: return "snapshot";
    case mem::ProjectCommitStage::Topic: return "topic";
    case mem::ProjectCommitStage::Cleanup: return "cleanup";
    case mem::ProjectCommitStage::Catalog: return "catalog";
    case mem::ProjectCommitStage::Index: return "index";
    case mem::ProjectCommitStage::Result: return "result";
    } return "invalid";
}
const char* Outcome(lubancode::platform::WriteOutcome o) {
    switch (o) {
    case lubancode::platform::WriteOutcome::NotCommitted: return "not_committed";
    case lubancode::platform::WriteOutcome::CommittedDurabilityNotRequested: return "visible";
    case lubancode::platform::WriteOutcome::CommittedDurabilityUnconfirmed: return "unconfirmed";
    case lubancode::platform::WriteOutcome::CommittedDurable: return "durable";
    } return "invalid";
}
Json ReportJson(const Report& r) {
    Json stages = Json::array();
    for (const auto& stage : r.stages) stages.push_back({{"stage", stage.stage}, {"outcome", stage.outcome}});
    return {{"sessionId", r.session_id}, {"operationId", r.operation_id}, {"turnId", r.turn_id},
        {"actionId", r.action_id}, {"attempt", r.attempt}, {"workspaceKey", r.workspace_key},
        {"planSha256", r.plan_sha256}, {"commitKey", r.commit_key}, {"requestedEventId", r.requested_event_id},
        {"receiptedEventId", r.receipted_event_id}, {"sourceEventRef", r.source_event_ref},
        {"saveRequestSha256", r.save_request_sha256}, {"requestSha256", r.request_sha256}, {"state", r.state},
        {"memoryId", r.memory_id}, {"memoryPath", r.memory_path}, {"contentSha256", r.content_sha256},
        {"committedAt", r.committed_at}, {"stages", std::move(stages)}, {"duplicate", r.duplicate},
        {"errorCode", r.error_code}, {"error", r.error}};
}
Report ParseReport(const Json& j) {
    Report r;
    r.session_id = j.at("sessionId").get<std::string>(); r.operation_id = j.at("operationId").get<std::string>();
    r.turn_id = j.at("turnId").get<std::string>(); r.action_id = j.at("actionId").get<std::string>();
    if (!j.at("attempt").is_number_unsigned()) throw std::runtime_error("attempt type");
    r.attempt = j.at("attempt").get<std::size_t>(); r.workspace_key = j.at("workspaceKey").get<std::string>();
    r.plan_sha256 = j.at("planSha256").get<std::string>(); r.commit_key = j.at("commitKey").get<std::string>();
    r.requested_event_id = j.at("requestedEventId").get<std::string>();
    r.receipted_event_id = j.at("receiptedEventId").get<std::string>(); r.source_event_ref = j.at("sourceEventRef").get<std::string>();
    r.save_request_sha256 = j.at("saveRequestSha256").get<std::string>(); r.request_sha256 = j.at("requestSha256").get<std::string>();
    r.state = j.at("state").get<std::string>(); r.memory_id = j.at("memoryId").get<std::string>();
    r.memory_path = j.at("memoryPath").get<std::string>(); r.content_sha256 = j.at("contentSha256").get<std::string>();
    r.committed_at = j.at("committedAt").get<std::string>(); r.duplicate = j.at("duplicate").get<bool>();
    r.error_code = j.at("errorCode").get<std::string>(); r.error = j.at("error").get<std::string>();
    if (!j.at("stages").is_array() || j.at("stages").size() > 7) throw std::runtime_error("stages type");
    for (const auto& s : j.at("stages")) r.stages.push_back({s.at("stage").get<std::string>(), s.at("outcome").get<std::string>()});
    if (ReportJson(r) != j) throw std::runtime_error("report shape");
    for (const auto& [key, value] : j.items()) { (void)key; if (value.is_string() && !Safe(value.get<std::string>())) throw std::runtime_error("text"); }
    for (const auto& s : r.stages) {
        if (s.stage != "intent" && s.stage != "snapshot" && s.stage != "topic" && s.stage != "cleanup" && s.stage != "catalog" && s.stage != "index" && s.stage != "result") throw std::runtime_error("stage");
        if (s.outcome != "not_committed" && s.outcome != "visible" && s.outcome != "unconfirmed" && s.outcome != "durable") throw std::runtime_error("outcome");
    }
    return r;
}
const Json* Binding(const v3::MessageLine& m) {
    if (!m.system_meta || !m.system_meta->contains("hostBindings")) return nullptr;
    const auto& h = m.system_meta->at("hostBindings");
    return h.is_object() && h.contains("memoryWrite") ? &h.at("memoryWrite") : nullptr;
}
} // namespace

Result<std::shared_ptr<SessionMemoryWrite>> SessionMemoryWrite::Prepare(
    const std::optional<memory::v1::WriteOptions>& selection, fs::path owned,
    lubancode::workspace::WorkspaceIdentity identity, std::string resume) {
    auto owner = std::shared_ptr<SessionMemoryWrite>(new SessionMemoryWrite);
    owner->owned_root_ = std::move(owned); owner->identity_ = std::move(identity); owner->resume_id_ = std::move(resume);
    owner->snapshot_.workspace_key = owner->identity_.workspace_key;
    owner->snapshot_.enabled = selection && selection->enabled;
    if (!owner->resume_id_.empty()) {
        auto dir = lubancode::workspace::index::ResolveDirByWorkspaceKey(owner->owned_root_ / "workspaces", owner->identity_.workspace_key);
        if (!dir) return std::unexpected(Fail("sdk.session.open_failed", "resume unavailable"));
        owner->expected_resume_dir_ = *dir / "sessions" / owner->resume_id_;
        auto bytes = Read(owner->expected_resume_dir_ / kPlanFile, owner->owned_root_, kPlanCap);
        if (!bytes) return std::unexpected(bytes.error());
        if (*bytes) {
            const auto plan = Json::parse(**bytes, nullptr, false);
            if (!plan.is_object() || !plan.contains("enabled") || !plan["enabled"].is_boolean())
                return std::unexpected(Fail("sdk.memory_write.plan_invalid"));
            const auto enabled = plan["enabled"].get<bool>();
            if (selection && selection->enabled != enabled) return std::unexpected(Fail("sdk.memory_write.resume_mismatch"));
            owner->snapshot_.enabled = enabled;
            if (plan != owner->Plan(owner->resume_id_) || plan.dump() != **bytes) return std::unexpected(Fail("sdk.memory_write.plan_invalid"));
            owner->saved_plan_bytes_ = **bytes; owner->snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(**bytes);
        } else {
            if (selection) return std::unexpected(Fail("sdk.memory_write.resume_mismatch", "legacy session cannot gain a write plan"));
            owner->legacy_ = true;
        }
    }
    return owner;
}
Json SessionMemoryWrite::Plan(const std::string& session) const {
    return {{"schemaVersion", 1}, {"enabled", snapshot_.enabled}, {"sessionId", session},
        {"workspaceKey", identity_.workspace_key}, {"projectRoot", lubancode::workspace::NormalizeIdentityPathText(identity_.project_root)},
        {"limits", {{"calls", kCallCap}, {"reportBytes", kReportCap}, {"reportTotalBytes", kTotalCap}, {"directoryEntries", 4096}}}};
}
Result<void> SessionMemoryWrite::CheckBinding(const v3::V3Ledger& source) const {
    if (legacy_) {
        for (const auto& message : source.messages) if (Binding(message)) return std::unexpected(Fail("sdk.memory_write.plan_invalid"));
        return {};
    }
    const Json expected{{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}};
    const auto matches = [&](const v3::MessageLine* m) { return m && Binding(*m) && *Binding(*m) == expected; };
    if (source.messages.empty() || !matches(&source.messages.front()) || !matches(source.FindMessage(source.context.system_message_ref)))
        return std::unexpected(Fail("sdk.memory_write.plan_invalid"));
    for (const auto& [revision, chain] : source.revision_chains) {
        (void)revision; if (!matches(source.FindMessage(chain.first))) return std::unexpected(Fail("sdk.memory_write.plan_invalid"));
    }
    return {};
}
std::expected<Json, std::string> SessionMemoryWrite::Open(const lubancode::trajectory::V3OpeningContext& context) {
    const auto fail = [](const Error& e) -> std::expected<Json, std::string> { return std::unexpected(e.code + ": " + e.message); };
    std::error_code ec;
    const auto dir = fs::canonical(context.session_dir, ec), root = fs::canonical(owned_root_, ec);
    if (ec || !Inside(dir, root / "workspaces")) return std::unexpected("sdk.memory_write.path_escape");
    session_dir_ = context.session_dir; snapshot_.session_id = context.session_id;
    snapshot_.memory_directory = lubancode::tools::PathToUtf8(dir.parent_path().parent_path() / "memory");
    if (context.source) {
        if (context.session_id != resume_id_ || context.session_dir != expected_resume_dir_) return std::unexpected("sdk.memory_write.resume_mismatch");
        auto bytes = Read(session_dir_ / kPlanFile, owned_root_, kPlanCap);
        if (!bytes) return fail(bytes.error());
        if (legacy_ ? bytes->has_value() : !bytes->has_value() || **bytes != saved_plan_bytes_) return std::unexpected("sdk.memory_write.plan_invalid");
        if (auto valid = CheckBinding(*context.source); !valid) return fail(valid.error());
        if (auto valid = CheckSavedReports(*context.source); !valid) return fail(valid.error());
    } else {
        if (!resume_id_.empty()) return std::unexpected("sdk.memory_write.resume_mismatch");
        auto prior = Read(session_dir_ / kPlanFile, owned_root_, kPlanCap);
        if (!prior) return fail(prior.error());
        if (*prior) return std::unexpected("sdk.memory_write.plan_invalid");
        const auto bytes = Plan(context.session_id).dump();
        const auto saved = WriteFrozenPlan(session_dir_ / kPlanFile, bytes);
        if (!saved) {
            const auto& error = saved.error();
            return std::unexpected("sdk.memory_write.plan_write_failed: " + Json{
                {"atomicCode", error.code},
                {"failureKind", error.failure_kind == lubancode::platform::WriteFailureKind::TransientReject
                    ? "TransientReject" : "Permanent"},
                {"outcome", WriteOutcomeName(error.outcome)}, {"message", error.message}}
                .dump(-1, ' ', false, Json::error_handler_t::replace));
        }
        if (saved->outcome != lubancode::platform::WriteOutcome::CommittedDurable) {
            return std::unexpected("sdk.memory_write.plan_write_failed: " + Json{
                {"outcome", WriteOutcomeName(saved->outcome)},
                {"message", "plan receipt was not CommittedDurable"}}
                .dump(-1, ' ', false, Json::error_handler_t::replace));
        }
        snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(bytes);
    }
    return legacy_ ? Json::object() : Json{{"hostBindings", {{"memoryWrite", {{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}}}}}};
}
lubancode::trajectory::V3OpeningParticipant SessionMemoryWrite::OpeningParticipant() {
    return [owner = shared_from_this()](const auto& context) { return owner->Open(context); };
}
std::unique_ptr<lubancode::tools::Tool> SessionMemoryWrite::BuildTool(lubancode::runtime::TrajectorySessionLedger& ledger) {
    return std::make_unique<mem::MemorySaveTool>([owner = shared_from_this(), &ledger](auto request, const auto& context) {
        return owner->Save(std::move(request), context, ledger);
    });
}
Result<void> SessionMemoryWrite::PersistReports(const std::string& operation, const std::string& turn) const {
    if (!SafeId(operation)) return std::unexpected(Fail("sdk.memory_write.report_invalid"));
    if (auto valid = Directory(session_dir_ / "sdk-memory-saves", session_dir_, true); !valid) return valid;
    Json reports = Json::array();
    if (const auto found = reports_.find(operation); found != reports_.end()) for (const auto& r : found->second) reports.push_back(ReportJson(r));
    Json value{{"schemaVersion", 1}, {"sessionId", snapshot_.session_id}, {"operationId", operation},
        {"turnId", turn}, {"workspaceKey", snapshot_.workspace_key}, {"planSha256", snapshot_.plan_sha256}, {"reports", std::move(reports)}};
    value["sha256"] = lubancode::platform::Sha256Hex(value.dump());
    const auto bytes = value.dump();
    if (bytes.size() > kReportCap) return std::unexpected(Fail("sdk.memory_write.report_limit"));
    const auto path = session_dir_ / "sdk-memory-saves" / (operation + ".json");
    auto existing = Read(path, session_dir_, kReportCap); // Never replace a link/bad target with a success.
    if (!existing) return std::unexpected(existing.error());
    const auto saved = lubancode::platform::AtomicWriteFile(path, bytes, lubancode::platform::WriteDurability::ProcessCrashDurability);
    if (!saved || saved->outcome != lubancode::platform::WriteOutcome::CommittedDurable)
        return std::unexpected(Fail("sdk.memory_write.report_write_failed", saved ? "unconfirmed" : saved.error().message));
    return {};
}
Result<void> SessionMemoryWrite::FinalizeOperation(const std::string& operation, const std::string& turn) {
    if (!snapshot_.enabled) return {};
    if (!reports_.contains(operation)) reports_.emplace(operation, std::vector<Report>{});
    return PersistReports(operation, turn);
}
Result<std::vector<Report>> SessionMemoryWrite::ReadReports(const std::string& operation) const {
    if (!SafeId(operation)) return std::unexpected(Fail("sdk.memory_write.report_invalid"));
    if (!snapshot_.enabled) return std::vector<Report>{};
    if (auto directory = Directory(session_dir_ / "sdk-memory-saves", session_dir_, false); !directory) return std::unexpected(directory.error());
    auto bytes = Read(session_dir_ / "sdk-memory-saves" / (operation + ".json"), session_dir_, kReportCap);
    if (!bytes) return std::unexpected(bytes.error());
    if (!*bytes) return std::unexpected(Fail("sdk.memory_write.report_unavailable"));
    return DecodeReports(operation, **bytes);
}
Result<std::vector<Report>> SessionMemoryWrite::DecodeReports(const std::string& operation, const std::string& bytes) const {
    try {
        auto value = Json::parse(bytes);
        const auto hash = value.at("sha256").get<std::string>(); value.erase("sha256");
        if (hash != lubancode::platform::Sha256Hex(value.dump()) || value.size() != 7 ||
            value.at("schemaVersion") != 1 || value.at("sessionId") != snapshot_.session_id ||
            value.at("operationId") != operation || value.at("workspaceKey") != snapshot_.workspace_key ||
            value.at("planSha256") != snapshot_.plan_sha256 || !value.at("turnId").is_string() ||
            !Safe(value.at("turnId").get<std::string>()) || !value.at("reports").is_array() || value.at("reports").size() > kCallCap)
            return std::unexpected(Fail("sdk.memory_write.report_invalid"));
        std::vector<Report> reports;
        for (const auto& j : value.at("reports")) {
            auto r = ParseReport(j);
            if (r.turn_id != value.at("turnId").get<std::string>()) return std::unexpected(Fail("sdk.memory_write.report_invalid"));
            reports.push_back(std::move(r));
        }
        return reports;
    } catch (const std::exception&) { return std::unexpected(Fail("sdk.memory_write.report_invalid")); }
}

lubancode::tools::Tool::Result SessionMemoryWrite::Save(
    std::expected<mem::SaveRequest, std::string> parsed,
    const lubancode::tools::ToolExecutionContext& context, lubancode::runtime::TrajectorySessionLedger& ledger) {
    using Control = lubancode::tools::ExecutionControl;
    const auto& i = context.invocation;
    lubancode::tools::Tool::Result result;
    if (!snapshot_.enabled || i.session_id != snapshot_.session_id || !SafeId(i.operation_id) ||
        i.turn_id.empty() || i.action_id.empty() || !Safe(i.turn_id) || !Safe(i.action_id) || !i.attempt) {
        result = {"sdk.memory_write.invocation_invalid", true}; result.error_code = "sdk.memory_write.invocation_invalid";
        return result;
    }
    auto& reports = reports_[i.operation_id];
    if (reports.size() >= kCallCap || indeterminate_) {
        result = {"Save execution stopped; an earlier side effect/report is unconfirmed or the report limit was reached.", true};
        result.error_code = "sdk.memory_write.stopped"; result.execution_control = Control::StopIndeterminate;
        indeterminate_ = true; return result;
    }
    Report r;
    r.session_id = i.session_id; r.operation_id = i.operation_id; r.turn_id = i.turn_id;
    r.action_id = i.action_id; r.attempt = i.attempt; r.workspace_key = snapshot_.workspace_key;
    r.plan_sha256 = snapshot_.plan_sha256; r.commit_key = CommitKey(i); r.state = "not_started";
    if (std::any_of(reports.begin(), reports.end(), [&](const auto& old) { return old.commit_key == r.commit_key; })) {
        result = {"sdk.memory_write.duplicate_invocation: do not re-execute a completed action", true};
        result.error_code = "sdk.memory_write.duplicate_invocation"; result.execution_control = Control::StopIndeterminate;
        indeterminate_ = true; return result;
    }
    lubancode::runtime::MemoryLedgerBridge bridge(ledger);
    if (!parsed) { r.error_code = "sdk.memory_write.invalid_input"; r.error = parsed.error(); }
    else {
        parsed->source_session = i.session_id; // host-owned; input cannot supply this field
        const auto request = RequestJson(*parsed);
        const auto valid = mem::store::ValidateSaveRequest(*parsed);
        if (!valid || !SafeJson(request) || request.dump().size() > 65536 || parsed->scope.level != "project") {
            r.error_code = "sdk.memory_write.invalid_input"; r.error = valid ? "invalid project request" : valid.error();
        } else {
            r.save_request_sha256 = lubancode::platform::Sha256Hex(request.dump());
            const auto requested = bridge.RecordSaveRequestedStrict(i, request, r.save_request_sha256, r.commit_key);
            if (!requested) {
                r.error_code = "sdk.memory_write.requested_failed"; r.error = requested.error();
                indeterminate_ = true; // topic is untouched; the main ledger itself is unconfirmed
            } else {
                r.requested_event_id = requested->event_id; r.source_event_ref = requested->source_event_ref;
                mem::ProjectCommitContext gate{identity_.project_root, lubancode::tools::Utf8ToPath(snapshot_.memory_directory),
                    session_dir_.parent_path().parent_path() / "lifecycle", snapshot_.workspace_key, r.commit_key,
                    i.session_id, r.source_event_ref};
                r.request_sha256 = mem::ProjectCommitRequestSha256(gate, *parsed);
                const auto saved = mem::CommitProjectUpsertWithCancellation(gate, *parsed,
                    [cancel = context.cancel] { return cancel && cancel->load(); });
                r.state = State(saved.state); r.memory_id = saved.memory_id; r.memory_path = saved.memory_path;
                r.content_sha256 = saved.content_sha256; r.committed_at = saved.committed_at; r.duplicate = saved.duplicate;
                r.error_code = saved.error_code; r.error = saved.error;
                for (const auto& stage : saved.stages) r.stages.push_back({Stage(stage.stage), Outcome(stage.outcome)});
                if (!saved.request_sha256.empty() && saved.request_sha256 != r.request_sha256) {
                    r.state = "indeterminate"; r.error_code = "sdk.memory_write.request_mismatch"; r.error = r.error_code;
                }
            }
        }
    }
    const auto receipted = bridge.RecordSaveReceiptStrict(i, ReportJson(r));
    if (receipted) r.receipted_event_id = *receipted;
    else { indeterminate_ = true; r.error += " | " + receipted.error(); }
    if (r.state == "indeterminate") indeterminate_ = true;
    reports.push_back(r);
    const auto persisted = PersistReports(i.operation_id, i.turn_id);
    if (!persisted) indeterminate_ = true;
    result.is_error = r.state != "committed" || indeterminate_;
    result.error_code = !persisted ? persisted.error().code : !receipted ? "sdk.memory_write.receipted_failed" : r.error_code;
    result.details = {{"memory_commit_state", r.state}, {"commit_key", r.commit_key}, {"request_sha256", r.request_sha256}};
    result.SetText("memory_save: " + r.state + "; key=" + r.commit_key + "; id=" + r.memory_id +
        (r.error.empty() ? "" : "; " + r.error));
    if (indeterminate_) {
        result.execution_control = Control::StopIndeterminate;
        result.AppendText("\nPartial writes or their final receipt may be unconfirmed. Do not retry automatically.");
    }
    return result;
}

Result<void> SessionMemoryWrite::ValidateReports(const std::string& operation, const std::string& turn,
    const std::vector<Report>& reports, const v3::V3Ledger& source, bool require_adopted) const {
    const auto bad = [](std::string detail = {}) -> Result<void> { return std::unexpected(Fail("sdk.memory_write.report_invalid", std::move(detail))); };
    if (source.session_id != snapshot_.session_id || reports.size() > kCallCap) return bad("session/call limit");
    std::set<std::string> requested_ids, receipt_ids, actions;
    for (const auto& r : reports) {
        if (r.session_id != snapshot_.session_id || r.workspace_key != snapshot_.workspace_key ||
            r.operation_id != operation || r.turn_id != turn || r.plan_sha256 != snapshot_.plan_sha256 ||
            r.action_id.empty() || !r.attempt || !actions.insert(r.action_id + ":" + std::to_string(r.attempt)).second ||
            (r.state != "not_started" && r.state != "committed" && r.state != "indeterminate")) return bad("report identity/state");
        const lubancode::tools::ToolInvocationIdentity identity{r.session_id, operation, turn, r.action_id, r.attempt};
        if (r.commit_key != CommitKey(identity)) return bad("commit key");
        const v3::EventLine* started = nullptr;
        for (const auto& event : source.events) {
            if (event.kind == v3::EventKindV3::ToolExecutionStarted && event.action_id == r.action_id &&
                event.turn_id == turn && event.payload.value("attempt", Json()) == r.attempt &&
                event.payload.value("toolName", Json()) == "memory_save") {
                if (started) return bad("duplicate start");
                started = &event;
            }
        }
        if (!started) return bad("no actual save action");
        // Continue preserves the file's original run, including same-ID SDK
        // recovery. Keep duplicate-start detection above independent of owner
        // validation so a foreign envelope cannot hide a second actual start.
        if (started->session_id != source.session_id || started->run_id != source.run_id)
            return bad("start owner");
        const auto* receipt = source.FindEvent(r.receipted_event_id);
        if (!receipt || receipt->kind != v3::EventKindV3::MemoryWriteReceipted || receipt->turn_id != turn ||
            receipt->session_id != source.session_id || receipt->run_id != started->run_id ||
            receipt->action_id != r.action_id || receipt->seq <= started->seq ||
            receipt->payload.value("sdkMemoryWrite", Json()) != 1 || receipt->payload.value("operationId", Json()) != operation ||
            receipt->payload.value("attempt", Json()) != r.attempt || !receipt_ids.insert(receipt->event_id).second) return bad("receipt source");
        auto original = ReportJson(r); original["receiptedEventId"] = "";
        if (receipt->payload.value("receipt", Json()) != original) return bad("receipt metadata differs");
        if (r.requested_event_id.empty()) {
            if (r.state != "not_started" || !r.source_event_ref.empty() || !r.request_sha256.empty() || !r.stages.empty() ||
                !r.memory_id.empty() || !r.memory_path.empty() || !r.content_sha256.empty() || r.error_code.empty()) return bad("no-request side effects");
        } else {
            const auto* requested = source.FindEvent(r.requested_event_id);
            if (!requested || requested->kind != v3::EventKindV3::MemorySaveRequested || requested->turn_id != turn ||
                requested->session_id != source.session_id || requested->run_id != started->run_id ||
                requested->action_id != r.action_id || requested->seq <= started->seq || requested->seq >= receipt->seq ||
                requested->payload.value("operationId", Json()) != operation || requested->payload.value("attempt", Json()) != r.attempt ||
                requested->payload.value("sdkMemoryWrite", Json()) != 1 || requested->payload.value("commitKey", Json()) != r.commit_key ||
                !requested_ids.insert(requested->event_id).second) return bad("requested source");
            const auto ref = "workspace_key=" + snapshot_.workspace_key + "/session_id=" + source.session_id +
                "/run_id=" + requested->run_id + "/event_id=" + requested->event_id;
            if (r.source_event_ref != ref) return bad("source ref");
            const auto raw = requested->payload.value("normalizedRequest", Json());
            auto request = mem::ParseMemorySaveInput(raw);
            if (!request || raw.value("source_session", Json()) != r.session_id) return bad("normalized request");
            request->source_session = r.session_id;
            if (RequestJson(*request) != raw || !mem::store::ValidateSaveRequest(*request) ||
                lubancode::platform::Sha256Hex(raw.dump()) != r.save_request_sha256 ||
                requested->payload.value("saveRequestSha256", Json()) != r.save_request_sha256) return bad("save request SHA");
            mem::ProjectCommitContext gate{identity_.project_root, lubancode::tools::Utf8ToPath(snapshot_.memory_directory),
                session_dir_.parent_path().parent_path() / "lifecycle", snapshot_.workspace_key, r.commit_key, r.session_id, r.source_event_ref};
            if (mem::ProjectCommitRequestSha256(gate, *request) != r.request_sha256) return bad("gate request SHA");
            const auto saved = mem::InspectProjectCommitReceipt(gate, *request);
            if (r.state == "committed") {
                if (saved.state != mem::ProjectCommitState::Committed || !saved.error_code.empty() ||
                    saved.memory_id != r.memory_id || saved.memory_path != r.memory_path || saved.content_sha256 != r.content_sha256 ||
                    saved.committed_at != r.committed_at || r.stages.size() != saved.stages.size() + 1 ||
                    r.stages.back() != memory::v1::SaveStage{"result", "durable"} || !r.error.empty() || !r.error_code.empty()) return bad("unconfirmed committed receipt");
                for (std::size_t n = 0; n < saved.stages.size(); ++n)
                    if (r.stages[n] != memory::v1::SaveStage{Stage(saved.stages[n].stage), Outcome(saved.stages[n].outcome)}) return bad("stages differ");
            } else if (r.state == "not_started") {
                if (saved.state != mem::ProjectCommitState::NotStarted || r.error_code.empty() ||
                    std::any_of(r.stages.begin(), r.stages.end(), [](const auto& s) { return s.stage == "topic" && s.outcome != "not_committed"; })) return bad("not-started side effect");
            } else if (r.error_code.empty()) return bad("unknown without error");
            const auto snapshot = std::find_if(r.stages.begin(), r.stages.end(), [](const auto& s) {
                return s.stage == "snapshot" && s.outcome != "not_committed";
            });
            if (snapshot != r.stages.end()) {
                const auto bytes = Read(gate.lifecycle_root / r.commit_key / "topic.snapshot.md", gate.lifecycle_root, 16384);
                if (!bytes || !*bytes || lubancode::platform::Sha256Hex(**bytes) != r.content_sha256) return bad("snapshot differs");
            }
        }
        if (require_adopted) {
            bool adopted = false;
            for (const auto& message : source.messages) {
                if (message.session_id != source.session_id || message.run_id != started->run_id ||
                    message.action_id != r.action_id || message.turn_id != turn || !message.result_selection_ref ||
                    message.message.value("role", Json()) != "tool" ||
                    message.message.value("tool_call_id", Json()) != r.action_id) continue;
                const auto* selected = source.FindEvent(*message.result_selection_ref);
                if (!selected || selected->kind != v3::EventKindV3::ToolResultSelected || selected->action_id != r.action_id ||
                    selected->session_id != source.session_id || selected->run_id != started->run_id ||
                    selected->turn_id != turn || selected->payload.value("attempt", Json()) != r.attempt ||
                    selected->seq <= receipt->seq || selected->seq >= message.seq) continue;
                for (const auto& [revision, chain] : source.revision_chains) {
                    (void)revision;
                    if (std::find(chain.second.begin(), chain.second.end(), message.message_id) != chain.second.end()) adopted = true;
                }
            }
            if (!adopted) return bad("save tool result was not adopted");
        }
    }
    // Negative calibration: a forged empty/downgraded report cannot erase actual
    // requested or receipted facts in this operation/turn.
    for (const auto& event : source.events) {
        if (event.payload.value("sdkMemoryWrite", Json()) != 1 || event.payload.value("operationId", Json()) != operation) continue;
        if (event.turn_id != turn || (event.kind == v3::EventKindV3::MemorySaveRequested && !requested_ids.contains(event.event_id)) ||
            (event.kind == v3::EventKindV3::MemoryWriteReceipted && !receipt_ids.contains(event.event_id))) return bad("unrepresented save fact");
    }
    return {};
}

Result<void> SessionMemoryWrite::CheckSavedReports(const v3::V3Ledger& source) {
    if (!snapshot_.enabled) return {};
    const auto path = session_dir_ / "sdk-memory-saves";
    if (auto valid = Directory(path, session_dir_, false); !valid) return valid;
    std::map<std::string, std::string> finals;
    std::set<std::string> accepted, complete;
    for (const auto& fact : lubancode::runtime::SessionService::ReadOperationFacts(session_dir_)) {
        if (fact.kind == "operation.accepted") accepted.insert(fact.operation_id);
        if (fact.kind != "operation.final") continue;
        if (!SafeId(fact.operation_id) || !finals.emplace(fact.operation_id, fact.turn_id).second) return std::unexpected(Fail("sdk.memory_write.report_invalid"));
        auto bytes = Read(session_dir_ / "sdk-results" / (fact.operation_id + ".json"), session_dir_, kTotalCap);
        if (!bytes) return std::unexpected(bytes.error());
        if (*bytes) {
            const auto value = Json::parse(**bytes, nullptr, false);
            if (value.is_object() && value.value("operationId", Json()) == fact.operation_id && value.value("turnId", Json()) == fact.turn_id &&
                value.value("complete", Json()) == true && fact.execution_status != "interrupted") complete.insert(fact.operation_id);
        }
    }
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    for (const auto& event : source.events) {
        if (event.kind == v3::EventKindV3::MemorySaveRequested && event.payload.value("sdkMemoryWrite", Json()) == 1 &&
            !complete.contains(event.payload.value("operationId", std::string()))) indeterminate_ = true;
    }
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found)) {
        if (!complete.empty()) return std::unexpected(Fail("sdk.memory_write.report_unavailable"));
        return {};
    }
    fs::directory_iterator it(path, ec), end;
    if (ec) return std::unexpected(Fail("sdk.memory_write.read_failed"));
    std::size_t enumerated = 0, used = 0;
    std::set<std::string> present;
    for (; it != end; it.increment(ec)) {
        if (ec) return std::unexpected(Fail("sdk.memory_write.read_failed"));
        if (++enumerated > 4096) return std::unexpected(Fail("sdk.memory_write.report_limit"));
        const auto file = it->path();
        if (file.extension() == ".tmp") continue;
        const auto operation = file.stem().string();
        if (file.extension() != ".json" || !SafeId(operation) || !accepted.contains(operation) || !present.insert(operation).second)
            return std::unexpected(Fail("sdk.memory_write.report_invalid"));
        auto bytes = Read(file, session_dir_, (std::min)(kReportCap, kTotalCap - used));
        // Failed/oversized reads abort the gate; they cannot be skipped to evade
        // the actual accumulated IO bound, even for an interrupted operation.
        if (!bytes) return std::unexpected(bytes.error());
        if (!*bytes) return std::unexpected(Fail("sdk.memory_write.report_unavailable"));
        used += (**bytes).size();
        auto reports = DecodeReports(operation, **bytes);
        if (!reports) { if (complete.contains(operation)) return std::unexpected(reports.error()); continue; }
        const auto value = Json::parse(**bytes, nullptr, false);
        const auto turn = value.value("turnId", std::string());
        if (complete.contains(operation) && finals.at(operation) != turn) return std::unexpected(Fail("sdk.memory_write.report_invalid"));
        auto valid = ValidateReports(operation, turn, *reports, source, complete.contains(operation));
        if (!valid && complete.contains(operation)) return valid;
        if (std::any_of(reports->begin(), reports->end(), [](const auto& r) { return r.state == "indeterminate"; })) {
            if (complete.contains(operation)) return std::unexpected(Fail("sdk.memory_write.report_invalid", "complete final claims unknown save"));
            // Open is not permission to continue an unresolved execution.
            indeterminate_ = true;
        }
    }
    if (ec) return std::unexpected(Fail("sdk.memory_write.read_failed"));
    for (const auto& operation : complete) if (!present.contains(operation)) return std::unexpected(Fail("sdk.memory_write.report_unavailable"));
    return {};
}
} // namespace lubancore::detail
