#include "child_observation_fixture.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>

#include "approval_mode.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/envelope.hpp"
#include "runtime/middleware_v3_sink.hpp"
#include "sdk/action_dispatch.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace lubancore_consumer {
void ActionCase(const std::string& name, const std::filesystem::path& base);
std::string ActionSeed(const std::filesystem::path& base);
void ActionRestore(const std::filesystem::path& base, const std::string& id, bool reject);
}
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
void PublicCase(const std::string& name) {
    Directory directory;
    CHECK_NOTHROW(lubancore_consumer::ActionCase(name, directory.root));
    if (name == "chain" || name == "deny") {
        unsigned journals = 0;
        for (const auto& entry : fs::recursive_directory_iterator(directory.root / "data" / "workspaces")) {
            if (!entry.is_regular_file() || entry.path().filename() != entry.path().parent_path().filename().string() + ".jsonl") continue;
            ++journals; const auto source = v3::ReadV3Ledger(entry.path()); REQUIRE(source.has_value());
            CHECK(Count(*source, v3::EventKindV3::ToolExecutionStarted) == (name == "chain" ? 1u : 0u));
            if (name == "deny") continue;
            unsigned rewrites = 0, supplements = 0;
            for (const auto& event : source->events) if (event.kind == v3::EventKindV3::HookEffectsApplied) {
                const auto type = event.payload.value("effectType", std::string{});
                if (type == "input.rewrite") { ++rewrites;
                    const Json expected{{"arguments", {{"value", "adopted"}}}};
                    CHECK(event.payload.at("appliedValueRef") == expected); }
                if (type == "result.supplement") { ++supplements;
                    const auto& value = event.payload.at("appliedValueRef");
                    CHECK(value.at("text").get<std::string>() == "supplement");
                    REQUIRE(value.contains("source")); const auto& from = value.at("source");
                    CHECK(from.at("definitionHash").get<std::string>() == "action-native-v1");
                    CHECK_FALSE(from.at("dispatchId").get<std::string>().empty());
                    CHECK_FALSE(from.at("invocationId").get<std::string>().empty()); }
            }
            CHECK(rewrites == 1); CHECK(supplements == 1);
        }
        CHECK(journals == 1);
    }
}

// Actual Prepare -> permission -> Started -> Execute path. These callbacks
// exercise the same loop which the public adapter uses, never a copied reducer.
void ExistingPermissionChain() {
    for (unsigned variant = 0; variant < 5; ++variant) {
        tools::ToolRegistry registry; auto tool = std::make_unique<Probe>("guarded");
        auto* executed = tool.get(); registry.Register(std::move(tool));
        agent::TurnWiring wiring; unsigned requests = 0, approvals = 0, starts = 0;
        std::set<std::string> temporary{"guarded"};
        wiring.on_pre_action = [variant](const auto&, const auto&, const Json&) {
            agent::TurnWiring::ActionPreDecision value;
            if (variant != 2) value.hook.decision = runtime::ToolHookDecision::Decision::Ask;
            if (variant == 4) value.hook.updated_input = Json{{"value", "rewrite"}};
            return value;
        };
        wiring.on_pre_tool_use_hook = [variant](const auto&, const auto&, const Json&) {
            runtime::ToolHookDecision value;
            value.decision = variant == 2 ? runtime::ToolHookDecision::Decision::Deny
                : variant == 3 ? runtime::ToolHookDecision::Decision::Ask
                : runtime::ToolHookDecision::Decision::Allow;
            return value;
        };
        wiring.on_permission_evaluate = [&](const auto&, const auto& name, tools::ApprovalClass kind,
            const Json& input, const runtime::ToolHookDecision& pre) {
            runtime::PermissionContext context; context.mode = lubancode::ApprovalMode::Yolo;
            context.always_allowed = &temporary;
            return runtime::EvaluatePermission(context, pre, kind, name, input);
        };
        wiring.on_permission_request = [&](const auto&, const auto&, const Json&) {
            ++requests; runtime::ToolHookDecision value;
            value.decision = variant == 0 ? runtime::ToolHookDecision::Decision::Deny
                : runtime::ToolHookDecision::Decision::Allow;
            return value;
        };
        wiring.on_tool_confirm = [&](const auto&, const auto&, const Json&) { ++approvals; return false; };
        wiring.on_tool_trace = [&](const auto& event) {
            if (event.kind == agent::ToolTraceEventKind::ExecutionStarted) ++starts;
        };
        if (variant == 4) wiring.on_mode_policy = [](const auto&, const Json& input) {
            return input.contains("value") ? std::string("effective floor refuses rewritten arguments") : std::string{};
        };
        const auto result = agent::RunOneTool(registry, {"wire-action", "guarded", Json::object()}, wiring, {});
        CHECK(result.is_error); CHECK(executed->calls == 0); CHECK(starts == 0);
        if (variant == 0) { CHECK(requests == 1); CHECK(approvals == 0); }
        if (variant == 1 || variant == 3) { CHECK(requests == 1); CHECK(approvals == 1); }
        if (variant == 2 || variant == 4) { CHECK(requests == 0); CHECK(approvals == 0); }
    }
    std::cout << "[sdk-action-native] existing-permission-chain\n";
}

void ActualSummaryStop() {
    // This real backend serializes the wire, unlike a public text-only custom
    // Backend. The healthy control proves a large-result summary was eligible.
    for (bool failed : {false, true}) {
        Directory directory; Rig rig(directory); rig.backend.large_first = true;
        rig.parent->SetContextWindowTokens(32768);
        std::string failure;
        rig.configure_wiring = [&](agent::TurnWiring& wiring) {
            wiring.on_pre_action = [&](const auto&, const auto&, const Json&) {
                agent::TurnWiring::ActionPreDecision value;
                if (!failure.empty()) { value.failed = true; value.hook.decision = runtime::ToolHookDecision::Decision::Deny;
                    value.hook.reason = failure; }
                return value;
            };
            wiring.on_post_action = [&](const auto& call, const auto& name, const Json&, const tools::Tool::Result& raw,
                const tools::ToolInvocationIdentity&, const agent::ToolTraceEvent& started, const agent::ToolTraceEvent& finished) {
                CHECK(started.kind == agent::ToolTraceEventKind::ExecutionStarted);
                CHECK(finished.kind == agent::ToolTraceEventKind::ExecutionFinished);
                CHECK_FALSE(started.execution_id.empty()); CHECK(started.execution_id == finished.execution_id);
                CHECK(started.tool_use_id == call); CHECK(finished.tool_use_id == call);
                CHECK(started.tool_name == name); CHECK(finished.tool_name == name);
                CHECK(started.turn_id == finished.turn_id);
                CHECK(started.thread_id.empty()); CHECK(finished.thread_id.empty());
                if (failed && name == "large_result") failure = "fixture.action.required_post_failed";
                return raw;
            };
            wiring.action_failure_reason = [&] { return failure; };
        };
        const auto result = rig.Run(); const auto source = rig.Source();
        CHECK(rig.large->calls == 1); CHECK(Count(source, v3::EventKindV3::ToolResultPersisted) > 0);
        if (failed) {
            REQUIRE_FALSE(result.has_value()); CHECK(result.error() == failure);
            CHECK(rig.backend.parent_calls == 1); CHECK(rig.backend.child_calls == 0); CHECK(rig.after->calls == 0);
            CHECK(rig.backend.summary_calls == 0); CHECK(Count(source, v3::EventKindV3::ToolResultSummaryFinished) == 0);
        } else {
            REQUIRE(result.has_value()); CHECK_FALSE(result->side_effect_indeterminate);
            CHECK(rig.backend.parent_calls == 2); CHECK(rig.backend.child_calls == 1); CHECK(rig.backend.summary_calls > 0);
        }
    }
    std::cout << "[sdk-action-native] summary-stop\n";
}

void ActualReceiptStop() {
    namespace mw = hooks::middleware;
    // A genuine SDK host adapter, native sink and writer injection. The public
    // Session has no writer-fault option; this is not claimed as that full stack.
    for (bool post : {false, true}) {
        Directory directory; Rig rig(directory); rig.backend.large_first = true;
        rig.parent->SetContextWindowTokens(32768);
        hooks::HookDispatcher dispatcher; mw::MiddlewarePool pool; unsigned old_hooks = 0;
        mw::MiddlewareDefinition definition; definition.name = "receipt";
        definition.point = post ? mw::HookPoint::PostAction : mw::HookPoint::PreAction;
        definition.implementation_ref = "builtin.action-receipt"; definition.definition_hash = "native-receipt-v1";
        definition.source_label = "actual native writer fault";
        definition.builtin = [&](const mw::InvocationCtx&, const Json&, mw::NextCall& next)
            -> std::expected<mw::HandlerReturn, mw::HandlerError> {
            const auto downstream = post ? next.Call() : next.Call(Json{{"arguments", {{"value", "rewrite"}}}});
            REQUIRE(downstream.kind == mw::DownstreamOutcome::Kind::Value);
            auto value = mw::HandlerReturn::Value(downstream.value);
            if (post) {
                value.effects.push_back({mw::EffectType::ResultSupplement, {{"text", "receipt supplement"}}});
                rig.io_armed->store(true); // real first receipt after this return
            }
            return value;
        };
        pool.AddDefinition(std::move(definition)); auto registry = pool.Publish(); REQUIRE(registry.has_value());
        dispatcher.SetMiddleware(std::make_shared<mw::MiddlewareDispatcher>(*registry));
        auto sink = std::make_shared<runtime::V3MiddlewareEventSink>(*rig.writer); dispatcher.SetMiddlewareSink(sink);
        rig.configure_wiring = [&](agent::TurnWiring& wiring) {
            const auto trigger = [&](const std::string& call, const std::string& name, const Json& arguments) {
                const auto origin = rig.bridge->V3DeclaredCallOrigin(call); REQUIRE(origin.has_value());
                mw::DispatchTrigger value; value.turn_id = origin->turn_id; value.action_id = origin->action_id;
                value.input = {{"arguments", arguments}}; value.cancel = &rig.cancel;
                value.action_scope = mw::ActionScope{rig.writer->session_id(), "internal-host-operation", call, name,
                    platform::PathToUtf8(directory.root), std::nullopt};
                return value;
            };
            wiring.on_pre_action = [&, trigger](const auto& call, const auto& name, const Json& arguments) {
                auto value = lubancore::detail::RunPreAction(dispatcher, trigger(call, name, arguments));
                if (!post) { REQUIRE(sink->recent_errors().empty()); rig.io_armed->store(true); }
                return value; // only the real deferred Adopt will trip the writer
            };
            wiring.on_post_action = [&, trigger](const auto& call, const auto& name, const Json& arguments,
                const tools::Tool::Result& raw, const tools::ToolInvocationIdentity&,
                const agent::ToolTraceEvent& started, const agent::ToolTraceEvent& finished) {
                CHECK(started.kind == agent::ToolTraceEventKind::ExecutionStarted);
                CHECK(finished.kind == agent::ToolTraceEventKind::ExecutionFinished);
                CHECK_FALSE(started.execution_id.empty()); CHECK(started.execution_id == finished.execution_id);
                CHECK(started.tool_use_id == call); CHECK(finished.tool_use_id == call);
                CHECK(started.tool_name == name); CHECK(finished.tool_name == name);
                CHECK(started.turn_id == finished.turn_id);
                CHECK(started.thread_id.empty()); CHECK(finished.thread_id.empty());
                auto context = trigger(call, name, arguments);
                context.input["result"] = {{"text", raw.content}, {"isError", raw.is_error},
                    {"outcome", agent::ToString(finished.outcome)}, {"errorCode", finished.error_code}};
                const auto value = lubancore::detail::RunPostAction(dispatcher, context, raw);
                REQUIRE(value.has_value()); return *value;
            };
            wiring.action_receipt_failure_reason = [sink] {
                const auto errors = sink->recent_errors();
                return errors.empty() ? std::string{} : "sdk.action.trajectory_failed: " + errors.front();
            };
            wiring.on_post_tool_use_hook = [&](const auto&, const auto&, const Json&, const tools::Tool::Result&) {
                ++old_hooks; return std::vector<std::string>{"old Post supplement"};
            };
            wiring.on_post_tool_hook = [&](const auto&, const auto&, const Json&, const tools::Tool::Result&) { ++old_hooks; };
        };
        const auto result = rig.Run();
        REQUIRE(result.has_value()); CHECK(result->side_effect_indeterminate);
        CHECK(result->side_effect_error.find("sdk.action.trajectory_failed") != std::string::npos);
        CHECK_FALSE(sink->recent_errors().empty()); CHECK(rig.backend.parent_calls == 1);
        CHECK(rig.backend.summary_calls == 0); CHECK(rig.backend.child_calls == 0); CHECK(rig.after->calls == 0);
        CHECK(old_hooks == 0);
        CHECK(rig.large->calls == (post ? 1 : 0));
        if (post) CHECK(Count(rig.Source(), v3::EventKindV3::ToolResultPersisted) > 0);
    }
    std::cout << "[sdk-action-native] receipt-stop\n";
}

fs::path ActionDirectory(const fs::path& base, const std::string& id) {
    fs::path result;
    for (const auto& item : fs::recursive_directory_iterator(base / "data" / "workspaces"))
        if (item.is_directory() && item.path().filename() == id && item.path().parent_path().filename() == "sessions") {
            REQUIRE(result.empty()); result = item.path();
        }
    REQUIRE_FALSE(result.empty()); return result;
}
void AdoptedActionSystem(const v3::V3Ledger& ledger, const std::string& session_id, const char* variant) {
    REQUIRE(ledger.session_id == session_id);
    REQUIRE_FALSE(ledger.messages.empty());
    const auto& initial = ledger.messages.front();
    REQUIRE(initial.seq == 1);
    REQUIRE(initial.message.value("role", std::string{}) == "system");
    REQUIRE_FALSE(initial.message_id.empty());
    const auto started = std::find_if(ledger.events.begin(), ledger.events.end(), [](const auto& event) {
        return event.kind == v3::EventKindV3::SessionStarted;
    });
    REQUIRE(started != ledger.events.end());
    const auto& chain = started->payload.at("context").at("contextChain");
    REQUIRE(chain.is_array());
    REQUIRE_FALSE(chain.empty());
    const auto started_ref = chain.front().at("messageRef").get<std::string>();
    REQUIRE(started_ref == initial.message_id);
    REQUIRE_FALSE(ledger.revision_chains.empty());
    for (const auto& [revision, adopted] : ledger.revision_chains) {
        REQUIRE(adopted.first == initial.message_id);
        std::cout << "[sdk-action-binding-ref] variant=" << variant << " revision=" << revision
                  << " system_ref=" << adopted.first << '\n';
    }
    REQUIRE(ledger.context.system_message_ref == initial.message_id);
    std::cout << "[sdk-action-binding-ref] variant=" << variant << " seq=" << initial.seq
              << " initial_ref=" << initial.message_id << " started_ref=" << started_ref
              << " effective_ref=" << ledger.context.system_message_ref << '\n';
}
std::string RehashBinding(const std::string& original, unsigned variant) {
    std::vector<Json> rows; std::istringstream input(original); std::string line;
    while (std::getline(input, line)) { REQUIRE_FALSE(line.empty()); rows.push_back(Json::parse(line)); }
    REQUIRE_FALSE(rows.empty()); REQUIRE(rows.front().contains("systemMeta"));
    if (variant == 0) rows.front()["systemMeta"]["hostBindings"].erase("extensionAction");
    else rows.front()["systemMeta"]["hostBindings"]["extensionAction"]["sha256"] = std::string(64, 'a');
    std::string previous(v3::kGenesisHash), changed;
    for (auto& row : rows) {
        if (row.value("kind", std::string{}) == "model.request.prepared")
            row["payload"]["readThroughHash"] = previous;
        row.erase("prevHash"); row.erase("lineHash");
        auto canonical = trajectory::CanonicalJsonDump(row); REQUIRE(canonical.has_value());
        const auto hash = v3::ComputeLineHash(previous, *canonical);
        row["prevHash"] = previous; row["lineHash"] = hash;
        auto rendered = trajectory::CanonicalJsonDump(row); REQUIRE(rendered.has_value());
        changed += *rendered + '\n'; previous = hash;
    }
    return changed;
}
void OwnedOpeningGate() {
    Directory directory; const auto id = lubancore_consumer::ActionSeed(directory.root);
    const auto own = ActionDirectory(directory.root, id), journal = own / (id + ".jsonl"), plan = own / "sdk-extension-plan.json";
    const auto original = Read(journal), saved_plan = Read(plan);
    const auto original_ledger = v3::ReadV3Ledger(journal);
    const auto original_error = original_ledger.has_value() ? std::string{} : original_ledger.error();
    REQUIRE_MESSAGE(original_ledger.has_value(), original_error);
    AdoptedActionSystem(*original_ledger, id, "original");
    for (unsigned variant = 0; variant < 2; ++variant) {
        const auto changed = RehashBinding(original, variant); Write(journal, changed);
        const auto readable = v3::ReadV3Ledger(journal);
        const auto error = readable.has_value() ? std::string{} : readable.error();
        REQUIRE_MESSAGE(readable.has_value(), error);
        AdoptedActionSystem(*readable, id, variant == 0 ? "missing-binding" : "wrong-fingerprint");
        REQUIRE(readable->messages.front().message_id == original_ledger->messages.front().message_id);
        CHECK_NOTHROW(lubancore_consumer::ActionRestore(directory.root, id, true)); CHECK(Read(journal) == changed);
    }
    Write(journal, original);
#ifndef _WIN32
    REQUIRE(fs::remove(plan)); REQUIRE(::mkfifo(plan.c_str(), 0600) == 0);
    CHECK_NOTHROW(lubancore_consumer::ActionRestore(directory.root, id, true)); CHECK(Read(journal) == original);
    REQUIRE(fs::remove(plan)); Write(plan, saved_plan);
#endif
    CHECK_NOTHROW(lubancore_consumer::ActionRestore(directory.root, id, false));
    std::cout << "[sdk-action-native] binding-opening\n";
}
} // namespace

TEST_CASE("SDK Action: owned Next chain and actual raw/formal supplement adoption") { PublicCase("chain"); }
TEST_CASE("SDK Action: downstream denial stays sticky without Started") { PublicCase("deny"); }
TEST_CASE("SDK Action: actual rewrite rechecks both schemas while legacy readonly stays") { PublicCase("rewrite-schema"); }
TEST_CASE("SDK Action: force Ask uses real pending and survives old grants and PermissionRequest") { PublicCase("force-ask"); ExistingPermissionChain(); }
TEST_CASE("SDK Action: Post failures preserve raw stop summary and do not leak to next turn") { PublicCase("post-failure"); ActualSummaryStop(); ActualReceiptStop(); }
TEST_CASE("SDK Action: explicit payload Next observer and aggregate supplement boundaries") { PublicCase("limits"); }
TEST_CASE("SDK Action: declaration preflight precedes factories and journals") { PublicCase("opening"); }
TEST_CASE("SDK Action: frozen plan and actual initial adopted binding reject before Continue") { PublicCase("resume"); OwnedOpeningGate(); }
TEST_CASE("SDK Action: checked Close drains actual callback and rejects late work") { PublicCase("close"); }
TEST_CASE("SDK Action: two same-project and two other-project sessions isolate owners and cancel") { PublicCase("isolation"); }
