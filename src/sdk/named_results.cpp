#include "sdk/named_results.hpp"

#include <stdexcept>
#include <utility>

#include "platform/text_encoding.hpp"

namespace lubancore::detail {
namespace {
namespace native = lubancode::trajectory;
namespace blob = named_results::v1;
blob::Scope Public(const native::CasScope& scope) { return {scope.workspace_key, scope.session_id}; }
blob::Reference Public(const native::NamedResultReference& ref) {
    return {Public(ref.scope), ref.binding_id, ref.logical_name, ref.sha256, ref.bytes, ref.media_type};
}
native::NamedResultReference Native(blob::Reference value) {
    return {{std::move(value.scope.workspace_key), std::move(value.scope.session_id)},
        std::move(value.binding_id), std::move(value.logical_name), std::move(value.sha256),
        value.bytes, std::move(value.media_type)};
}
blob::Durability Public(native::CasDurability value) {
    switch (value) {
        case native::CasDurability::Buffered: return blob::Durability::Buffered;
        case native::CasDurability::ProcessCrash: return blob::Durability::ProcessCrash;
        case native::CasDurability::PowerLoss: return blob::Durability::PowerLoss;
    }
    throw std::invalid_argument("invalid named durability");
}
native::CasDurability Native(blob::Durability value) {
    switch (value) {
        case blob::Durability::Buffered: return native::CasDurability::Buffered;
        case blob::Durability::ProcessCrash: return native::CasDurability::ProcessCrash;
        case blob::Durability::PowerLoss: return native::CasDurability::PowerLoss;
    }
    // Preserve an invalid host scalar as received; validation rejects it, and
    // oversized-claim fingerprints must not collapse distinct invalid values.
    return static_cast<native::CasDurability>(static_cast<int>(value));
}
native::CasCommitState Native(blob::CommitState value) {
    switch (value) {
        case blob::CommitState::NotCommitted: return native::CasCommitState::NotCommitted;
        case blob::CommitState::Committed: return native::CasCommitState::Committed;
        case blob::CommitState::Indeterminate: return native::CasCommitState::Indeterminate;
    }
    return static_cast<native::CasCommitState>(static_cast<int>(value));
}
class FactoryAdapter;
class StoreAdapter final : public native::NamedResultStore {
public:
    StoreAdapter(std::shared_ptr<FactoryAdapter> owner, std::unique_ptr<blob::Store> store)
        : owner_(std::move(owner)), store_(std::move(store)) {}
    ~StoreAdapter() override {
        native::NamedResultProviderScope callback;
        store_.reset(); owner_.reset();
    }
    native::NamedResultWriteReceipt PublishNew(const native::NamedResultWriteRequest& input, bool& invoked) override {
        native::NamedResultProviderScope callback;
        // All argument allocations precede the actual callback marker. A failed
        // preparation is known not to have called the host's PublishNew.
        blob::WriteRequest request{Public(input.reference), input.request_key, std::string(input.bytes), Public(input.required)};
        invoked = true;
        auto actual = store_->PublishNew(std::move(request));
        native::NamedResultWriteReceipt result;
        result.state = Native(actual.state); result.reference = Native(std::move(actual.reference));
        result.request_key = std::move(actual.request_key);
        if (actual.confirmed_durability) result.confirmed_durability = Native(*actual.confirmed_durability);
        result.ancestors_confirmed = actual.ancestors_confirmed;
        result.error = {std::move(actual.error.code), std::move(actual.error.message)};
        return result;
    }
    std::expected<std::string, native::CasError> Read(const native::NamedResultReference& ref, std::size_t cap) override {
        native::NamedResultProviderScope callback;
        auto result = store_->Read(Public(ref), cap);
        if (!result) return std::unexpected(native::CasError{std::move(result.error().code), std::move(result.error().message)});
        return std::move(*result);
    }
    std::expected<native::NamedResultNames, native::CasError> SnapshotNames(native::NamedResultListLimits limits) override {
        native::NamedResultProviderScope callback;
        auto result = store_->SnapshotNames({limits.entries, limits.name_bytes, limits.total_name_bytes});
        if (!result) return std::unexpected(native::CasError{std::move(result.error().code), std::move(result.error().message)});
        return native::NamedResultNames{{std::move(result->scope.workspace_key), std::move(result->scope.session_id)},
            std::move(result->binding_id), std::move(result->token), result->complete, std::move(result->names)};
    }
private:
    std::shared_ptr<FactoryAdapter> owner_;
    std::unique_ptr<blob::Store> store_;
};
class FactoryAdapter final : public native::NamedResultFactory, public std::enable_shared_from_this<FactoryAdapter> {
public:
    explicit FactoryAdapter(blob::Options options) : binding_(std::move(options.binding_id)), provider_(std::move(options.provider)) {}
    ~FactoryAdapter() override { native::NamedResultProviderScope callback; provider_.reset(); }
    const std::string& binding_id() const noexcept override { return binding_; }
    std::expected<std::shared_ptr<native::NamedResultStore>, native::CasError> Open(const native::CasScope& scope, bool resume) override {
        native::NamedResultProviderScope callback;
        auto result = provider_->Open({Public(scope), binding_, resume ? blob::OpenMode::Resume : blob::OpenMode::NewSession});
        if (!result) return std::unexpected(native::CasError{std::move(result.error().code), std::move(result.error().message)});
        if (!*result) return std::unexpected(native::CasError{"named_result.empty_store", {}});
        return std::shared_ptr<native::NamedResultStore>(std::make_shared<StoreAdapter>(shared_from_this(), std::move(*result)));
    }
private:
    std::string binding_;
    std::unique_ptr<blob::Provider> provider_;
};
} // namespace

Result<std::shared_ptr<lubancode::trajectory::NamedResultFactory>> MakeNamedResultFactory(std::optional<blob::Options> options) {
    native::NamedResultProviderScope callback;
    if (!options) return std::shared_ptr<native::NamedResultFactory>{};
    const auto& binding = options->binding_id;
    if (!options->provider || binding.empty() || binding.size() > 200 || binding == "file-v1" ||
        binding.find_first_of("\r\n") != std::string::npos || binding.find('\0') != std::string::npos ||
        !lubancode::platform::IsValidUtf8(binding)) {
        options.reset();
        return std::unexpected(Error{"sdk.named_results.invalid_options", {}});
    }
    try { return std::shared_ptr<native::NamedResultFactory>(std::make_shared<FactoryAdapter>(std::move(*options))); }
    catch (...) { options.reset(); throw; }
}
} // namespace lubancore::detail
