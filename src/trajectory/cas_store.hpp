#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace lubancode::trajectory {

// Internal values only. Identity is supplied by the locked session owner;
// neither a content address nor a provider's filesystem path grants authority.
struct CasScope {
    std::string workspace_key;
    std::string session_id;
    bool operator==(const CasScope&) const = default;
};

enum class CasDurability { Buffered, ProcessCrash, PowerLoss };
enum class CasCommitState { NotCommitted, Committed, Indeterminate };

struct CasReference {
    CasScope scope;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::string media_type;
    bool operator==(const CasReference&) const = default;
};

struct CasError {
    std::string code;
    std::string message;
};

struct CasWriteRequest {
    CasReference reference;
    std::string_view bytes;
    CasDurability required = CasDurability::ProcessCrash;
};

struct CasWriteReceipt {
    CasCommitState state = CasCommitState::NotCommitted;
    CasReference reference;
    std::optional<CasDurability> confirmed_durability;
    CasError error;
    bool Confirms(CasDurability required) const noexcept;
};

class CasStore {
public:
    virtual ~CasStore() = default;
    virtual CasWriteReceipt Store(const CasWriteRequest& request) = 0;
    virtual std::expected<std::string, CasError> Read(
        const CasReference& reference, std::size_t byte_cap) = 0;
};

class MemoryCapabilityFactory {
public:
    virtual ~MemoryCapabilityFactory() = default;
    virtual std::expected<std::shared_ptr<CasStore>, CasError> Open(const CasScope& scope) = 0;
};

// The actual owner, rather than a provider, validates scope and returned bytes.
// A closed write capability still permits bounded immutable reads while its
// owned handle is alive; Session API rules decide whether a query is allowed.
class MemoryCapability {
public:
    MemoryCapability(CasScope scope, std::shared_ptr<CasStore> store);
    const CasScope& scope() const noexcept { return scope_; }
    CasWriteReceipt Store(std::string_view bytes, std::string media_type,
                          CasDurability required = CasDurability::ProcessCrash);
    std::expected<std::string, CasError> Read(
        const CasReference& reference, std::size_t byte_cap) const;
    void CloseWrites() noexcept;
    static std::string LogicalReference(std::string_view sha256);

private:
    CasScope scope_;
    std::shared_ptr<CasStore> store_;
    mutable std::mutex mutex_;
    bool writes_closed_ = false;
};

// A borrowed shared_ptr must not keep writing after its real owner goes away.
// This movable owner's destruction seals writes even on opening rejection.
class MemoryCapabilityLease {
public:
    MemoryCapabilityLease() = default;
    explicit MemoryCapabilityLease(std::shared_ptr<MemoryCapability> value);
    ~MemoryCapabilityLease();
    MemoryCapabilityLease(MemoryCapabilityLease&& other) noexcept;
    MemoryCapabilityLease& operator=(MemoryCapabilityLease&& other) noexcept;
    MemoryCapabilityLease(const MemoryCapabilityLease&) = delete;
    MemoryCapabilityLease& operator=(const MemoryCapabilityLease&) = delete;
    std::shared_ptr<MemoryCapability> share() const noexcept { return value_; }
    void CloseWrites() noexcept;

private:
    std::shared_ptr<MemoryCapability> value_;
};

// Only the default File adapter takes a local path. Provider factories receive
// logical scope, never an instruction to create their objects at this path.
enum class FileCasBoundary { AfterNativeClose, AfterPublish };
using FileCasFault = std::function<std::optional<std::string>(FileCasBoundary)>;
std::shared_ptr<CasStore> MakeFileCasStore(CasScope scope,
    std::filesystem::path artifact_root, FileCasFault test_fault = {});

std::expected<MemoryCapabilityLease, CasError> OpenMemoryCapability(
    const CasScope& scope, const std::filesystem::path& default_artifact_root,
    const std::shared_ptr<MemoryCapabilityFactory>& factory = {});

} // namespace lubancode::trajectory
