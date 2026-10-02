#include "child_observation_fixture.hpp"

#include <map>
#include <sstream>
#include <variant>

#include "agent/tool_batch_budget.hpp"
#include "runtime/v3_tool_result_material.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/child_adoption.hpp"
#include "trajectory/v3/compact.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;

v3::ChildAdoptionCheck CheckHistory(const Rig& rig, const v3::V3Ledger& source, std::size_t index = 0) {
    REQUIRE(rig.child_sources.size() > index);
    const auto& call = rig.child_sources[index].parent_action;
    const auto parent_calls = rig.backend.parent_calls, child_calls = rig.backend.child_calls;
    const auto summary_calls = rig.backend.summary_calls, tools = rig.after->calls;
    const auto parent_bytes = Read(rig.writer->path());
    const auto child_bytes = Read(rig.child_paths[index]);
    const auto before_seq = rig.writer->next_seq();
    auto result = v3::ValidateChildAdoption(source, rig.directory.root, call.turn_id, call.action_id, 1);
    CHECK(rig.backend.parent_calls == parent_calls); CHECK(rig.backend.child_calls == child_calls);
    CHECK(rig.backend.summary_calls == summary_calls); CHECK(rig.after->calls == tools);
    CHECK(Read(rig.writer->path()) == parent_bytes); CHECK(Read(rig.child_paths[index]) == child_bytes);
    CHECK(rig.writer->next_seq() == before_seq);
    if (result.state != v3::ChildAdoptionState::Validated) CHECK_FALSE(result.adoption.has_value());
    return result;
}

void RealRun(Rig& rig) {
    const auto run = rig.Run();
    REQUIRE_MESSAGE(run.has_value(), (run ? std::string() : run.error()));
    std::string capture_errors;
    for (const auto& receipt : rig.captures) {
        if (!capture_errors.empty()) capture_errors += "; ";
        capture_errors += "status=" + std::to_string(static_cast<int>(receipt.status)) +
            ",error=" + receipt.error_code;
    }
    INFO("original_side_effect_error=" << run->side_effect_error);
    INFO("actual_capture_receipts=" << capture_errors);
    CHECK_FALSE(run->side_effect_indeterminate);
    CHECK(rig.backend.parent_calls >= 2); CHECK(rig.backend.child_calls >= 1);
    rig.CheckChild();
}

const v3::EventLine& FindKind(const v3::V3Ledger& source, v3::EventKindV3 kind, std::string_view action) {
    const auto found = std::find_if(source.events.begin(), source.events.end(), [kind, action](const auto& row) {
        return row.kind == kind && row.action_id == action;
    });
    REQUIRE(found != source.events.end());
    return *found;
}

// These are altered versions of a real runtime-produced ledger. Rehash through
// the common canonical/hash implementation, then REQUIRE common Reader/Verify
// success before attributing rejection to the private adoption validator.
std::string Alter(const std::string& original, const std::function<void(std::vector<Json>&)>& change) {
    std::istringstream stream(original);
    std::string line;
    std::vector<Json> lines;
    while (std::getline(stream, line)) if (!line.empty()) lines.push_back(Json::parse(line));
    REQUIRE_FALSE(lines.empty());
    change(lines);
    std::string previous(v3::kGenesisHash), output;
    std::uint64_t seq = 0;
    for (auto& row : lines) {
        row["seq"] = ++seq;
        if (row.value("kind", std::string()) == "model.request.prepared") {
            row["payload"]["readThroughSeq"] = seq - 1;
            row["payload"]["readThroughHash"] = previous;
        }
        row.erase("prevHash"); row.erase("lineHash");
        const auto canonical = trajectory::CanonicalJsonDump(row); REQUIRE(canonical.has_value());
        const auto hash = v3::ComputeLineHash(previous, *canonical);
        row["prevHash"] = previous; row["lineHash"] = hash;
        const auto full = trajectory::CanonicalJsonDump(row); REQUIRE(full.has_value());
        output += *full + '\n'; previous = hash;
    }
    return output;
}

Json& Row(std::vector<Json>& rows, const std::string& id) {
    for (auto& row : rows) if (row.value("eventId", std::string()) == id || row.value("messageId", std::string()) == id) return row;
    FAIL("actual row is missing");
    throw std::runtime_error("actual row is missing");
}

struct Restore {
    fs::path path;
    std::string original;
    explicit Restore(fs::path target) : path(std::move(target)), original(Read(path)) {}
    ~Restore() {
        std::error_code error;
        // The fixture owns this exact temporary leaf. A directory/FIFO/symlink
        // fault must be removed before restoring the original regular file.
        fs::remove(path, error);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(original.data(), static_cast<std::streamsize>(original.size()));
    }
};

v3::V3Ledger Verified(const fs::path& path) {
    const auto verified = v3::VerifyV3File(path);
    REQUIRE_MESSAGE(verified.ok, (verified.error_code + ":" + verified.message));
    const auto source = v3::ReadV3Ledger(path);
    REQUIRE_MESSAGE(source.has_value(), (source ? std::string() : source.error()));
    return *source;
}

void CloseParent(Rig& rig) {
    const auto closed = rig.writer->Close(); REQUIRE_MESSAGE(closed.has_value(), (closed ? std::string() : closed.error()));
}

std::map<std::string, std::string> ArtifactBytes(const fs::path& root) {
    std::map<std::string, std::string> values;
    for (const auto& file : fs::directory_iterator(root / "artifacts"))
        if (fs::is_regular_file(file.symlink_status())) values.emplace(file.path().filename().string(), Read(file.path()));
    return values;
}
} // namespace

TEST_CASE("child history adoption validates only an actual full chain and returns owned values") {
    Directory directory;
    std::optional<v3::ChildHistoricalAdoption> owned;
    {
        Rig rig(directory); RealRun(rig);
        const auto source = rig.Source(); const auto original_artifacts = ArtifactBytes(directory.root);
        const auto checked = CheckHistory(rig, source);
        REQUIRE_MESSAGE(checked.state == v3::ChildAdoptionState::Validated, checked.issue);
        REQUIRE(checked.adoption.has_value()); owned = checked.adoption;
        const auto& value = *owned;
        CHECK(value.parent_session_id == rig.writer->session_id()); CHECK(value.parent_run_id == rig.writer->run_id());
        CHECK(value.action_id == rig.child_sources.front().parent_action.action_id); CHECK(value.attempt == 1);
        CHECK(value.spawn_event_id == rig.child_sources.front().spawn.event_id);
        CHECK(value.child_terminal_hash == rig.terminals.front().terminal->hash);
        CHECK(value.child_terminal_event_id == rig.terminals.front().terminal->event_id);
        CHECK(value.producer_execution_claim == "succeeded"); CHECK(value.producer_append_claim == "committed");
        CHECK(value.producer_seal_claim == "closed"); CHECK(value.child_close_quality == "clean");
        CHECK(value.raw_text_sha256 == platform::Sha256Hex(rig.raw_child_text)); CHECK(value.raw_text_bytes == rig.raw_child_text.size());
        REQUIRE(value.selected_source_event_ids.size() == 2); REQUIRE(value.effective_persisted_event_ids.size() == 1);
        CHECK(value.selected_source_event_ids.front() == value.raw_persisted_event_id);
        CHECK_FALSE(value.prepared_event_id.empty()); CHECK_FALSE(value.admission_event_id.empty());
        CHECK(v3::CheckPreparedAgainstChain(source, value.prepared_event_id).empty());
        REQUIRE(source.FindMessage(value.consumed_tool_message_id));
        const auto second = CheckHistory(rig, source); REQUIRE(second.adoption.has_value());
        CHECK(second.adoption->prepared_event_id == value.prepared_event_id);
        CHECK(ArtifactBytes(directory.root) == original_artifacts);
        CloseParent(rig); const auto closed_query = CheckHistory(rig, source);
        CHECK(closed_query.state == v3::ChildAdoptionState::Validated);
    }
    REQUIRE(owned.has_value()); CHECK_FALSE(owned->prepared_event_id.empty()); CHECK(owned->producer_seal_claim == "closed");
    std::cout << "[child-adoption-path] complete\n";
}

TEST_CASE("child history keeps raw and actual PostToolUse effective material separate") {
    // Regression for the real planner -> native-material producer -> result
    // store -> preview path. The original live Agent/bridge run below retains
    // its capacity and verifies actual consumption by the next model request.
    {
        Directory material_directory;
        auto store = v3::ResultStore::Open(material_directory.root);
        REQUIRE_MESSAGE(store.has_value(), (store ? std::string() : store.error()));
        for (int shape = 0; shape != 5; ++shape) {
            INFO("native material shape=" << shape);
            api::ToolResultBlock result;
            result.tool_use_id = "budget-shape-" + std::to_string(shape);
            result.content = "short child answer";
            if (shape == 1) result.blocks = {tools::TextContent{result.content}};
            if (shape == 2) result.blocks = {tools::TextContent{"distinct native answer"}};
            if (shape == 3) result.blocks = {tools::TextContent{result.content}, tools::TextContent{"actual feedback"}};
            if (shape == 4) {
                tools::ResourceLinkContent link;
                link.uri = "file:///native-material";
                link.name = "native material";
                result.blocks = {std::move(link)};
            }
            const bool needs_raw = shape >= 2;
            api::Message batch;
            batch.content = {result};
            const auto full = agent::PlanToolBatchBudget(batch, 65536);
            REQUIRE(full.error.empty()); REQUIRE(full.preview_bytes.size() == 1);
            CHECK(full.preview_bytes.front() == (needs_raw ? std::size_t{32768} : result.content.size()));
            const auto plan = agent::PlanToolBatchBudget(batch, 4096);
            REQUIRE(plan.error.empty()); REQUIRE(plan.preview_bytes.size() == 1);
            CHECK(plan.preview_bytes.front() == (needs_raw ? std::size_t{4096} : result.content.size()));
            CHECK(plan.total_preview_bytes <= 4096);
            v3::ResultStore::PersistRequest material;
            material.result_kind = "text";
            material.tool_call_id = result.tool_use_id;
            material.outputs.push_back({"combined", "text/plain", result.content, true, {},
                static_cast<std::uint64_t>(result.content.size()), false});
            runtime::PreserveNativeToolPayload(result, material);
            REQUIRE(material.outputs.size() == (needs_raw ? 2 : 1));
            const auto stored = store->Persist(material);
            REQUIRE_MESSAGE(stored.ok, stored.error);
            for (const auto& output : material.outputs) {
                const auto ref = std::find_if(stored.result_ref.begin(), stored.result_ref.end(), [&](const auto& value) {
                    return value.at("kind").template get<std::string>() == output.channel;
                });
                REQUIRE(ref != stored.result_ref.end());
                const auto path = material_directory.root / platform::Utf8ToPath(ref->at("path").get<std::string>());
                CHECK(Read(path) == output.data);
            }
            if (!needs_raw) {
                result.capture_complete = false;
                batch.content = {result};
                const auto incomplete = agent::PlanToolBatchBudget(batch, 4096);
                REQUIRE(incomplete.error.empty()); CHECK(incomplete.preview_bytes.front() == 1024);
                continue;
            }
            CHECK(material.outputs.back().channel == "raw_payload");
            Json blocks = Json::array();
            for (const auto& block : result.blocks) blocks.push_back(tools::BlockToJson(block));
            CHECK(Json::parse(material.outputs.back().data) == blocks);
            auto request = v3::PreviewFromPersistedMaterials(material, stored, plan.preview_bytes.front(), material_directory.root);
            const auto preview = v3::BuildToolPreview(request);
            REQUIRE_FALSE(preview.preview_unrepresentable);
            CHECK(preview.text.size() <= plan.preview_bytes.front());
            REQUIRE(request.channels.size() == 2);
            for (const auto& channel : request.channels) {
                CHECK(preview.text.find(channel.display_path) != std::string::npos);
                CHECK(preview.text.find(channel.channel) != std::string::npos);
            }
            // The old body-only cap cannot carry the actual persisted sources.
            request.max_preview_bytes = result.content.size();
            CHECK(v3::BuildToolPreview(request).preview_unrepresentable);
            CHECK(agent::PlanToolBatchBudget(batch, 1023).error == "tool_batch.minimum_preview_exceeds_capacity");
            const auto minimum = agent::PlanToolBatchBudget(batch, 1024);
            REQUIRE(minimum.error.empty()); CHECK(minimum.total_preview_bytes == 1024);
            // Renderer-only boundary requests use a deliberately long display
            // path; they do not claim that such a file exists. The original
            // payload files above and the index below are really persisted.
            // A short first full_output item permits genuine index rescue.
            request.max_preview_bytes = plan.preview_bytes.front();
            request.channels.back().display_path.assign(8192, 'p');
            const auto overflow = v3::BuildToolPreview(request);
            REQUIRE(overflow.listing_overflow);
            const auto listing = store->PersistListing("long-source-" + std::to_string(shape), overflow.listing_text);
            REQUIRE_MESSAGE(listing.has_value(), (listing ? std::string() : listing.error()));
            request.output_index_path = platform::PathToUtf8(
                (material_directory.root / platform::Utf8ToPath(*listing)).lexically_normal());
            CHECK(Read(material_directory.root / platform::Utf8ToPath(*listing)) == overflow.listing_text);
            const auto rescued = v3::BuildToolPreview(request);
            CHECK_FALSE(rescued.preview_unrepresentable);
            CHECK(rescued.output_index_path == request.output_index_path);
            CHECK(rescued.omitted_output_count == 1);
            CHECK(rescued.text.size() <= plan.preview_bytes.front());
            // The 1 KiB floor cannot guarantee arbitrary mandatory sources.
            // Unlike full_output, captured_output must retain this long path.
            request.max_preview_bytes = minimum.preview_bytes.front();
            request.channels.back().capture_complete = false;
            request.channels.back().capture_reason = "renderer-long-source-fixture";
            CHECK(v3::BuildToolPreview(request).preview_unrepresentable);
        }
        auto mixed = api::Message{};
        api::ToolResultBlock plain, rich;
        plain.tool_use_id = "plain"; plain.content = "ten-bytes!";
        plain.blocks = {tools::TextContent{plain.content}};
        rich.tool_use_id = "rich"; rich.content = "short";
        rich.blocks = {tools::TextContent{"short"}, tools::TextContent{"feedback"}};
        mixed.content = {plain, rich};
        const auto shared = agent::PlanToolBatchBudget(mixed, 4106);
        REQUIRE(shared.error.empty());
        CHECK(shared.preview_bytes == std::vector<std::size_t>{10, 4096});
        CHECK(shared.total_preview_bytes == 4106);
        plain.content.clear(); plain.blocks = {tools::TextContent{plain.content}};
        mixed.content = {plain};
        const auto empty = agent::PlanToolBatchBudget(mixed, 1);
        REQUIRE(empty.error.empty()); CHECK(empty.total_preview_bytes == 1);
    }
    Directory directory; Rig rig(directory);
    rig.configure_wiring = [](agent::TurnWiring& wiring) {
        wiring.on_post_tool_use_hook = [](const std::string&, const std::string& name,
            const Json&, const tools::Tool::Result&) -> std::vector<std::string> {
            return name == "agent" ? std::vector<std::string>{"actual post-hook feedback"} : std::vector<std::string>{};
        };
    };
    RealRun(rig); const auto source = rig.Source(); const auto checked = CheckHistory(rig, source);
    REQUIRE_MESSAGE(checked.state == v3::ChildAdoptionState::Validated, checked.issue); REQUIRE(checked.adoption.has_value());
    const auto* tool = source.FindMessage(checked.adoption->consumed_tool_message_id); REQUIRE(tool);
    const auto effective = tool->message.at("content").get<std::string>();
    CHECK(effective.find("actual post-hook feedback") != std::string::npos);
    CHECK(rig.raw_child_text.find("actual post-hook feedback") == std::string::npos);
    CHECK(checked.adoption->raw_text_sha256 == platform::Sha256Hex(rig.raw_child_text));
    CHECK(checked.adoption->raw_text_sha256 != platform::Sha256Hex(effective));
    int seen = 0;
    for (const auto& message : rig.backend.parent_requests.back().messages)
        for (const auto& block : message.content)
            if (const auto* result = std::get_if<api::ToolResultBlock>(&block); result && result->content == effective) ++seen;
    CHECK(seen == 1);
    {
        Directory summarized_directory; Rig summarized(summarized_directory);
        summarized.parent->SetContextWindowTokens(32768);
        summarized.configure_wiring = [](agent::TurnWiring& wiring) {
            wiring.on_post_tool_use_hook = [](const std::string&, const std::string& name,
                const Json&, const tools::Tool::Result&) -> std::vector<std::string> {
                return name == "agent" ? std::vector<std::string>{std::string(200000, 'H')} : std::vector<std::string>{};
            };
        };
        RealRun(summarized); const auto summary_source = summarized.Source();
        const auto full = CheckHistory(summarized, summary_source);
        REQUIRE_MESSAGE(full.state == v3::ChildAdoptionState::Validated, full.issue); REQUIRE(full.adoption);
        REQUIRE(summarized.backend.summary_calls > 0);
        const auto projected = v3::ProjectResultPreview(summary_source, full.adoption->original_tool_message_id);
        CHECK(projected.summary_valid); CHECK_FALSE(projected.summary_event_ref.empty());
        CHECK_FALSE(projected.summary_candidate_refs.empty());
    }
    std::cout << "[child-adoption-path] post-hook\n";
}

TEST_CASE("child history remains valid after native compact removes its old current-chain version") {
    {
        Directory reduced_directory; Rig reduced(reduced_directory);
        std::string replacement;
        reduced.configure_wiring = [&](agent::TurnWiring& wiring) {
            const auto rewrite = wiring.rewrite_tool_results_for_history;
            wiring.rewrite_tool_results_for_history = [&, rewrite](api::Message& results) {
                const auto committed = rewrite(results);
                REQUIRE(committed.ok());
                const auto source = reduced.Source();
                const auto action = reduced.child_sources.front().parent_action.action_id;
                const auto original = std::find_if(source.messages.begin(), source.messages.end(), [&](const auto& row) {
                    return row.action_id == action && row.result_selection_ref && !row.source_tool_message_ref;
                });
                REQUIRE(original != source.messages.end());
                replacement = "short child evidence";
                REQUIRE(replacement.size() < original->message.at("content").get_ref<const std::string&>().size());
                const auto changed = reduced.writer->ReduceToolPreviews(16384, "native-adoption-fixture", 200, 100,
                    {{original->message_id, replacement, std::nullopt}}, {action});
                REQUIRE_MESSAGE(changed.ok, changed.error);
                for (auto& block : results.content)
                    if (auto* value = std::get_if<api::ToolResultBlock>(&block); value && value->tool_use_id == "reused-provider") {
                        value->content = replacement;
                        for (auto& payload : value->blocks)
                            if (auto* text = std::get_if<tools::TextContent>(&payload)) text->text.clear();
                        if (!value->blocks.empty()) value->blocks.insert(value->blocks.begin(), tools::TextContent{replacement});
                    }
                return committed;
            };
        };
        RealRun(reduced); const auto source = reduced.Source(); const auto checked = CheckHistory(reduced, source);
        REQUIRE_MESSAGE(checked.state == v3::ChildAdoptionState::Validated, checked.issue); REQUIRE(checked.adoption);
        CHECK(checked.adoption->original_tool_message_id != checked.adoption->consumed_tool_message_id);
        const auto* consumed = source.FindMessage(checked.adoption->consumed_tool_message_id); REQUIRE(consumed);
        CHECK(consumed->message.at("content").get<std::string>() == replacement);
        int actual = 0;
        for (const auto& message : reduced.backend.parent_requests.back().messages)
            for (const auto& block : message.content)
                if (const auto* result = std::get_if<api::ToolResultBlock>(&block); result && result->content == replacement) ++actual;
        CHECK(actual == 1);
    }
    Directory directory; Rig rig(directory); RealRun(rig);
    const auto before = CheckHistory(rig, rig.Source()); REQUIRE(before.adoption.has_value());
    auto compact = v3::CompactSession::Begin(*rig.writer, "manual", "history validation fixture", std::nullopt, Json::object());
    REQUIRE(compact.info.began); REQUIRE(compact.session);
    std::vector<std::string> removed;
    for (std::size_t index = 1; index < rig.writer->context().chain.size(); ++index)
        removed.push_back(rig.writer->context().chain[index].message_ref);
    auto& session = *compact.session;
    const auto frozen = session.Freeze(*rig.writer, removed, {}, {}); REQUIRE(frozen.eligible);
    REQUIRE(frozen.event.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(session.AppendPrompt(*rig.writer, {{"role", "user"}, {"content", "compress prior native history"}}).status == v3::WriteReceipt::Status::Committed);
    const auto request = rig.writer->NewRequestId(), step = rig.writer->NewStepId();
    REQUIRE(rig.writer->PrepareRequest(request, session.turn_id(), step, "compact",
        rig.writer->context().system_message_ref, removed, Json::object(), session.compact_id()).status == v3::WriteReceipt::Status::Committed);
    // This native compact protocol fixture writes a real candidate/applied
    // chain; the earlier conversation prepared was emitted by the real Agent.
    REQUIRE(session.WriteCandidate(*rig.writer, {{"role", "assistant"}, {"content", "prior child work summary"}}, request, step,
        "test", "openai-chat-completions", "compact-model", nullptr).status == v3::WriteReceipt::Status::Committed);
    REQUIRE(session.StartValidation(*rig.writer).status == v3::WriteReceipt::Status::Committed);
    REQUIRE(session.CompleteValidation(*rig.writer, true, {{{"code", "fixture"}, {"passed", true}}}).status == v3::WriteReceipt::Status::Committed);
    const auto applied = session.Apply(*rig.writer, "prior child work summary", 200, 20, {{"metric", "fixture"}});
    REQUIRE_MESSAGE(applied.ok, applied.error);
    const auto source = rig.Source();
    CHECK(std::none_of(source.context.chain.begin(), source.context.chain.end(), [&](const auto& node) {
        return node.message_ref == before.adoption->original_tool_message_id;
    }));
    const auto historical = CheckHistory(rig, source);
    REQUIRE_MESSAGE(historical.state == v3::ChildAdoptionState::Validated, historical.issue); REQUIRE(historical.adoption.has_value());
    CHECK(historical.adoption->prepared_event_id == before.adoption->prepared_event_id);
    CHECK(historical.adoption->prepared_context_revision < source.context.revision);
    std::cout << "[child-adoption-path] historical-chain\n";
}

TEST_CASE("child history rejects altered producer and checkpoint claims after common verification") {
    Directory directory; Rig rig(directory); RealRun(rig); CloseParent(rig);
    Restore saved(rig.writer->path()); const auto good = rig.Source();
    const auto action = rig.child_sources.front().parent_action.action_id;
    const auto observed = FindKind(good, v3::EventKindV3::SubagentObserved, action).event_id;
    for (int variant = 0; variant != 7; ++variant) {
        CAPTURE(variant);
        const auto bytes = Alter(saved.original, [&](auto& rows) {
            auto& row = Row(rows, observed); auto& display = row["payload"]["display"];
            if (variant == 0) display["rawTextSha256"] = std::string(64, 'a');
            if (variant == 1) display["rawTextBytes"] = display["rawTextBytes"].template get<std::uint64_t>() + 1;
            if (variant == 2) row["payload"]["childCheckpointRef"]["lineHash"] = std::string(64, 'b');
            if (variant == 3) display["terminalRef"]["id"] = "foreign-terminal";
            if (variant == 4) display["seal"] = "close_failed";
            if (variant == 5) display["execution"] = "indeterminate";
            if (variant == 6) row["sessionId"] = "foreign-parent";
        });
        Write(rig.writer->path(), bytes); const auto verified = Verified(rig.writer->path());
        const auto checked = CheckHistory(rig, verified);
        CHECK_MESSAGE(checked.state == v3::ChildAdoptionState::Rejected, checked.issue);
        Write(rig.writer->path(), saved.original);
    }
    CHECK(Read(rig.writer->path()) == saved.original);
    std::cout << "[child-adoption-path] observation-gap\n";
}

TEST_CASE("child history checks persistent child parent sources rather than same-string native identity") {
    Directory directory, peer_directory; Rig rig(directory, "actual-parent-A"), peer(peer_directory, "actual-parent-B");
    RealRun(rig); RealRun(peer); CloseParent(rig); CloseParent(peer);
    Restore parent_saved(rig.writer->path()), child_saved(rig.child_paths.front());
    CHECK(rig.child_sources.front().child.session_id == peer.child_sources.front().child.session_id);
    CHECK(rig.child_sources.front().child.run_id == peer.child_sources.front().child.run_id);
    const auto parent = rig.Source(); const auto action = rig.child_sources.front().parent_action.action_id;
    const auto observed = FindKind(parent, v3::EventKindV3::SubagentObserved, action).event_id;
    const auto peer_child = Verified(peer.child_paths.front()); REQUIRE(peer_child.LastEntry());
    const auto& peer_terminal = peer_child.events.back(); REQUIRE(peer_terminal.kind == v3::EventKindV3::SessionEnded);
    Write(rig.child_paths.front(), Read(peer.child_paths.front()));
    Write(rig.writer->path(), Alter(parent_saved.original, [&](auto& rows) {
        auto& payload = Row(rows, observed)["payload"];
        payload["childCheckpointRef"]["seq"] = peer_terminal.seq;
        payload["childCheckpointRef"]["lineHash"] = peer_terminal.line_hash;
        payload["display"]["terminalRef"] = {{"sessionId", peer_child.session_id}, {"runId", peer_child.run_id},
            {"id", peer_terminal.event_id}, {"seq", peer_terminal.seq}, {"hash", peer_terminal.line_hash}};
    }));
    const auto swapped = CheckHistory(rig, Verified(rig.writer->path()));
    CHECK(swapped.state == v3::ChildAdoptionState::Rejected); CHECK(swapped.issue == "subagent.adoption.child_parent_source_mismatch");
    Write(rig.child_paths.front(), child_saved.original); Write(rig.writer->path(), parent_saved.original);
    CHECK(CheckHistory(rig, Verified(rig.writer->path())).state == v3::ChildAdoptionState::Validated);
    const auto wrong_dir = v3::ValidateChildAdoption(parent, peer_directory.root, rig.child_sources.front().parent_action.turn_id, action, 1);
    CHECK(wrong_dir.state == v3::ChildAdoptionState::Rejected); CHECK(wrong_dir.issue == "subagent.adoption.parent_directory_mismatch");
    std::cout << "[child-adoption-path] source-gap\n";
}

TEST_CASE("child history verifies actual named artifact bytes and rejects nonregular or escaped sources") {
    Directory directory; Rig rig(directory); RealRun(rig); CloseParent(rig);
    const auto source = rig.Source(); const auto checked = CheckHistory(rig, source); REQUIRE(checked.adoption.has_value());
    const auto* raw = source.FindEvent(checked.adoption->raw_persisted_event_id); REQUIRE(raw);
    Json combined;
    for (const auto& ref : raw->payload.at("result_ref")) if (ref.at("kind") == "combined") combined = ref;
    REQUIRE(combined.is_object()); const auto path = directory.root / platform::Utf8ToPath(combined.at("path").get<std::string>());
    Restore saved(path);
    for (int variant = 0; variant != 4; ++variant) {
        CAPTURE(variant);
        REQUIRE(fs::remove(path));
        if (variant == 0) Write(path, std::string(saved.original.size(), 'x'));
        if (variant == 1) REQUIRE(fs::create_directory(path));
        if (variant == 2) {
            std::ofstream large(path, std::ios::binary); REQUIRE(large.is_open());
            large.seekp(64 * 1024 * 1024); large.put('x'); large.close(); REQUIRE_FALSE(large.fail());
        }
        if (variant == 3) { /* Real missing artifact: no replacement. */ }
        const auto before = Read(rig.writer->path());
        const auto failed = CheckHistory(rig, source);
        CHECK(failed.state == v3::ChildAdoptionState::Rejected); CHECK(Read(rig.writer->path()) == before);
        std::error_code error; fs::remove(path, error); Write(path, saved.original);
    }
#ifndef _WIN32
    REQUIRE(fs::remove(path)); REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
    const auto fifo = CheckHistory(rig, source); CHECK(fifo.state == v3::ChildAdoptionState::Rejected);
    REQUIRE(fs::remove(path)); Write(path, saved.original);
    Directory outside; Write(outside.root / "replacement", saved.original);
    REQUIRE(fs::remove(path)); fs::create_symlink(outside.root / "replacement", path);
    const auto escaped = CheckHistory(rig, source); CHECK(escaped.state == v3::ChildAdoptionState::Rejected);
    CHECK(escaped.issue == "subagent.adoption.artifact_outside_parent");
    REQUIRE(fs::remove(path)); Write(path, saved.original);
#endif
    CHECK(CheckHistory(rig, source).state == v3::ChildAdoptionState::Validated);
    std::cout << "[child-adoption-path] artifact-gap\n";
}

TEST_CASE("child history never accepts selected source or adopted message owner substitutions") {
    Directory directory; Rig rig(directory); RealRun(rig); CloseParent(rig);
    Restore saved(rig.writer->path()); const auto good = rig.Source(); const auto initial = CheckHistory(rig, good); REQUIRE(initial.adoption.has_value());
    const auto& value = *initial.adoption;
    for (int variant = 0; variant != 9; ++variant) {
        CAPTURE(variant);
        Write(rig.writer->path(), Alter(saved.original, [&](auto& rows) {
            auto& selected = Row(rows, value.selected_event_id); auto& message = Row(rows, value.original_tool_message_id);
            if (variant == 0) selected["payload"]["sourceResultEventRefs"][0] = {{"sessionId", "foreign-parent"}, {"runId", "foreign-run"},
                {"id", value.raw_persisted_event_id}, {"seq", 1}, {"hash", std::string(64, 'a')}};
            if (variant == 1) selected["sessionId"] = "foreign-parent";
            if (variant == 2) message["sessionId"] = "foreign-parent";
            if (variant == 3) message["turnId"] = "foreign-turn";
            if (variant == 4) message["message"]["role"] = "user";
            if (variant == 5) message["message"]["content"] = "forged body differs from actual saved output";
            if (variant == 6) {
                // The common schema requires envelope/body call identity to
                // agree. Keep that shape valid and break only its selected
                // action ownership, so the new validator is what rejects it.
                message["actionId"] = "foreign-action";
                message["message"]["tool_call_id"] = "foreign-action";
            }
            if (variant == 7) Row(rows, value.prepared_event_id)["payload"]["contextRevision"] =
                value.prepared_context_revision + 100000;
            if (variant == 8) std::swap(selected, message);
        }));
        const auto verified = Verified(rig.writer->path());
        const auto rejected = CheckHistory(rig, verified); CHECK_MESSAGE(rejected.state == v3::ChildAdoptionState::Rejected, rejected.issue);
        Write(rig.writer->path(), saved.original);
    }
    // Legal historical crash prefixes remain incomplete, not permanently
    // rejected and not accepted. They are slices of the actual native ledger.
    for (const auto& stop : {value.raw_persisted_event_id, value.selected_event_id,
                            value.original_tool_message_id, value.admission_event_id}) {
        std::istringstream lines(saved.original); std::string line, prefix;
        bool found = false;
        while (std::getline(lines, line)) {
            const auto row = Json::parse(line); prefix += line + '\n';
            if (row.value("eventId", std::string()) == stop || row.value("messageId", std::string()) == stop) { found = true; break; }
        }
        REQUIRE(found); Write(rig.writer->path(), prefix);
        const auto incomplete = CheckHistory(rig, Verified(rig.writer->path()));
        CHECK_MESSAGE(incomplete.state == v3::ChildAdoptionState::Incomplete, incomplete.issue);
        Write(rig.writer->path(), saved.original);
    }
    std::cout << "[child-adoption-path] adoption-gap\n";
}

TEST_CASE("child history scopes provider reuse and distinguishes ordinary tools from incomplete children") {
    Directory directory; Rig rig(directory); rig.backend.parent_dispatches = 2; RealRun(rig);
    REQUIRE(rig.child_sources.size() == 2);
    const auto source = rig.Source(); const auto first = CheckHistory(rig, source, 0), second = CheckHistory(rig, source, 1);
    REQUIRE(first.adoption.has_value()); REQUIRE(second.adoption.has_value());
    CHECK(first.adoption->action_id != second.adoption->action_id);
    CHECK(first.adoption->spawn_event_id != second.adoption->spawn_event_id);
    CHECK(first.adoption->child_terminal_hash != second.adoption->child_terminal_hash);
    const auto actions = v3::FoldToolActions(source);
    const auto normal = std::find_if(actions.begin(), actions.end(), [](const auto& item) { return item.tool_name == "after_child"; });
    REQUIRE(normal != actions.end());
    const auto ordinary = v3::ValidateChildAdoption(source, directory.root, normal->turn_id, normal->tool_call_id, 1);
    CHECK(ordinary.state == v3::ChildAdoptionState::NotApplicable); CHECK_FALSE(ordinary.adoption.has_value());
    const auto& owner = rig.child_sources.front().parent_action;
    const auto wrong_turn = v3::ValidateChildAdoption(source, directory.root, "foreign-turn", owner.action_id, 1);
    CHECK(wrong_turn.state == v3::ChildAdoptionState::Rejected);
    const auto wrong_attempt = v3::ValidateChildAdoption(source, directory.root, owner.turn_id, owner.action_id, 2);
    CHECK(wrong_attempt.state == v3::ChildAdoptionState::Rejected);
    CloseParent(rig); Restore saved(rig.writer->path());
    const auto observed = FindKind(source, v3::EventKindV3::SubagentObserved, owner.action_id).event_id;
    std::istringstream lines(saved.original); std::string line, prefix;
    while (std::getline(lines, line)) {
        const auto row = Json::parse(line); if (row.value("eventId", std::string()) == observed) break;
        prefix += line + '\n';
    }
    Write(rig.writer->path(), prefix); const auto pending = CheckHistory(rig, Verified(rig.writer->path()));
    CHECK(pending.state == v3::ChildAdoptionState::Incomplete); CHECK(pending.issue == "subagent.adoption.observation_pending");
    Write(rig.writer->path(), saved.original);
    std::cout << "[child-adoption-path] scope-reuse\n";
}
