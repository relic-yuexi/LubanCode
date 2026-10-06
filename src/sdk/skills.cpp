#include "sdk/skills.hpp"
#include "sdk/prepare_journal.hpp"
#include "sdk/plan_write.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>

#include "platform/atomic_write.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "tools/path_utils.hpp"
#include "tools/skill_tool.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "trajectory/v3/reader.hpp"
#include "workspace/index.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
constexpr std::size_t kMaxPlanBytes = 256 * 1024;
constexpr const char* kPlanFile = "sdk-skills-plan.json";

Error Fail(std::string code, std::string message = {}) {
    return {std::move(code), std::move(message)};
}
bool Inside(const fs::path& root, const fs::path& path) {
    auto r = root.begin();
    auto p = path.begin();
    for (; r != root.end(); ++r, ++p) {
        if (p == path.end() || *r != *p) return false;
    }
    return p != path.end();
}
Result<void> OwnedDirectory(const fs::path& root, const fs::path& directory) {
    const auto normalized = directory.lexically_normal();
    if (!Inside(root, normalized)) return std::unexpected(Fail("sdk.skill.plan_invalid", "session directory escapes owned root"));
    std::error_code ec;
    auto cursor = root;
    for (const auto& part : normalized.lexically_relative(root)) {
        cursor /= part;
        const auto status = fs::symlink_status(cursor, ec);
        if (ec || fs::is_symlink(status) || !fs::is_directory(status))
            return std::unexpected(Fail("sdk.skill.plan_invalid", "session directory is missing, linked or unreadable"));
    }
    const auto real = fs::canonical(normalized, ec);
    if (ec || real != normalized) return std::unexpected(Fail("sdk.skill.plan_invalid", "session directory mapping changed"));
    return {};
}
Result<std::optional<std::string>> ReadPlan(const fs::path& root, const fs::path& directory) {
    auto owned = OwnedDirectory(root, directory);
    if (!owned) return std::unexpected(owned.error());
    const auto path = directory / kPlanFile;
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        return std::optional<std::string>{};
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        return std::unexpected(Fail("sdk.skill.plan_invalid", "plan is linked, nonregular or unreadable"));
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::unexpected(Fail("sdk.skill.plan_invalid", "cannot open plan"));
    std::string bytes;
    std::array<char, 4096> buffer{};
    while (file) {
        const auto remaining = kMaxPlanBytes + 1 - bytes.size();
        file.read(buffer.data(), static_cast<std::streamsize>(std::min(buffer.size(), remaining)));
        bytes.append(buffer.data(), static_cast<std::size_t>(file.gcount()));
        if (bytes.size() > kMaxPlanBytes) return std::unexpected(Fail("sdk.skill.plan_invalid", "plan exceeds 256 KiB"));
    }
    if (file.bad() || (file.fail() && !file.eof()) || !lubancode::platform::IsValidUtf8(bytes) || bytes.find('\0') != std::string::npos)
        return std::unexpected(Fail("sdk.skill.plan_invalid", "plan read failed or has invalid text"));
    return std::optional<std::string>{std::move(bytes)};
}
Json Declaration(Json plan) {
    if (!plan.is_object()) return nullptr;
    plan.erase("toolNames");
    const auto found = plan.find("entries");
    if (found == plan.end() || !found->is_array()) return nullptr;
    for (auto& entry : *found) {
        if (!entry.is_object()) return nullptr;
        entry.erase("missingTools");
    }
    return plan;
}
const Json* SkillsBinding(const v3::MessageLine& message) {
    if (!message.system_meta || !message.system_meta->is_object()) return nullptr;
    const auto hosts = message.system_meta->find("hostBindings");
    if (hosts == message.system_meta->end() || !hosts->is_object()) return nullptr;
    const auto skill = hosts->find("skills");
    return skill == hosts->end() ? nullptr : &*skill;
}
} // namespace

Result<std::shared_ptr<SessionSkills>> SessionSkills::Prepare(
    const std::optional<skills::v1::Selection>& selection, fs::path owned_root,
    std::string workspace_key, std::string resume_id, std::string user_system, std::shared_ptr<SessionPrepareJournal> journal) {
    auto result = std::shared_ptr<SessionSkills>(new SessionSkills);
    result->owned_root_ = std::move(owned_root);
    result->resume_id_ = std::move(resume_id);
    result->user_system_ = std::move(user_system);
    result->snapshot_.enabled = selection.has_value();
    if (selection) {
        const auto& root = selection->root;
        if (root.empty() || root.find('\0') != std::string::npos || !lubancode::platform::IsValidUtf8(root) ||
            !lubancode::tools::Utf8ToPath(root).is_absolute())
            return std::unexpected(Fail("sdk.skill.invalid_selection", "root must be an absolute UTF-8 directory"));
    }
    // Inspect the saved plan and journal before scanning or creating any MCP/backend.
    if (!result->resume_id_.empty()) {
        const auto workspace_dir = lubancode::workspace::index::ResolveDirByWorkspaceKey(
            result->owned_root_ / "workspaces", workspace_key);
        if (!workspace_dir) return std::unexpected(Fail("sdk.session.open_failed", "resume session is unavailable"));
        result->expected_resume_dir_ = *workspace_dir / "sessions" / result->resume_id_;
        std::error_code ec;
        const auto session_status = fs::symlink_status(result->expected_resume_dir_, ec);
        if (ec == std::errc::no_such_file_or_directory ||
            (!ec && session_status.type() == fs::file_type::not_found))
            return std::unexpected(Fail("sdk.session.open_failed", "resume session is unavailable"));
        // Existing linked/unreadable directories and one-sided plans still pass
        // through strict validation; absence must keep the established SDK code.
        auto bytes = ReadPlan(result->owned_root_, result->expected_resume_dir_);
        if (!bytes) return std::unexpected(bytes.error());
        if (*bytes) {
            result->saved_bytes_ = **bytes;
            result->saved_plan_ = Json::parse(result->saved_bytes_, nullptr, false);
            const auto& plan = result->saved_plan_;
            if (!plan.is_object() || plan.value("schemaVersion", Json()) != 1 ||
                plan.value("sessionId", Json()) != result->resume_id_ ||
                !plan.contains("enabled") || !plan["enabled"].is_boolean() || plan.dump() != result->saved_bytes_)
                return std::unexpected(Fail("sdk.skill.plan_invalid", "invalid or noncanonical saved plan"));
            if (plan["enabled"] != selection.has_value())
                return std::unexpected(Fail("sdk.skill.resume_mismatch", "resume must preserve explicit Skills admission"));
            result->snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(result->saved_bytes_);
        } else {
            if (selection) return std::unexpected(Fail("sdk.skill.resume_mismatch", "legacy session did not admit Skills"));
            result->legacy_ = true;
        }
        auto stream = v3::FindV3SessionStream(result->expected_resume_dir_);
        if (!stream) return std::unexpected(Fail("sdk.skill.plan_invalid", "saved V3 journal is unavailable"));
        auto ledger = ReadPrepareJournal(*stream, journal.get());
        if (!ledger) return std::unexpected(Fail("sdk.skill.plan_invalid", ledger.error()));
        auto bound = result->CheckBinding(**ledger);
        if (!bound) return std::unexpected(bound.error());
    }
    if (selection) {
        auto scan = lubancode::tools::ScanSkillsDirStrict(lubancode::tools::Utf8ToPath(selection->root),
            selection->names, "技能仅来自宿主显式目录；未列名不加载。");
        if (!scan) return std::unexpected(Fail("sdk.skill.open_failed", scan.error().code + ": " + scan.error().message));
        result->scan_ = std::move(*scan);
        result->snapshot_.root = lubancode::tools::PathToUtf8(result->scan_->read_policy.canonical_root_path);
        result->snapshot_.prompt_segment = result->scan_->prompt_segment;
        for (const auto& meta : result->scan_->skills)
            result->snapshot_.entries.push_back({meta.name, meta.description, meta.dir_path, meta.content_hash, meta.requires_tools, {}});
    }
    if (!result->resume_id_.empty() && !result->legacy_) {
        auto same = result->CompareSaved(false);
        if (!same) return std::unexpected(same.error());
    }
    return result;
}

SessionSkills::Json SessionSkills::Plan(const std::string& id) const {
    Json entries = Json::array();
    if (scan_) {
        for (std::size_t i = 0; i < scan_->skills.size(); ++i) {
            const auto& meta = scan_->skills[i];
            entries.push_back(Json{{"name", meta.name}, {"description", meta.description},
                {"directory", meta.dir_path}, {"sourceDirectory", meta.source_dir_path}, {"skillPath", meta.skill_path},
                {"contentSha256", meta.content_hash}, {"requiresTools", meta.requires_tools},
                {"missingTools", snapshot_.entries[i].missing_tools}});
        }
    }
    return Json{{"schemaVersion", 1}, {"sessionId", id}, {"enabled", snapshot_.enabled},
        {"sourceRoot", scan_ ? lubancode::tools::PathToUtf8(scan_->read_policy.source_root_path) : ""},
        {"canonicalRoot", snapshot_.root}, {"entries", std::move(entries)},
        {"promptSegment", snapshot_.prompt_segment}, {"promptSha256", lubancode::platform::Sha256Hex(snapshot_.prompt_segment)},
        {"toolNames", snapshot_.tool_names}};
}
Result<void> SessionSkills::CompareSaved(bool include_tools) const {
    const auto candidate = Plan(resume_id_);
    if ((include_tools ? candidate : Declaration(candidate)) !=
        (include_tools ? saved_plan_ : Declaration(saved_plan_)))
        return std::unexpected(Fail("sdk.skill.resume_mismatch", "selection, fingerprint, prompt or tool surface changed"));
    return {};
}
Result<void> SessionSkills::BindToolSurface(std::set<std::string> names) {
    if (surface_bound_) return std::unexpected(Fail("sdk.skill.open_failed", "tool surface already frozen"));
    if (enabled()) {
        if (!names.insert("skill").second) return std::unexpected(Fail("sdk.tool.duplicate", "skill"));
        tool_names_ = std::move(names);
        snapshot_.tool_names.assign(tool_names_.begin(), tool_names_.end());
        for (auto& entry : snapshot_.entries)
            for (const auto& required : entry.requires_tools)
                if (!tool_names_.contains(required)) entry.missing_tools.push_back(required);
    }
    if (!resume_id_.empty() && !legacy_) {
        auto same = CompareSaved(true);
        if (!same) return same;
    }
    if (Plan(resume_id_).dump().size() > kMaxPlanBytes)
        return std::unexpected(Fail("sdk.skill.plan_invalid", "plan exceeds 256 KiB"));
    surface_bound_ = true;
    return {};
}
std::unique_ptr<lubancode::tools::Tool> SessionSkills::BuildTool() const {
    if (!scan_ || !surface_bound_) return {};
    return std::make_unique<lubancode::tools::SkillTool>(scan_->skills, tool_names_, scan_->read_policy);
}
std::string SessionSkills::EffectiveSystem() const {
    // Empty resume means preserve the complete saved root, not append Skills again.
    if (!resume_id_.empty() && user_system_.empty()) return {};
    if (snapshot_.prompt_segment.empty()) return user_system_;
    return user_system_.empty() ? snapshot_.prompt_segment : user_system_ + "\n\n" + snapshot_.prompt_segment;
}
Result<void> SessionSkills::CheckBinding(const v3::V3Ledger& ledger) const {
    if (ledger.session_id != resume_id_) return std::unexpected(Fail("sdk.skill.plan_invalid", "journal identity mismatch"));
    const auto* effective = ledger.FindMessage(ledger.context.system_message_ref);
    if (!effective || !effective->system_meta || !effective->system_meta->is_object() ||
        effective->message.value("role", Json()) != "system" ||
        !effective->message.contains("content") || !effective->message["content"].is_string())
        return std::unexpected(Fail("sdk.skill.plan_invalid", "effective system is not a text root"));
    if (effective->system_meta->contains("settingsVersion")) {
        const auto& version = effective->system_meta->at("settingsVersion");
        if ((!version.is_number_unsigned() && !version.is_number_integer()) ||
            (version.is_number_integer() && !version.is_number_unsigned() && version.get<std::int64_t>() < 1) ||
            (version.is_number_unsigned() && version.get<std::uint64_t>() == 0))
            return std::unexpected(Fail("sdk.skill.plan_invalid", "effective system settings version is invalid"));
    }
    const Json expected{{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}};
    if (legacy_) {
        for (const auto& message : ledger.messages)
            if (SkillsBinding(message)) return std::unexpected(Fail("sdk.skill.plan_invalid", "journal binds a missing Skills plan"));
        return {};
    }
    if (ledger.messages.empty() || ledger.messages.front().seq != 1)
        return std::unexpected(Fail("sdk.skill.plan_invalid", "initial system is unavailable"));
    const auto matches = [&](const v3::MessageLine* message) {
        const auto* binding = message ? SkillsBinding(*message) : nullptr;
        return binding && *binding == expected;
    };
    if (!matches(&ledger.messages.front())) return std::unexpected(Fail("sdk.skill.plan_invalid", "initial system binding differs"));
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
    if (!adopted) return std::unexpected(Fail("sdk.skill.plan_invalid", "initial system was not adopted"));
    for (const auto& [revision, chain] : ledger.revision_chains) {
        (void)revision;
        if (!matches(ledger.FindMessage(chain.first)))
            return std::unexpected(Fail("sdk.skill.plan_invalid", "adopted system lost its Skills binding"));
    }
    if (!matches(ledger.FindMessage(ledger.context.system_message_ref)))
        return std::unexpected(Fail("sdk.skill.plan_invalid", "effective system lost its Skills binding"));
    return {};
}
std::expected<SessionSkills::Json, std::string> SessionSkills::Open(const lubancode::trajectory::V3OpeningContext& context) {
    const auto failure = [](const Error& error) -> std::expected<Json, std::string> {
        return std::unexpected(error.code + ": " + error.message);
    };
    if (!surface_bound_) return std::unexpected("sdk.skill.open_failed: tool surface is not frozen");
    auto owned = OwnedDirectory(owned_root_, context.session_dir);
    if (!owned) return failure(owned.error());
    if (context.source) {
        if (context.session_id != resume_id_ || context.session_dir != expected_resume_dir_)
            return std::unexpected("sdk.skill.resume_mismatch: opening identity changed");
        auto bytes = ReadPlan(owned_root_, context.session_dir);
        if (!bytes) return failure(bytes.error());
        if (legacy_ ? bytes->has_value() : !bytes->has_value() || **bytes != saved_bytes_)
            return std::unexpected("sdk.skill.plan_invalid: saved plan changed before locked adoption");
        auto binding = CheckBinding(*context.source);
        if (!binding) return failure(binding.error());
    } else {
        if (!resume_id_.empty()) return std::unexpected("sdk.skill.resume_mismatch: resume became a new session");
        auto existing = ReadPlan(owned_root_, context.session_dir);
        if (!existing) return failure(existing.error());
        if (existing->has_value()) return std::unexpected("sdk.skill.plan_invalid: new plan already exists");
        const auto bytes = Plan(context.session_id).dump();
        if (bytes.size() > kMaxPlanBytes) return std::unexpected("sdk.skill.plan_invalid: plan exceeds 256 KiB");
        auto written = WriteFrozenPlan(context.session_dir / kPlanFile, bytes);
        if (!written) {
            const auto& error = written.error();
            return std::unexpected("sdk.skill.plan_write_failed: " + Json{
                {"atomicCode", error.code},
                {"failureKind", error.failure_kind == lubancode::platform::WriteFailureKind::TransientReject
                    ? "TransientReject" : "Permanent"},
                {"outcome", PlanWriteOutcomeName(error.outcome)}, {"message", error.message}}
                .dump(-1, ' ', false, Json::error_handler_t::replace));
        }
        if (written->outcome != lubancode::platform::WriteOutcome::CommittedDurable)
            return std::unexpected("sdk.skill.plan_write_failed: " + Json{
                {"outcome", PlanWriteOutcomeName(written->outcome)},
                {"message", "plan receipt was not CommittedDurable"}}
                .dump(-1, ' ', false, Json::error_handler_t::replace));
        snapshot_.plan_sha256 = lubancode::platform::Sha256Hex(bytes);
    }
    snapshot_.session_id = context.session_id;
    return legacy_ ? Json::object() : Json{{"hostBindings", {{"skills", {{"schemaVersion", 1}, {"sha256", snapshot_.plan_sha256}}}}}};
}
lubancode::trajectory::V3OpeningParticipant SessionSkills::OpeningParticipant() {
    return [owner = shared_from_this()](const auto& context) { return owner->Open(context); };
}
} // namespace lubancore::detail
