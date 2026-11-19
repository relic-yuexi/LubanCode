#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <lubancore/api.hpp>

namespace lubancore::memory_blobs::v1 {

// Logical identity supplied by the actual locked Session owner. A reference
// grants no authority to another Session or Writer.
struct Scope {
    std::string workspace_key;
    std::string session_id;
    bool operator==(const Scope&) const = default;
};
enum class Durability { Buffered, ProcessCrash, PowerLoss };
enum class CommitState { NotCommitted, Committed, Indeterminate };
struct Reference {
    Scope scope;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::string media_type;
    bool operator==(const Reference&) const = default;
};
struct WriteRequest {
    Reference reference;
    std::string bytes;
    Durability required_durability = Durability::ProcessCrash;
};
struct WriteReceipt {
    CommitState state = CommitState::NotCommitted;
    Reference reference;
    std::optional<Durability> confirmed_durability;
    Error error;
};

// Synchronous cooperative calls. Core validates the complete returned identity,
// confirmation, byte count and SHA. Read must honor byte_cap; Core checks again.
// A thrown Write may have published bytes and is treated as Indeterminate.
// Destruction releases resources; it cannot upgrade an earlier write receipt.
class Store {
public:
    virtual ~Store() = default;
    virtual WriteReceipt Write(WriteRequest request) = 0;
    virtual Result<std::string> Read(Reference reference, std::size_t byte_cap) = 0;
};
class Provider {
public:
    virtual ~Provider() = default;
    virtual Result<std::unique_ptr<Store>> Open(Scope scope) = 0;
};

// SessionOptions owns the Provider; each Open returns an exclusively owned Store.
// Open, Write, Read and destruction must return cooperatively. Blocking Runtime
// or Session lifecycle/Wait calls from these callbacks are rejected before locks.
// Selecting this provider does not enable recall/save or replace named results,
// Journal/plan files or project Memory. Null retains the original local CAS.

} // namespace lubancore::memory_blobs::v1
