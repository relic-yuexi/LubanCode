#include "child_observation_fixture.hpp"

namespace {
void CheckLedgerOnlySummary(const Directory& directory) {
    auto opened = v3::V3Writer::Start(directory.root / "summary-child.jsonl",
        "summary-child", "summary-run", "summary child system");
    REQUIRE(opened.has_value());
    auto writer = std::move(*opened);
    runtime::ActionSummarySource material;
    material.action_id = writer.NewActionId(); material.parent_turn_id = writer.NewTurnId();
    material.text.assign(8192, 'x'); material.execution_state = "done"; material.budget_bytes = 2048;
    const auto step = writer.NewStepId();
    auto action = v3::ToolActionSession::Admit(writer, material.parent_turn_id, step,
        material.action_id, "queued", std::nullopt, "summary-source");
    REQUIRE(action.Start(writer, "args-ref", {"read", "builtin", "1", "test"}).status == v3::WriteReceipt::Status::Committed);
    const auto finished = action.Finish(writer, 0);
    REQUIRE(finished.status == v3::WriteReceipt::Status::Committed);
    auto store = v3::ResultStore::Open(directory.root);
    REQUIRE(store.has_value());
    v3::ResultStore::PersistRequest request;
    request.content = material.text; request.result_kind = "text";
    request.execution_event_ref = finished.id; request.tool_call_id = material.action_id;
    request.outputs.push_back({"combined", "text/plain", material.text, true, "", material.text.size(), false});
    const auto saved = store->Persist(request); REQUIRE(saved.ok);
    material.result_refs = saved.result_ref;
    const auto persisted = action.PersistedResult(writer, saved.result_ref, finished.id);
    REQUIRE(persisted.status == v3::WriteReceipt::Status::Committed);
    material.persisted_event_ref = persisted.id;
    runtime::ActionSummaryProfile profile;
    profile.provider = "test"; profile.wire = "openai-chat-completions"; profile.model = "summary-model";
    Backend backend; int remaining = 8;
    const auto summary = runtime::SummarizeActionResult(writer, backend, profile, material, remaining);
    REQUIRE_MESSAGE(summary.accepted, summary.reason); CHECK(backend.summary_calls == 1);
    const auto selected = action.SelectResult(writer, {persisted.id}, {}, "done", std::nullopt,
        trajectory::Durability::PowerLoss, summary.terminal_event_ref);
    REQUIRE(selected.status == v3::WriteReceipt::Status::Committed);
    REQUIRE(action.AppendToolMessage(writer, summary.text, selected.id).status == v3::WriteReceipt::Status::Committed);
    const auto tool_message = writer.context().chain.back().message_ref;
    const auto path = writer.path(); REQUIRE(writer.Close().has_value());
    const auto before = v3::ReadV3Ledger(path); REQUIRE(before.has_value());
    CHECK(v3::ExpandResultPreview(*before, directory.root, tool_message).complete);
    // Keep a valid, genuinely adopted action-summary ledger, but replace its
    // external result files. The bounded projection must use ledger bytes only;
    // the ordinary expansion still reports their actual hash gaps.
    for (const auto& ref : saved.result_ref)
        Write(directory.root / platform::Utf8ToPath(ref.at("path").get<std::string>()), std::string(1024 * 1024, 'z'));
    const auto bytes = Read(path);
    const auto bounded = v3::ReadV3LedgerBounded(path, bytes.size(), 131072, 4 * 1024 * 1024);
    REQUIRE_MESSAGE(bounded.has_value(), (bounded ? std::string() : bounded.error()));
    const auto projected = v3::ProjectResultPreview(*bounded, tool_message);
    CHECK(projected.summary_valid); CHECK(projected.summary_event_ref == summary.terminal_event_ref);
    CHECK(projected.artifacts.empty()); CHECK_FALSE(projected.result_refs.empty());
    const auto expanded = v3::ExpandResultPreview(*bounded, directory.root, tool_message);
    CHECK_FALSE(expanded.complete); REQUIRE_FALSE(expanded.artifacts.empty());
    CHECK(std::all_of(expanded.artifacts.begin(), expanded.artifacts.end(), [](const auto& item) {
        return item.exists && !item.hash_ok && item.gap_reason == "hash_mismatch";
    }));
    CHECK(backend.summary_calls == 1); CHECK(backend.parent_calls == 0); CHECK(backend.child_calls == 0);
    std::cout << "[child-observation-path] ledger-summary\n";
}

} // namespace

TEST_CASE("child parent observation reaches the next real prepared request through local results") {
    Directory directory; Rig rig(directory);
    const auto result = rig.Run();
    INFO("parent result: ", (result ? result->side_effect_error : result.error()));
    REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error()));
    CHECK_FALSE(result->side_effect_indeterminate);
    CHECK(rig.backend.parent_calls == 2); CHECK(rig.backend.child_calls == 1);
    CHECK(rig.after->calls == 1); rig.CheckChild();
    const auto source = rig.Source();
    REQUIRE(Count(source, v3::EventKindV3::SubagentObserved) == 1);
    const auto observed = std::find_if(source.events.begin(), source.events.end(), [](const auto& event) {
        return event.kind == v3::EventKindV3::SubagentObserved;
    });
    REQUIRE(observed != source.events.end());
    const auto& ref = *rig.terminals.front().terminal;
    CHECK(observed->payload.at("childCheckpointRef").size() == 4);
    CHECK(observed->payload.at("childCheckpointRef").at("sessionId").get<std::string>() == ref.session_id);
    CHECK(observed->payload.at("childCheckpointRef").at("runId").get<std::string>() == ref.run_id);
    CHECK(observed->payload.at("childCheckpointRef").at("seq").get<std::uint64_t>() == ref.seq);
    CHECK(observed->payload.at("childCheckpointRef").at("lineHash").get<std::string>() == ref.hash);
    CHECK(observed->payload.at("display").at("terminalRef").at("id").get<std::string>() == ref.event_id);
    CHECK(observed->payload.at("display").at("rawTextSha256").get<std::string>() == platform::Sha256Hex(rig.raw_child_text));
    const auto actions = v3::FoldToolActions(source);
    const auto action = std::find_if(actions.begin(), actions.end(), [&](const auto& entry) {
        return entry.tool_call_id == *observed->action_id;
    });
    REQUIRE(action != actions.end());
    const v3::EventLine* selected = nullptr;
    const v3::MessageLine* tool = nullptr;
    for (const auto& event : source.events)
        if (event.kind == v3::EventKindV3::ToolResultSelected && event.action_id == observed->action_id) selected = &event;
    for (const auto& message : source.messages)
        if (message.action_id == observed->action_id && message.message.value("role", std::string()) == "tool") tool = &message;
    REQUIRE(selected != nullptr); REQUIRE(tool != nullptr);
    CHECK(observed->seq < selected->seq); CHECK(selected->seq < tool->seq);
    REQUIRE(tool->result_selection_ref.has_value()); CHECK(*tool->result_selection_ref == selected->event_id);
    for (const auto& parent_ref : selected->payload.at("sourceResultEventRefs")) {
        REQUIRE(parent_ref.is_string());
        const auto* persisted = source.FindEvent(parent_ref.get<std::string>());
        REQUIRE(persisted != nullptr);
        CHECK(persisted->kind == v3::EventKindV3::ToolResultPersisted);
        CHECK(persisted->action_id == observed->action_id);
        CHECK(persisted->session_id == source.session_id);
        CHECK(persisted->seq > observed->seq);
    }
    const v3::EventLine* prepared = nullptr;
    for (const auto& event : source.events)
        if (event.kind == v3::EventKindV3::ModelRequestPrepared && event.seq > tool->seq) prepared = &event;
    REQUIRE(prepared != nullptr);
    CHECK(v3::CheckPreparedAgainstChain(source, prepared->event_id).empty());
    const auto& inputs = prepared->payload.at("inputMessageRefs");
    CHECK(std::any_of(inputs.begin(), inputs.end(), [&](const auto& input) {
        return input.is_string() && input.template get<std::string>() == tool->message_id;
    }));
    REQUIRE(rig.backend.parent_requests.size() == 2);
    std::size_t actual = 0;
    for (const auto& message : rig.backend.parent_requests.back().messages)
        for (const auto& block : message.content)
            if (const auto* item = std::get_if<api::ToolResultBlock>(&block);
                item && item->tool_use_id == "reused-provider" && item->content == tool->message.at("content").get<std::string>()) ++actual;
    CHECK(actual == 1);
    std::cout << "[child-observation-path] adopted\n";
}

TEST_CASE("child unknown stops summary calls while healthy large-sibling summaries remain available") {
    int path = 0;
    SUBCASE("native observation IO") { path = 0; }
    SUBCASE("actual child capture failure after a large sibling") { path = 1; }
    SUBCASE("healthy large sibling still uses ordinary action summaries") { path = 2; }
    Directory directory; Rig rig(directory);
    if (path == 0) rig.failure = Failure::ObservationIo;
    else {
        rig.backend.large_first = true;
        rig.parent->SetContextWindowTokens(32768);
        if (path == 1) rig.failure = Failure::CaptureTempDirectory;
    }
    const auto result = rig.Run();
    INFO("parent result: ", (result ? result->side_effect_error : result.error()));
    if (path < 2) {
        rig.CheckStopped(result);
        if (path == 0) {
            CHECK(result->side_effect_error.find("subagent.observation.append_failed") != std::string::npos);
            CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
            std::cout << "[child-observation-path] observation-unknown\n";
        } else {
            CHECK(rig.large->calls == 1);
            CHECK(result->side_effect_error.find("tool.capture.persist_failed") != std::string::npos);
            CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 1);
            CHECK(Count(rig.Source(), v3::EventKindV3::ToolResultSummaryFinished) == 0);
            std::cout << "[child-observation-path] summary-halted\n";
        }
    } else {
        REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error()));
        CHECK_FALSE(result->side_effect_indeterminate);
        CHECK(rig.large->calls == 1); CHECK(rig.after->calls == 1);
        CHECK(rig.backend.child_calls == 1); CHECK(rig.backend.parent_calls == 2);
        CHECK(rig.backend.summary_calls > 0);
        CHECK(Count(rig.Source(), v3::EventKindV3::ToolResultSummaryFinished) > 0);
        rig.CheckChild();
        std::cout << "[child-observation-path] summary-healthy\n";
    }
}

TEST_CASE("child success and actual capture directory failure preserve the committed observation") {
    Directory directory; Rig rig(directory); rig.failure = Failure::CaptureFile;
    const auto result = rig.Run();
    INFO("parent result: ", (result ? result->side_effect_error : result.error()));
    rig.CheckStopped(result);
    CHECK(result->side_effect_error.find("tool.capture.store_unavailable") != std::string::npos);
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 1);
    std::cout << "[child-observation-path] capture-unknown\n";
}

TEST_CASE("child unknown cannot disappear at real rewrite or batch commit failure") {
    int path = 0;
    SUBCASE("actual rewrite receipt") { path = 0; }
    SUBCASE("actual batch receipt without rewrite") { path = 1; }
    SUBCASE("real res filename numeric overflow after observation and capture") { path = 2; }
    SUBCASE("first real child preview failure stops a later sibling summary") { path = 3; }
    Directory directory; Rig rig(directory);
    rig.failure = path == 2 ? Failure::BadResultName : path == 3 ? Failure::None : Failure::CaptureFile;
    rig.bypass_rewrite = path == 1;
    if (path == 3) {
        rig.force_child_preview_failure = true; rig.backend.large_after = true;
        rig.parent->SetContextWindowTokens(32768);
    }
    const auto result = rig.Run();
    INFO("parent result: ", (result ? result->side_effect_error : result.error()));
    if (path < 2) {
        rig.CheckStopped(result);
        CHECK(result->side_effect_error.find("tool.capture.store_unavailable") != std::string::npos);
        CHECK(result->side_effect_error.find("tool.capture.failed") != std::string::npos);
        std::cout << "[child-observation-path] " << (path == 1 ? "commit-unknown\n" : "rewrite-unknown\n");
    } else {
        REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error()));
        CHECK(result->side_effect_indeterminate);
        CHECK(result->side_effect_error.find(path == 2 ? "subagent.result.exception:" : "tool.preview.unrepresentable:") != std::string::npos);
        CHECK(rig.backend.child_calls == 1); CHECK(rig.backend.parent_calls == 1);
        CHECK(rig.backend.summary_calls == 0);
        // Rewrite fails after the batch has already executed. Its sibling
        // execution stays recorded; no second child/model/capture starts.
        CHECK(rig.after->calls == 1); rig.CheckChild();
        REQUIRE(rig.captures.size() == 1); CHECK(rig.captures.front().ok());
        REQUIRE(rig.repeated_captures.size() == 1); CHECK(rig.repeated_captures.front().ok());
        CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 1);
        const auto action = rig.child_sources.front().parent_action.action_id;
        const auto source = rig.Source();
        CHECK(std::count_if(source.events.begin(), source.events.end(), [&](const auto& event) {
            return event.kind == v3::EventKindV3::ToolResultPersisted && event.action_id == action;
        }) == (path == 2 ? 1 : 2));
        if (path == 3) {
            CHECK(rig.large->calls == 1);
            CHECK(Count(source, v3::EventKindV3::ToolResultSummaryFinished) == 0);
            const auto actions = v3::FoldToolActions(source);
            const auto sibling = std::find_if(actions.begin(), actions.end(), [](const auto& value) {
                return value.tool_name == std::optional<std::string>("large_result");
            });
            REQUIRE(sibling != actions.end()); REQUIRE(sibling->selected_event_ref.has_value());
            std::cout << "[child-observation-path] mid-batch-summary-halted\n";
        } else std::cout << "[child-observation-path] rewrite-exception\n";
    }
}

TEST_CASE("cancelled child and actual Close boundary failure never form a positive parent observation") {
    Directory directory; Rig rig(directory); rig.close_failure = true; rig.backend.cancel_child = &rig.cancel;
    const auto result = rig.Run();
    INFO("parent result: ", (result ? result->side_effect_error : result.error()));
    REQUIRE(result.has_value()); CHECK(result->side_effect_indeterminate);
    CHECK(rig.backend.parent_calls == 1); CHECK(rig.backend.child_calls == 1);
    CHECK(rig.after->calls == 0); REQUIRE(rig.terminals.size() == 1);
    CHECK(rig.terminals.front().execution == runtime::SubagentExecutionOutcome::Cancelled);
    CHECK(rig.terminals.front().confirmation == runtime::SubagentAppendConfirmation::Committed);
    CHECK(rig.terminals.front().seal == runtime::SubagentSealState::CloseFailed);
    CHECK_FALSE(rig.terminals.front().durable()); CHECK(rig.close_calls == 1); rig.CheckChild();
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
    std::cout << "[child-observation-path] cancel-close-failed\n";
}

TEST_CASE("parent Attach rejection closes the real spawned child before any child execution") {
    bool close_failed = false;
    SUBCASE("checked child Close") { close_failed = false; }
    SUBCASE("real Close then injected failure") { close_failed = true; }
    Directory directory; Rig rig(directory); rig.reject_attach = true; rig.close_failure = close_failed;
    const auto result = rig.Run();
    INFO("parent result: ", (result ? result->side_effect_error : result.error()));
    REQUIRE(result.has_value());
    CHECK(result->side_effect_indeterminate == close_failed);
    CHECK(rig.backend.child_calls == 0); CHECK(rig.backend.parent_calls == (close_failed ? 1 : 2));
    REQUIRE(rig.rejected.has_value()); REQUIRE(rig.rejected->cleanup_receipt.has_value());
    CHECK(rig.rejected->error_code == "subagent.attach.owner_mismatch");
    CHECK(rig.terminals.front().execution == runtime::SubagentExecutionOutcome::StartupRejected);
    CHECK(rig.terminals.front().confirmation == runtime::SubagentAppendConfirmation::Committed);
    CHECK(rig.terminals.front().seal == (close_failed ? runtime::SubagentSealState::CloseFailed : runtime::SubagentSealState::Closed));
    CHECK(rig.close_calls == 1); rig.CheckChild();
    const auto tasks = rig.dispatch->TaskSnapshots(); REQUIRE(tasks.size() == 1);
    CHECK(tasks.front().state != tools::AgentTaskState::Running);
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
}

TEST_CASE("provider local ID reuse binds each new response and turn to its current action") {
    Directory directory; Rig rig(directory); rig.backend.parent_dispatches = 2;
    const auto first = rig.Run();
    INFO("first parent result: ", (first ? first->side_effect_error : first.error()));
    REQUIRE(first.has_value()); CHECK_FALSE(first->side_effect_indeterminate);
    CHECK(rig.backend.child_calls == 2); CHECK(rig.backend.parent_calls == 3);
    REQUIRE(rig.child_sources.size() == 2);
    CHECK(rig.child_sources[0].parent_action.action_id != rig.child_sources[1].parent_action.action_id);
    CHECK(rig.child_sources[0].parent_action.declared_message_ref != rig.child_sources[1].parent_action.declared_message_ref);
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 2); rig.CheckChild(0); rig.CheckChild(1);
    rig.backend.parent_calls = 0; rig.backend.parent_dispatches = 1;
    const auto second = rig.Run();
    INFO("second parent result: ", (second ? second->side_effect_error : second.error()));
    REQUIRE(second.has_value()); CHECK_FALSE(second->side_effect_indeterminate);
    REQUIRE(rig.child_sources.size() == 3);
    CHECK(rig.child_sources[2].parent_action.turn_id != rig.child_sources[0].parent_action.turn_id);
    CHECK(rig.child_sources[2].parent_action.action_id != rig.child_sources[0].parent_action.action_id);
    CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 3); rig.CheckChild(2);
}

TEST_CASE("child source absence and bounded same bytes verification reject honest read gaps") {
    int path = 0;
    SUBCASE("actual missing child and fixed byte line record limits") { path = 0; }
    SUBCASE("genuine adopted summary uses only bounded ledger bytes") { path = 1; }
#ifndef _WIN32
    // Unix permits this real directory-alias fixture without additional host
    // privilege. Windows does not promise unprivileged symlink creation.
    SUBCASE("actual subagents directory alias cannot leave its parent owner") { path = 2; }
#endif
    if (path == 1) {
        Directory directory; CheckLedgerOnlySummary(directory);
    } else if (path == 2) {
        Directory directory; Rig rig(directory); rig.failure = Failure::ChildRootAlias;
        const auto result = rig.Run();
        INFO("parent result: ", (result ? result->side_effect_error : result.error()));
        rig.CheckStopped(result);
        CHECK(result->side_effect_error.find("subagent.observation.child_path_outside_parent") != std::string::npos);
        CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
        std::cout << "[child-observation-path] source-owner-alias\n";
    } else {
    Directory directory; Rig rig(directory); rig.failure = Failure::MissingChild;
    const auto result = rig.Run();
    INFO("parent result: ", (result ? result->side_effect_error : result.error()));
    REQUIRE(result.has_value()); CHECK(result->side_effect_indeterminate);
    CHECK(rig.backend.parent_calls == 1); CHECK(rig.backend.child_calls == 1);
    CHECK(rig.after->calls == 0); CHECK(Count(rig.Source(), v3::EventKindV3::SubagentObserved) == 0);
    Write(rig.child_paths.front(), rig.removed_child_bytes); rig.CheckChild();
    const auto& path = rig.child_paths.front();
    const auto complete = v3::ReadV3LedgerBounded(path, rig.removed_child_bytes.size(), 131072, 4 * 1024 * 1024);
    REQUIRE(complete.has_value());
    const auto too_many_bytes = v3::ReadV3LedgerBounded(path, rig.removed_child_bytes.size() - 1, 131072, 4 * 1024 * 1024);
    REQUIRE_FALSE(too_many_bytes.has_value()); CHECK(too_many_bytes.error().find("limit_exceeded") != std::string::npos);
    const auto too_many_lines = v3::ReadV3LedgerBounded(path, rig.removed_child_bytes.size(), 1, 4 * 1024 * 1024);
    REQUIRE_FALSE(too_many_lines.has_value()); CHECK(too_many_lines.error() == "v3reader.record_limit_exceeded");
    const auto long_line = v3::ReadV3LedgerBounded(path, rig.removed_child_bytes.size(), 131072, 1);
    REQUIRE_FALSE(long_line.has_value()); CHECK(long_line.error() == "v3reader.line_limit_exceeded");
    std::cout << "[child-observation-path] source-gap\n";
    }
}
