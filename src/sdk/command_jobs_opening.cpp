#include "sdk/command_jobs_opening.hpp"

#include <system_error>
#include <utility>

#include "platform/bounded_read.hpp"
#include "platform/paths.hpp"
#include "platform/process.hpp"
#include "platform/sha256.hpp"
#include "platform/secure_file.hpp"
#include "sdk/plan_write.hpp"
#include "trajectory/safety.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "workspace/index.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
constexpr const char* kPlanFile = "sdk-command-jobs-plan.json";
Error Invalid(std::string message) { return {"sdk.job.plan_invalid", std::move(message)}; }
Json Encode(const std::optional<jobs::v1::CommandOptions>& value, const std::string& cwd) {
    Json options = nullptr;
    if (value) options = {{"registration_timeout_ms", value->registration_timeout_ms},
        {"command_timeout_ms", value->command_timeout_ms}, {"max_output_bytes", value->max_output_bytes},
        {"max_running", value->max_running}, {"max_registered_jobs", value->max_registered_jobs}};
    return {{"schemaVersion", 1}, {"cwd", cwd}, {"command", std::move(options)}};
}
Result<std::optional<std::string>> ReadPlan(const fs::path& root, const fs::path& directory) {
    if (!directory.is_absolute() || !lubancode::trajectory::IsSafeContainedPath(directory, root))
        return std::unexpected(Invalid("session directory is outside its trusted root"));
    std::error_code error;
    if (!fs::is_directory(directory, error) || error)
        return std::unexpected(Invalid("session directory is unavailable"));
    const auto path = lubancode::platform::FileIoPath(directory / kPlanFile);
    if (!lubancode::platform::RejectReparsePoint(path)) return std::unexpected(Invalid("linked plan"));
    const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || (!error && status.type() == fs::file_type::not_found))
        return std::optional<std::string>{};
    if (error || !fs::is_regular_file(status)) return std::unexpected(Invalid("plan is not a regular file"));
    auto bytes = lubancode::platform::ReadBoundedRegularFile(path, 8192);
    if (!bytes) return std::unexpected(Invalid(bytes.error()));
    return std::optional<std::string>{std::move(*bytes)};
}
const Json* Binding(const v3::MessageLine& message) {
    if (!message.system_meta || !message.system_meta->is_object()) return nullptr;
    const auto hosts = message.system_meta->find("hostBindings");
    if (hosts == message.system_meta->end() || !hosts->is_object()) return nullptr;
    const auto found = hosts->find("commandJobs");
    return found == hosts->end() ? nullptr : &*found;
}
} // namespace

Result<void> ValidateCommandJobOptions(const jobs::v1::CommandOptions& value) {
    if (!value.registration_timeout_ms || value.registration_timeout_ms > 86400000 ||
        !value.command_timeout_ms || value.command_timeout_ms > value.registration_timeout_ms ||
        !value.max_output_bytes || value.max_output_bytes > lubancode::platform::kDefaultMaxOutputBytes ||
        !value.max_running || value.max_running > value.max_registered_jobs ||
        !value.max_registered_jobs || value.max_registered_jobs > 64)
        return std::unexpected(Error{"sdk.job.invalid_options", "explicit positive bounded command Job budgets are required"});
    return {};
}

Result<std::shared_ptr<SessionCommandJobPlan>> SessionCommandJobPlan::Prepare(
    const std::optional<jobs::v1::CommandOptions>& requested, fs::path root,
    std::string workspace_key, std::string resume_id, std::string cwd) {
    if (requested) { auto valid = ValidateCommandJobOptions(*requested); if (!valid) return std::unexpected(valid.error()); }
    auto owner = std::shared_ptr<SessionCommandJobPlan>(new SessionCommandJobPlan);
    owner->root_ = std::move(root); owner->resume_id_ = std::move(resume_id); owner->cwd_ = std::move(cwd);
    owner->options_ = requested;
    if (!owner->resume_id_.empty()) {
        auto workspace = lubancode::workspace::index::ResolveDirByWorkspaceKey(owner->root_ / "workspaces", workspace_key);
        if (!workspace) return std::unexpected(Error{"sdk.session.open_failed", "resume session is unavailable"});
        owner->resume_dir_ = *workspace / "sessions" / lubancode::platform::Utf8ToPath(owner->resume_id_);
        std::error_code error;
        const auto status = fs::symlink_status(owner->resume_dir_, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && status.type() == fs::file_type::not_found))
            return std::unexpected(Error{"sdk.session.open_failed", "resume session is unavailable"});
        // True absence keeps the established Session error. Existing linked,
        // non-directory or unreadable material still passes through strict Job validation.
        auto saved = ReadPlan(owner->root_, owner->resume_dir_);
        if (!saved) return std::unexpected(saved.error());
        if (!*saved) {
            if (requested) return std::unexpected(Invalid("old Session has no command Job plan"));
            owner->legacy_ = true;
        } else {
            try {
                const auto parsed = Json::parse(**saved);
                if (parsed.size() != 3 || parsed.at("schemaVersion") != 1 || parsed.at("cwd") != owner->cwd_)
                    return std::unexpected(Invalid("saved command Job plan does not match this Session"));
                std::optional<jobs::v1::CommandOptions> actual;
                const auto& command = parsed.at("command");
                if (!command.is_null()) {
                    if (!command.is_object() || command.size() != 5) return std::unexpected(Invalid("invalid Job budget object"));
                    for (const auto& field : {"registration_timeout_ms", "command_timeout_ms", "max_output_bytes", "max_running", "max_registered_jobs"})
                        if (!command.at(field).is_number_unsigned()) return std::unexpected(Invalid("Job budgets require positive integers"));
                    if (command.at("max_running").get<std::uint64_t>() > 64 ||
                        command.at("max_registered_jobs").get<std::uint64_t>() > 64)
                        return std::unexpected(Invalid("Job counts exceed the finite registration budget"));
                    actual = jobs::v1::CommandOptions{command.at("registration_timeout_ms").get<std::uint64_t>(),
                        command.at("command_timeout_ms").get<std::uint64_t>(), command.at("max_output_bytes").get<std::uint64_t>(),
                        command.at("max_running").get<std::size_t>(), command.at("max_registered_jobs").get<std::size_t>()};
                    auto valid = ValidateCommandJobOptions(*actual); if (!valid) return std::unexpected(valid.error());
                }
                if (Encode(actual, owner->cwd_).dump() != **saved || (requested && requested != actual))
                    return std::unexpected(Invalid("command Job declaration changed"));
                owner->options_ = actual; owner->saved_bytes_ = **saved;
            } catch (...) { return std::unexpected(Invalid("malformed command Job plan")); }
        }
        auto stream = v3::FindV3SessionStream(owner->resume_dir_);
        if (!stream) return std::unexpected(Invalid("saved V3 journal is unavailable"));
        auto ledger = v3::ReadV3Ledger(*stream);
        if (!ledger) return std::unexpected(Invalid(ledger.error()));
        owner->plan_ = Encode(owner->options_, owner->cwd_).dump();
        owner->hash_ = lubancode::platform::Sha256Hex(owner->plan_);
        auto bound = owner->CheckBinding(*ledger); if (!bound) return std::unexpected(bound.error());
    } else {
        owner->plan_ = Encode(requested, owner->cwd_).dump();
        owner->hash_ = lubancode::platform::Sha256Hex(owner->plan_);
    }
    return owner;
}

Result<void> SessionCommandJobPlan::CheckBinding(const v3::V3Ledger& ledger) const {
    if (legacy_) {
        for (const auto& message : ledger.messages) if (Binding(message))
            return std::unexpected(Invalid("missing plan for a declared Job binding"));
        return {};
    }
    const Json expected{{"schemaVersion", 1}, {"sha256", hash_}};
    const auto matches = [&](const v3::MessageLine* message) {
        const auto* binding = message ? Binding(*message) : nullptr;
        return message && message->session_id == resume_id_ && binding && *binding == expected;
    };
    if (ledger.session_id != resume_id_ || ledger.messages.empty() || ledger.messages.front().seq != 1 ||
        !matches(&ledger.messages.front())) return std::unexpected(Invalid("initial Job binding mismatch"));
    bool initial_adopted = false;
    for (const auto& event : ledger.events) {
        if (event.kind != v3::EventKindV3::SessionStarted) continue;
        const auto context = event.payload.find("context");
        if (context != event.payload.end() && context->is_object()) {
            const auto chain = context->find("contextChain");
            initial_adopted = chain != context->end() && chain->is_array() && !chain->empty() &&
                chain->front().value("messageRef", Json()) == ledger.messages.front().message_id;
        }
        break;
    }
    if (!initial_adopted) return std::unexpected(Invalid("initial Job binding was not adopted"));
    for (const auto& [revision, chain] : ledger.revision_chains) {
        (void)revision;
        if (!matches(ledger.FindMessage(chain.first))) return std::unexpected(Invalid("adopted system lost its Job binding"));
    }
    if (!matches(ledger.FindMessage(ledger.context.system_message_ref)))
        return std::unexpected(Invalid("effective system lost its Job binding"));
    return {};
}

std::expected<Json, std::string> SessionCommandJobPlan::Open(const lubancode::trajectory::V3OpeningContext& context) {
    auto saved = ReadPlan(root_, context.session_dir);
    if (!saved) return std::unexpected(saved.error().code + ": " + saved.error().message);
    if (context.source) {
        if (context.session_id != resume_id_ || context.session_dir != resume_dir_ ||
            (legacy_ ? saved->has_value() : (!*saved || **saved != saved_bytes_)))
            return std::unexpected("sdk.job.plan_changed_before_opening");
        auto checked = CheckBinding(*context.source);
        if (!checked) return std::unexpected(checked.error().code + ": " + checked.error().message);
    } else {
        if (!resume_id_.empty() || saved->has_value()) return std::unexpected("sdk.job.preexisting_plan");
        auto written = WriteFrozenPlan(context.session_dir / kPlanFile, plan_);
        if (!written || written->outcome != lubancode::platform::WriteOutcome::CommittedDurable)
            return std::unexpected("sdk.job.plan_write_failed");
    }
    if (legacy_) return Json::object();
    return Json{{"hostBindings", {{"commandJobs", {{"schemaVersion", 1}, {"sha256", hash_}}}}}};
}

lubancode::trajectory::V3OpeningParticipant SessionCommandJobPlan::OpeningParticipant() {
    return [owner = shared_from_this()](const auto& context) { return owner->Open(context); };
}
} // namespace lubancore::detail
