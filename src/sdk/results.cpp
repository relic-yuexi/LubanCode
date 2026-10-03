#include "sdk/results.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string_view>

#include "platform/atomic_write.hpp"
#include "platform/secure_file.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "tools/path_utils.hpp"

namespace lubancore::results::detail {
struct SnapshotAccess {
    static v1::SavedSnapshot Make(v1::ToolResultData data, v1::SessionResultPolicy policy) {
        return v1::SavedSnapshot(std::move(data), std::move(policy));
    }
};
} // namespace lubancore::results::detail

namespace lubancore::detail {
namespace {
namespace v3 = lubancode::trajectory::v3;
namespace out = results::v1;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using State = out::ArtifactState;
constexpr std::uint64_t kMaxTextBytes = 8 * 1024 * 1024;
constexpr std::uint64_t kMaxMetadataBytes = 8 * 1024 * 1024;
constexpr std::uint64_t kMaxVerificationBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxResults = 256;
constexpr std::size_t kMaxChannels = 64;

Error Failure(std::string code, std::string message = {}) { return {std::move(code), std::move(message)}; }
bool ValidId(std::string_view id) {
    return !id.empty() && id.size() <= 200 &&
        id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string_view::npos;
}
bool IsTerminal(v3::EventKindV3 kind) {
    using K = v3::EventKindV3;
    return kind == K::ToolExecutionFinished || kind == K::ToolExecutionFailed ||
        kind == K::ToolExecutionCancelled || kind == K::ToolExecutionRejected || kind == K::ToolExecutionUnknown;
}
bool SafePath(const fs::path& file) {
    // Include every ancestor: checking only the leaf misses symlink/junction
    // directories. The owned persistence root is not a hostile-filesystem sandbox.
    for (auto path = file; !path.empty();) {
        if (!lubancode::platform::RejectReparsePoint(path)) return false;
        const auto parent = path.parent_path();
        if (parent == path) break;
        path = parent;
    }
    return true;
}
std::string ExtensionFor(std::string_view media) {
    if (media == "application/json") return "json";
    if (media.starts_with("text/")) return "txt";
    return "bin";
}
bool TextChannel(std::string_view channel) {
    return channel == "stdout" || channel == "stderr" || channel == "combined" || channel == "report";
}
std::uint64_t Uint(const Json& value) {
    if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
        throw std::runtime_error("invalid nonnegative integer");
    return value.get<std::uint64_t>();
}
ToolResultArtifact Artifact(const Json& ref) {
    return {ref.at("artifactId").get<std::string>(), ref.at("kind").get<std::string>(),
        ref.at("path").get<std::string>(), ref.at("sha256").get<std::string>(),
        Uint(ref.at("bytes")), ref.at("mediaType").get<std::string>()};
}

struct ReadArtifactResult {
    State state = State::Unreadable;
    std::string issue;
    std::string data;
    bool verified = false;
};
ReadArtifactResult ReadArtifact(const fs::path& root, const ToolResultArtifact& ref,
    bool retain, std::uint64_t& verification_remaining, std::uint64_t retain_limit) {
    ReadArtifactResult result;
    const auto file = root / lubancode::tools::Utf8ToPath(ref.path);
    if (!SafePath(file)) { result.state = State::Corrupt; result.issue = "sdk.result.path_rejected"; return result; }
    std::error_code ec;
    const auto status = fs::symlink_status(file, ec);
    if (status.type() == fs::file_type::not_found) {
        result.state = State::Missing; result.issue = "sdk.result.missing_artifact"; return result;
    }
    if (ec || !fs::is_regular_file(status)) { result.issue = "sdk.result.unreadable"; return result; }
    const auto size = fs::file_size(file, ec);
    if (ec) { result.issue = "sdk.result.unreadable"; return result; }
    if (size != ref.bytes) { result.state = State::Corrupt; result.issue = "sdk.result.bytes_mismatch"; return result; }
    if (ref.bytes > verification_remaining || (retain && ref.bytes > retain_limit)) {
        result.state = State::TooLarge; result.issue = "sdk.result.too_large"; return result;
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) { result.issue = "sdk.result.unreadable"; return result; }
    if (retain) result.data.reserve(static_cast<std::size_t>(ref.bytes));
    lubancode::platform::Sha256Stream hash;
    std::array<char, 65536> buffer{};
    std::uint64_t read = 0;
    while (read < ref.bytes) {
        const auto wanted = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), ref.bytes - read));
        input.read(buffer.data(), wanted);
        const auto count = input.gcount();
        if (count <= 0) break;
        const auto chunk = std::string_view(buffer.data(), static_cast<std::size_t>(count));
        hash.Update(chunk);
        if (retain) result.data.append(chunk);
        read += static_cast<std::uint64_t>(count);
    }
    verification_remaining -= read;
    char extra = 0;
    input.read(&extra, 1);
    if (input.bad()) { result.data.clear(); result.issue = "sdk.result.unreadable"; return result; }
    if (read != ref.bytes || input.gcount() != 0 || !SafePath(file)) {
        result.data.clear(); result.state = State::Corrupt; result.issue = "sdk.result.bytes_mismatch"; return result;
    }
    if (hash.FinalHex() != ref.sha256) {
        result.data.clear(); result.state = State::Corrupt; result.issue = "sdk.result.hash_mismatch"; return result;
    }
    result.state = State::Verified;
    result.verified = true;
    return result;
}

Result<out::SessionResultPolicy> ReadPolicy(const fs::path& path, const std::string& session_id) {
    if (!SafePath(path)) return std::unexpected(Failure("sdk.result.policy_invalid", "result policy path is a link"));
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec || fs::file_size(path, ec) > 1024 || ec)
        return std::unexpected(Failure("sdk.result.policy_invalid", "result policy is not a bounded regular file"));
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected(Failure("sdk.result.policy_invalid", "cannot read result policy"));
    std::array<char, 1025> buffer{};
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (input.bad() || input.gcount() > 1024 || !SafePath(path))
        return std::unexpected(Failure("sdk.result.policy_invalid", "result policy exceeded its read bound"));
    const auto json = Json::parse(std::string_view(buffer.data(), static_cast<std::size_t>(input.gcount())), nullptr, false);
    if (!json.is_object() || json.size() != 4 || !json.contains("schemaVersion") ||
        !json.at("schemaVersion").is_number_integer() || json.at("schemaVersion") != 1 ||
        json.value("sessionId", std::string()) != session_id || !json.contains("version"))
        return std::unexpected(Failure("sdk.result.policy_invalid", "invalid saved result policy identity"));
    const auto mode = json.value("mode", std::string());
    const auto version = Uint(json.at("version"));
    if ((mode != "preview" && mode != "full") || version == 0)
        return std::unexpected(Failure("sdk.result.policy_invalid", "invalid saved result policy"));
    return out::SessionResultPolicy{session_id, mode == "full" ? out::Mode::Full : out::Mode::Preview, version};
}
} // namespace

Result<out::SessionResultPolicy> FreezeResultPolicy(const fs::path& session_dir, const std::string& session_id,
    const std::optional<out::SessionResultOptions>& requested, bool resume) {
    try {
        if (requested && (requested->version == 0 ||
            (requested->mode != out::Mode::Preview && requested->mode != out::Mode::Full)))
            return std::unexpected(Failure("sdk.result.policy_invalid", "invalid requested result policy"));
        const auto path = session_dir / "sdk-result-policy.json";
        if (!SafePath(path)) return std::unexpected(Failure("sdk.result.policy_invalid", "result policy path is a link"));
        std::error_code ec;
        const bool exists = fs::exists(path, ec);
        if (ec) return std::unexpected(Failure("sdk.result.policy_invalid", "cannot inspect result policy"));
        out::SessionResultPolicy frozen{session_id, out::Mode::Preview, 1};
        if (exists) {
            auto saved = ReadPolicy(path, session_id);
            if (!saved) return std::unexpected(saved.error());
            frozen = std::move(*saved);
        } else if (!resume && requested) {
            frozen.mode = requested->mode;
            frozen.version = requested->version;
        }
        // A legacy resume has Preview/v1 identity, not an invitation to upgrade.
        if (requested && (requested->mode != frozen.mode || requested->version != frozen.version))
            return std::unexpected(Failure("sdk.result.policy_resume_mismatch", "requested result policy differs from saved session"));
        if (!exists) {
            const Json saved{{"schemaVersion", 1}, {"sessionId", session_id},
                {"mode", frozen.mode == out::Mode::Full ? "full" : "preview"}, {"version", frozen.version}};
            const auto written = lubancode::platform::AtomicWriteFile(path, saved.dump(),
                lubancode::platform::WriteDurability::ProcessCrashDurability);
            if (!written) return std::unexpected(Failure("sdk.result.policy_write_failed", "cannot persist result policy"));
        }
        return frozen;
    } catch (...) {
        return std::unexpected(Failure("sdk.result.policy_invalid", "invalid result policy"));
    }
}

Result<OperationToolResultIndex> IndexToolResults(const v3::V3Ledger& ledger,
    const std::string& session_id, const std::string& operation_id, const std::string& turn_id) {
    try {
        if (ledger.session_id != session_id || !ValidId(session_id) || !ValidId(operation_id) || !ValidId(turn_id))
            return std::unexpected(Failure("sdk.result.index_invalid", "result index scope does not match session and turn"));
        OperationToolResultIndex index;
        const auto actions = v3::FoldToolActions(ledger);
        std::set<std::string> owned_business_actions;
        for (const auto& action : actions)
            if (!action.provider_reply_required) owned_business_actions.insert(action.tool_call_id);
        std::set<std::string> selected_sources;
        for (const auto& event : ledger.events) {
            if (event.kind != v3::EventKindV3::ToolResultSelected || event.turn_id != turn_id) continue;
            if (event.action_id && owned_business_actions.contains(*event.action_id)) continue;
            for (const auto& source : event.payload.at("sourceResultEventRefs")) {
                if (!source.is_string()) return std::unexpected(Failure("sdk.result.index_invalid", "cross-session result source is unsupported"));
                const auto id = source.get<std::string>();
                const auto* persisted = ledger.FindEvent(id);
                if (!persisted || persisted->kind != v3::EventKindV3::ToolResultPersisted || persisted->seq >= event.seq ||
                    persisted->turn_id != event.turn_id || persisted->action_id != event.action_id)
                    return std::unexpected(Failure("sdk.result.index_invalid", "selected result source scope mismatch"));
                selected_sources.insert(id);
            }
        }
        for (const auto& event : ledger.events) {
            if (event.kind != v3::EventKindV3::ToolResultPersisted || event.turn_id != turn_id) continue;
            if (event.action_id && owned_business_actions.contains(*event.action_id)) continue;
            if (index.entries.size() == kMaxResults) return std::unexpected(Failure("sdk.result.index_too_large", "too many persisted results in operation"));
            if (event.session_id != session_id || !ValidId(event.event_id) || !event.action_id || !ValidId(*event.action_id) ||
                event.payload.at("tool_call_id") != *event.action_id)
                return std::unexpected(Failure("sdk.result.index_invalid", "persisted result action scope mismatch"));
            const auto attempt = Uint(event.payload.at("attempt"));
            if (attempt == 0) return std::unexpected(Failure("sdk.result.index_invalid", "invalid result attempt"));
            const auto terminal_id = event.payload.at("executionEventRef").get<std::string>();
            if (!ValidId(terminal_id)) return std::unexpected(Failure("sdk.result.index_invalid", "invalid execution event identity"));
            const auto* terminal = ledger.FindEvent(terminal_id);
            if (!terminal || !IsTerminal(terminal->kind) || terminal->seq >= event.seq ||
                terminal->session_id != session_id || terminal->turn_id != turn_id || terminal->action_id != event.action_id ||
                Uint(terminal->payload.at("attempt")) != attempt)
                return std::unexpected(Failure("sdk.result.index_invalid", "persisted result terminal scope mismatch"));
            ToolResultIndexEntry entry;
            entry.execution_event_id = terminal_id;
            entry.summary.attempt = attempt;
            entry.summary.selected = selected_sources.contains(event.event_id);
            entry.summary.identity = {session_id, operation_id, turn_id, *event.action_id, event.event_id, {}};
            if (const auto* action = v3::FindActionSnapshot(actions, *event.action_id); action && action->tool_name)
                entry.summary.tool_name = *action->tool_name;
            if (entry.summary.tool_name.size() > 512)
                return std::unexpected(Failure("sdk.result.index_too_large", "tool name exceeds query bound"));
            const auto& refs = event.payload.at("result_ref");
            if (refs.size() > kMaxChannels + 1) return std::unexpected(Failure("sdk.result.index_too_large", "too many result channels"));
            std::set<std::string> ids, paths;
            std::size_t metadata_count = 0;
            for (const auto& ref : refs) {
                auto artifact = Artifact(ref);
                if (!ValidId(artifact.id) || !ids.insert(artifact.id).second || !paths.insert(artifact.path).second)
                    return std::unexpected(Failure("sdk.result.index_invalid", "duplicate or invalid artifact identity"));
                if (artifact.kind == "result_metadata") {
                    ++metadata_count;
                    entry.summary.identity.result_id = artifact.id;
                    if (artifact.media_type != "application/json" || artifact.path != "artifacts/" + artifact.id + ".json")
                        return std::unexpected(Failure("sdk.result.index_invalid", "metadata artifact path does not match identity"));
                }
                entry.artifacts.push_back(std::move(artifact));
            }
            if (metadata_count != 1) return std::unexpected(Failure("sdk.result.index_invalid", "result must have exactly one metadata artifact"));
            for (const auto& ref : entry.artifacts) {
                if (ref.kind == "result_metadata") continue;
                const auto& result_id = entry.summary.identity.result_id;
                if (ref.id != result_id + "-" + ref.kind || ref.path != "artifacts/" + result_id + "." + ref.kind + "." + ExtensionFor(ref.media_type))
                    return std::unexpected(Failure("sdk.result.index_invalid", "channel artifact path does not match identity"));
            }
            index.entries.push_back(std::move(entry));
        }
        return index;
    } catch (...) {
        return std::unexpected(Failure("sdk.result.index_invalid", "invalid persisted result identity"));
    }
}

Result<out::SavedSnapshot> ReadIndexedToolResult(const fs::path& session_dir, const ToolResultIndexEntry& entry,
    const out::SessionResultPolicy& policy, out::ToolResultReadOptions options) {
    try {
        if (options.max_total_text_bytes == 0 || options.max_total_text_bytes > kMaxTextBytes)
            return std::unexpected(Failure("sdk.result.read_limit_invalid", "text budget must be between 1 byte and 8 MiB"));
        if (policy.session_id != entry.summary.identity.session_id)
            return std::unexpected(Failure("sdk.result.identity_mismatch", "snapshot policy belongs to another session"));
        const auto& identity = entry.summary.identity;
        for (const auto* id : {&identity.session_id, &identity.operation_id, &identity.turn_id,
                              &identity.tool_call_id, &identity.persisted_event_id, &identity.result_id}) {
            if (!ValidId(*id)) return std::unexpected(Failure("sdk.result.index_invalid", "invalid frozen result identity"));
        }
        if (entry.artifacts.size() > kMaxChannels + 1)
            return std::unexpected(Failure("sdk.result.index_too_large"));
        std::set<std::string> artifact_ids, artifact_paths;
        std::size_t metadata_count = 0;
        for (const auto& ref : entry.artifacts) {
            if (!ValidId(ref.id) || !ValidId(ref.kind) || ref.sha256.size() != 64 ||
                ref.sha256.find_first_not_of("0123456789abcdef") != std::string::npos ||
                !artifact_ids.insert(ref.id).second || !artifact_paths.insert(ref.path).second)
                return std::unexpected(Failure("sdk.result.index_invalid", "invalid frozen artifact identity"));
            if (ref.kind == "result_metadata") {
                ++metadata_count;
                if (ref.id != identity.result_id || ref.media_type != "application/json" || ref.path != "artifacts/" + identity.result_id + ".json")
                    return std::unexpected(Failure("sdk.result.index_invalid", "metadata identity does not match result"));
            } else if (ref.id != identity.result_id + "-" + ref.kind ||
                ref.path != "artifacts/" + identity.result_id + "." + ref.kind + "." + ExtensionFor(ref.media_type)) {
                return std::unexpected(Failure("sdk.result.index_invalid", "channel identity does not match result"));
            }
        }
        if (metadata_count != 1) return std::unexpected(Failure("sdk.result.index_invalid", "metadata identity is not unique"));
        out::ToolResultData data;
        data.summary = entry.summary;
        data.execution_event_id = entry.execution_event_id;
        const auto metadata = std::find_if(entry.artifacts.begin(), entry.artifacts.end(),
            [](const auto& ref) { return ref.kind == "result_metadata"; });
        if (metadata == entry.artifacts.end()) return std::unexpected(Failure("sdk.result.index_invalid"));
        data.metadata_sha256 = metadata->sha256;
        data.metadata_bytes = metadata->bytes;
        std::uint64_t verification_remaining = kMaxVerificationBytes;
        auto material = ReadArtifact(session_dir, *metadata, true, verification_remaining, kMaxMetadataBytes);
        data.metadata_state = material.state;
        if (!material.verified) return results::detail::SnapshotAccess::Make(std::move(data), policy);
        const auto json = Json::parse(material.data, nullptr, false);
        const auto metadata_invalid = [&]() -> Result<out::SavedSnapshot> {
            data.metadata_state = State::Corrupt;
            data.channels.clear();
            return results::detail::SnapshotAccess::Make(std::move(data), policy);
        };
        if (!json.is_object() || json.value("result_id", std::string()) != entry.summary.identity.result_id ||
            json.value("tool_call_id", std::string()) != entry.summary.identity.tool_call_id ||
            Uint(json.at("attempt")) != entry.summary.attempt ||
            json.value("execution_event_ref", std::string()) != entry.execution_event_id ||
            !json.contains("outputs") || !json.at("outputs").is_array() || json.at("outputs").size() > kMaxChannels)
            return metadata_invalid();
        data.result_kind = json.at("result_kind").get<std::string>();
        if (data.result_kind != "text" && data.result_kind != "process" && data.result_kind != "structured" && data.result_kind != "multimodal")
            return metadata_invalid();
        data.content_present = json.contains("content");
        data.structured_content_present = json.contains("structured_content");
        std::set<std::string> channels, used_refs;
        std::uint64_t text_remaining = options.max_total_text_bytes;
        for (const auto& output : json.at("outputs")) {
            if (!output.is_object()) return metadata_invalid();
            out::ToolResultChannel channel;
            channel.channel = output.at("channel").get<std::string>();
            if (!ValidId(channel.channel) || !channels.insert(channel.channel).second) return metadata_invalid();
            channel.encoding = output.at("encoding").get<std::string>();
            channel.capture_complete = output.at("capture_complete").get<bool>();
            channel.capture_reason = output.value("capture_reason", std::string());
            if (!channel.capture_complete && channel.capture_reason.empty()) return metadata_invalid();
            channel.output_bytes = Uint(output.at("output_bytes"));
            const auto count_kind = output.at("byte_count_kind").get<std::string>();
            if (count_kind != "exact" && count_kind != "lower_bound") return metadata_invalid();
            channel.output_bytes_lower_bound = count_kind == "lower_bound";
            const auto ref = std::find_if(entry.artifacts.begin(), entry.artifacts.end(),
                [&](const auto& artifact) { return artifact.kind == channel.channel; });
            if (!output.contains("ref")) {
                if (ref != entry.artifacts.end() || output.contains("captured_bytes")) return metadata_invalid();
                if (channel.capture_complete && (channel.output_bytes != 0 || channel.output_bytes_lower_bound)) return metadata_invalid();
                channel.state = State::Empty;
                channel.artifact_verified = true; // Verified descriptor establishes zero captured bytes, not complete execution.
                if (TextChannel(channel.channel) && channel.encoding == "utf-8") {
                    channel.media_type = "text/plain"; // Known text channel, zero captured bytes; no file was created.
                    channel.text = "";
                }
                else channel.state = State::MetadataOnly;
                data.channels.push_back(std::move(channel));
                continue;
            }
            if (ref == entry.artifacts.end()) return metadata_invalid();
            const auto& nested = output.at("ref");
            if (nested.at("artifact_id") != ref->id || nested.at("path") != ref->path || nested.at("sha256") != ref->sha256 ||
                Uint(nested.at("bytes")) != ref->bytes || nested.at("media_type") != ref->media_type ||
                Uint(output.at("captured_bytes")) != ref->bytes || channel.output_bytes < ref->bytes)
                return metadata_invalid();
            if (channel.capture_complete && (channel.output_bytes != ref->bytes || channel.output_bytes_lower_bound)) return metadata_invalid();
            used_refs.insert(ref->id);
            channel.artifact_id = ref->id;
            channel.media_type = ref->media_type;
            channel.sha256 = ref->sha256;
            channel.captured_bytes = ref->bytes;
            const bool textual = TextChannel(channel.channel) && channel.media_type == "text/plain" && channel.encoding == "utf-8";
            auto captured = ReadArtifact(session_dir, *ref, textual, verification_remaining, text_remaining);
            channel.state = captured.state;
            channel.artifact_verified = captured.verified;
            channel.issue_code = std::move(captured.issue);
            if (captured.verified && textual) {
                if (!lubancode::platform::IsValidUtf8(captured.data)) {
                    channel.state = State::Corrupt;
                    channel.issue_code = "sdk.result.invalid_utf8";
                } else {
                    text_remaining -= ref->bytes;
                    channel.text = std::move(captured.data);
                    if (ref->bytes == 0) channel.state = State::Empty;
                }
            } else if (captured.verified) {
                channel.state = State::MetadataOnly;
                channel.issue_code = "sdk.result.non_text";
            }
            data.channels.push_back(std::move(channel));
        }
        if (used_refs.size() + 1 != entry.artifacts.size()) return metadata_invalid();
        return results::detail::SnapshotAccess::Make(std::move(data), policy);
    } catch (...) {
        return std::unexpected(Failure("sdk.result.metadata_invalid", "result descriptor has an invalid shape"));
    }
}
} // namespace lubancore::detail
