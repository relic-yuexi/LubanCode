#include "trajectory/cas_store.hpp"

#include <limits>
#include <exception>
#include <utility>

#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "trajectory/blob_store.hpp"
#include "trajectory/safety.hpp"

namespace lubancode::trajectory {
namespace {

bool Hash(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == value.npos;
}
bool Text(std::string_view value) {
    return !value.empty() && value.find('\0') == value.npos && platform::IsValidUtf8(std::string(value));
}
bool Scope(const CasScope& value) {
    return Text(value.workspace_key) && Text(value.session_id);
}
bool Durability(CasDurability value) {
    return value == CasDurability::Buffered || value == CasDurability::ProcessCrash || value == CasDurability::PowerLoss;
}
CasWriteReceipt Failed(CasCommitState state, CasReference reference,
                       std::string code, std::string message = {}) {
    return {state, std::move(reference), std::nullopt, {std::move(code), std::move(message)}};
}

class FileCasStore final : public CasStore {
public:
    FileCasStore(CasScope scope, std::filesystem::path root, FileCasFault fault)
        : scope_(std::move(scope)), root_(std::move(root)), blobs_(root_), fault_(std::move(fault)) {}
    CasWriteReceipt Store(const CasWriteRequest& request) override {
        if (request.reference.scope != scope_)
            return Failed(CasCommitState::NotCommitted, request.reference, "cas.scope_mismatch");
        if (!IsSafeContainedPath(root_, root_.parent_path()))
            return Failed(CasCommitState::NotCommitted, request.reference, "cas.path_escape");
        return blobs_.StoreDetailed(request, fault_);
    }
    std::expected<std::string, CasError> Read(const CasReference& reference, std::size_t cap) override {
        if (reference.scope != scope_) return std::unexpected(CasError{"cas.scope_mismatch", {}});
        if (!IsSafeContainedPath(root_, root_.parent_path())) return std::unexpected(CasError{"cas.path_escape", {}});
        return blobs_.ReadBoundedVerified(reference, cap);
    }
private:
    CasScope scope_;
    std::filesystem::path root_;
    BlobStore blobs_;
    FileCasFault fault_;
};

} // namespace

bool CasWriteReceipt::Confirms(CasDurability required) const noexcept {
    return Durability(required) && state == CasCommitState::Committed && confirmed_durability &&
           Durability(*confirmed_durability) &&
           static_cast<int>(*confirmed_durability) >= static_cast<int>(required) && error.code.empty();
}

MemoryCapability::MemoryCapability(CasScope scope, std::shared_ptr<CasStore> store)
    : scope_(std::move(scope)), store_(std::move(store)) {}

CasWriteReceipt MemoryCapability::Store(std::string_view bytes, std::string media_type,
                                       CasDurability required) {
    CasReference reference{scope_, platform::Sha256Hex(bytes),
        static_cast<std::uint64_t>(bytes.size()), std::move(media_type)};
    std::lock_guard lock(mutex_);
    if (writes_closed_)
        return Failed(CasCommitState::NotCommitted, reference, "cas.owner_closed");
    if (!store_ || !Scope(scope_) || !Text(reference.media_type) || !Hash(reference.sha256) || !Durability(required))
        return Failed(CasCommitState::NotCommitted, reference, "cas.invalid_request");
    try {
        auto receipt = store_->Store({reference, bytes, required});
        if (receipt.reference != reference ||
            (receipt.state != CasCommitState::NotCommitted && receipt.state != CasCommitState::Committed &&
             receipt.state != CasCommitState::Indeterminate) ||
            (receipt.confirmed_durability && !Durability(*receipt.confirmed_durability)))
            return Failed(CasCommitState::Indeterminate, reference, "cas.receipt_mismatch");
        if (receipt.state == CasCommitState::NotCommitted && receipt.confirmed_durability)
            return Failed(CasCommitState::Indeterminate, reference, "cas.receipt_mismatch");
        if (receipt.state == CasCommitState::Committed && !receipt.Confirms(required) && receipt.error.code.empty())
            receipt.error = {"cas.durability_unconfirmed", {}};
        return receipt;
    } catch (...) {
        // A provider may already have published before throwing. Never retry
        // it as a known rejection or fabricate a confirmation.
        return Failed(CasCommitState::Indeterminate, reference, "cas.provider_exception");
    }
}

std::expected<std::string, CasError> MemoryCapability::Read(
    const CasReference& reference, std::size_t cap) const {
    std::lock_guard lock(mutex_);
    if (!store_ || !Scope(scope_) || reference.scope != scope_)
        return std::unexpected(CasError{"cas.scope_mismatch", {}});
    if (!Hash(reference.sha256) || !Text(reference.media_type) || reference.bytes > cap ||
        cap == (std::numeric_limits<std::size_t>::max)())
        return std::unexpected(CasError{"cas.invalid_read", {}});
    try {
        auto bytes = store_->Read(reference, cap);
        if (!bytes) return std::unexpected(bytes.error());
        if (bytes->size() > cap || bytes->size() != reference.bytes ||
            platform::Sha256Hex(*bytes) != reference.sha256)
            return std::unexpected(CasError{"cas.read_mismatch", {}});
        return bytes;
    } catch (...) { return std::unexpected(CasError{"cas.provider_exception", {}}); }
}

void MemoryCapability::CloseWrites() noexcept {
    try { std::lock_guard lock(mutex_); writes_closed_ = true; }
    catch (...) { std::terminate(); } // An owner must not release an unsealed writer capability.
}

std::string MemoryCapability::LogicalReference(std::string_view hash) {
    if (!Hash(hash)) return {};
    return "artifacts/sha256/" + std::string(hash.substr(0, 2)) + "/" + std::string(hash);
}

MemoryCapabilityLease::MemoryCapabilityLease(std::shared_ptr<MemoryCapability> value)
    : value_(std::move(value)) {}
MemoryCapabilityLease::~MemoryCapabilityLease() { CloseWrites(); }
MemoryCapabilityLease::MemoryCapabilityLease(MemoryCapabilityLease&& other) noexcept
    : value_(std::move(other.value_)) {}
MemoryCapabilityLease& MemoryCapabilityLease::operator=(MemoryCapabilityLease&& other) noexcept {
    if (this != &other) { CloseWrites(); value_ = std::move(other.value_); }
    return *this;
}
void MemoryCapabilityLease::CloseWrites() noexcept { if (value_) value_->CloseWrites(); }

std::shared_ptr<CasStore> MakeFileCasStore(CasScope scope, std::filesystem::path root, FileCasFault fault) {
    return std::make_shared<FileCasStore>(std::move(scope), std::move(root), std::move(fault));
}

std::expected<MemoryCapabilityLease, CasError> OpenMemoryCapability(
    const CasScope& scope, const std::filesystem::path& root,
    const std::shared_ptr<MemoryCapabilityFactory>& factory) {
    if (!Scope(scope)) return std::unexpected(CasError{"cas.invalid_scope", {}});
    try {
        std::shared_ptr<CasStore> store;
        if (factory) {
            auto opened = factory->Open(scope);
            if (!opened) return std::unexpected(opened.error());
            store = std::move(*opened);
        } else store = MakeFileCasStore(scope, root);
        if (!store) return std::unexpected(CasError{"cas.open_failed", "provider returned no store"});
        return MemoryCapabilityLease(std::make_shared<MemoryCapability>(scope, std::move(store)));
    } catch (...) { return std::unexpected(CasError{"cas.open_failed", "provider factory failed"}); }
}

} // namespace lubancode::trajectory
