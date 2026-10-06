#include "trajectory/named_result_blobs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <exception>
#include <limits>
#include <set>
#include <system_error>
#include <type_traits>
#include <utility>

#include "platform/bounded_read.hpp"
#include "platform/paths.hpp"
#include "platform/secure_file.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"

namespace lubancode::trajectory {
namespace {
namespace fs = std::filesystem;
thread_local bool in_provider = false;
CasError Error(std::string code, std::string message = {}) { return {std::move(code), std::move(message)}; }
bool Text(std::string_view value) {
    return !value.empty() && value.find_first_of("\r\n") == std::string_view::npos &&
        value.find('\0') == std::string_view::npos && platform::IsValidUtf8(std::string(value));
}
bool Name(std::string_view value) {
    return Text(value) && value != "." && value != ".." &&
        value.find_first_of("/\\:") == std::string_view::npos;
}
bool Hash(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
bool BoundedError(const CasError& error) {
    return error.code.size() <= 200 && error.message.size() <= 4096 &&
        (error.code.empty() || Text(error.code)) && platform::IsValidUtf8(error.message) &&
        error.message.find('\0') == std::string::npos;
}
bool SafePath(const fs::path& path) {
    for (auto part = path; !part.empty();) {
        if (!platform::RejectReparsePoint(part)) return false;
        const auto parent = part.parent_path();
        if (parent == part) break;
        part = parent;
    }
    return true;
}
bool Confirms(CasDurability actual, CasDurability requested) {
    const auto rank = [](CasDurability value) {
        switch (value) { case CasDurability::Buffered: return 0; case CasDurability::ProcessCrash: return 1;
                        case CasDurability::PowerLoss: return 2; }
        return -1;
    };
    return rank(actual) >= 0 && rank(requested) >= 0 && rank(actual) >= rank(requested);
}
// Preserve the legacy File Open scan: one current entry, one prefix high-water,
// caller's directory cap, and the original stoull exception boundary. This path
// does not materialize/copy a complete namespace for the generic provider API.
std::expected<std::uint64_t, CasError> ScanFileNext(const fs::path& session, const std::string& prefix,
    std::size_t max_directory_entries) {
    const auto artifacts = session / "artifacts";
    const auto native_artifacts = platform::FileIoPath(artifacts);
    std::error_code ec; fs::create_directories(native_artifacts, ec);
    if (ec) return std::unexpected(Error("named_result.directory_unreadable", "结果仓建目录失败: " + artifacts.string()));
    std::uint64_t next = 1;
    std::size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(native_artifacts, ec)) {
        if (max_directory_entries && ++entries > max_directory_entries)
            return std::unexpected(Error("named_result.directory_limit", "result directory entry limit exceeded"));
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) != 0) continue;
        const auto dot = name.find('.');
        const auto number = name.substr(prefix.size(), dot == std::string::npos ? std::string::npos : dot - prefix.size());
        if (number.empty() || !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        const std::uint64_t value = std::stoull(number);
        if (value >= next) next = value + 1;
    }
    if (max_directory_entries && ec) return std::unexpected(Error("named_result.directory_unreadable", "result directory read failed"));
    return next;
}
class FileStore final : public NamedResultStore {
public:
    FileStore(CasScope scope, fs::path root) : scope_(std::move(scope)), root_(std::move(root)) {}
    NamedResultWriteReceipt PublishNew(const NamedResultWriteRequest& request, bool& invoked) override {
        NamedResultWriteReceipt result;
        result.reference = request.reference; result.request_key = request.request_key;
        result.error = Error("named_result.file_publish_failed");
        const auto path = root_ / platform::Utf8ToPath(request.reference.logical_name);
        invoked = true;
        result.native.emplace(platform::CreateImmutableFileDetailed(path, request.bytes,
            platform::WriteDurability::ProcessCrashDurability));
        const auto& native = *result.native;
        result.ancestors_confirmed = native.ancestor_chain_confirmed;
        if (native.outcome != platform::WriteOutcome::NotCommitted) result.state = CasCommitState::Committed;
        if (native.outcome == platform::WriteOutcome::CommittedDurable) {
            result.confirmed_durability = CasDurability::ProcessCrash;
            if (native.ok()) result.error = {};
        }
        return result;
    }
    std::expected<std::string, CasError> Read(const NamedResultReference& reference, std::size_t cap) override {
        const auto path = root_ / platform::Utf8ToPath(reference.logical_name);
        if (!SafePath(path)) return std::unexpected(Error("named_result.path_rejected"));
        std::error_code error;
        const auto status = fs::symlink_status(platform::FileIoPath(path), error);
        if (error == std::errc::no_such_file_or_directory || (!error && status.type() == fs::file_type::not_found))
            return std::unexpected(Error("named_result.missing"));
        if (error || !fs::is_regular_file(status)) return std::unexpected(Error("named_result.unreadable"));
        auto data = platform::ReadBoundedRegularFile(path, cap);
        if (!data || !SafePath(path)) return std::unexpected(Error("named_result.unreadable"));
        return std::move(*data);
    }
    std::expected<NamedResultNames, CasError> SnapshotNames(NamedResultListLimits limits) override {
        NamedResultNames result; result.scope = scope_; result.binding_id = "file-v1";
        std::error_code error;
        // Match the legacy lazy ResultStore::Open boundary, including its cap.
        fs::create_directories(platform::FileIoPath(root_), error);
        if (error) return std::unexpected(Error("named_result.directory_unreadable"));
        std::size_t used = 0;
        for (const auto& entry : fs::directory_iterator(platform::FileIoPath(root_), error)) {
            if (error) break;
            const auto name = platform::PathToUtf8(entry.path().filename());
            if ((limits.entries && result.names.size() >= limits.entries) ||
                (limits.name_bytes && name.size() > limits.name_bytes) ||
                (limits.total_name_bytes && (used > limits.total_name_bytes || name.size() > limits.total_name_bytes - used)))
                return std::unexpected(Error("named_result.directory_limit"));
            used += name.size(); result.names.push_back(name);
        }
        if (error) return std::unexpected(Error("named_result.directory_unreadable"));
        std::sort(result.names.begin(), result.names.end());
        std::string encoded;
        for (const auto& name : result.names) { encoded += std::to_string(name.size()); encoded += ':'; encoded += name; }
        result.token = platform::Sha256Hex(encoded); result.complete = true;
        return result;
    }
private:
    CasScope scope_;
    fs::path root_;
};
} // namespace

bool InNamedResultProvider() noexcept { return in_provider; }
NamedResultProviderScope::NamedResultProviderScope() noexcept : previous_(in_provider) { in_provider = true; }
NamedResultProviderScope::~NamedResultProviderScope() { in_provider = previous_; }

NamedResultCapability::NamedResultCapability(CasScope scope, std::string binding,
    std::shared_ptr<NamedResultStore> store, fs::path directory)
    : scope_(std::move(scope)), binding_id_(std::move(binding)), store_(std::move(store)),
      file_session_directory_(std::move(directory)) {}

std::string NamedResultCapability::DisplayPath(std::string_view artifact_path) const {
    if (!external()) return platform::PathToUtf8(
        (file_session_directory_ / platform::Utf8ToPath(std::string(artifact_path))).lexically_normal());
    return "host-result://" + scope_.session_id + "/" + std::string(artifact_path);
}

std::expected<NamedResultMaterial, CasError> NamedResultCapability::BeginMaterial(std::string prefix,
    std::size_t directory_cap, bool refresh_names) {
    if (InNamedResultProvider()) return std::unexpected(Error("named_result.reentrant"));
    std::unique_lock writing(write_mutex_);
    if (writes_closed_) return std::unexpected(Error("named_result.owner_closed"));
    if (first_unknown_) return std::unexpected(Error("named_result.publication_unconfirmed"));
    if (!store_ || (!prefix.empty() && (prefix.size() > 32 ||
        prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)))
        return std::unexpected(Error("named_result.invalid_prefix"));
    if (!external()) {
        std::uint64_t next = 0;
        if (!prefix.empty()) {
            auto found = next_.find(prefix);
            if (refresh_names || found == next_.end()) {
                auto scanned = ScanFileNext(file_session_directory_, prefix, directory_cap);
                if (!scanned) return std::unexpected(scanned.error());
                if (found == next_.end()) found = next_.emplace(prefix, *scanned).first;
                else found->second = (std::max)(found->second, *scanned);
            }
            next = found->second;
        }
        return NamedResultMaterial(shared_from_this(), std::move(writing), std::move(prefix), next);
    }
    try {
        NamedResultListLimits limits;
        if (directory_cap) limits.entries = (std::min)(directory_cap, limits.entries);
        std::optional<NamedResultNames> fresh;
        if (!names_ || refresh_names) {
            std::lock_guard serial(store_mutex_); NamedResultProviderScope callback;
            auto listed = store_->SnapshotNames(limits);
            if (!listed) return std::unexpected(BoundedError(listed.error()) ? listed.error() : Error("named_result.invalid_inventory_error"));
            fresh.emplace(std::move(*listed));
        }
        const auto& names = fresh ? fresh : names_;
        if (names->scope != scope_ || names->binding_id != binding_id_ || !names->complete || !Text(names->token) || names->token.size() > 200 ||
            (limits.entries && names->names.size() > limits.entries))
            return std::unexpected(Error("named_result.snapshot_invalid"));
        std::set<std::string> seen;
        std::uint64_t next = 1;
        std::size_t total = 0;
        for (const auto& name : names->names) {
            if (!Name(name) || !seen.insert(name).second || (limits.name_bytes && name.size() > limits.name_bytes) ||
                (limits.total_name_bytes && (total > limits.total_name_bytes || name.size() > limits.total_name_bytes - total)))
                return std::unexpected(Error("named_result.snapshot_invalid"));
            total += name.size();
            if (prefix.empty() || !name.starts_with(prefix)) continue;
            const auto dot = name.find('.');
            const auto number = std::string_view(name).substr(prefix.size(),
                dot == std::string::npos ? std::string::npos : dot - prefix.size());
            if (number.empty() || number.find_first_not_of("0123456789") != std::string_view::npos) continue;
            std::uint64_t value = 0;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
            if (parsed.ec != std::errc{} || value == (std::numeric_limits<std::uint64_t>::max)())
                return std::unexpected(Error("named_result.number_overflow"));
            next = (std::max)(next, value + 1);
        }
        if (!prefix.empty()) {
            auto [entry, inserted] = next_.try_emplace(prefix, next);
            (void)inserted; entry->second = (std::max)(entry->second, next); next = entry->second;
        }
        if (fresh) names_ = std::move(fresh); // never retain an unvalidated host inventory
        return NamedResultMaterial(shared_from_this(), std::move(writing), std::move(prefix), next);
    } catch (...) { return std::unexpected(Error("named_result.snapshot_failed")); }
}

NamedResultMaterial::NamedResultMaterial(std::shared_ptr<NamedResultCapability> owner,
    std::unique_lock<std::mutex> writing, std::string prefix, std::uint64_t number)
    : owner_(std::move(owner)), write_lock_(std::move(writing)), prefix_(std::move(prefix)), number_(number) {}

NamedResultWriteReceipt NamedResultMaterial::Publish(const std::string& name, std::string_view bytes,
    std::string media_type, bool& invoked) {
    invoked = false;
    NamedResultWriteReceipt fallback;
    fallback.reference = {owner_->scope_, owner_->binding_id_, name, platform::Sha256Hex(bytes),
        static_cast<std::uint64_t>(bytes.size()), std::move(media_type)};
    fallback.request_key = name + ":" + fallback.reference.sha256;
    fallback.error = Error("named_result.publication_unconfirmed");
    if (!Name(name) || owner_->writes_closed_ || owner_->first_unknown_) return fallback;
    try {
        const NamedResultWriteRequest request{fallback.reference, fallback.request_key, bytes, CasDurability::ProcessCrash};
        std::lock_guard serial(owner_->store_mutex_); NamedResultProviderScope callback;
        auto receipt = owner_->store_->PublishNew(request, invoked);
        if (owner_->external()) {
            const std::array<std::string_view, 9> fields{receipt.reference.scope.workspace_key,
                receipt.reference.scope.session_id, receipt.reference.binding_id, receipt.reference.logical_name,
                receipt.reference.sha256, receipt.reference.media_type, receipt.request_key,
                receipt.error.code, receipt.error.message};
            constexpr std::array<std::size_t, 9> limits{200, 200, 200, 1024, 64, 200, 1200, 200, 4096};
            bool oversized = false;
            for (std::size_t i = 0; i < fields.size(); ++i) oversized |= fields[i].size() > limits[i];
            if (oversized) {
                fallback.state = invoked ? CasCommitState::Indeterminate : CasCommitState::NotCommitted;
                fallback.native = std::move(receipt.native);
                auto& claim = fallback.oversize_claim.emplace();
                claim.state = static_cast<int>(receipt.state); claim.bytes = receipt.reference.bytes;
                claim.ancestors_confirmed = receipt.ancestors_confirmed;
                if (receipt.confirmed_durability) claim.durability = static_cast<int>(*receipt.confirmed_durability);
                for (std::size_t i = 0; i < fields.size(); ++i) claim.field_bytes[i] = fields[i].size();
                platform::Sha256Stream hash;
                hash.Update("named-result-receipt-v1:");
                hash.Update(std::to_string(claim.state) + ":" + std::to_string(claim.bytes) + ":" +
                    (claim.durability ? std::to_string(*claim.durability) : "absent") + ":" +
                    (claim.ancestors_confirmed ? "true:" : "false:"));
                for (const auto field : fields) { hash.Update(std::to_string(field.size()) + ":"); hash.Update(field); }
                claim.sha256 = hash.FinalHex(); claim.fingerprint_complete = true;
                return fallback;
            }
        }
        if (receipt.reference != request.reference || receipt.request_key != request.request_key ||
            (receipt.state != CasCommitState::NotCommitted && receipt.state != CasCommitState::Committed &&
             receipt.state != CasCommitState::Indeterminate) ||
            (!receipt.error.code.empty() && !Text(receipt.error.code)) ||
            !platform::IsValidUtf8(receipt.error.message) || receipt.error.message.find('\0') != std::string::npos ||
            (receipt.state == CasCommitState::NotCommitted && receipt.confirmed_durability) ||
            (receipt.confirmed_durability && !Confirms(*receipt.confirmed_durability, CasDurability::Buffered)) ||
            (receipt.confirmed_durability == CasDurability::PowerLoss && !receipt.ancestors_confirmed) ||
            (!invoked && receipt.state != CasCommitState::NotCommitted)) {
            // Retain the provider's actual owned reply, including contradictory
            // identity/state. The material owner classifies an invoked invalid
            // reply as unknown rather than rewriting it into a fake receipt.
            return receipt;
        }
        receipt.validated = true;
        return receipt;
    } catch (...) {
        fallback.state = invoked ? CasCommitState::Indeterminate : CasCommitState::NotCommitted;
        return fallback;
    }
}
void NamedResultMaterial::Commit() {
    if (!prefix_.empty()) owner_->next_.at(prefix_) = number_ + 1;
}
void NamedResultMaterial::PreserveUnknown(const std::shared_ptr<NamedPublication>& publication) noexcept {
    if (!owner_->first_unknown_ && publication && publication->knowledge == NamedPublicationKnowledge::Indeterminate)
        owner_->first_unknown_ = publication;
}

std::expected<std::string, CasError> NamedResultCapability::Read(std::string_view artifact_path,
    std::string hash, std::uint64_t bytes, std::string media, std::size_t cap) const {
    if (InNamedResultProvider()) return std::unexpected(Error("named_result.reentrant"));
    if (!artifact_path.starts_with("artifacts/") || !Name(artifact_path.substr(10)) || !Hash(hash) ||
        !Text(media) || bytes > cap || cap == (std::numeric_limits<std::size_t>::max)())
        return std::unexpected(Error("named_result.invalid_read"));
    try {
        NamedResultReference reference{scope_, binding_id_, std::string(artifact_path.substr(10)),
            std::move(hash), bytes, std::move(media)};
        std::lock_guard serial(store_mutex_); NamedResultProviderScope callback;
        auto data = store_->Read(reference, cap);
        if (!data) return std::unexpected(!external() || BoundedError(data.error()) ? data.error() : Error("named_result.invalid_read_error"));
        if (data->size() > cap || data->size() != bytes || platform::Sha256Hex(*data) != reference.sha256)
            return std::unexpected(Error("named_result.read_mismatch"));
        return std::move(*data);
    } catch (...) { return std::unexpected(Error("named_result.read_failed")); }
}
std::optional<NamedPublication> NamedResultCapability::FirstUnconfirmedPublication() const {
    if (InNamedResultProvider()) return std::nullopt;
    std::lock_guard lock(write_mutex_);
    return first_unknown_ ? std::optional(*first_unknown_) : std::nullopt;
}
void NamedResultCapability::CloseWrites() noexcept {
    try { std::lock_guard lock(write_mutex_); writes_closed_ = true; }
    catch (...) { std::terminate(); }
}

std::expected<NamedResultLease, CasError> OpenNamedResultCapability(const CasScope& scope,
    const fs::path& directory, const std::shared_ptr<NamedResultFactory>& factory, bool resume) {
    if (InNamedResultProvider() || !Text(scope.workspace_key) || !Text(scope.session_id))
        return std::unexpected(Error("named_result.invalid_open"));
    try {
        std::shared_ptr<NamedResultStore> store;
        const auto binding = factory ? factory->binding_id() : "file-v1";
        if (!Text(binding) || binding.size() > 200 || (factory && binding == "file-v1"))
            return std::unexpected(Error("named_result.invalid_binding"));
        if (factory) {
            NamedResultProviderScope callback;
            auto opened = factory->Open(scope, resume);
            if (!opened) return std::unexpected(BoundedError(opened.error()) ? opened.error() : Error("named_result.invalid_open_error"));
            store = std::move(*opened);
        } else {
            const auto root = directory / "artifacts";
            store = std::make_shared<FileStore>(scope, root);
        }
        if (!store) return std::unexpected(Error("named_result.empty_store"));
        auto capability = std::make_shared<NamedResultCapability>(scope, binding, std::move(store), factory ? fs::path{} : directory);
        // External domains validate inventory before admitting a writer. File
        // keeps the old lazy, prefix-aware and caller-bounded Open boundary.
        if (factory) { auto checked = capability->BeginMaterial({}); if (!checked) return std::unexpected(checked.error()); }
        return NamedResultLease(std::move(capability));
    } catch (...) { return std::unexpected(Error("named_result.open_failed")); }
}

} // namespace lubancode::trajectory
