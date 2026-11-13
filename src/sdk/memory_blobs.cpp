#include "sdk/memory_blobs.hpp"

#include <stdexcept>
#include <utility>

namespace lubancore::detail {
namespace {
namespace blob = memory_blobs::v1;
namespace native = lubancode::trajectory;
thread_local bool in_memory_blob_provider = false;
class ProviderScope {
public:
    ProviderScope() noexcept : previous_(in_memory_blob_provider) { in_memory_blob_provider = true; }
    ~ProviderScope() { in_memory_blob_provider = previous_; }
private:
    bool previous_;
};
blob::Scope Public(const native::CasScope& value) { return {value.workspace_key, value.session_id}; }
blob::Reference Public(const native::CasReference& value) {
    return {Public(value.scope), value.sha256, value.bytes, value.media_type};
}
native::CasReference Native(const blob::Reference& value) {
    return {{value.scope.workspace_key, value.scope.session_id}, value.sha256, value.bytes, value.media_type};
}
blob::Durability Public(native::CasDurability value) {
    switch (value) {
        case native::CasDurability::Buffered: return blob::Durability::Buffered;
        case native::CasDurability::ProcessCrash: return blob::Durability::ProcessCrash;
        case native::CasDurability::PowerLoss: return blob::Durability::PowerLoss;
    }
    throw std::invalid_argument("invalid native Memory CAS durability");
}
std::optional<native::CasDurability> Native(blob::Durability value) {
    switch (value) {
        case blob::Durability::Buffered: return native::CasDurability::Buffered;
        case blob::Durability::ProcessCrash: return native::CasDurability::ProcessCrash;
        case blob::Durability::PowerLoss: return native::CasDurability::PowerLoss;
    }
    return std::nullopt;
}
class FactoryAdapter;
class StoreAdapter final : public native::CasStore {
public:
    StoreAdapter(std::shared_ptr<FactoryAdapter> owner, std::unique_ptr<blob::Store> store)
        : owner_(std::move(owner)), store_(std::move(store)) {}
    ~StoreAdapter() override {
        ProviderScope scope;
        store_.reset();
        owner_.reset();
    }
    native::CasWriteReceipt Store(const native::CasWriteRequest& request) override {
        ProviderScope scope;
        auto result = store_->Write({Public(request.reference), std::string(request.bytes), Public(request.required)});
        native::CasWriteReceipt receipt;
        receipt.reference = Native(result.reference);
        receipt.error = {std::move(result.error.code), std::move(result.error.message)};
        switch (result.state) {
            case blob::CommitState::NotCommitted: receipt.state = native::CasCommitState::NotCommitted; break;
            case blob::CommitState::Committed: receipt.state = native::CasCommitState::Committed; break;
            case blob::CommitState::Indeterminate: receipt.state = native::CasCommitState::Indeterminate; break;
            default:
                receipt.state = native::CasCommitState::Indeterminate;
                receipt.error = {"cas.receipt_mismatch", "provider returned an invalid commit state"};
                return receipt;
        }
        if (result.confirmed_durability) {
            receipt.confirmed_durability = Native(*result.confirmed_durability);
            if (!receipt.confirmed_durability) {
                receipt.state = native::CasCommitState::Indeterminate;
                receipt.error = {"cas.receipt_mismatch", "provider returned an invalid durability"};
            }
        }
        return receipt;
    }
    std::expected<std::string, native::CasError> Read(const native::CasReference& reference, std::size_t cap) override {
        ProviderScope scope;
        auto result = store_->Read(Public(reference), cap);
        if (!result) return std::unexpected(native::CasError{std::move(result.error().code), std::move(result.error().message)});
        return std::move(*result);
    }
private:
    std::shared_ptr<FactoryAdapter> owner_;
    std::unique_ptr<blob::Store> store_;
};
class FactoryAdapter final : public native::MemoryCapabilityFactory,
    public std::enable_shared_from_this<FactoryAdapter> {
public:
    explicit FactoryAdapter(std::unique_ptr<blob::Provider> provider) : provider_(std::move(provider)) {}
    ~FactoryAdapter() override { ProviderScope scope; provider_.reset(); }
    std::expected<std::shared_ptr<native::CasStore>, native::CasError> Open(const native::CasScope& scope) override {
        ProviderScope borrow;
        auto result = provider_->Open(Public(scope));
        if (!result) return std::unexpected(native::CasError{std::move(result.error().code), std::move(result.error().message)});
        if (!*result) return std::unexpected(native::CasError{"cas.open_failed", "provider returned no Memory Store"});
        return std::shared_ptr<native::CasStore>(std::make_shared<StoreAdapter>(shared_from_this(), std::move(*result)));
    }
private:
    std::unique_ptr<blob::Provider> provider_;
};
} // namespace

bool InMemoryBlobProvider() noexcept { return in_memory_blob_provider; }
std::shared_ptr<native::MemoryCapabilityFactory> MakeMemoryBlobFactory(std::unique_ptr<blob::Provider> provider) {
    if (!provider) return {};
    ProviderScope scope;
    try { return std::make_shared<FactoryAdapter>(std::move(provider)); }
    catch (...) { provider.reset(); throw; }
}

} // namespace lubancore::detail
