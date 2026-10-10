#include "trajectory/v3/child_adoption.hpp"

#include <algorithm>
#include <expected>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "platform/bounded_read.hpp"
#include "platform/paths.hpp"
#include "platform/sha256.hpp"
#include "platform/text_encoding.hpp"
#include "trajectory/v3/subagent.hpp"
#include "trajectory/v3/result_store.hpp"

namespace lubancode::trajectory::v3 {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t kItemBytes = 64 * 1024 * 1024;
constexpr std::size_t kArtifactBytes = 256 * 1024 * 1024;
constexpr std::size_t kArtifactCount = 64;

struct Invalid : std::runtime_error { using std::runtime_error::runtime_error; };
struct Incomplete : std::runtime_error { using std::runtime_error::runtime_error; };
void Require(bool good, const char* code) { if (!good) throw Invalid(code); }
bool SafeText(std::string_view text, std::size_t cap = 1024) {
    return !text.empty() && text.size() <= cap && text.find('\0') == text.npos && platform::IsValidUtf8(std::string(text));
}
std::string Text(const Json& value) {
    Require(value.is_string(), "invalid_type");
    auto text = value.get<std::string>();
    Require(SafeText(text), "invalid_text");
    return text;
}
std::uint64_t Uint(const Json& value) {
    Require(value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0), "invalid_number");
    return value.get<std::uint64_t>();
}
bool Hash(std::string_view text) {
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == text.npos;
}
bool Inside(const fs::path& root, const fs::path& path) {
    auto a = root.begin(), b = path.begin();
    for (; a != root.end(); ++a, ++b) if (b == path.end() || *a != *b) return false;
    return b != path.end();
}
fs::path Canonical(const fs::path& path) {
    std::error_code error;
    auto actual = fs::canonical(path, error);
    Require(!error, "path_unavailable");
    return actual;
}
fs::path Relative(std::string_view text, std::string_view first) {
    Require(SafeText(text) && text.find_first_of("\\:") == text.npos, "invalid_relative_path");
    const auto path = platform::Utf8ToPath(std::string(text));
    Require(!path.is_absolute() && !path.has_root_path() && path.begin() != path.end() && *path.begin() == platform::Utf8ToPath(std::string(first)), "invalid_relative_path");
    for (const auto& part : path) Require(part != "." && part != "..", "invalid_relative_path");
    return path;
}
template<class Line> bool Owner(const Line& row, const V3Ledger& parent) {
    return row.session_id == parent.session_id && row.run_id == parent.run_id;
}
bool ActionOwner(const EventLine& row, const V3Ledger& parent, const ToolActionSnapshot& action) {
    return Owner(row, parent) && row.turn_id == action.turn_id && row.step_id == action.step_id && row.action_id == action.tool_call_id;
}
std::vector<std::string> LocalRefs(const Json& values,
    std::size_t cap = (std::numeric_limits<std::size_t>::max)()) {
    Require(values.is_array() && values.size() <= cap, "invalid_local_refs");
    std::vector<std::string> refs;
    std::set<std::string> distinct;
    for (const auto& value : values) {
        auto ref = Text(value); // Five-key cross-session objects are deliberately rejected.
        Require(distinct.insert(ref).second, "duplicate_local_ref");
        refs.push_back(std::move(ref));
    }
    return refs;
}
Json EventRef(const EventLine& event) {
    return {{"sessionId", event.session_id}, {"runId", event.run_id}, {"id", event.event_id},
            {"seq", event.seq}, {"hash", event.line_hash}};
}

struct ArtifactReader {
    const fs::path& parent_root;
    std::optional<fs::path> artifact_root;
    std::size_t used = 0;
    struct Entry { Json reference; std::string bytes; };
    std::map<std::string, Entry> opened;

    const std::string& Read(const Json& ref) {
        Require(ref.is_object() && ref.size() == 6, "invalid_artifact_ref");
        const auto path = Text(ref.at("path"));
        const auto hash = Text(ref.at("sha256"));
        const auto size = Uint(ref.at("bytes"));
        (void)Text(ref.at("artifactId"));
        (void)Text(ref.at("kind"));
        (void)Text(ref.at("mediaType"));
        Require(Hash(hash) && size <= kItemBytes, "invalid_artifact_size_or_hash");
        if (const auto prior = opened.find(path); prior != opened.end()) {
            Require(prior->second.reference == ref, "artifact_reference_conflict");
            return prior->second.bytes;
        }
        Require(opened.size() < kArtifactCount && size <= kArtifactBytes - used, "artifact_budget_exceeded");
        const auto relative = Relative(path, "artifacts");
        if (!artifact_root) {
            artifact_root = Canonical(parent_root / "artifacts");
            Require(Inside(parent_root, *artifact_root), "artifact_root_outside_parent");
        }
        const auto actual = Canonical(parent_root / relative);
        Require(Inside(*artifact_root, actual), "artifact_outside_parent");
        // Any failed bounded read ends this whole check immediately. Its extra
        // byte or partial read cannot be ignored to evade the cumulative cap.
        auto bytes = platform::ReadBoundedRegularFile(actual, (std::min)(kItemBytes, kArtifactBytes - used));
        if (!bytes) throw Invalid("artifact_" + bytes.error());
        used += bytes->size();
        Require(bytes->size() == size && platform::Sha256Hex(*bytes) == hash, "artifact_bytes_or_hash_mismatch");
        auto [where, inserted] = opened.emplace(path, Entry{ref, std::move(*bytes)});
        (void)inserted;
        return where->second.bytes;
    }
};

struct ResultSet {
    Json metadata;
    std::map<std::string, Json> outputs;
    ResultStore::PersistRequest material;
    ResultStore::PersistedResult persisted;
};
bool TerminalKind(EventKindV3 kind) {
    return kind == EventKindV3::ToolExecutionFinished || kind == EventKindV3::ToolExecutionFailed ||
        kind == EventKindV3::ToolExecutionCancelled || kind == EventKindV3::ToolExecutionRejected ||
        kind == EventKindV3::ToolExecutionUnknown;
}
ResultSet ReadResult(const EventLine& event, const V3Ledger& parent,
    const ToolActionSnapshot& action, std::uint64_t attempt, ArtifactReader& artifacts) {
    Require(event.kind == EventKindV3::ToolResultPersisted && ActionOwner(event, parent, action), "persisted_owner_mismatch");
    Require(Uint(event.payload.at("attempt")) == attempt && Text(event.payload.at("tool_call_id")) == action.tool_call_id, "persisted_attempt_mismatch");
    const auto execution = Text(event.payload.at("executionEventRef"));
    const auto* terminal = parent.FindEvent(execution);
    Require(terminal && TerminalKind(terminal->kind) && ActionOwner(*terminal, parent, action) && terminal->seq < event.seq &&
            Uint(terminal->payload.at("attempt")) == attempt, "persisted_execution_mismatch");
    const auto& refs = event.payload.at("result_ref");
    Require(refs.is_array() && !refs.empty() && refs.size() <= kArtifactCount, "invalid_result_refs");
    ResultSet result;
    bool has_metadata = false;
    for (const auto& ref : refs) {
        const auto kind = Text(ref.at("kind"));
        const auto& bytes = artifacts.Read(ref);
        if (kind == "result_metadata") {
            Require(!has_metadata, "duplicate_result_metadata");
            has_metadata = true;
            result.metadata = Json::parse(bytes);
            Require(result.metadata.is_object(), "invalid_result_metadata");
        } else Require(result.outputs.emplace(kind, ref).second, "duplicate_output_kind");
    }
    Require(has_metadata && Text(result.metadata.at("tool_call_id")) == action.tool_call_id &&
        Uint(result.metadata.at("attempt")) == attempt && Text(result.metadata.at("execution_event_ref")) == execution,
        "result_metadata_owner_mismatch");
    const auto& outputs = result.metadata.at("outputs");
    Require(outputs.is_array() && outputs.size() <= kArtifactCount, "invalid_metadata_outputs");
    std::set<std::string> seen;
    for (const auto& output : outputs) {
        const auto channel = Text(output.at("channel"));
        Require(seen.insert(channel).second, "duplicate_metadata_output");
        if (!output.contains("ref")) {
            Require(!result.outputs.contains(channel), "missing_metadata_output_ref");
        } else {
            const auto found = result.outputs.find(channel);
            Require(found != result.outputs.end(), "unlisted_metadata_output_ref");
            const auto& ref = found->second;
            const auto& recorded = output.at("ref");
            Require(Text(recorded.at("artifact_id")) == Text(ref.at("artifactId")) &&
                Text(recorded.at("path")) == Text(ref.at("path")) && Text(recorded.at("sha256")) == Text(ref.at("sha256")) &&
                Uint(recorded.at("bytes")) == Uint(ref.at("bytes")) && Text(recorded.at("media_type")) == Text(ref.at("mediaType")) &&
                Uint(output.at("captured_bytes")) == Uint(ref.at("bytes")), "metadata_output_ref_mismatch");
        }
        ResultStore::ChannelOutput restored;
        restored.channel = channel;
        if (const auto found = result.outputs.find(channel); found != result.outputs.end()) {
            restored.data = artifacts.Read(found->second);
            restored.media_type = Text(found->second.at("mediaType"));
        }
        Require(output.at("capture_complete").is_boolean(), "invalid_capture_complete");
        restored.capture_complete = output.at("capture_complete").get<bool>();
        if (!restored.capture_complete) restored.capture_reason = Text(output.at("capture_reason"));
        restored.output_bytes = Uint(output.at("output_bytes"));
        const auto count_kind = Text(output.at("byte_count_kind"));
        Require(count_kind == "exact" || count_kind == "lower_bound", "invalid_output_count_kind");
        restored.output_bytes_lower_bound = count_kind == "lower_bound";
        Require(Text(output.at("encoding")) == "utf-8", "unsupported_output_encoding");
        Require(platform::IsValidUtf8(restored.data), "invalid_output_encoding");
        result.material.outputs.push_back(std::move(restored));
    }
    for (const auto& [kind, ref] : result.outputs) {
        (void)ref;
        Require(seen.contains(kind), "unrepresented_output_ref");
    }
    result.material.content = result.metadata.at("content");
    Require(result.material.content.is_string(), "invalid_result_content");
    result.material.preview_policy = result.metadata.at("preview_policy");
    result.persisted.result_id = Text(result.metadata.at("result_id"));
    for (const auto& ref : refs) result.persisted.result_ref.push_back(ref);
    return result;
}

bool ToolOwner(const MessageLine& message, const V3Ledger& parent, const ToolActionSnapshot& action) {
    return Owner(message, parent) && message.turn_id == action.turn_id && message.step_id == action.step_id &&
        message.action_id == action.tool_call_id && message.purpose == MessagePurpose::Conversation &&
        message.message.value("role", std::string()) == "tool" &&
        message.message.value("tool_call_id", std::string()) == action.tool_call_id;
}
bool VersionOf(const V3Ledger& parent, const ToolActionSnapshot& action,
    const MessageLine& message, const MessageLine& original, const std::string& selected,
    std::uint64_t prepared_seq) {
    const MessageLine* current = &message;
    std::set<std::string> visited;
    while (current) {
        Require(ToolOwner(*current, parent, action) && current->result_selection_ref == selected,
                "derived_tool_owner_mismatch");
        if (current->message_id == original.message_id) return true;
        if (!current->source_tool_message_ref) return false;
        Require(visited.insert(current->message_id).second, "derived_tool_cycle");
        const auto* source = parent.FindMessage(*current->source_tool_message_ref);
        Require(source && source->seq < current->seq, "derived_tool_source_mismatch");
        const EventLine* reduction = nullptr;
        for (const auto& event : parent.events) {
            if (event.kind != EventKindV3::ContextToolPreviewsReduced) continue;
            const auto refs = LocalRefs(event.payload.at("replacementRefs"));
            if (std::find(refs.begin(), refs.end(), current->message_id) == refs.end()) continue;
            Require(!reduction && Owner(event, parent) && event.seq > current->seq && event.seq < prepared_seq,
                    "derived_tool_reduction_owner_or_order");
            const auto before = parent.revision_chains.find(Uint(event.payload.at("beforeRevision")));
            const auto after = parent.revision_chains.find(Uint(event.payload.at("afterRevision")));
            Require(before != parent.revision_chains.end() && after != parent.revision_chains.end() &&
                std::find(before->second.second.begin(), before->second.second.end(), source->message_id) != before->second.second.end() &&
                std::find(after->second.second.begin(), after->second.second.end(), current->message_id) != after->second.second.end() &&
                std::find(after->second.second.begin(), after->second.second.end(), source->message_id) == after->second.second.end(),
                "derived_tool_reduction_chain_mismatch");
            Require(current->message.at("content").is_string() && current->message.at("content").get_ref<const std::string&>().size() <=
                Uint(event.payload.at("newPreviewBudget")), "derived_tool_reduction_budget_mismatch");
            reduction = &event;
        }
        Require(reduction, "derived_tool_reduction_missing");
        current = source;
    }
    return false;
}
ChildAdoptionCheck Verdict(ChildAdoptionState state, std::string_view issue) {
    return {state, "subagent.adoption." + std::string(issue), std::nullopt};
}
} // namespace

ChildAdoptionCheck ValidateChildAdoption(const V3Ledger& parent, const fs::path& parent_dir,
    std::string_view turn_id, std::string_view action_id, std::uint64_t attempt,
    const std::function<void(const V3Ledger&)>& checked_child) {
    try {
        Require(SafeText(parent.session_id) && SafeText(parent.run_id) && SafeText(turn_id) && SafeText(action_id) && attempt,
                "invalid_scope");
        const auto snapshots = FoldToolActions(parent);
        const auto* action = FindActionSnapshot(snapshots, action_id);
        Require(action && action->turn_id == turn_id && !action->step_id.empty() && action->assistant_message_ref,
                "action_scope_mismatch");
        Require(std::any_of(action->attempts.begin(), action->attempts.end(), [attempt](const auto& item) { return item.attempt == attempt; }),
                "attempt_unavailable");
        const auto* declaration = parent.FindMessage(*action->assistant_message_ref);
        Require(declaration && Owner(*declaration, parent) && declaration->turn_id == turn_id && declaration->step_id == action->step_id,
                "declaration_owner_mismatch");
        std::vector<const EventLine*> spawns, observations;
        for (const auto& event : parent.events) {
            if (event.action_id != action_id) continue;
            if (event.kind == EventKindV3::SubagentSpawnRequested) {
                Require(ActionOwner(event, parent, *action), "spawn_owner_mismatch");
                if (Uint(event.payload.at("attempt")) == attempt) spawns.push_back(&event);
            } else if (event.kind == EventKindV3::SubagentObserved) {
                Require(ActionOwner(event, parent, *action), "observation_owner_mismatch");
                observations.push_back(&event);
            }
        }
        if (spawns.empty() && observations.empty()) return Verdict(ChildAdoptionState::NotApplicable, "no_child_material");
        Require(spawns.size() == 1, "spawn_ambiguous_or_missing");
        const auto& spawn = *spawns.front();
        const auto task = Text(spawn.payload.at("taskId"));
        std::erase_if(observations, [&](const auto* event) {
            const auto payload_task = Text(event->payload.at("taskId"));
            if (event->task_id != task && payload_task != task) return true;
            Require(event->task_id == task && payload_task == task, "observation_task_mismatch");
            return false;
        });
        if (observations.empty()) return Verdict(ChildAdoptionState::Incomplete, "observation_pending");
        Require(observations.size() == 1, "observation_ambiguous");
        const auto& observed = *observations.front();
        const auto& display = observed.payload.at("display");
        if (!display.is_object() || !display.contains("schemaVersion")) return Verdict(ChildAdoptionState::NotApplicable, "legacy_observation");
        Require(Uint(display.at("schemaVersion")) == 1, "unsupported_observation_version");
        Require(declaration->seq < spawn.seq && spawn.seq < observed.seq, "spawn_observation_order_mismatch");
        Require(spawn.task_id == task && observed.task_id == task && Text(observed.payload.at("taskId")) == task,
                "task_owner_mismatch");
        const ParentActionRef parent_action{parent.session_id, parent.run_id, std::string(turn_id), action->step_id,
                                           std::string(action_id), *action->assistant_message_ref};
        Require(spawn.payload.at("parentActionRef") == parent_action.ToJson(), "spawn_action_ref_mismatch");

        ChildHistoricalAdoption value;
        value.parent_session_id = parent.session_id; value.parent_run_id = parent.run_id;
        value.turn_id = turn_id; value.action_id = action_id; value.attempt = attempt;
        value.spawn_event_id = spawn.event_id; value.observation_event_id = observed.event_id;
        const auto& child_ref = spawn.payload.at("childSessionRef");
        value.child_session_id = Text(child_ref.at("sessionId")); value.child_run_id = Text(child_ref.at("runId"));
        const auto relative = Relative(Text(child_ref.at("journalPath")), "subagents");
        value.producer_execution_claim = Text(display.at("execution"));
        value.producer_append_claim = Text(display.at("appendConfirmation"));
        value.producer_seal_claim = Text(display.at("seal"));
        Require(value.producer_append_claim == "committed" && value.producer_seal_claim == "closed" &&
            (value.producer_execution_claim == "succeeded" || value.producer_execution_claim == "failed" ||
             value.producer_execution_claim == "cancelled" || value.producer_execution_claim == "startup_rejected"),
            "producer_confirmation_unavailable");
        const auto parent_root = Canonical(parent_dir);
        Require(Canonical(parent.path.parent_path()) == parent_root, "parent_directory_mismatch");
        const auto children_root = Canonical(parent_root / "subagents");
        Require(Inside(parent_root, children_root), "child_root_outside_parent");
        const auto actual_child = Canonical(parent_root / relative);
        Require(Inside(children_root, actual_child), "child_outside_parent");
        auto child = ReadV3LedgerBounded(actual_child, kItemBytes, 131072, 4 * 1024 * 1024);
        if (!child) throw Invalid("child_source_invalid:" + child.error());
        Require(child->session_id == value.child_session_id && child->run_id == value.child_run_id &&
                !child->messages.empty() && child->messages.front().seq == 1 && child->messages.front().system_meta,
                "child_owner_mismatch");
        for (const auto& message : child->messages) Require(Owner(message, *child), "child_row_owner_mismatch");
        std::size_t ended = 0;
        for (const auto& event : child->events) {
            Require(Owner(event, *child), "child_row_owner_mismatch");
            if (event.kind == EventKindV3::SessionEnded) ++ended;
        }
        const auto& terminal_ref = display.at("terminalRef");
        Require(terminal_ref.is_object() && terminal_ref.size() == 5, "invalid_terminal_ref");
        value.child_terminal_event_id = Text(terminal_ref.at("id"));
        const auto* terminal = child->FindEvent(value.child_terminal_event_id);
        const auto last = child->LastEntry();
        Require(ended == 1 && terminal && terminal->kind == EventKindV3::SessionEnded && last && !last->is_message &&
            terminal->seq == last->seq && terminal_ref == EventRef(*terminal), "child_terminal_mismatch");
        value.child_terminal_seq = terminal->seq; value.child_terminal_hash = terminal->line_hash;
        value.child_terminal_reason = terminal->payload.at("reason").get<std::string>();
        Require(value.child_terminal_reason.size() <= 4 * 1024 * 1024 && platform::IsValidUtf8(value.child_terminal_reason), "invalid_terminal_reason");
        value.child_close_quality = Text(terminal->payload.at("closeQuality"));
        Require(value.child_close_quality == (value.producer_execution_claim == "succeeded" ? "clean" : "incomplete"),
                "producer_terminal_quality_mismatch");
        Require(observed.payload.at("childCheckpointRef") ==
                ChildCheckpointRef{value.child_session_id, value.child_run_id, terminal->seq, terminal->line_hash}.ToJson(),
                "checkpoint_terminal_mismatch");
        const auto& meta = *child->messages.front().system_meta;
        Require(meta.at("parentActionRef") == parent_action.ToJson() && Text(meta.at("taskId")) == task &&
                meta.at("spawnEventRef") == EventRef(spawn), "child_parent_source_mismatch");

        ArtifactReader artifacts{parent_root};
        const EventLine* raw = nullptr;
        ResultSet raw_result;
        for (const auto& event : parent.events) {
            if (event.kind != EventKindV3::ToolResultPersisted || event.action_id != action_id) continue;
            if (Uint(event.payload.at("attempt")) != attempt) continue;
            Require(event.seq > observed.seq, "raw_before_observation");
            auto material = ReadResult(event, parent, *action, attempt, artifacts);
            if (material.metadata.at("preview_policy").value("policy", std::string()) == "raw-capture-before-post-hook") {
                Require(!raw, "raw_capture_ambiguous");
                raw = &event; raw_result = std::move(material);
            }
        }
        if (!raw) return Verdict(ChildAdoptionState::Incomplete, "raw_capture_pending");
        value.raw_persisted_event_id = raw->event_id;
        value.raw_text_sha256 = Text(display.at("rawTextSha256")); value.raw_text_bytes = Uint(display.at("rawTextBytes"));
        Require(Hash(value.raw_text_sha256) && value.raw_text_bytes <= kItemBytes && raw_result.metadata.at("content").is_string(),
                "invalid_raw_content");
        const auto raw_text = raw_result.metadata.at("content").get<std::string>();
        Require(raw_text.size() == value.raw_text_bytes && platform::Sha256Hex(raw_text) == value.raw_text_sha256,
                "raw_content_mismatch");
        if (const auto combined = raw_result.outputs.find("combined"); combined != raw_result.outputs.end())
            Require(artifacts.Read(combined->second) == raw_text, "raw_combined_mismatch");
        else Require(raw_text.empty(), "raw_combined_missing");
        Require(display.contains("structuredSha256") == display.contains("structuredBytes") &&
                display.contains("structuredSha256") == raw_result.metadata.contains("structured_content"), "raw_structured_presence_mismatch");
        if (display.contains("structuredSha256")) {
            value.raw_structured_sha256 = Text(display.at("structuredSha256"));
            value.raw_structured_bytes = Uint(display.at("structuredBytes"));
            const auto structured = raw_result.metadata.at("structured_content").dump();
            Require(Hash(*value.raw_structured_sha256) && structured.size() == *value.raw_structured_bytes &&
                structured.size() <= kItemBytes && platform::Sha256Hex(structured) == *value.raw_structured_sha256, "raw_structured_mismatch");
        }
        const EventLine* selected = nullptr;
        for (const auto& event : parent.events) {
            if (event.kind != EventKindV3::ToolResultSelected || event.action_id != action_id) continue;
            if (Uint(event.payload.at("attempt")) != attempt) continue;
            Require(!selected && ActionOwner(event, parent, *action) && Text(event.payload.at("tool_call_id")) == action_id,
                    "selected_owner_or_ambiguity");
            selected = &event;
        }
        if (!selected) return Verdict(ChildAdoptionState::Incomplete, "selection_pending");
        Require(raw->seq < selected->seq, "raw_selection_order_mismatch");
        value.selected_event_id = selected->event_id;
        value.selected_source_event_ids = LocalRefs(selected->payload.at("sourceResultEventRefs"), kArtifactCount);
        Require(!value.selected_source_event_ids.empty() && value.selected_source_event_ids.front() == raw->event_id,
                "selected_raw_source_mismatch");
        std::optional<ResultSet> effective_material;
        for (const auto& id : value.selected_source_event_ids) {
            if (id == raw->event_id) continue;
            const auto* persisted = parent.FindEvent(id);
            Require(persisted && persisted->seq > raw->seq && persisted->seq < selected->seq, "effective_result_order_mismatch");
            auto effective = ReadResult(*persisted, parent, *action, attempt, artifacts);
            Require(effective.metadata.at("preview_policy").value("policy", std::string()) == "v3-tool-preview", "unsupported_effective_policy");
            if (effective_material) throw Incomplete("effective_merge_unavailable");
            effective_material = std::move(effective);
            value.effective_persisted_event_ids.push_back(id);
        }
        Require(!value.effective_persisted_event_ids.empty(), "selected_effective_source_missing");
        const MessageLine* original = nullptr;
        for (const auto& message : parent.messages) {
            if (message.result_selection_ref != selected->event_id || message.source_tool_message_ref) continue;
            Require(!original && ToolOwner(message, parent, *action) && selected->seq < message.seq, "tool_message_owner_or_order");
            original = &message;
        }
        if (!original) return Verdict(ChildAdoptionState::Incomplete, "tool_message_pending");
        value.original_tool_message_id = original->message_id;
        const auto projection = ProjectResultPreview(parent, original->message_id);
        Require(projection.complete && projection.summary_valid && projection.source_result_event_refs == value.selected_source_event_ids,
                "tool_result_projection_invalid");
        const auto budget = Uint(effective_material->material.preview_policy.at("maxPreviewBytes"));
        Require(budget > 0 && budget <= 32768, "invalid_preview_budget");
        const auto content = effective_material->material.content.get<std::string>();
        const auto combined = effective_material->outputs.find("combined");
        Require((combined == effective_material->outputs.end() && content.empty()) ||
            (combined != effective_material->outputs.end() && artifacts.Read(combined->second) == content),
            "effective_combined_mismatch");
        const auto channels = PreviewFromPersistedMaterials(effective_material->material,
            effective_material->persisted, budget, parent.path.parent_path());
        Require(!channels.channels.empty() && channels.channels.front().channel == "combined", "missing_effective_combined");
        if (projection.summary_event_ref.empty()) {
            std::string expected = content;
            if (content.size() > budget || !channels.channels.front().capture_complete || channels.channels.size() > 1) {
                const auto preview = BuildToolPreview(channels);
                // The live producer's extreme listing file has no six-key
                // immutable ref in the persisted record. Do not guess its
                // identity from a path and claim a complete historical chain.
                if (preview.listing_overflow) throw Incomplete("output_index_identity_unavailable");
                Require(!preview.preview_unrepresentable && preview.text.size() <= budget, "preview_unrepresentable");
                expected = preview.text;
            }
            Require(projection.result_preview == expected, "tool_preview_material_mismatch");
        } else {
            const auto* summary = parent.FindEvent(projection.summary_event_ref);
            Require(summary && Owner(*summary, parent) && summary->turn_id == action->turn_id &&
                summary->action_id == action_id && Uint(summary->payload.at("attempt")) == attempt &&
                Uint(summary->payload.at("budgetBytes")) == budget && projection.result_preview.size() <= budget, "summary_owner_mismatch");
            for (const auto& id : projection.summary_candidate_refs) {
                const auto* candidate = parent.FindMessage(id);
                Require(candidate && Owner(*candidate, parent) && candidate->request_id && candidate->seq < summary->seq,
                        "summary_candidate_owner_mismatch");
                const EventLine* request = nullptr;
                for (const auto& event : parent.events) {
                    if (event.kind != EventKindV3::ModelRequestPrepared || event.request_id != candidate->request_id) continue;
                    Require(!request, "summary_request_ambiguous"); request = &event;
                }
                Require(request && Owner(*request, parent) && request->turn_id == candidate->turn_id &&
                    request->step_id == candidate->step_id && request->seq < candidate->seq &&
                    request->payload.value("purpose", std::string()) == "action_summary" &&
                    Text(request->payload.at("sourceActionId")) == action_id &&
                    LocalRefs(request->payload.at("sourceResultEventRefs"), kArtifactCount) == value.selected_source_event_ids,
                    "summary_request_source_mismatch");
                const auto prompts = LocalRefs(request->payload.at("inputMessageRefs"));
                Require(!prompts.empty(), "summary_prompt_missing");
                for (const auto& id : prompts) {
                    const auto* prompt = parent.FindMessage(id);
                    Require(prompt && Owner(*prompt, parent) && prompt->purpose == MessagePurpose::ActionSummary &&
                        prompt->parent_turn_id == action->turn_id && prompt->action_id == action_id &&
                        prompt->seq < request->seq, "summary_prompt_owner_mismatch");
                }
            }
        }
        const auto outcome = Text(selected->payload.at("effectiveOutcome"));
        Require((outcome == "done" || outcome == "failed") &&
            original->message.value("is_error", false) == (outcome == "failed"), "selected_effective_outcome_mismatch");
        const EventLine* admitted = nullptr;
        for (const auto& event : parent.events) {
            if (event.kind != EventKindV3::ContextInputApplied) continue;
            const auto refs = LocalRefs(event.payload.at("addedMessageRefs"));
            if (std::find(refs.begin(), refs.end(), original->message_id) == refs.end()) continue;
            Require(!admitted && Owner(event, parent) && event.seq > original->seq, "admission_owner_or_order");
            const auto revision = Uint(event.payload.at("afterRevision"));
            const auto chain = parent.revision_chains.find(revision);
            Require(chain != parent.revision_chains.end() &&
                std::find(chain->second.second.begin(), chain->second.second.end(), original->message_id) != chain->second.second.end(),
                "admission_chain_mismatch");
            admitted = &event;
        }
        if (!admitted) return Verdict(ChildAdoptionState::Incomplete, "admission_pending");
        value.admission_event_id = admitted->event_id;
        for (const auto& event : parent.events) {
            if (event.kind != EventKindV3::ModelRequestPrepared || event.seq <= admitted->seq ||
                event.payload.value("purpose", std::string()) != "conversation") continue;
            Require(Owner(event, parent), "prepared_owner_mismatch");
            const auto refs = LocalRefs(event.payload.at("inputMessageRefs"));
            const MessageLine* consumed = nullptr;
            for (const auto& id : refs) {
                const auto* message = parent.FindMessage(id);
                if (!message || message->action_id != action_id) continue;
                Require(!consumed && message->seq < event.seq && VersionOf(parent, *action, *message, *original, selected->event_id, event.seq),
                        "prepared_tool_version_mismatch");
                consumed = message;
            }
            if (!consumed) continue;
            Require(CheckPreparedAgainstChain(parent, event.event_id).empty(), "prepared_chain_mismatch");
            value.consumed_tool_message_id = consumed->message_id; value.prepared_event_id = event.event_id;
            value.prepared_context_revision = Uint(event.payload.at("contextRevision"));
            if (checked_child) checked_child(*child);
            return {ChildAdoptionState::Validated, {}, std::move(value)};
        }
        if (checked_child) checked_child(*child);
        return Verdict(ChildAdoptionState::Incomplete, "prepared_consumption_pending");
    } catch (const Incomplete& error) {
        return Verdict(ChildAdoptionState::Incomplete, error.what());
    } catch (const Invalid& error) {
        return Verdict(ChildAdoptionState::Rejected, error.what());
    } catch (const std::exception&) {
        return Verdict(ChildAdoptionState::Rejected, "invalid_material");
    }
}
} // namespace lubancode::trajectory::v3
