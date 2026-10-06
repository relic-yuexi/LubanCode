#pragma once

#include <lubancore/api.hpp>
#include <lubancore/authorization.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace lubancore {
class LUBANCORE_API Runtime;

namespace managed::v1 {

// Trusted host registration, not an untrusted request. The SDK resolves cwd and
// registers a tenant/project-separated workspace. Empty workspace_key derives
// the actual key here only; a nonempty key must match. No Policy grants are made.
struct ProjectOptions {
    authorization::v1::ProjectBinding binding;
    std::string cwd;
    std::shared_ptr<authorization::v1::PolicyProvider> policy;
};

class LUBANCORE_API Project {
public:
    ~Project();
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    authorization::v1::ProjectBinding binding() const;
private:
    struct Impl;
    explicit Project(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
    friend class ::lubancore::Runtime;
};

// Open permission alone does not grant AcquireView, ReadSession or CloseSession.
// The trusted host can use this actual ID to grant an exact Session scope.
struct OpenReceipt { std::string session_id; };
enum class StorageState { Open, Closing, Closed };
struct Identity {
    authorization::v1::ResourceScope resource;
    std::uint64_t project_binding_version = 0;
    std::string run_id;
    authorization::v1::AuthenticatedSubject creation_subject;
    std::uint64_t opening_policy_revision = 0;
    StorageState state = StorageState::Open;
};

// A subject-bound observation, never the public Local Session owner. Dropping
// views does not close the Session. Runtime supervision owns active storage.
// Only storage identity and explicit authorized Close are available in this
// phase: no Submit, Run, resume, history, results, event waits or external sinks.
class LUBANCORE_API View {
public:
    ~View();
    View(const View&) = delete;
    View& operator=(const View&) = delete;
    // Actual opening identity, retained as owned values after Close. Every call
    // rechecks current ReadSession permission, including a closed observation.
    Result<Identity> ReadIdentity() const;
    // Rechecks CloseSession even on repeated Close; actual cleanup is serialized
    // with Runtime::Shutdown. Close errors are stable and contain no root paths.
    Result<void> Close();
private:
    struct Impl;
    explicit View(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
    friend class ::lubancore::Runtime;
};

} // namespace managed::v1
} // namespace lubancore
