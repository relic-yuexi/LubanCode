#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <lubancore/api.hpp>
#include <lubancore/memory_blobs.hpp>

namespace lubancore::named_results::v1 {

using Scope = memory_blobs::v1::Scope;
using CommitState = memory_blobs::v1::CommitState;
using Durability = memory_blobs::v1::Durability;
enum class OpenMode { NewSession, Resume };
struct OpenRequest { Scope scope; std::string binding_id; OpenMode mode = OpenMode::NewSession; };
struct Reference {
    Scope scope;
    std::string binding_id, logical_name, sha256;
    std::uint64_t bytes = 0;
    std::string media_type;
    bool operator==(const Reference&) const = default;
};
struct WriteRequest {
    Reference reference;
    std::string request_key;
    std::string bytes;
    Durability required_durability = Durability::ProcessCrash;
};
struct WriteReceipt {
    CommitState state = CommitState::NotCommitted;
    Reference reference;
    std::string request_key;
    std::optional<Durability> confirmed_durability;
    // File and immediate-name confirmation alone cannot establish every ancestor
    // namespace. PowerLoss requires this stronger confirmation explicitly.
    bool ancestors_confirmed = false;
    Error error;
};
struct ListLimits {
    std::size_t entries = 8192, name_bytes = 1024, total_name_bytes = 8 * 1024 * 1024;
};
struct NameSnapshot {
    Scope scope;
    std::string binding_id, token;
    bool complete = false;
    std::vector<std::string> names;
};

// Synchronous cooperative callbacks. PublishNew must never replace an existing
// logical name, including one with identical bytes. A publishing call that throws
// may already have written: Core retains unknown and stops this Session's writes.
// Read validates the given scope/reference and honors cap; Core verifies bytes
// and SHA again. SnapshotNames includes every occupied first-level name, including
// orphan channels/temporary names, not just results already present in Journal.
// Receipt string limits: scope/binding/media/error code 200 bytes, logical name
// 1024, SHA 64, request key 1200, error message 4096. Snapshot token: 200 bytes.
// Oversized publishing replies are unknown: Core retains scalar claims, original
// field lengths and a complete-claim fingerprint when available, never a clipped
// value presented as the original. File native witnesses are kept separately.
class Store {
public:
    virtual ~Store() = default;
    virtual WriteReceipt PublishNew(WriteRequest request) = 0;
    virtual Result<std::string> Read(Reference reference, std::size_t cap) = 0;
    virtual Result<NameSnapshot> SnapshotNames(ListLimits limits) = 0;
};
class Provider {
public:
    virtual ~Provider() = default;
    virtual Result<std::unique_ptr<Store>> Open(OpenRequest request) = 0;
};
struct Options {
    // Stable host namespace, frozen with the locked workspace/Session identity.
    // Required nonempty bounded UTF-8; "file-v1" is reserved for the default.
    std::string binding_id;
    std::unique_ptr<Provider> provider;
};

// The Session owns the Provider. Its one Store survives Close for bounded result
// queries, and dies after the last reader. Callbacks/destruction must return
// cooperatively; recursive provider reads and blocking lifecycle/Wait calls fail
// before locks. A Store must not retain a strong public Session cycle.
// Omitted options retain File. Same-ID external resume requires an explicit
// matching Provider/binding; omitted options cannot reopen that scene as File.
// This LocalTrusted slice excludes child/ancestor/new-ID storage domains.
// Journal, frozen plans, Memory CAS and outbound ResultProjector are unchanged.

} // namespace lubancore::named_results::v1
