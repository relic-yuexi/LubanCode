#include "sdk/subagents.hpp"
#include "sdk/prepare_journal.hpp"
#include "sdk/operation_ledger.hpp"
#include "sdk/plan_write.hpp"
#include "sdk/usage_result.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <utility>

#include "platform/atomic_write.hpp"
#include "platform/bounded_read.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "runtime/session_service.hpp"
#include "tools/path_utils.hpp"
#include "trajectory/v3/reader.hpp"
#include "trajectory/v3/child_adoption.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "workspace/index.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
namespace tools = lubancode::tools;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
constexpr std::size_t kMaxPlanBytes = 65536;
constexpr std::size_t kMaxTools = 128;
constexpr std::size_t kMaxRecoveryResultBytes = 64 * 1024 * 1024;
constexpr const char* kPlanFile = "sdk-subagent-plan.json";

Error Fail(std::string code, std::string message = {}) { return {std::move(code), std::move(message)}; }
bool Text(const std::string& text, std::size_t max) {
    return !text.empty() && text.size() <= max && text.find('\0') == std::string::npos &&
        lubancode::platform::IsValidUtf8(text);
}
bool ToolName(const std::string& text) {
    return Text(text, 200) && text.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string::npos;
}
bool Positive(std::int64_t value) {
    return value > 0 && value <= std::numeric_limits<int>::max();
}
bool StepOrSecond(const Json& value) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() > 0 &&
        value.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<int>::max());
    return value.is_number_integer() && Positive(value.get<std::int64_t>());
}
bool ExploreName(const std::string& name) {
    // Intersection with the existing AgentTool role gate; never use names as
    // proof of read-only effects. Unknown custom/MCP metadata stays excluded.
    return name == "read_file" || name == "search" || name == "web_fetch" ||
        name == "web_search" || name == "lsp";
}
bool ReadOnly(lubancode::EffectClass effect) {
    return effect == lubancode::EffectClass::ReadOnlyLocal || effect == lubancode::EffectClass::ReadOnlyRemote;
}
Result<std::optional<std::string>> ReadPlan(const fs::path& root, const fs::path& directory) {
    const auto relative = directory.lexically_normal().lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        return std::unexpected(Fail("sdk.subagent.plan_invalid", "session directory escapes owned root"));
    std::error_code ec;
    auto cursor = root;
    for (const auto& part : relative) {
        cursor /= part;
        const auto status = fs::symlink_status(cursor, ec);
        if (ec || fs::is_symlink(status) || !fs::is_directory(status))
            return std::unexpected(Fail("sdk.subagent.plan_invalid", "session directory is linked or unavailable"));
    }
    if (fs::canonical(directory, ec) != directory.lexically_normal() || ec)
        return std::unexpected(Fail("sdk.subagent.plan_invalid", "session directory mapping changed"));
    const auto path = directory / kPlanFile;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        return std::optional<std::string>{};
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        return std::unexpected(Fail("sdk.subagent.plan_invalid", "plan is linked, nonregular or unreadable"));
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected(Fail("sdk.subagent.plan_invalid", "cannot read plan"));
    std::string bytes(kMaxPlanBytes + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > kMaxPlanBytes || input.bad() || (input.fail() && !input.eof()) ||
        bytes.find('\0') != std::string::npos || !lubancode::platform::IsValidUtf8(bytes))
        return std::unexpected(Fail("sdk.subagent.plan_invalid", "plan exceeds 64 KiB or has invalid bytes"));
    return std::optional<std::string>{std::move(bytes)};
}
const Json* Binding(const v3::MessageLine& message) {
    if (!message.system_meta || !message.system_meta->is_object()) return nullptr;
    const auto hosts = message.system_meta->find("hostBindings");
    if (hosts == message.system_meta->end() || !hosts->is_object()) return nullptr;
    const auto found = hosts->find("sdkSubagents");
    return found == hosts->end() ? nullptr : &*found;
}
Json Declaration(Json plan) {
    plan.erase("toolFacts");
    if (plan.contains("profiles") && plan["profiles"].is_array())
        for (auto& profile : plan["profiles"]) profile.erase("effectiveTools");
    return plan;
}
class BorrowedTool final : public tools::Tool {
public:
    explicit BorrowedTool(tools::Tool& tool) : tool_(tool) {}
    std::string name() const override { return tool_.name(); }
    std::string description() const override { return tool_.description(); }
    Json input_schema() const override { return tool_.input_schema(); }
    bool needs_confirm() const override { return tool_.needs_confirm(); }
    tools::ApprovalClass approval_class() const override { return tool_.approval_class(); }
    lubancode::EffectClass effect_class() const override { return tool_.effect_class(); }
    tools::Idempotency idempotency() const override { return tool_.idempotency(); }
    tools::RecoveryCapability recovery_capability() const override { return tool_.recovery_capability(); }
    std::string version_or_digest() const override { return tool_.version_or_digest(); }
    Result execute(const Json& input) override { return tool_.execute(input); }
    Result execute(const Json& input, const tools::ToolExecutionContext& context) override {
        return tool_.execute(input, context);
    }
private:
    tools::Tool& tool_;
};
class DispatchTool final : public tools::Tool {
public:
    explicit DispatchTool(SessionSubagents& owner) : owner_(owner) {}
    std::string name() const override { return "agent"; }
    std::string description() const override { return "Run one explicitly admitted foreground child task; wait for its result."; }
    Json input_schema() const override { return owner_.InputSchema(); }
    Result execute(const Json& input) override { return owner_.Execute(input, {}); }
    Result execute(const Json& input, const tools::ToolExecutionContext& context) override {
        return owner_.Execute(input, context);
    }
private:
    SessionSubagents& owner_;
};
} // namespace

subagents::v1::LiveTerminalReceipt CopyChildReceipt(const lubancode::runtime::SubagentTerminalReceipt& receipt) {
    namespace rt = lubancode::runtime;
    subagents::v1::LiveTerminalReceipt out;
    out.child_session_id = receipt.session_id; out.child_run_id = receipt.run_id; out.terminal_kind = receipt.terminal_kind;
    switch (receipt.execution) {
    case rt::SubagentExecutionOutcome::Succeeded: out.execution_state = "succeeded"; break;
    case rt::SubagentExecutionOutcome::Failed: out.execution_state = "failed"; break;
    case rt::SubagentExecutionOutcome::Cancelled: out.execution_state = "cancelled"; break;
    case rt::SubagentExecutionOutcome::StartupRejected: out.execution_state = "startup_rejected"; break;
    case rt::SubagentExecutionOutcome::Indeterminate: out.execution_state = "indeterminate"; break;
    }
    switch (receipt.confirmation) {
    case rt::SubagentAppendConfirmation::Committed: out.append_confirmation = "committed"; break;
    case rt::SubagentAppendConfirmation::RejectedBeforeCommit: out.append_confirmation = "rejected_before_commit"; break;
    case rt::SubagentAppendConfirmation::DurabilityUnconfirmed: out.append_confirmation = "durability_unconfirmed"; break;
    }
    switch (receipt.seal) {
    case rt::SubagentSealState::NotAttempted: out.seal_state = "not_attempted"; break;
    case rt::SubagentSealState::Closed: out.seal_state = "closed"; break;
    case rt::SubagentSealState::CloseFailed: out.seal_state = "close_failed"; break;
    }
    if (receipt.terminal) {
        out.terminal_event_id = receipt.terminal->event_id; out.terminal_seq = receipt.terminal->seq;
        out.terminal_hash = receipt.terminal->hash;
    }
    out.broken_after_append = receipt.broken_after_append; out.broken_after_close = receipt.broken_after_close;
    out.append_error_code = receipt.append_error_code; out.close_error_code = receipt.close_error_code;
    return out;
}

Result<std::vector<subagents::v1::Report>> ReadSubagentReports(
    const v3::V3Ledger& source, const fs::path& directory, const std::string& session_id,
    const std::string& operation_id, const std::string& turn_id, bool require_complete, bool allow_unconsumed,
    const OperationUsage* usage) {
    const auto bad = [](std::string why) -> Result<std::vector<subagents::v1::Report>> {
        return std::unexpected(Fail("sdk.subagent.report_invalid", std::move(why)));
    };
    if (source.session_id != session_id) return bad("verified parent belongs to another Session");
    std::vector<subagents::v1::Report> reports;
    std::set<std::pair<std::string, std::uint64_t>> seen;
    std::vector<bool> bound(usage ? usage->attempts.size() : 0, false);
    std::string_view usage_error;
    const auto usage_bad = [](std::string_view why) -> Result<std::vector<subagents::v1::Report>> {
        return std::unexpected(Fail("sdk.usage.result_invalid", std::string(why)));
    };
    const auto checked_child = [&](const v3::V3Ledger& child) {
        if (!usage) return;
        std::set<std::string> requests;
        for (std::size_t i = 0; i < usage->attempts.size(); ++i) {
            const auto& record = usage->attempts[i];
            if (!record.subordinate || record.source_session_id != child.session_id || record.source_run_id != child.run_id) continue;
            const auto checked = usage_result::ValidateRequestBinding(record, child);
            if (!checked) { usage_error = checked.error(); return; }
            if (bound[i] || !requests.insert(record.trajectory_request_id).second) {
                usage_error = "sdk.usage.child_request_ambiguous"; return;
            }
            bound[i] = true;
        }
    };
    try {
        for (const auto& event : source.events) {
            if (event.kind != v3::EventKindV3::ToolExecutionStarted || event.turn_id != turn_id ||
                event.payload.value("toolName", Json()) != "agent") continue;
            if (event.session_id != session_id || event.run_id != source.run_id || !event.action_id ||
                !event.payload.contains("attempt") || !event.payload["attempt"].is_number_unsigned() ||
                event.payload["attempt"].get<std::uint64_t>() == 0)
                return bad("started child action owner or attempt differs");
            const auto attempt = event.payload["attempt"].get<std::uint64_t>();
            if (!seen.emplace(*event.action_id, attempt).second) return bad("duplicate started child action");
            const auto checked = usage ? v3::ValidateChildAdoption(source, directory, turn_id, *event.action_id, attempt, {}, checked_child) :
                v3::ValidateChildAdoption(source, directory, turn_id, *event.action_id, attempt);
            if (!usage_error.empty()) return usage_bad(usage_error);
            // A schema/plan refusal reached no child spawn. Its ordinary tool
            // result remains queryable; it is not an invented child report.
            if (checked.state == v3::ChildAdoptionState::NotApplicable) continue;
            if (reports.size() == 128) return bad("one operation exceeds 128 child reports");
            subagents::v1::Report report;
            report.operation_id = operation_id; report.parent_turn_id = turn_id;
            report.parent_action_id = *event.action_id; report.parent_attempt = attempt; report.issue = checked.issue;
            if (checked.state == v3::ChildAdoptionState::Rejected) return bad(checked.issue);
            if (checked.state == v3::ChildAdoptionState::Validated && checked.adoption) {
                const auto& a = *checked.adoption;
                if (a.parent_session_id != session_id || a.parent_run_id != event.run_id ||
                    a.turn_id != turn_id || a.action_id != *event.action_id || a.attempt != attempt)
                    return bad("strict adoption envelope differs");
                subagents::v1::HistoricalAdoption h;
                h.parent_session_id = a.parent_session_id; h.parent_run_id = a.parent_run_id;
                h.turn_id = a.turn_id; h.action_id = a.action_id; h.attempt = a.attempt;
                h.spawn_event_id = a.spawn_event_id; h.observation_event_id = a.observation_event_id;
                h.raw_persisted_event_id = a.raw_persisted_event_id;
                h.child_session_id = a.child_session_id; h.child_run_id = a.child_run_id;
                h.child_terminal_event_id = a.child_terminal_event_id; h.child_terminal_seq = a.child_terminal_seq;
                h.child_terminal_hash = a.child_terminal_hash; h.child_terminal_reason = a.child_terminal_reason;
                h.child_close_quality = a.child_close_quality; h.producer_execution_claim = a.producer_execution_claim;
                h.producer_append_claim = a.producer_append_claim; h.producer_seal_claim = a.producer_seal_claim;
                h.raw_text_sha256 = a.raw_text_sha256; h.raw_text_bytes = a.raw_text_bytes;
                h.raw_structured_sha256 = a.raw_structured_sha256; h.raw_structured_bytes = a.raw_structured_bytes;
                h.effective_persisted_event_ids = a.effective_persisted_event_ids;
                h.selected_source_event_ids = a.selected_source_event_ids; h.selected_event_id = a.selected_event_id;
                h.original_tool_message_id = a.original_tool_message_id; h.admission_event_id = a.admission_event_id;
                h.consumed_tool_message_id = a.consumed_tool_message_id; h.prepared_event_id = a.prepared_event_id;
                h.prepared_context_revision = a.prepared_context_revision;
                report.adoption_state = subagents::v1::AdoptionState::Validated; report.adoption = std::move(h);
            } else {
                if (require_complete && !(allow_unconsumed && checked.issue == "subagent.adoption.prepared_consumption_pending"))
                    return bad("complete operation has incomplete child adoption: " + checked.issue);
                report.adoption_state = subagents::v1::AdoptionState::Incomplete;
            }
            reports.push_back(std::move(report));
        }
    } catch (const std::exception& error) { return bad(error.what()); }
    if (usage) for (std::size_t i = 0; i < usage->attempts.size(); ++i)
        if (usage->attempts[i].subordinate && !bound[i]) return usage_bad("sdk.usage.child_source_unavailable");
    return reports;
}

Result<std::shared_ptr<SessionSubagentPlan>> SessionSubagentPlan::Prepare(
    const std::optional<subagents::v1::Options>& options, fs::path root, std::string workspace_key,
    std::string resume_id, std::string cwd, std::string parent_model, std::string permission_floor,
    int parent_max_steps, int parent_max_wall_seconds, std::shared_ptr<SessionPrepareJournal> journal) {
    auto owner = std::shared_ptr<SessionSubagentPlan>(new SessionSubagentPlan);
    owner->root_ = std::move(root); owner->resume_id_ = std::move(resume_id);
    owner->parent_max_steps_ = parent_max_steps; owner->parent_max_wall_seconds_ = parent_max_wall_seconds;
    owner->snapshot_.cwd = std::move(cwd); owner->snapshot_.permission_floor = std::move(permission_floor);
    std::optional<subagents::v1::Options> effective = options;
    if (!owner->resume_id_.empty()) {
        const auto workspace = lubancode::workspace::index::ResolveDirByWorkspaceKey(owner->root_ / "workspaces", workspace_key);
        if (!workspace) return std::unexpected(Fail("sdk.session.open_failed", "resume session is unavailable"));
        owner->resume_dir_ = *workspace / "sessions" / owner->resume_id_;
        std::error_code ec;
        const auto status = fs::symlink_status(owner->resume_dir_, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
            return std::unexpected(Fail("sdk.session.open_failed", "resume session is unavailable"));
        auto bytes = ReadPlan(owner->root_, owner->resume_dir_);
        if (!bytes) return std::unexpected(bytes.error());
        if (!*bytes) {
            if (options && options->enabled) return std::unexpected(Fail("sdk.subagent.resume_mismatch", "legacy session admitted no child plan"));
            owner->legacy_ = true;
        } else {
            owner->saved_bytes_ = **bytes;
            owner->saved_plan_ = Json::parse(owner->saved_bytes_, nullptr, false);
            const auto& saved = owner->saved_plan_;
            if (!saved.is_object() || saved.dump() != owner->saved_bytes_ || saved.value("schemaVersion", Json()) != 1 ||
                saved.value("sessionId", Json()) != owner->resume_id_ || !saved.contains("enabled") ||
                !saved["enabled"].is_boolean() || !saved.contains("profiles") || !saved["profiles"].is_array() || saved["profiles"].size() > 2)
                return std::unexpected(Fail("sdk.subagent.plan_invalid", "invalid or noncanonical saved plan"));
            owner->snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(owner->saved_bytes_);
            if (!effective) {
                subagents::v1::Options inherited;
                inherited.enabled = saved["enabled"].get<bool>();
                for (const auto& entry : saved["profiles"]) {
                    if (!entry.is_object() || !entry.contains("type") || !entry["type"].is_string() ||
                        !entry.contains("requestedTools") || !entry["requestedTools"].is_array() || entry["requestedTools"].size() > kMaxTools ||
                        !entry.contains("requestedModel") || !entry["requestedModel"].is_string() ||
                        !entry.contains("maxStepsPerTurn") || !StepOrSecond(entry["maxStepsPerTurn"]) ||
                        !entry.contains("maxWallSeconds") || !StepOrSecond(entry["maxWallSeconds"]))
                        return std::unexpected(Fail("sdk.subagent.plan_invalid", "saved profile shape is invalid"));
                    subagents::v1::Profile profile;
                    profile.type = entry["type"].get<std::string>(); profile.model = entry["requestedModel"].get<std::string>();
                    for (const auto& tool : entry["requestedTools"]) {
                        if (!tool.is_string()) return std::unexpected(Fail("sdk.subagent.plan_invalid", "saved tool name is not text"));
                        profile.tools.push_back(tool.get<std::string>());
                    }
                    profile.max_steps_per_turn = entry["maxStepsPerTurn"].get<std::int64_t>();
                    profile.max_wall_seconds = entry["maxWallSeconds"].get<std::int64_t>();
                    inherited.profiles.push_back(std::move(profile));
                }
                effective = std::move(inherited);
            }
        }
        auto stream = v3::FindV3SessionStream(owner->resume_dir_);
        if (!stream) return std::unexpected(Fail("sdk.subagent.plan_invalid", "saved V3 journal is unavailable"));
        auto ledger = ReadPrepareJournal(*stream, journal.get());
        if (!ledger) return std::unexpected(Fail("sdk.subagent.plan_invalid", ledger.error()));
        auto checked = owner->CheckBinding(**ledger);
        if (!checked) return std::unexpected(checked.error());
        checked = owner->CheckRecoveredChildren(**ledger);
        if (!checked) return std::unexpected(checked.error());
    }
    if (effective) {
        if ((!effective->enabled && !effective->profiles.empty()) ||
            (effective->enabled && (effective->profiles.empty() || effective->profiles.size() > 2)))
            return std::unexpected(Fail("sdk.subagent.invalid_plan", "enabled requires one or two exact profiles; disabled requires none"));
        if (effective->enabled && !Text(parent_model, 256))
            return std::unexpected(Fail("sdk.subagent.invalid_plan", "parent request model must be bounded UTF-8 text"));
        owner->snapshot_.enabled = effective->enabled;
        std::set<std::string> types;
        for (auto profile : effective->profiles) {
            if ((profile.type != "general-purpose" && profile.type != "Explore") || !types.insert(profile.type).second ||
                profile.tools.empty() || profile.tools.size() > kMaxTools || !Positive(profile.max_steps_per_turn) ||
                !Positive(profile.max_wall_seconds) || (!profile.model.empty() && !Text(profile.model, 256)) ||
                (parent_max_steps > 0 && profile.max_steps_per_turn > parent_max_steps) ||
                (parent_max_wall_seconds > 0 && profile.max_wall_seconds > parent_max_wall_seconds))
                return std::unexpected(Fail("sdk.subagent.invalid_plan", "unsupported/duplicate type, missing/overflow/widening budget or invalid model"));
            std::set<std::string> names;
            for (const auto& name : profile.tools)
                if (!ToolName(name) || name == "agent" || name == "memory_save" || name == "todo_write" || !names.insert(name).second)
                    return std::unexpected(Fail("sdk.subagent.invalid_plan", "invalid, duplicate or inadmissible child tool"));
            std::sort(profile.tools.begin(), profile.tools.end());
            const auto model = profile.model.empty() ? parent_model : profile.model;
            owner->snapshot_.profiles.push_back({std::move(profile), {}, model});
        }
        std::sort(owner->snapshot_.profiles.begin(), owner->snapshot_.profiles.end(),
            [](const auto& a, const auto& b) { return a.requested.type < b.requested.type; });
    }
    if (!owner->resume_id_.empty() && !owner->legacy_ &&
        Declaration(owner->Plan(owner->resume_id_)) != Declaration(owner->saved_plan_))
        return std::unexpected(Fail("sdk.subagent.resume_mismatch", "frozen host admission, model, cwd/floor or budget changed"));
    return owner;
}

SessionSubagentPlan::Json SessionSubagentPlan::Plan(const std::string& id) const {
    Json profiles = Json::array();
    for (const auto& p : snapshot_.profiles) profiles.push_back({{"type", p.requested.type},
        {"requestedTools", p.requested.tools}, {"effectiveTools", p.effective_tools},
        {"requestedModel", p.requested.model}, {"effectiveModel", p.effective_model},
        {"maxStepsPerTurn", p.requested.max_steps_per_turn}, {"maxWallSeconds", p.requested.max_wall_seconds}});
    return Json{{"schemaVersion", 1}, {"sessionId", id}, {"enabled", snapshot_.enabled},
        {"cwd", snapshot_.cwd}, {"permissionFloor", snapshot_.permission_floor},
        {"maxDepth", 1}, {"executionMode", "foreground"}, {"profiles", std::move(profiles)}, {"toolFacts", tool_facts_}};
}
Result<void> SessionSubagentPlan::BindTools(const tools::ToolRegistry& registry) {
    if (bound_) return std::unexpected(Fail("sdk.subagent.invalid_plan", "tool surface already frozen"));
    tool_facts_ = Json::array();
    if (enabled() && registry.Find("agent")) return std::unexpected(Fail("sdk.tool.duplicate", "agent"));
    std::set<std::string> all;
    for (auto& profile : snapshot_.profiles) {
        for (const auto& name : profile.requested.tools) {
            const auto* tool = registry.Find(name);
            const auto* facts = registry.RegistrationOf(name);
            if (!tool || !facts) return std::unexpected(Fail("sdk.subagent.tool_missing", name));
            if (profile.requested.type != "Explore" || (ReadOnly(facts->effect_class) && ExploreName(name)))
                profile.effective_tools.push_back(name);
            all.insert(name);
        }
        if (profile.effective_tools.empty()) return std::unexpected(Fail("sdk.subagent.empty_tool_surface", profile.requested.type));
    }
    for (const auto& name : all) {
        const auto* tool = registry.Find(name); const auto* facts = registry.RegistrationOf(name);
        const auto schema = tool->input_schema().dump();
        if (schema.size() > kMaxPlanBytes) return std::unexpected(Fail("sdk.subagent.invalid_plan", "selected tool schema exceeds 64 KiB"));
        tool_facts_.push_back({{"name", name}, {"sourceKind", static_cast<int>(facts->source_kind)},
            {"sourceInstance", facts->source_instance}, {"effectClass", static_cast<int>(facts->effect_class)},
            {"versionOrDigest", facts->version_or_digest}, {"schemaSha256", lubancode::platform::Sha256Hex(schema)}});
    }
    const auto candidate = Plan(resume_id_).dump();
    if (candidate.size() > kMaxPlanBytes) return std::unexpected(Fail("sdk.subagent.invalid_plan", "plan exceeds 64 KiB"));
    if (!resume_id_.empty() && !legacy_ && candidate != saved_bytes_)
        return std::unexpected(Fail("sdk.subagent.resume_mismatch", "selected/discovered tool face changed"));
    bound_ = true;
    return {};
}
Result<void> SessionSubagentPlan::CheckBinding(const v3::V3Ledger& ledger) const {
    if (ledger.session_id != resume_id_) return std::unexpected(Fail("sdk.subagent.plan_invalid", "journal identity differs"));
    if (legacy_) {
        for (const auto& message : ledger.messages)
            if (Binding(message)) return std::unexpected(Fail("sdk.subagent.plan_invalid", "journal binds a missing child plan"));
        return {};
    }
    const Json expected{{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}};
    const auto matches = [&](const v3::MessageLine* message) {
        const auto* binding = message ? Binding(*message) : nullptr;
        if (!binding || *binding != expected || message->message.value("role", Json()) != "system" ||
            !message->message.contains("content") || !message->message["content"].is_string()) return false;
        if (message->system_meta->contains("settingsVersion")) {
            const auto& version = message->system_meta->at("settingsVersion");
            if ((!version.is_number_unsigned() && !version.is_number_integer()) ||
                (version.is_number_integer() && !version.is_number_unsigned() && version.get<std::int64_t>() < 1) ||
                (version.is_number_unsigned() && version.get<std::uint64_t>() == 0)) return false;
        }
        return true;
    };
    if (ledger.messages.empty() || ledger.messages.front().seq != 1 || !matches(&ledger.messages.front()))
        return std::unexpected(Fail("sdk.subagent.plan_invalid", "initial system binding is missing or changed"));
    bool adopted = false;
    for (const auto& event : ledger.events) {
        if (event.kind != v3::EventKindV3::SessionStarted) continue;
        const auto context = event.payload.find("context");
        if (context != event.payload.end() && context->is_object()) {
            const auto chain = context->find("contextChain");
            adopted = chain != context->end() && chain->is_array() && !chain->empty() &&
                chain->front().value("messageRef", Json()) == ledger.messages.front().message_id;
        }
        break;
    }
    if (!adopted) return std::unexpected(Fail("sdk.subagent.plan_invalid", "initial system was not adopted"));
    for (const auto& [revision, chain] : ledger.revision_chains) {
        (void)revision;
        if (!matches(ledger.FindMessage(chain.first))) return std::unexpected(Fail("sdk.subagent.plan_invalid", "adopted system lost its plan binding"));
    }
    if (!matches(ledger.FindMessage(ledger.context.system_message_ref)))
        return std::unexpected(Fail("sdk.subagent.plan_invalid", "effective system lost its plan binding"));
    return {};
}
Result<void> SessionSubagentPlan::CheckRecoveredChildren(const v3::V3Ledger& ledger) const {
    if (legacy_ || !saved_plan_.value("enabled", false)) return {};
    const auto valid_operations = ValidateOperationLedger(resume_dir_);
    if (!valid_operations) return valid_operations;
    // A durable final which claims a known outcome must already have a complete
    // adoption chain. Check before Continue/system transfer, then repeat under
    // the existing opening lock. Dispatched/indeterminate crash windows remain
    // recoverable gaps; they cannot acquire a manufactured successful report.
    std::set<std::string> turns;
    for (const auto& fact : lubancode::runtime::SessionService::ReadOperationFacts(resume_dir_)) {
        if (fact.kind != "operation.final") continue;
        if (fact.execution_status != "success" && fact.execution_status != "error" &&
            fact.execution_status != "cancelled" && fact.execution_status != "interrupted")
            return std::unexpected(Fail("sdk.subagent.report_invalid", "SDK final has an invalid execution status"));
        if (fact.execution_status == "interrupted") continue;
        if (!fact.turn_id.empty() && !turns.insert(fact.turn_id).second)
            return std::unexpected(Fail("sdk.subagent.report_invalid", "turn belongs to multiple accepted operations"));
        const auto invalid_result = []() -> Result<void> {
            return std::unexpected(Fail("sdk.subagent.report_invalid", "known final has no matching owned SDK result"));
        };
        const auto results = resume_dir_ / "sdk-results";
        const auto result_path = results / tools::Utf8ToPath(fact.operation_id + ".json");
        std::error_code error;
        const auto result_dir_status = fs::symlink_status(results, error);
        if (error || fs::is_symlink(result_dir_status) || !fs::is_directory(result_dir_status)) return invalid_result();
        if (fs::canonical(results, error) != results || error) return invalid_result();
        const auto result_status = fs::symlink_status(result_path, error);
        if (error || fs::is_symlink(result_status) || !fs::is_regular_file(result_status)) return invalid_result();
        if (fs::canonical(result_path, error) != result_path || error) return invalid_result();
        auto bytes = lubancode::platform::ReadBoundedRegularFile(result_path, kMaxRecoveryResultBytes);
        if (!bytes) return invalid_result();
        const auto result = Json::parse(*bytes, nullptr, false);
        if (!result.is_object() || result.value("operationId", Json()) != fact.operation_id ||
            result.value("turnId", Json()) != fact.turn_id || result.value("complete", Json()) != true ||
            !result.contains("error") || !result["error"].is_string() ||
            !result.contains("finalText") || !result["finalText"].is_string()) return invalid_result();
        const auto same_turn = [&](const auto& row) {
            return row.session_id == resume_id_ && row.run_id == ledger.run_id && row.turn_id == fact.turn_id;
        };
        const bool has_turn = !fact.turn_id.empty() &&
            (std::any_of(ledger.messages.begin(), ledger.messages.end(), same_turn) ||
             std::any_of(ledger.events.begin(), ledger.events.end(), same_turn));
        // Failure before RecordInput can have a reserved turn ID but no V3 turn
        // row. A successful final or actual final-message reference cannot.
        if (!has_turn && (fact.execution_status == "success" || !fact.final_message_refs.empty()))
            return std::unexpected(Fail("sdk.subagent.report_invalid", "known final has no owned V3 turn source"));
        for (const auto& ref : fact.final_message_refs) {
            const auto* message = ledger.FindMessage(ref);
            if (!message || !same_turn(*message) || message->message.value("role", Json()) != "assistant")
                return std::unexpected(Fail("sdk.subagent.report_invalid", "final message belongs to another V3 turn"));
        }
        const bool allow_unconsumed = fact.execution_status == "cancelled" ||
            (fact.execution_status == "error" && result["error"] == "sdk.turn.limit_reached");
        std::optional<OperationUsage> usage;
        if (const auto saved = result.find("usage"); saved != result.end()) {
            auto decoded = usage_result::Decode(*saved);
            if (!decoded) return std::unexpected(Fail("sdk.usage.result_invalid", std::string(decoded.error())));
            usage = std::move(*decoded);
            std::set<std::string> requests;
            for (const auto& record : usage->attempts) {
                if (record.subordinate) continue;
                if (record.source_session_id != resume_id_ || record.source_run_id != ledger.run_id ||
                    record.turn_id != fact.turn_id || !requests.insert(record.trajectory_request_id).second)
                    return std::unexpected(Fail("sdk.usage.result_invalid", "direct usage owner differs before opening"));
                const auto bound = usage_result::ValidateRequestBinding(record, ledger);
                if (!bound) return std::unexpected(Fail("sdk.usage.result_invalid", std::string(bound.error())));
            }
        }
        // Both the prepare read and locked opening repeat this check before any
        // old system transfer. Rejecting usage must leave the parent untouched.
        auto reports = ReadSubagentReports(ledger, resume_dir_, resume_id_, fact.operation_id, fact.turn_id,
            true, allow_unconsumed, usage ? &*usage : nullptr);
        if (!reports) return std::unexpected(reports.error());
    }
    return {};
}
std::expected<SessionSubagentPlan::Json, std::string> SessionSubagentPlan::Open(
    const lubancode::trajectory::V3OpeningContext& context) {
    const auto bad = [](const Error& e) -> std::expected<Json, std::string> { return std::unexpected(e.code + ": " + e.message); };
    if (!bound_) return std::unexpected("sdk.subagent.open_failed: selected tool surface is not frozen");
    auto bytes = ReadPlan(root_, context.session_dir);
    if (!bytes) return bad(bytes.error());
    if (context.source) {
        if (context.session_id != resume_id_ || context.session_dir != resume_dir_)
            return std::unexpected("sdk.subagent.resume_mismatch: opening identity changed");
        if (legacy_ ? bytes->has_value() : !bytes->has_value() || **bytes != saved_bytes_)
            return std::unexpected("sdk.subagent.plan_invalid: plan changed before locked adoption");
        auto checked = CheckBinding(*context.source);
        if (!checked) return bad(checked.error());
        checked = CheckRecoveredChildren(*context.source);
        if (!checked) return bad(checked.error());
    } else {
        if (!resume_id_.empty() || bytes->has_value()) return std::unexpected("sdk.subagent.plan_invalid: unexpected new/resume plan state");
        const auto plan = Plan(context.session_id).dump();
        auto written = WriteFrozenPlan(context.session_dir / kPlanFile, plan);
        if (!written) {
            const auto& error = written.error();
            return std::unexpected("sdk.subagent.plan_write_failed: " + Json{
                {"atomicCode", error.code},
                {"failureKind", error.failure_kind == lubancode::platform::WriteFailureKind::TransientReject
                    ? "TransientReject" : "Permanent"},
                {"outcome", PlanWriteOutcomeName(error.outcome)}, {"message", error.message}}
                .dump(-1, ' ', false, Json::error_handler_t::replace));
        }
        if (written->outcome != lubancode::platform::WriteOutcome::CommittedDurable)
            return std::unexpected("sdk.subagent.plan_write_failed: " + Json{
                {"outcome", PlanWriteOutcomeName(written->outcome)},
                {"message", "plan receipt was not CommittedDurable"}}
                .dump(-1, ' ', false, Json::error_handler_t::replace));
        snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(plan);
    }
    snapshot_.session_id = context.session_id;
    return legacy_ ? Json::object() : Json{{"hostBindings", {{"sdkSubagents", {{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}}}}}};
}
lubancode::trajectory::V3OpeningParticipant SessionSubagentPlan::OpeningParticipant() {
    return [owner = shared_from_this()](const auto& context) { return owner->Open(context); };
}

Result<std::unique_ptr<SessionSubagents>> SessionSubagents::Build(
    std::shared_ptr<SessionSubagentPlan> plan, lubancode::api::Backend& backend,
    tools::ToolRegistry& registry, lubancode::agent::AgentProfile profile) {
    auto result = std::unique_ptr<SessionSubagents>(new SessionSubagents);
    result->plan_ = std::move(plan); result->parent_profile_ = std::move(profile);
    result->general_ = std::make_unique<tools::ToolRegistry>(); result->explore_ = std::make_unique<tools::ToolRegistry>();
    for (const auto& face : result->plan_->Describe().profiles) {
        auto& overlay = face.requested.type == "Explore" ? *result->explore_ : *result->general_;
        for (const auto& name : face.effective_tools) {
            auto* original = registry.Find(name); const auto* registration = registry.RegistrationOf(name);
            if (!original || !registration) return std::unexpected(Fail("sdk.subagent.tool_missing", name));
            tools::ToolRegistration copied;
            copied.tool = std::make_unique<BorrowedTool>(*original);
            copied.source_kind = registration->source_kind; copied.source_instance = registration->source_instance;
            copied.effect_class = registration->effect_class; copied.idempotency = registration->idempotency;
            copied.recovery = registration->recovery; copied.version_or_digest = registration->version_or_digest;
            copied.package_origin = registration->package_origin;
            overlay.Register(std::move(copied));
        }
    }
    result->agent_ = std::make_unique<tools::AgentTool>(backend, *result->general_, result->plan_->Describe().cwd);
    result->agent_->SetExploreRegistry(result->explore_.get());
    result->agent_->SetDispatchGovernance(1, 1);
    result->agent_->SetBackgroundByDefault(false);
    const auto faces = result->plan_->Describe().profiles;
    result->agent_->SetAgentTypesProvider([faces] {
        std::vector<tools::AgentTypeInfo> types;
        for (const auto& face : faces) types.push_back({face.requested.type, "Explicit SDK foreground profile"});
        return types;
    });
    return result;
}
SessionSubagents::~SessionSubagents() { RequestClose(); agent_.reset(); }
std::unique_ptr<tools::Tool> SessionSubagents::BuildTool() { return std::make_unique<DispatchTool>(*this); }
void SessionSubagents::BindTurn(tools::AgentTool::Hooks hooks, std::string session_id,
    std::string operation_id, std::string turn_id) {
    session_id_ = std::move(session_id); operation_id_ = std::move(operation_id); turn_id_ = std::move(turn_id);
    agent_->SetHooks(std::move(hooks));
}
void SessionSubagents::ClearTurn() noexcept {
    try { agent_->SetHooks({}); session_id_.clear(); operation_id_.clear(); turn_id_.clear(); }
    catch (...) { RequestClose(); }
}
void SessionSubagents::RequestClose() noexcept {
    closing_.store(true);
    if (agent_) agent_->coordinator()->RequestClose();
}
Json SessionSubagents::InputSchema() const {
    Json types = Json::array();
    for (const auto& p : plan_->Describe().profiles) types.push_back(p.requested.type);
    return Json{{"type", "object"}, {"additionalProperties", false}, {"required", {"title", "prompt", "agent_type"}},
        {"properties", {{"title", {{"type", "string"}}}, {"prompt", {{"type", "string"}}},
            {"agent_type", {{"type", "string"}, {"enum", std::move(types)}}},
            {"execution_mode", {{"type", "string"}, {"enum", {"foreground", "auto"}}}},
            {"isolation", {{"type", "string"}, {"enum", {"none"}}}},
            {"max_steps_per_turn", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<int>::max()}}},
            {"max_time_secs", {{"type", "integer"}, {"minimum", 1}, {"maximum", std::numeric_limits<int>::max()}}},
            {"model", {{"type", "string"}, {"description", "May only equal this type's frozen effective model."}}}}}};
}
tools::Tool::Result SessionSubagents::Execute(const Json& input, const tools::ToolExecutionContext& context) {
    const auto refuse = [](const std::string& why) { return tools::Tool::Result::Error("sdk.subagent.dispatch_rejected: " + why); };
    const auto* caller = tools::CurrentDispatchIdentity();
    if (closing_.load() || (context.cancel && context.cancel->load())) return refuse("session closing or invocation cancelled");
    if (!caller || !caller->IsMain() || caller->depth != 0 || caller->agent_run_id.empty() ||
        session_id_.empty() || operation_id_.empty() || turn_id_.empty() ||
        context.invocation.session_id != session_id_ || context.invocation.operation_id != operation_id_ ||
        context.invocation.turn_id != turn_id_ || context.invocation.action_id.empty() || context.invocation.attempt == 0)
        return refuse("actual main operation ownership is required");
    if (!input.is_object()) return refuse("input must be an object");
    const std::set<std::string> keys{"title", "prompt", "agent_type", "execution_mode", "isolation", "max_steps_per_turn", "max_time_secs", "model"};
    for (auto i = input.begin(); i != input.end(); ++i) if (!keys.contains(i.key())) return refuse("unsupported input field");
    for (const auto* key : {"title", "prompt", "agent_type"})
        if (!input.contains(key) || !input[key].is_string() || !Text(input[key].get<std::string>(), key == std::string("prompt") ? kMaxPlanBytes : 256))
            return refuse("title/prompt/type must be bounded UTF-8 text");
    if (input.contains("execution_mode") && (!input["execution_mode"].is_string() ||
        (input["execution_mode"] != "foreground" && input["execution_mode"] != "auto"))) return refuse("only foreground or resolved foreground auto is supported");
    if (input.contains("isolation") && input["isolation"] != "none") return refuse("worktree isolation is not admitted");
    const auto snapshot = plan_->Describe();
    const auto found = std::find_if(snapshot.profiles.begin(), snapshot.profiles.end(),
        [&](const auto& p) { return p.requested.type == input["agent_type"].get<std::string>(); });
    if (found == snapshot.profiles.end()) return refuse("type is outside the frozen host plan");
    Json admitted = input;
    for (const auto& [key, cap] : std::array<std::pair<const char*, std::int64_t>, 2>{{
        {"max_steps_per_turn", found->requested.max_steps_per_turn}, {"max_time_secs", found->requested.max_wall_seconds}}}) {
        if (input.contains(key) && (!StepOrSecond(input[key]) || input[key].get<std::int64_t>() > cap))
            return refuse("invocation budget must be positive and may only narrow the frozen host cap");
        admitted[key] = input.contains(key) ? input[key] : Json(cap);
    }
    if (input.contains("model") && (!input["model"].is_string() || input["model"] != found->effective_model))
        return refuse("invocation model must preserve the frozen binding");
    admitted.erase("model"); admitted["execution_mode"] = "foreground"; admitted["isolation"] = "none";
    auto profile = parent_profile_;
    profile.request.model = found->effective_model;
    profile.runtime.max_steps_per_turn = static_cast<int>(found->requested.max_steps_per_turn);
    profile.runtime.max_wall_secs = static_cast<int>(found->requested.max_wall_seconds);
    agent_->SetAgentProfile(std::move(profile));
    agent_->SetContextWindowTokens(parent_profile_.runtime.context_window_tokens);
    agent_->SetWallClockTimeout(admitted["max_time_secs"].get<int>());
    return agent_->execute(admitted, context);
}

} // namespace lubancore::detail
