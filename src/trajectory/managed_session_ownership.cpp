#include "trajectory/managed_session_ownership.hpp"

#include <initializer_list>
#include <system_error>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include "platform/bounded_read.hpp"
#include "platform/paths.hpp"
#include "platform/secure_file.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::trajectory {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Capture = ManagedSessionOwnershipCapture;
using Publication = ManagedSessionOwnershipPublication;

bool OpaqueId(const std::string& value) {
    if (value.empty() || value.size() > 512 || !platform::IsValidUtf8(value)) return false;
    for (const unsigned char c : value) if (c < 0x20 || c == 0x7f) return false;
    return true;
}
bool ValidOwnership(const ManagedSessionOwnership& value) {
    return OpaqueId(value.tenant_id) && OpaqueId(value.project_id) && OpaqueId(value.workspace_key) &&
        OpaqueId(value.session_id) && value.binding_version != 0;
}
Json OwnershipJson(const ManagedSessionOwnership& value) {
    return {{"schemaVersion", 1}, {"mode", "Managed"}, {"tenantId", value.tenant_id},
        {"projectId", value.project_id}, {"workspaceKey", value.workspace_key},
        {"sessionId", value.session_id}, {"bindingVersion", value.binding_version}};
}
std::expected<ManagedSessionOwnership, std::string> Parse(const std::string& bytes) {
    if (bytes.size() > kManagedSessionOwnershipMaxBytes || !platform::IsValidUtf8(bytes))
        return std::unexpected("managed.ownership.invalid_metadata");
    const auto value = Json::parse(bytes, nullptr, false);
    if (!value.is_object() || value.size() != 7 || !value.contains("schemaVersion") ||
        !value["schemaVersion"].is_number_integer() || value["schemaVersion"] != 1 ||
        !value.contains("mode") || !value["mode"].is_string() || value["mode"] != "Managed" ||
        !value.contains("bindingVersion") || !value["bindingVersion"].is_number_unsigned())
        return std::unexpected("managed.ownership.invalid_metadata");
    for (const auto* key : {"tenantId", "projectId", "workspaceKey", "sessionId"})
        if (!value.contains(key) || !value[key].is_string())
            return std::unexpected("managed.ownership.invalid_metadata");
    ManagedSessionOwnership ownership{value["tenantId"].get<std::string>(), value["projectId"].get<std::string>(),
        value["workspaceKey"].get<std::string>(), value["sessionId"].get<std::string>(),
        value["bindingVersion"].get<std::uint64_t>()};
    if (!ValidOwnership(ownership) || OwnershipJson(ownership).dump() != bytes)
        return std::unexpected("managed.ownership.invalid_metadata");
    return ownership;
}
std::expected<fs::path, std::string> Directory(const fs::path& declared) {
    if (declared.empty() || !declared.is_absolute()) return std::unexpected("managed.ownership.invalid_directory");
    const auto text = platform::PathToUtf8(declared);
    if (text.find('\0') != std::string::npos || !platform::IsValidUtf8(text))
        return std::unexpected("managed.ownership.invalid_directory");
    for (const auto& component : declared)
        if (component == "." || component == "..") return std::unexpected("managed.ownership.invalid_directory");
    auto dir = declared;
    while (dir != dir.root_path() && dir.filename().empty()) dir = dir.parent_path();
    if (dir == dir.root_path()) return std::unexpected("managed.ownership.invalid_directory");
    const auto native = platform::FileIoPath(dir);
    if (!platform::RejectReparsePoint(native)) return std::unexpected("managed.ownership.directory_is_link");
    std::error_code ec;
    const auto status = fs::symlink_status(native, ec);
    if (ec || fs::is_symlink(status) || !fs::is_directory(status))
        return std::unexpected("managed.ownership.invalid_directory");
    return dir;
}
std::expected<void, std::string> LockPair(const fs::path& dir, const SessionLock& lock) {
    if (!lock.holds()) return std::unexpected("managed.ownership.lock_required");
    if (!lock.path().is_absolute() || lock.path().filename() != "session.lock")
        return std::unexpected("managed.ownership.lock_mismatch");
    std::error_code ec;
    if (!fs::equivalent(platform::FileIoPath(dir), platform::FileIoPath(lock.path().parent_path()), ec) || ec)
        return std::unexpected("managed.ownership.lock_mismatch");
    const auto native = platform::FileIoPath(lock.path());
    if (!platform::RejectReparsePoint(native)) return std::unexpected("managed.ownership.lock_mismatch");
    const auto status = fs::symlink_status(native, ec);
    if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
        return std::unexpected("managed.ownership.lock_mismatch");
    return {};
}
std::expected<void, std::string> NewDirectory(const fs::path& dir) {
    std::error_code ec;
    fs::directory_iterator current(platform::FileIoPath(dir), ec), end;
    if (ec) return std::unexpected("managed.ownership.directory_read_failed");
    for (; current != end; current.increment(ec)) {
        if (ec) return std::unexpected("managed.ownership.directory_read_failed");
        if (current->path().filename() != "session.lock")
            return std::unexpected("managed.ownership.legacy_directory");
    }
    if (ec) return std::unexpected("managed.ownership.directory_read_failed");
    return {};
}
std::expected<void, std::string> CaptureShape(const Capture& capture) {
    if (capture.session_dir.empty() || !capture.session_dir.is_absolute())
        return std::unexpected("managed.ownership.invalid_capture");
    if (capture.state == Capture::State::Absent) {
        if (!capture.bytes.empty() || capture.ownership) return std::unexpected("managed.ownership.invalid_capture");
        return {};
    }
    if (capture.state != Capture::State::Marked || !capture.ownership)
        return std::unexpected("managed.ownership.invalid_capture");
    const auto parsed = Parse(capture.bytes);
    if (!parsed || *parsed != *capture.ownership || parsed->session_id != platform::PathToUtf8(capture.session_dir.filename()))
        return std::unexpected("managed.ownership.invalid_capture");
    return {};
}
}

std::expected<Capture, std::string> CaptureManagedSessionOwnership(const fs::path& session_dir) {
    try {
        auto dir = Directory(session_dir);
        if (!dir) return std::unexpected(dir.error());
        Capture capture;
        capture.session_dir = std::move(*dir);
        const auto path = platform::FileIoPath(capture.session_dir / kManagedSessionOwnershipFile);
        if (!platform::RejectReparsePoint(path)) return std::unexpected("managed.ownership.metadata_is_link");
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory || (!ec && status.type() == fs::file_type::not_found)) return capture;
        if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
            return std::unexpected("managed.ownership.invalid_file");
        auto bytes = platform::ReadBoundedRegularFile(path, kManagedSessionOwnershipMaxBytes);
        if (!bytes) return std::unexpected("managed.ownership." + bytes.error());
        auto ownership = Parse(*bytes);
        if (!ownership) return std::unexpected(ownership.error());
        if (ownership->session_id != platform::PathToUtf8(capture.session_dir.filename()))
            return std::unexpected("managed.ownership.directory_mismatch");
        capture.state = Capture::State::Marked;
        capture.bytes = std::move(*bytes);
        capture.ownership = std::move(*ownership);
        return capture;
    } catch (...) {
        return std::unexpected("managed.ownership.capture_failed");
    }
}

std::expected<Capture, std::string> CaptureManagedSessionOwnershipLocked(const fs::path& session_dir, const SessionLock& lock) {
    try {
        auto dir = Directory(session_dir);
        if (!dir) return std::unexpected(dir.error());
        auto paired = LockPair(*dir, lock);
        if (!paired) return std::unexpected(paired.error());
        return CaptureManagedSessionOwnership(*dir);
    } catch (...) { return std::unexpected("managed.ownership.capture_failed"); }
}
std::expected<void, std::string> CheckLocalTrustedSessionOwnership(const Capture& capture) {
    auto valid = CaptureShape(capture);
    if (!valid) return valid;
    if (capture.state != Capture::State::Absent) return std::unexpected("managed.ownership.local_trusted_rejected");
    return {};
}
std::expected<void, std::string> CheckManagedSessionOwnership(const Capture& capture, const ManagedSessionOwnership& expected) {
    if (!ValidOwnership(expected)) return std::unexpected("managed.ownership.invalid_expected");
    auto valid = CaptureShape(capture);
    if (!valid) return valid;
    if (capture.state == Capture::State::Absent) return std::unexpected("managed.ownership.required");
    if (*capture.ownership != expected) return std::unexpected("managed.ownership.mismatch");
    return {};
}
std::expected<void, std::string> CheckManagedSessionOwnershipCaptureUnchanged(const Capture& before, const Capture& locked) {
    auto valid = CaptureShape(before);
    if (!valid) return valid;
    valid = CaptureShape(locked);
    if (!valid) return valid;
    if (before.session_dir != locked.session_dir || before.state != locked.state || before.bytes != locked.bytes ||
        before.ownership != locked.ownership) return std::unexpected("managed.ownership.source_changed");
    return {};
}

ManagedSessionOwnershipAttempt::ManagedSessionOwnershipAttempt(Capture capture, ManagedSessionOwnership expected, std::string bytes)
    : capture_(std::move(capture)), expected_(std::move(expected)), bytes_(std::move(bytes)) {}
std::expected<std::unique_ptr<ManagedSessionOwnershipAttempt>, std::string> ManagedSessionOwnershipAttempt::Prepare(
    const fs::path& session_dir, const SessionLock& lock, ManagedSessionOwnership expected) {
    try {
        if (!ValidOwnership(expected)) return std::unexpected("managed.ownership.invalid_expected");
        auto capture = CaptureManagedSessionOwnershipLocked(session_dir, lock);
        if (!capture) return std::unexpected(capture.error());
        if (expected.session_id != platform::PathToUtf8(capture->session_dir.filename()))
            return std::unexpected("managed.ownership.directory_mismatch");
        if (capture->state == Capture::State::Marked) {
            auto checked = CheckManagedSessionOwnership(*capture, expected);
            if (!checked) return std::unexpected(checked.error());
        } else {
            auto empty = NewDirectory(capture->session_dir);
            if (!empty) return std::unexpected(empty.error());
        }
        auto bytes = OwnershipJson(expected).dump();
        if (bytes.size() > kManagedSessionOwnershipMaxBytes) return std::unexpected("managed.ownership.invalid_expected");
        return std::unique_ptr<ManagedSessionOwnershipAttempt>(new ManagedSessionOwnershipAttempt(
            std::move(*capture), std::move(expected), std::move(bytes)));
    } catch (...) { return std::unexpected("managed.ownership.prepare_failed"); }
}

Publication ManagedSessionOwnershipAttempt::Publish(const SessionLock& lock) {
    if (first_) return *first_;
    // Allocate the complete fallback before the only possible write. It survives
    // a thrown native call or result publication/copy; no later read upgrades it.
    Publication fallback;
    fallback.expected = expected_;
    fallback.captured_bytes = capture_.bytes;
    fallback.publication_bytes = bytes_;
    fallback.error_code = "managed.ownership.publish_failed";
    std::string unconfirmed_code = "managed.ownership.publish_unconfirmed";
    static_assert(std::is_nothrow_move_constructible_v<Publication>);
    first_.emplace(std::move(fallback));
    auto& out = *first_;
    try {
        auto current = CaptureManagedSessionOwnershipLocked(capture_.session_dir, lock);
        if (!current) { out.error_code = current.error(); return out; }
        auto same = CheckManagedSessionOwnershipCaptureUnchanged(capture_, *current);
        if (!same) { out.error_code = same.error(); return out; }
        if (current->state == Capture::State::Marked) {
            auto checked = CheckManagedSessionOwnership(*current, expected_);
            if (!checked) { out.error_code = checked.error(); return out; }
            out.knowledge = Publication::Knowledge::ExistingMatching;
            out.error_code.clear();
            return out;
        }
        auto empty = NewDirectory(current->session_dir);
        if (!empty) { out.error_code = empty.error(); return out; }
        // Path/string preparation may allocate. Complete it before recording the
        // actual call boundary so a preparation failure never claims native IO.
        const auto target = current->session_dir / kManagedSessionOwnershipFile;
        out.knowledge = Publication::Knowledge::Unconfirmed;
        out.error_code = "managed.ownership.publish_unconfirmed";
        out.requested = platform::WriteDurability::ProcessCrashDurability;
        out.native = platform::AtomicWriteFile(target, bytes_, *out.requested);
        // Exactly one native call; no retry/readback.
        if (!*out.native) {
            const auto& error = out.native->error();
            out.knowledge = error.outcome == platform::WriteOutcome::NotCommitted
                ? Publication::Knowledge::NotCommitted : Publication::Knowledge::Unconfirmed;
            out.error_code = error.code;
            out.message = error.message;
        } else if (out.native->value().outcome == platform::WriteOutcome::CommittedDurable) {
            out.knowledge = Publication::Knowledge::Committed;
            out.error_code.clear();
        }
        return out;
    } catch (...) {
        // Requested is set only at the actual call boundary. A thrown call may
        // have changed the target; keep Unconfirmed and any already saved receipt.
        out.knowledge = out.requested ? Publication::Knowledge::Unconfirmed
                                     : Publication::Knowledge::RejectedBeforeIO;
        if (out.requested) out.error_code.swap(unconfirmed_code);
        return out;
    }
}

} // namespace lubancode::trajectory
