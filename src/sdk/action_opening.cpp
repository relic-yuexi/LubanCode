#include "sdk/action_opening.hpp"
#include "sdk/prepare_journal.hpp"

#include <algorithm>
#include <system_error>

#include "platform/atomic_write.hpp"
#include "platform/bounded_read.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "sdk/extensions.hpp"
#include "trajectory/v3/session_switch.hpp"
#include "workspace/index.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
using Json = nlohmann::json;
constexpr std::size_t kPlanBytes = 4 * 1024 * 1024;
constexpr const char* kPlanFile = "sdk-extension-plan.json";
Error Invalid(std::string message) { return {"sdk.action.plan_invalid", std::move(message)}; }

Result<void> OwnedDirectory(const fs::path& root, const fs::path& directory) {
    auto r = root.begin(), p = directory.begin();
    for (; r != root.end(); ++r, ++p) if (p == directory.end() || *r != *p)
        return std::unexpected(Invalid("session directory escapes the owned root"));
    if (p == directory.end()) return std::unexpected(Invalid("session directory is the owned root"));
    auto cursor = root;
    std::error_code ec;
    for (const auto& part : directory.lexically_relative(root)) {
        cursor /= part;
        const auto status = fs::symlink_status(lubancode::platform::FileIoPath(cursor), ec);
        if (ec || fs::is_symlink(status) || !fs::is_directory(status))
            return std::unexpected(Invalid("session directory is missing, linked or unreadable"));
    }
    if (fs::canonical(directory, ec) != directory || ec)
        return std::unexpected(Invalid("session directory mapping changed"));
    return {};
}
Result<std::optional<std::string>> ReadPlan(const fs::path& root, const fs::path& directory) {
    auto owned = OwnedDirectory(root, directory);
    if (!owned) return std::unexpected(owned.error());
    const auto path = lubancode::platform::FileIoPath(directory / kPlanFile);
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found))
        return std::optional<std::string>{};
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        return std::unexpected(Invalid("extension plan is linked, nonregular or unreadable"));
    auto bytes = lubancode::platform::ReadBoundedRegularFile(path, kPlanBytes);
    if (!bytes) return std::unexpected(Invalid("cannot read bounded regular extension plan: " + bytes.error()));
    if (bytes->find('\0') != std::string::npos || !lubancode::platform::IsValidUtf8(*bytes))
        return std::unexpected(Invalid("extension plan contains invalid text"));
    return std::optional<std::string>{std::move(*bytes)};
}
const Json* Binding(const v3::MessageLine& message) {
    if (!message.system_meta || !message.system_meta->is_object()) return nullptr;
    const auto hosts = message.system_meta->find("hostBindings");
    if (hosts == message.system_meta->end() || !hosts->is_object()) return nullptr;
    const auto found = hosts->find("extensionAction");
    return found == hosts->end() ? nullptr : &*found;
}
bool DeclaresAction(const std::vector<extensions::v1::Registration>& registrations) {
    for (const auto& registration : registrations) for (const auto& handler : registration.manifest.handlers)
        if (handler.point == extensions::v1::Point::PreAction || handler.point == extensions::v1::Point::PostAction) return true;
    return false;
}
} // namespace

Result<std::shared_ptr<SessionActionOpening>> SessionActionOpening::Prepare(
    const std::vector<extensions::v1::Registration>& registrations, fs::path root,
    const std::string& workspace_key, std::string resume_id, std::shared_ptr<SessionPrepareJournal> journal) {
    auto owner = std::shared_ptr<SessionActionOpening>(new SessionActionOpening);
    owner->owned_root_ = std::move(root);
    owner->resume_id_ = std::move(resume_id);
    owner->enabled_ = DeclaresAction(registrations);
    std::shared_ptr<const v3::V3Ledger> ledger;
    if (!owner->resume_id_.empty()) {
        auto workspace = lubancode::workspace::index::ResolveDirByWorkspaceKey(owner->owned_root_ / "workspaces", workspace_key);
        if (!workspace) {
            if (!owner->enabled_) return owner;
            return std::unexpected(Error{"sdk.session.open_failed", "resume session is unavailable"});
        }
        owner->resume_dir_ = *workspace / "sessions" / owner->resume_id_;
        auto saved = ReadPlan(owner->owned_root_, owner->resume_dir_);
        // Probe only: an old no-Action plan keeps its historical late rejection
        // behavior. A recognized persisted Action declaration stays strict even
        // when the caller attempts to remove every Action registration.
        if (saved && *saved) {
            auto parsed = Json::parse(**saved, nullptr, false);
            if (parsed.is_object() && parsed.contains("sdkActionVersion")) owner->enabled_ = true;
        }
        auto stream = v3::FindV3SessionStream(owner->resume_dir_);
        if (stream) {
            auto read = ReadPrepareJournal(*stream, journal.get());
            if (read) {
                ledger = std::move(*read);
                for (const auto& message : ledger->messages) if (Binding(message)) owner->enabled_ = true;
            } else if (owner->enabled_) return std::unexpected(Invalid(read.error()));
        } else if (owner->enabled_) return std::unexpected(Invalid("saved V3 journal is unavailable"));
        if (owner->enabled_) {
            if (!saved) return std::unexpected(saved.error());
            if (!*saved || !ledger) return std::unexpected(Invalid("Action plan or journal is missing"));
            owner->saved_bytes_ = **saved;
        }
    }
    if (!owner->enabled_) return owner;
    auto plan = SessionExtensions::DescribeDeclarations(registrations);
    if (!plan) return std::unexpected(plan.error());
    owner->plan_ = std::move(*plan);
    if (owner->plan_.size() > kPlanBytes) return std::unexpected(Invalid("extension plan exceeds 4 MiB"));
    const auto json = Json::parse(owner->plan_, nullptr, false);
    if (!json.is_object() || json.value("sdkActionVersion", Json()) != 1)
        return std::unexpected(Error{"sdk.extension.resume_mismatch", "resume must retain its admitted Action declarations"});
    owner->plan_hash_ = lubancode::platform::Sha256Hex(owner->plan_);
    if (!owner->resume_id_.empty()) {
        if (owner->saved_bytes_ != owner->plan_)
            return std::unexpected(Error{"sdk.extension.resume_mismatch", "frozen extension declarations differ before opening"});
        auto checked = owner->CheckBinding(*ledger);
        if (!checked) return std::unexpected(checked.error());
    }
    return owner;
}

Result<void> SessionActionOpening::CheckBinding(const v3::V3Ledger& ledger) const {
    const Json expected{{"schemaVersion", 1}, {"sha256", plan_hash_}};
    const auto matches = [&](const v3::MessageLine* message) {
        const auto* bound = message ? Binding(*message) : nullptr;
        return message && message->session_id == resume_id_ && message->message.value("role", Json()) == "system" && bound && *bound == expected;
    };
    if (ledger.session_id != resume_id_ || ledger.messages.empty() || ledger.messages.front().seq != 1 || !matches(&ledger.messages.front()))
        return std::unexpected(Invalid("initial system has no matching owned Action binding"));
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
    if (!adopted) return std::unexpected(Invalid("initial Action system binding was not adopted"));
    for (const auto& [revision, chain] : ledger.revision_chains) {
        (void)revision;
        if (!matches(ledger.FindMessage(chain.first))) return std::unexpected(Invalid("adopted system lost its Action binding"));
    }
    if (!matches(ledger.FindMessage(ledger.context.system_message_ref)))
        return std::unexpected(Invalid("effective system lost its Action binding"));
    return {};
}

std::expected<Json, std::string> SessionActionOpening::Open(const lubancode::trajectory::V3OpeningContext& context) {
    if (!enabled_) return Json::object();
    const auto fail = [](const Error& error) -> std::expected<Json, std::string> {
        return std::unexpected(error.code + ": " + error.message);
    };
    auto saved = ReadPlan(owned_root_, context.session_dir);
    if (!saved) return fail(saved.error());
    if (context.source) {
        if (context.session_id != resume_id_ || context.session_dir != resume_dir_ || !*saved || **saved != saved_bytes_)
            return std::unexpected("sdk.action.plan_invalid: extension plan changed before locked adoption");
        auto checked = CheckBinding(*context.source);
        if (!checked) return fail(checked.error());
    } else {
        if (!resume_id_.empty() || saved->has_value()) return std::unexpected("sdk.action.plan_invalid: new Action opening has preexisting metadata");
        auto committed = lubancode::platform::AtomicWriteFile(context.session_dir / kPlanFile, plan_,
            lubancode::platform::WriteDurability::ProcessCrashDurability);
        if (!committed) return std::unexpected("sdk.action.plan_write_failed: " + committed.error().message);
        if (committed->outcome != lubancode::platform::WriteOutcome::CommittedDurable)
            return std::unexpected("sdk.action.plan_write_failed: plan receipt was not CommittedDurable");
    }
    return Json{{"hostBindings", {{"extensionAction", {{"schemaVersion", 1}, {"sha256", plan_hash_}}}}}};
}

lubancode::trajectory::V3OpeningParticipant SessionActionOpening::OpeningParticipant() {
    return [owner = shared_from_this()](const auto& context) { return owner->Open(context); };
}
} // namespace lubancore::detail
