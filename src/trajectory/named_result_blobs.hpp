#pragma once

#include <cstddef>
#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/atomic_write.hpp"
#include "trajectory/cas_store.hpp"

namespace lubancode::trajectory {

struct NamedResultReference {
    CasScope scope;
    std::string binding_id, logical_name, sha256;
    std::uint64_t bytes = 0;
    std::string media_type;
    bool operator==(const NamedResultReference&) const = default;
};
struct NamedResultWriteRequest {
    NamedResultReference reference;
    std::string request_key;
    std::string_view bytes;
    CasDurability required = CasDurability::ProcessCrash;
};
struct NamedOversizeClaim {
    // Scope workspace/session, binding, name, hash, media, key, error code/message.
    std::array<std::uint64_t, 9> field_bytes{};
    int state = 0;
    std::optional<int> durability;
    std::uint64_t bytes = 0;
    bool ancestors_confirmed = false, fingerprint_complete = false;
    std::string sha256;
    bool operator==(const NamedOversizeClaim&) const = default;
};
struct NamedResultWriteReceipt {
    CasCommitState state = CasCommitState::NotCommitted;
    NamedResultReference reference;
    std::string request_key;
    std::optional<CasDurability> confirmed_durability;
    bool ancestors_confirmed = false;
    CasError error;
    bool validated = false; // Core observation, never supplied by the public provider.
    std::optional<NamedOversizeClaim> oversize_claim;
    // Only File supplies this owned, actual native observation.
    std::optional<platform::ImmutableWriteReceipt> native;
    bool operator==(const NamedResultWriteReceipt& other) const {
        return state == other.state && reference == other.reference && request_key == other.request_key &&
            confirmed_durability == other.confirmed_durability && ancestors_confirmed == other.ancestors_confirmed &&
            error.code == other.error.code && error.message == other.error.message &&
            validated == other.validated && oversize_claim == other.oversize_claim && native == other.native;
    }
};
struct NamedResultListLimits {
    std::size_t entries = 8192, name_bytes = 1024, total_name_bytes = 8 * 1024 * 1024;
};
struct NamedResultNames {
    CasScope scope;
    std::string binding_id, token;
    bool complete = false;
    std::vector<std::string> names;
};

class NamedResultStore {
public:
    virtual ~NamedResultStore() = default;
    // The adapter sets invoked immediately before the actual public/native
    // publishing call, after preparing owned arguments and receipt slots.
    virtual NamedResultWriteReceipt PublishNew(const NamedResultWriteRequest&, bool& invoked) = 0;
    virtual std::expected<std::string, CasError> Read(const NamedResultReference&, std::size_t cap) = 0;
    virtual std::expected<NamedResultNames, CasError> SnapshotNames(NamedResultListLimits) = 0;
};
class NamedResultFactory {
public:
    virtual ~NamedResultFactory() = default;
    virtual const std::string& binding_id() const noexcept = 0;
    virtual std::expected<std::shared_ptr<NamedResultStore>, CasError> Open(const CasScope&, bool resume) = 0;
};

enum class NamedPublicationKnowledge { NotCommitted, Committed, Indeterminate };
struct NamedFilePublication {
    std::string logical_name;
    bool called = false;
    std::optional<platform::ImmutableWriteReceipt> native;
    std::optional<NamedResultWriteReceipt> receipt;
    bool operator==(const NamedFilePublication&) const = default;
};
struct NamedPublication {
    NamedPublicationKnowledge knowledge = NamedPublicationKnowledge::NotCommitted;
    std::vector<NamedFilePublication> files;
    std::string error_code, error;
    bool operator==(const NamedPublication&) const = default;
};

bool InNamedResultProvider() noexcept;
class NamedResultProviderScope {
public:
    NamedResultProviderScope() noexcept;
    ~NamedResultProviderScope();
private:
    bool previous_;
};

class NamedResultCapability;
class NamedResultMaterial {
public:
    NamedResultMaterial(NamedResultMaterial&&) noexcept = default;
    NamedResultMaterial& operator=(NamedResultMaterial&& other) noexcept {
        if (this != &other) {
            write_lock_ = {}; // unlock before dropping the mutex's actual owner
            owner_ = std::move(other.owner_); write_lock_ = std::move(other.write_lock_);
            prefix_ = std::move(other.prefix_); number_ = other.number_;
        }
        return *this;
    }
    NamedResultMaterial(const NamedResultMaterial&) = delete;
    std::uint64_t number() const noexcept { return number_; }
    NamedResultWriteReceipt Publish(const std::string& name, std::string_view bytes,
                                   std::string media_type, bool& invoked);
    void Commit();
    void PreserveUnknown(const std::shared_ptr<NamedPublication>& publication) noexcept;
private:
    friend class NamedResultCapability;
    NamedResultMaterial(std::shared_ptr<NamedResultCapability>, std::unique_lock<std::mutex>,
                        std::string prefix, std::uint64_t number);
    std::shared_ptr<NamedResultCapability> owner_;
    std::unique_lock<std::mutex> write_lock_;
    std::string prefix_;
    std::uint64_t number_ = 0;
};

// Only material transactions take write_mutex_. Reads have a distinct lifetime
// and never borrow a Writer. CloseWrites therefore does not wait for new reads.
class NamedResultCapability : public std::enable_shared_from_this<NamedResultCapability> {
public:
    NamedResultCapability(CasScope, std::string binding, std::shared_ptr<NamedResultStore>,
                          std::filesystem::path file_session_directory = {});
    const CasScope& scope() const noexcept { return scope_; }
    const std::string& binding_id() const noexcept { return binding_id_; }
    bool external() const noexcept { return file_session_directory_.empty(); }
    const std::filesystem::path& FileSessionDirectory() const noexcept { return file_session_directory_; }
    std::string DisplayPath(std::string_view artifact_path) const;
    std::expected<NamedResultMaterial, CasError> BeginMaterial(std::string prefix,
        std::size_t directory_cap = 0, bool refresh_names = false);
    std::expected<std::string, CasError> Read(std::string_view artifact_path, std::string sha256,
        std::uint64_t bytes, std::string media_type, std::size_t cap) const;
    std::optional<NamedPublication> FirstUnconfirmedPublication() const;
    // Admission may run under a host API lock or inside a provider callback.
    // Observe the retained first unknown without waiting for the material gate
    // or allocating/copying the receipt. This never confirms or clears it.
    bool HasUnconfirmedPublication() const noexcept {
        return publication_unconfirmed_.load(std::memory_order_acquire);
    }
    void CloseWrites() noexcept;
private:
    friend class NamedResultMaterial;
    CasScope scope_;
    std::string binding_id_;
    std::shared_ptr<NamedResultStore> store_;
    std::filesystem::path file_session_directory_;
    mutable std::mutex write_mutex_, store_mutex_;
    std::map<std::string, std::uint64_t> next_;
    std::optional<NamedResultNames> names_;
    bool writes_closed_ = false;
    std::shared_ptr<NamedPublication> first_unknown_;
    std::atomic<bool> publication_unconfirmed_{false};
};

class NamedResultLease {
public:
    NamedResultLease() = default;
    explicit NamedResultLease(std::shared_ptr<NamedResultCapability> value) : value_(std::move(value)) {}
    ~NamedResultLease() { CloseWrites(); }
    NamedResultLease(NamedResultLease&&) noexcept = default;
    NamedResultLease& operator=(NamedResultLease&& other) noexcept {
        if (this != &other) { CloseWrites(); value_ = std::move(other.value_); }
        return *this;
    }
    NamedResultLease(const NamedResultLease&) = delete;
    std::shared_ptr<NamedResultCapability> share() const noexcept { return value_; }
    void CloseWrites() noexcept { if (value_) value_->CloseWrites(); }
private:
    std::shared_ptr<NamedResultCapability> value_;
};

std::expected<NamedResultLease, CasError> OpenNamedResultCapability(const CasScope& scope,
    const std::filesystem::path& session_directory,
    const std::shared_ptr<NamedResultFactory>& factory = {}, bool resume = false);

} // namespace lubancode::trajectory
