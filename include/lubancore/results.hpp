#pragma once

#include "lubancore/api.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lubancore::results {
namespace detail { struct SnapshotAccess; }
namespace v1 {

enum class Mode { Preview, Full };
struct SessionResultOptions {
    Mode mode = Mode::Preview;
    std::uint64_t version = 1;
    bool operator==(const SessionResultOptions&) const = default;
};
struct SessionResultPolicy {
    std::string session_id;
    Mode mode = Mode::Preview;
    std::uint64_t version = 1;
    bool operator==(const SessionResultPolicy&) const = default;
};
// Host-owned permission. A true value never enables Full in another session.
// Changing configuration or resolved secrets requires a new version.
struct NodeResultPolicy {
    bool allow_full_tool_results = false;
    std::size_t preview_max_bytes = 4096;
    std::string version;
};

struct ToolResultIdentity {
    std::string session_id, operation_id, turn_id, tool_call_id, persisted_event_id, result_id;
    bool operator==(const ToolResultIdentity&) const = default;
};
struct ToolResultSummary {
    ToolResultIdentity identity;
    std::uint64_t attempt = 0;
    bool selected = false;
    std::string tool_name;
};
enum class ArtifactState { Verified, Empty, MetadataOnly, Missing, Corrupt, Unreadable, TooLarge };
struct ToolResultChannel {
    std::string channel, artifact_id, media_type, encoding, sha256;
    std::uint64_t captured_bytes = 0, output_bytes = 0;
    bool output_bytes_lower_bound = false, capture_complete = false;
    std::string capture_reason;
    ArtifactState state = ArtifactState::MetadataOnly;
    bool artifact_verified = false;
    std::optional<std::string> text;
    std::string issue_code;
};
struct ToolResultData {
    ToolResultSummary summary;
    std::string result_kind, execution_event_id;
    ArtifactState metadata_state = ArtifactState::Missing;
    std::string metadata_sha256;
    std::uint64_t metadata_bytes = 0;
    bool content_present = false, structured_content_present = false;
    std::vector<ToolResultChannel> channels;
};
struct ToolResultReadOptions {
    // Trusted local material, never an outbound preview window. Hard cap: 8 MiB.
    std::size_t max_total_text_bytes = 1024 * 1024;
};

// Constructed only by the durable SDK reader, after checking the completed
// operation, exact ledger identity and artifact integrity. No EventJSON factory.
class SavedSnapshot {
public:
    const ToolResultData& result() const { return result_; }
    const SessionResultPolicy& policy() const { return policy_; }
    const std::string& session_id() const { return result_.summary.identity.session_id; }
    const std::string& operation_id() const { return result_.summary.identity.operation_id; }
private:
    SavedSnapshot(ToolResultData result, SessionResultPolicy policy)
        : result_(std::move(result)), policy_(std::move(policy)) {}
    ToolResultData result_;
    SessionResultPolicy policy_;
    friend struct ::lubancore::results::detail::SnapshotAccess;
};

class ResultProjector;
class LUBANCORE_API FrozenProjection {
public:
    // A closed, redacted wire record. Returned strings are independent copies.
    // The whole serialized record is bounded to 1 MiB. The transport must also
    // bound its final envelope; Full is never silently downgraded or fragmented.
    Result<std::string> ForTransmission(const ResultProjector& current) const;
    Result<std::string> SerializeForStorage() const;
private:
    struct Impl;
    explicit FrozenProjection(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
    std::shared_ptr<const Impl> impl_;
    friend class ResultProjector;
};

class LUBANCORE_API ResultProjector {
public:
    static Result<std::shared_ptr<ResultProjector>> Create(
        NodeResultPolicy node, SessionResultPolicy session,
        std::vector<std::string> known_secrets = {});
    ~ResultProjector();
    Result<FrozenProjection> Project(const SavedSnapshot& snapshot) const;
    // Dedicated storage schema, bound to actual saved SDK material. This restores
    // the old prefix; it never reprojects it. Digest checks detect corruption and
    // configuration changes, and do not authenticate a host-owned file.
    Result<FrozenProjection> RestoreSavedProjection(
        std::string_view storage, const SavedSnapshot& snapshot) const;
    std::string BindingFingerprint() const;
private:
    struct Impl;
    explicit ResultProjector(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class FrozenProjection;
};

} // namespace v1
} // namespace lubancore::results
