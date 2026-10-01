#include "lubancore/results.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <span>

#include <nlohmann/json.hpp>
#include "platform/sha256.hpp"
#include "remote/result_sync.hpp"
#include "runtime/secret_resolver.hpp"

namespace lubancore::results::v1 {
namespace {
namespace remote = lubancode::remote;
namespace runtime = lubancode::runtime;
namespace platform = lubancode::platform;
using Json = nlohmann::json;

Error Failure(remote::ResultSyncError error) { return {std::string(remote::ResultSyncErrorCode(error)), {}}; }
Error InvalidStorage() { return Failure(remote::ResultSyncError::InvalidFrozenRecord); }
remote::ResultSyncMode NativeMode(Mode mode) {
    return mode == Mode::Full ? remote::ResultSyncMode::Full : remote::ResultSyncMode::Preview;
}
remote::ResultSyncIdentity NativeIdentity(const ToolResultIdentity& id) {
    return {id.session_id, id.tool_call_id, id.result_id, id.operation_id, id.turn_id, id.persisted_event_id};
}
remote::SessionResultSyncPolicy NativePolicy(const SessionResultPolicy& policy) {
    return {policy.session_id, NativeMode(policy.mode), policy.version};
}
bool TextChannel(const ToolResultChannel& c) {
    return (c.channel == "stdout" || c.channel == "stderr" || c.channel == "combined" || c.channel == "report") &&
           c.media_type == "text/plain" && c.encoding == "utf-8";
}
const char* SafeKind(const ToolResultChannel& c) {
    for (const char* kind : {"stdout", "stderr", "combined", "report", "image", "blob", "raw_payload"})
        if (c.channel == kind) return kind;
    return "other";
}
const char* SafeState(ArtifactState state) {
    switch (state) {
        case ArtifactState::Verified: return "verified";
        case ArtifactState::Empty: return "empty";
        case ArtifactState::MetadataOnly: return "metadata_only";
        case ArtifactState::Missing: return "missing";
        case ArtifactState::Corrupt: return "corrupt";
        case ArtifactState::Unreadable: return "unreadable";
        case ArtifactState::TooLarge: return "too_large";
    }
    return "invalid";
}
Json ChannelMetadata(const ToolResultData& data) {
    Json result = Json::array();
    for (const auto& c : data.channels) {
        // No raw channel name, media type, capture reason, issue text, artifact
        // reference or hash crosses this boundary. Strings here are constants.
        result.push_back({{"kind", SafeKind(c)}, {"contentKind", TextChannel(c) ? "text" : "binary"},
            {"capturedBytes", c.captured_bytes}, {"outputBytes", c.output_bytes},
            {"outputBytesLowerBound", c.output_bytes_lower_bound}, {"captureComplete", c.capture_complete},
            {"state", SafeState(c.state)}});
    }
    return result;
}
std::string SourceFingerprint(const SavedSnapshot& snapshot) {
    const auto& d = snapshot.result();
    const auto& id = d.summary.identity;
    Json source{{"identity", {id.session_id, id.operation_id, id.turn_id, id.tool_call_id,
                                id.persisted_event_id, id.result_id}},
        {"attempt", d.summary.attempt}, {"selected", d.summary.selected}, {"tool", d.summary.tool_name},
        {"kind", d.result_kind}, {"execution", d.execution_event_id},
        {"metadataState", SafeState(d.metadata_state)}, {"metadataSha256", d.metadata_sha256},
        {"metadataBytes", d.metadata_bytes}, {"content", d.content_present},
        {"structuredContent", d.structured_content_present}, {"channels", Json::array()}};
    for (const auto& c : d.channels) {
        source["channels"].push_back({{"channel", c.channel}, {"artifact", c.artifact_id},
            {"media", c.media_type}, {"encoding", c.encoding}, {"sha256", c.sha256},
            {"captured", c.captured_bytes}, {"output", c.output_bytes},
            {"lowerBound", c.output_bytes_lower_bound}, {"complete", c.capture_complete},
            {"reason", c.capture_reason}, {"state", SafeState(c.state)}, {"verified", c.artifact_verified},
            {"issue", c.issue_code}, {"textPresent", c.text.has_value()},
            {"textSha256", c.text ? platform::Sha256Hex(*c.text) : std::string()}});
    }
    return platform::Sha256Hex(source.dump());
}
bool ClosedStorage(const Json& value) {
    if (!value.is_object()) return false;
    const std::set<std::string> expected{"schema", "bindingSha256", "sourceSha256", "native", "channels", "sha256"};
    std::set<std::string> keys;
    for (auto it = value.begin(); it != value.end(); ++it) keys.insert(it.key());
    return keys == expected && value["schema"].is_string() &&
           value["schema"].get_ref<const std::string&>() == "lubancore.result-projection.sdk.v1" &&
           value["bindingSha256"].is_string() && value["sourceSha256"].is_string() &&
           value["sha256"].is_string() && value["native"].is_object() && value["channels"].is_array();
}
} // namespace

struct ResultProjector::Impl {
    remote::NodeResultSyncPolicy node;
    SessionResultPolicy session;
    runtime::SecretRedactor secrets;
    std::string binding;
    Impl(remote::NodeResultSyncPolicy n, SessionResultPolicy s) : node(std::move(n)), session(std::move(s)) {}
};
struct FrozenProjection::Impl {
    remote::FrozenToolResult native;
    Json channels;
    std::string binding, source;
    Impl(remote::FrozenToolResult n, Json c, std::string b, std::string s)
        : native(std::move(n)), channels(std::move(c)), binding(std::move(b)), source(std::move(s)) {}
};

ResultProjector::ResultProjector(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ResultProjector::~ResultProjector() = default;

Result<std::shared_ptr<ResultProjector>> ResultProjector::Create(
    NodeResultPolicy node, SessionResultPolicy session, std::vector<std::string> known_secrets) {
    struct ClearSecrets {
        std::vector<std::string>& values;
        ~ClearSecrets() { for (auto& value : values) runtime::BestEffortZeroizeString(value); }
    } clear_inputs{known_secrets};
    auto parsed = remote::ParseNodeResultSyncPolicy(
        {{"allow_full_tool_results", node.allow_full_tool_results}, {"preview_max_bytes", node.preview_max_bytes}},
        node.version);
    if (!parsed) return std::unexpected(Failure(parsed.error()));
    if (session.version == 0 || (session.mode != Mode::Preview && session.mode != Mode::Full))
        return std::unexpected(Failure(remote::ResultSyncError::InvalidConfig));
    if (session.mode == Mode::Full && !node.allow_full_tool_results)
        return std::unexpected(Failure(remote::ResultSyncError::FullSyncDisabled));
    auto impl = std::make_unique<Impl>(std::move(*parsed), std::move(session));
    const remote::ResultSyncIdentity probe{impl->session.session_id, impl->session.session_id,
        impl->session.session_id, impl->session.session_id, impl->session.session_id, impl->session.session_id};
    const remote::SavedToolResult empty;
    auto id_check = remote::ProjectSavedToolResult(impl->node, NativePolicy(impl->session), probe, empty, impl->secrets);
    if (!id_check) return std::unexpected(Failure(id_check.error()));
    std::sort(known_secrets.begin(), known_secrets.end());
    // Hash the canonical set without making a second JSON copy of plaintext.
    platform::Sha256Stream digest;
    const auto hash_bytes = [&](std::string_view value) {
        digest.Update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.data()), value.size()));
    };
    const Json config{{"allowFull", node.allow_full_tool_results}, {"previewBytes", node.preview_max_bytes},
        {"nodeVersion", node.version}, {"sessionId", impl->session.session_id},
        {"sessionMode", impl->session.mode == Mode::Full ? "full" : "preview"},
        {"sessionVersion", impl->session.version}};
    hash_bytes(config.dump());
    std::string_view previous;
    for (const auto& secret : known_secrets) {
        if (secret.empty() || secret == previous) continue;
        previous = secret;
        hash_bytes(":" + std::to_string(secret.size()) + ":");
        hash_bytes(secret);
        const runtime::SecretValue value{std::string(secret)};
        impl->secrets.Register(value);
    }
    impl->binding = digest.FinalHex();
    // Validate session ID and secret-sensitive policy IDs using the same kernel.
    auto checked = remote::ProjectSavedToolResult(impl->node, NativePolicy(impl->session), probe, empty, impl->secrets);
    if (!checked) return std::unexpected(Failure(checked.error()));
    return std::shared_ptr<ResultProjector>(new ResultProjector(std::move(impl)));
}

std::string ResultProjector::BindingFingerprint() const { return impl_->binding; }

Result<FrozenProjection> ResultProjector::Project(const SavedSnapshot& snapshot) const {
    if (snapshot.policy().session_id != snapshot.session_id() || snapshot.policy() != impl_->session)
        return std::unexpected(Failure(remote::ResultSyncError::SessionMismatch));
    const auto& data = snapshot.result();
    if (data.metadata_state == ArtifactState::Missing)
        return std::unexpected(Failure(remote::ResultSyncError::ResultMissing));
    if (data.metadata_state == ArtifactState::TooLarge)
        return std::unexpected(Failure(remote::ResultSyncError::ResultTooLarge));
    if (data.metadata_state != ArtifactState::Verified)
        return std::unexpected(Failure(remote::ResultSyncError::InvalidSource));
    if (data.channels.size() > 1024)
        return std::unexpected(Failure(remote::ResultSyncError::ResultTooLarge));
    // combined is the canonical aggregate when present; stdout/stderr then carry
    // metadata only. Otherwise use fixed channel order, preserving duplicates in
    // ledger order. One aggregate is redacted and cut once, never per channel.
    const bool combined = std::any_of(data.channels.begin(), data.channels.end(), [](const auto& c) {
        return c.channel == "combined" && TextChannel(c);
    });
    std::string text;
    bool has_text = false, complete = true;
    std::uint64_t original = 0;
    bool bytes_known = true;
    for (const char* kind : {"combined", "stdout", "stderr", "report"}) {
        if (combined && (std::string_view(kind) == "stdout" || std::string_view(kind) == "stderr")) continue;
        for (const auto& c : data.channels) {
            if (c.channel != kind || !TextChannel(c)) continue;
            has_text = true;
            if (c.state == ArtifactState::TooLarge) return std::unexpected(Failure(remote::ResultSyncError::ResultTooLarge));
            if (c.state == ArtifactState::Missing) return std::unexpected(Failure(remote::ResultSyncError::ResultMissing));
            if ((c.state != ArtifactState::Verified && c.state != ArtifactState::Empty) || !c.artifact_verified || !c.text)
                return std::unexpected(Failure(remote::ResultSyncError::InvalidSource));
            if (c.text->size() != c.captured_bytes ||
                c.output_bytes < c.captured_bytes || (c.capture_complete &&
                (c.output_bytes_lower_bound || c.output_bytes != c.captured_bytes)))
                return std::unexpected(Failure(remote::ResultSyncError::InvalidSource));
            if (c.text->size() > remote::kMaxResultInputBytes - text.size())
                return std::unexpected(Failure(remote::ResultSyncError::ResultTooLarge));
            text += *c.text;
            complete = complete && c.capture_complete;
            bytes_known = bytes_known && !c.output_bytes_lower_bound;
            if (c.output_bytes > std::numeric_limits<std::uint64_t>::max() - original)
                return std::unexpected(Failure(remote::ResultSyncError::InvalidSource));
            original += c.output_bytes;
        }
    }
    remote::SavedToolResult saved;
    saved.kind = has_text ? remote::ResultContentKind::Text : remote::ResultContentKind::Binary;
    if (!has_text) {
        // Pure binary/structured material still reports capture completeness.
        // It never gains a body; Full is rejected by the shared kernel.
        complete = std::all_of(data.channels.begin(), data.channels.end(), [](const auto& c) {
            return c.capture_complete;
        });
    }
    saved.capture_complete = complete;
    saved.text = text;
    if (has_text && bytes_known) saved.original_bytes = original;
    auto native = remote::ProjectSavedToolResult(impl_->node, NativePolicy(impl_->session),
        NativeIdentity(data.summary.identity), saved, impl_->secrets);
    if (!native) return std::unexpected(Failure(native.error()));
    FrozenProjection frozen(std::make_shared<FrozenProjection::Impl>(std::move(*native), ChannelMetadata(data),
        impl_->binding, SourceFingerprint(snapshot)));
    auto wire = frozen.ForTransmission(*this);
    if (!wire) return std::unexpected(wire.error());
    // Generate a restorable bounded record before committing the projection.
    auto storage = frozen.SerializeForStorage();
    if (!storage) return std::unexpected(storage.error());
    return frozen;
}

Result<std::string> FrozenProjection::ForTransmission(const ResultProjector& current) const {
    auto checked = impl_->native.ForTransmission(current.impl_->node, NativePolicy(current.impl_->session),
                                                 current.impl_->secrets);
    if (!checked) return std::unexpected(Failure(checked.error()));
    if (impl_->binding != current.impl_->binding)
        return std::unexpected(Failure(remote::ResultSyncError::RedactionChanged));
    (*checked)["channels"] = impl_->channels;
    const auto bytes = checked->dump();
    if (bytes.size() > remote::kMaxFullResultBytes)
        return std::unexpected(Failure(remote::ResultSyncError::ResultTooLarge));
    return bytes;
}

Result<std::string> FrozenProjection::SerializeForStorage() const {
    auto native = impl_->native.SerializeForStorage();
    if (!native) return std::unexpected(Failure(native.error()));
    Json record{{"schema", "lubancore.result-projection.sdk.v1"}, {"bindingSha256", impl_->binding},
        {"sourceSha256", impl_->source}, {"native", Json::parse(*native)}, {"channels", impl_->channels}};
    record["sha256"] = platform::Sha256Hex(record.dump());
    const auto bytes = record.dump();
    if (bytes.size() > remote::kMaxFullResultBytes)
        return std::unexpected(Failure(remote::ResultSyncError::ResultTooLarge));
    return bytes;
}

Result<FrozenProjection> ResultProjector::RestoreSavedProjection(
    std::string_view storage, const SavedSnapshot& snapshot) const {
    if (storage.size() > remote::kMaxFullResultBytes)
        return std::unexpected(Failure(remote::ResultSyncError::ResultTooLarge));
    if (snapshot.policy().session_id != snapshot.session_id() || snapshot.policy() != impl_->session)
        return std::unexpected(Failure(remote::ResultSyncError::SessionMismatch));
    auto record = Json::parse(storage, nullptr, false);
    if (!ClosedStorage(record)) return std::unexpected(InvalidStorage());
    const auto expected_digest = record["sha256"].get<std::string>();
    record.erase("sha256");
    if (platform::Sha256Hex(record.dump()) != expected_digest) return std::unexpected(InvalidStorage());
    if (record["bindingSha256"].get_ref<const std::string&>() != impl_->binding)
        return std::unexpected(Failure(remote::ResultSyncError::PolicyChanged));
    if (record["sourceSha256"].get_ref<const std::string&>() != SourceFingerprint(snapshot) ||
        record["channels"] != ChannelMetadata(snapshot.result()))
        return std::unexpected(InvalidStorage());
    auto native = remote::FrozenToolResult::RestoreSavedProjection(record["native"].dump(), impl_->node,
        NativePolicy(impl_->session), NativeIdentity(snapshot.result().summary.identity), impl_->secrets);
    if (!native) return std::unexpected(Failure(native.error()));
    FrozenProjection frozen(std::make_shared<FrozenProjection::Impl>(std::move(*native), record["channels"],
        impl_->binding, record["sourceSha256"].get<std::string>()));
    auto checked = frozen.ForTransmission(*this);
    if (!checked) return std::unexpected(checked.error());
    return frozen;
}
} // namespace lubancore::results::v1
