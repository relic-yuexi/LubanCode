#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include "agent/loop.hpp"
#include "platform/paths.hpp"
#include "runtime/session_service.hpp"
#include "runtime/trajectory_session.hpp"
#include "sdk/job_operations.hpp"
#include "sdk/operation_ledger.hpp"
#include "tools/registry.hpp"
#include "tools/run_command.hpp"
#include "trajectory/canonical_json.hpp"
#include "trajectory/v3/tool_action.hpp"
#include "workspace/identity.hpp"

namespace {
namespace fs = std::filesystem;
namespace v3 = lubancode::trajectory::v3;
namespace rt = lubancode::runtime;
namespace sdk = lubancore::detail;
namespace agent = lubancode::agent;
using namespace lubancode::tools;
using Json = nlohmann::json;
using Knowledge = sdk::JobOperationKnowledge;
using Held = sdk::JobOperationRecoveryState;
using namespace std::chrono_literals;

struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> number{0};
        for (unsigned attempt = 0; attempt != 128; ++attempt) {
            auto candidate = fs::temp_directory_path() / ("sdk-job-operations-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++number));
            std::error_code error;
            if (fs::create_directory(candidate, error)) { root = fs::canonical(candidate); break; }
            const bool acceptable = !error || error == std::errc::file_exists;
            REQUIRE_MESSAGE(acceptable, error.message());
        }
        REQUIRE_FALSE(root.empty());
        REQUIRE(fs::create_directory(root / "a")); REQUIRE(fs::create_directory(root / "b"));
    }
    ~Directory() { std::error_code error; fs::remove_all(root, error); }
};
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); REQUIRE(input.is_open());
    std::string value((std::istreambuf_iterator<char>(input)), {}); REQUIRE_FALSE(input.bad()); return value;
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc); REQUIRE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); output.close(); REQUIRE_FALSE(output.fail());
}
v3::V3Ledger Ledger(const fs::path& path) {
    auto value = v3::ReadV3Ledger(path); const auto error = value ? std::string() : value.error();
    REQUIRE_MESSAGE(value.has_value(), error); return std::move(*value);
}
void Committed(const v3::WriteReceipt& receipt) {
    REQUIRE_MESSAGE(receipt.status == v3::WriteReceipt::Status::Committed, receipt.error_message);
    REQUIRE_FALSE(receipt.id.empty()); REQUIRE(receipt.seq > 0); REQUIRE(v3::IsHex64(receipt.line_hash));
}
std::vector<Json> Rows(const std::string& bytes) {
    std::vector<Json> rows;
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto end = bytes.find('\n', offset); REQUIRE(end != std::string::npos);
        rows.push_back(Json::parse(bytes.substr(offset, end - offset))); offset = end + 1;
    }
    return rows;
}
std::string Dump(const std::vector<Json>& rows, bool rehash = false) {
    std::string bytes, previous(v3::kGenesisHash);
    for (auto row : rows) {
        if (rehash) {
            row.erase("prevHash"); row.erase("lineHash");
            auto canonical = lubancode::trajectory::CanonicalJsonDump(row); REQUIRE(canonical);
            const auto hash = v3::ComputeLineHash(previous, *canonical);
            row["prevHash"] = previous; row["lineHash"] = hash; previous = hash;
        }
        auto canonical = lubancode::trajectory::CanonicalJsonDump(row); REQUIRE(canonical); bytes += *canonical + "\n";
    }
    return bytes;
}
void Mark(const char* route) { std::fprintf(stderr, "[sdk-job-operations-path] %s\n", route); std::fflush(stderr); }
struct Counts { std::atomic<unsigned> execute{0}, gate{0}, owned_gate{0}, clock{0}, thread{0}, post{0}; };
class Command final : public RunCommandTool {
public:
    explicit Command(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) {}
    Result execute(const Json& input, const ToolExecutionContext& context) override {
        ++counts_->execute; return RunCommandTool::execute(input, context);
    }
private:
    std::shared_ptr<Counts> counts_;
};
struct Rig {
    fs::path cwd;
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    std::unique_ptr<rt::SessionService> service;
    std::shared_ptr<std::recursive_mutex> serial = std::make_shared<std::recursive_mutex>();
    std::shared_ptr<ToolJobCoordinator> coordinator;
    sdk::JobOperations table;
    sdk::MainOperationTurnStart parent;
    unsigned sequence = 0;
    Rig(const Directory& directory, fs::path actual_cwd = {}, fs::path store = {}, bool anchored = true)
        : cwd(actual_cwd.empty() ? directory.root / "a" : std::move(actual_cwd)) {
        rt::SessionLaunchRequest launch;
        launch.cwd_utf8 = lubancode::platform::PathToUtf8(cwd);
        launch.workspace_identity = lubancode::workspace::MakeFallbackIdentity(cwd);
        launch.workspaces_root = store.empty() ? directory.root / "data" : std::move(store);
        launch.lubancode_version = "job-operation-fixture"; launch.wire_name = "chat";
        launch.v3_system_content = "job binding system";
        service = std::make_unique<rt::SessionService>(std::move(launch));
        REQUIRE_MESSAGE(service->runtime() != nullptr, service->launch_error()); REQUIRE(service->v3_format());
        const auto input = service->SubmitInput({"actual-parent", "actual binding input", {}});
        REQUIRE_MESSAGE(input.accepted, input.error_code);
        if (anchored) { parent = sdk::BeginMainOperationTurn(*service); REQUIRE(parent.ready()); REQUIRE(parent.facts); }
        else { const auto pop = service->PopPendingInput(); REQUIRE(pop.status == rt::SessionService::PendingPop::Status::Ok);
            parent.turn_id = Writer().NewTurnId(); }
        ToolJobCoordinator::Options options;
        options.prepared_registration = PreparedRegistrationContext{serial, "fixture-project", cwd};
        options.clock_ms = [owner = counts] { ++owner->clock; return std::int64_t{1}; };
        options.thread_starter = [owner = counts](std::thread& thread, std::function<void()> body) {
            ++owner->thread; thread = std::thread(std::move(body));
        };
        coordinator = std::make_shared<ToolJobCoordinator>(Writer(),
            [owner = counts](const std::string&, const Json&) { ++owner->gate; return JobAuthDecision{true, false, {}}; },
            [owner = counts](const JobExecutionContext&) { ++owner->execute; return Tool::Result::Text("legacy"); }, options);
    }
    ~Rig() { (void)table.Close(); coordinator.reset(); if (service) (void)service->Close("fixture_exit"); }
    v3::V3Writer& Writer() { return *service->trajectory()->v3_main_writer(); }
    fs::path Path() { return Writer().path(); }
    fs::path Dir() { return service->trajectory()->session_dir(); }
    OwnedJobAdoption Adopt(bool rewrite = false) {
        const auto number = ++sequence;
        lubancode::api::ToolUseBlock call{"provider-" + std::to_string(number), "run_command",
            Json{{"command", "echo original"}, {"cwd", lubancode::platform::PathToUtf8(cwd)}}};
        const auto step = Writer().NewStepId();
        const auto action_id = Writer().NewActionId();
        v3::MessageDraft message;
        message.turn_id = parent.turn_id; message.step_id = step; message.request_id = Writer().NewRequestId();
        message.origin = v3::MessageOrigin::SessionRuntime;
        message.provider = "fixture"; message.wire = "chat"; message.model = "fixture"; message.response_model = "fixture";
        message.usage = nullptr;
        message.message = {{"role", "assistant"}, {"content", "declaration"}, {"tool_calls", Json::array({
            {{"id", call.id}, {"type", "function"}, {"function", {{"name", call.name}, {"arguments", call.input.dump()}}}}})}};
        const auto written = Writer().AppendMessage(std::move(message), v3::Durability::PowerLoss); Committed(written);
        Committed(Writer().AdmitMessages({written.id}, v3::Durability::PowerLoss));
        auto action = v3::ToolActionSession::Admit(Writer(), parent.turn_id, step, action_id,
            "prepared-parent", written.id, call.id, Json::object(), v3::Durability::PowerLoss);
        REQUIRE(action.last_event_id());
        ToolRegistry registry; ToolRegistration tool;
        tool.tool = std::make_unique<RunCommandTool>(); tool.source_kind = lubancode::ToolSourceKind::Builtin;
        tool.source_instance = "native-command"; tool.version_or_digest = "native-v1";
        tool.effect_class = lubancode::EffectClass::LocalProcessUnknown; registry.Register(std::move(tool));
        agent::TurnWiring wiring;
        wiring.on_pre_tool_use_hook = [rewrite, input = call.input](const auto&, const auto&, const auto&) {
            rt::ToolHookDecision pre; pre.decision = rt::ToolHookDecision::Decision::Allow;
            if (rewrite) { auto updated = input; updated["command"] = "echo effective"; pre.updated_input = std::move(updated); }
            return pre;
        };
        wiring.on_permission_evaluate = [](const auto&, const auto&, auto, const auto&, const auto&) {
            rt::PermissionVerdict result; result.action = rt::PermissionVerdict::Action::Allow; return result;
        };
        auto prepared = agent::PrepareOwnedToolInput(registry, call, wiring, {});
        const auto prepare_error = prepared ? std::string() : prepared.error().content;
        REQUIRE_MESSAGE(prepared.has_value(), prepare_error);
        const auto owner = coordinator->PreparedOwner(); REQUIRE(owner);
        PreparedJobRequest request;
        request.owner = *owner; request.provider_tool_call_id = call.id; request.assistant_message_ref = written.id;
        request.parent_action_id = action_id; request.turn_id = parent.turn_id; request.step_id = step;
        request.tool_name = call.name; request.original_input = call.input; request.effective_input = prepared->effective_input;
        request.tool_identity = {call.name, prepared->source_instance, "native-v1", lubancode::platform::PathToUtf8(cwd)};
        request.policy.allow_background = true; request.policy.side_effect_class = "external";
        const auto registered = coordinator->RegisterPreparedJob(request);
        REQUIRE_MESSAGE(registered.state == PreparedJobRegistrationState::Registered, registered.error); REQUIRE(registered.facts);
        OwnedJobCapability cap;
        cap.command = std::make_shared<Command>(counts); cap.command_limits = {15000, 1024};
        cap.scope_gate = [facts = registered.facts, owner = counts](const OwnedJobScope& scope, const Json& input,
            const v3::ToolIdentity& identity, const JobExecutionPolicy& policy) {
            ++owner->owned_gate;
            const bool valid = scope.owner == facts->owner && scope.job_id == facts->job_id &&
                scope.action_id == facts->action_id && scope.attempt == 1 && input == facts->effective_input &&
                identity.ToJson() == facts->tool_identity.ToJson() && policy.ToJson() == facts->policy.ToJson();
            CHECK(valid); return JobAuthDecision{valid, false, {}};
        };
        cap.post = [owner = counts](const OwnedJobCompletion&) { ++owner->post; return v3::WriteReceipt{}; };
        auto adopted = coordinator->AdoptPreparedJob(*owner, registered.facts->job_id, std::move(cap));
        REQUIRE_MESSAGE(adopted.state == OwnedJobAdoptionState::Adopted, adopted.error);
        REQUIRE(adopted.receipt); Committed(*adopted.receipt); Idle(); return adopted;
    }
    void Idle() const {
        CHECK(counts->execute.load() == 0); CHECK(counts->thread.load() == 0); CHECK(counts->gate.load() == 0);
        CHECK(counts->clock.load() == 0); CHECK(counts->post.load() == 0);
        CHECK(counts->owned_gate.load() == sequence);
        CHECK(coordinator->running_count() == 0); CHECK(coordinator->queued_count() == 0);
    }
};
void Actual(const v3::V3Ledger& ledger, const sdk::JobOperationRegistration& registration) {
    REQUIRE(registration.bound()); REQUIRE(registration.receipt); REQUIRE(registration.facts); Committed(*registration.receipt);
    const auto* event = ledger.FindEvent(registration.receipt->id); REQUIRE(event != nullptr);
    CHECK(event->kind == v3::EventKindV3::SdkJobOperationBound); CHECK_FALSE(event->status);
    CHECK(event->seq == registration.facts->seq); CHECK(event->line_hash == registration.facts->line_hash);
    CHECK(event->session_id == registration.facts->session_id); CHECK(event->run_id == registration.facts->run_id);
    CHECK(event->action_id == registration.facts->action_id); CHECK(registration.facts->attempt == 1);
    CHECK(registration.facts->operation_id == "jobop-" + event->event_id);
    CHECK(registration.facts->operation_id != registration.facts->parent_operation_id);
    const auto folded = v3::ReadJobOperationBindings(ledger); REQUIRE(folded); REQUIRE(folded->size() >= 1);
    CHECK(folded->back().operation_id == registration.facts->operation_id);
}
struct Gate {
    std::mutex mutex; std::condition_variable cv; bool entered = false, released = false;
    bool Enter() { std::unique_lock lock(mutex); entered = true; cv.notify_all(); return cv.wait_for(lock, 20s, [&] { return released; }); }
    void Release() { std::lock_guard lock(mutex); released = true; cv.notify_all(); }
};
} // namespace

TEST_CASE("private Job Operation uses actual Service and adopted prepared source" * doctest::test_suite("sdk-job-operations")) {
    Directory directory; Rig rig(directory); const auto adopted = rig.Adopt(true); const auto before = Ledger(rig.Path()).lines;
    const auto parent = sdk::CheckMainOperationTurnBindings(rig.Dir(), Ledger(rig.Path()));
    REQUIRE(parent.state == sdk::OperationTurnMaterialState::Incomplete); REQUIRE(parent.facts.size() == 1);
    unsigned publications = 0;
    const auto bound = rig.table.Bind(*rig.service, *rig.coordinator, adopted.facts->job_id, [&](const auto& facts) {
        ++publications; CHECK(facts.parent_operation_id == rig.parent.facts->operation_id); rig.Idle();
    });
    Actual(Ledger(rig.Path()), bound); CHECK(publications == 1); CHECK(Ledger(rig.Path()).lines == before + 1);
    CHECK(bound.facts->original_input_sha256 == adopted.facts->original_input_sha256);
    CHECK(bound.facts->effective_input_sha256 == adopted.facts->effective_input_sha256);
    CHECK(bound.facts->original_input_sha256 != bound.facts->effective_input_sha256);
    const auto bytes = Bytes(rig.Path()); const auto duplicate = rig.table.Bind(*rig.service, *rig.coordinator, adopted.facts->job_id);
    CHECK(duplicate.knowledge == Knowledge::RejectedBeforeWrite); CHECK_FALSE(duplicate.receipt); CHECK(Bytes(rig.Path()) == bytes);
    auto held = sdk::ReadJobOperations(rig.Dir(), Ledger(rig.Path())); REQUIRE(held.state == Held::PassiveHold);
    REQUIRE(held.facts.size() == 1); CHECK(held.facts.front().operation_id == bound.facts->operation_id);
    const auto capacity_job = rig.Adopt(), excess_job = rig.Adopt(); sdk::JobOperations limited(1);
    const auto gap = limited.Bind(*rig.service, *rig.coordinator, capacity_job.facts->job_id, [](const auto&) {
        throw std::runtime_error("capacity retains publication gap");
    });
    REQUIRE(gap.knowledge == Knowledge::CommittedPublicationGap); REQUIRE(gap.receipt); Committed(*gap.receipt);
    const auto capacity_bytes = Bytes(rig.Path()); const auto excess = limited.Bind(*rig.service, *rig.coordinator, excess_job.facts->job_id);
    CHECK(excess.knowledge == Knowledge::RejectedBeforeWrite); CHECK(excess.error.code == "sdk.job_operation.capacity");
    CHECK_FALSE(excess.receipt); CHECK_FALSE(limited.Snapshot(excess_job.facts->job_id)); CHECK(Bytes(rig.Path()) == capacity_bytes);
    REQUIRE(limited.Snapshot(capacity_job.facts->job_id)); CHECK(limited.Snapshot(capacity_job.facts->job_id)->knowledge == gap.knowledge);
    CHECK_FALSE(limited.Close()); rig.Idle();
    Rig unanchored(directory, directory.root / "b", directory.root / "old", false); const auto old = unanchored.Adopt();
    const auto old_bytes = Bytes(unanchored.Path());
    const auto rejected = unanchored.table.Bind(*unanchored.service, *unanchored.coordinator, old.facts->job_id);
    CHECK(rejected.knowledge == Knowledge::RejectedBeforeWrite); CHECK_FALSE(rejected.receipt);
    CHECK(Bytes(unanchored.Path()) == old_bytes); unanchored.Idle(); rig.Idle(); Mark("source");
}

TEST_CASE("private Job Operation preserves first native uncertainty and publication gap" * doctest::test_suite("sdk-job-operations")) {
    for (unsigned variant = 0; variant < 2; ++variant) {
        Directory directory; Rig rig(directory); const auto adopted = rig.Adopt();
        if (variant == 0) {
            REQUIRE(rig.Writer().Close()); v3::V3WriterOptions options;
            options.inject_io_failure = [] { return std::optional<std::string>("actual binding injected"); };
            auto continued = v3::V3Writer::Continue(rig.Path(), options); REQUIRE(continued); rig.Writer() = std::move(*continued);
        }
        unsigned published = 0;
        const auto value = rig.table.Bind(*rig.service, *rig.coordinator, adopted.facts->job_id, [&](const auto&) {
            ++published; throw std::runtime_error("owned publication failed");
        });
        REQUIRE_FALSE(value.bound()); REQUIRE(value.receipt);
        if (variant == 0) {
            CHECK(value.knowledge == Knowledge::Unconfirmed); CHECK(value.receipt->status == v3::WriteReceipt::Status::Rejected);
            CHECK(value.receipt->error_code == "v3writer.injected"); CHECK(rig.Writer().broken()); CHECK(published == 0);
        } else { CHECK(value.knowledge == Knowledge::CommittedPublicationGap); Committed(*value.receipt); CHECK(published == 1); }
        const auto first = *value.receipt; const auto bytes = Bytes(rig.Path());
        CHECK_FALSE(rig.table.Bind(*rig.service, *rig.coordinator, adopted.facts->job_id).bound());
        REQUIRE(rig.table.Snapshot(adopted.facts->job_id)); const auto stored = *rig.table.Snapshot(adopted.facts->job_id);
        REQUIRE(stored.receipt); CHECK(stored.knowledge == value.knowledge); CHECK(stored.receipt->status == first.status);
        CHECK(stored.receipt->id == first.id); CHECK(stored.receipt->error_code == first.error_code); CHECK(Bytes(rig.Path()) == bytes);
        CHECK_FALSE(rig.table.Close()); rig.Idle();
    }
    Mark("gap");
}

TEST_CASE("private Job Operation history is passive after checked close and Continue" * doctest::test_suite("sdk-job-operations")) {
    Directory directory; Rig rig(directory); const auto adopted = rig.Adopt();
    const auto value = rig.table.Bind(*rig.service, *rig.coordinator, adopted.facts->job_id); REQUIRE(value.bound());
    REQUIRE(rig.table.Close()); REQUIRE(rig.coordinator->Shutdown());
    const auto closed = rig.service->Close("checked_fixture"); CHECK(closed.error_code.empty());
    const auto path = rig.Path(), dir = rig.Dir(); const auto bytes = Bytes(path);
    auto continued = v3::V3Writer::Continue(path); REQUIRE(continued);
    const auto held = sdk::ReadJobOperations(dir, Ledger(path)); REQUIRE(held.state == Held::PassiveHold); REQUIRE(held.facts.size() == 1);
    CHECK(held.facts[0].operation_id == value.facts->operation_id);
    sdk::JobOperations restored; CHECK_FALSE(restored.Snapshot(adopted.facts->job_id));
    auto not_live = restored.Bind(*rig.service, *rig.coordinator, adopted.facts->job_id);
    CHECK(not_live.knowledge == Knowledge::RejectedBeforeWrite); CHECK_FALSE(not_live.receipt); CHECK(Bytes(path) == bytes);
    REQUIRE(continued->Close()); rig.Idle(); Mark("history");
}

TEST_CASE("private Job Operation strict reader rejects rehashed native relationships" * doctest::test_suite("sdk-job-operations")) {
    Directory directory; Rig rig(directory); const auto adopted = rig.Adopt();
    REQUIRE(rig.table.Bind(*rig.service, *rig.coordinator, adopted.facts->job_id).bound());
    const auto original = Bytes(rig.Path()); const auto rows = Rows(original);
    for (unsigned variant = 0; variant < 5; ++variant) {
        auto changed = rows;
        auto found = std::find_if(changed.begin(), changed.end(), [](const auto& row) {
            return row.value("kind", std::string()) == "sdk.job.operation.bound";
        }); REQUIRE(found != changed.end());
        if (variant == 0) (*found)["payload"]["originalInputSha256"] = std::string(64, 'f');
        if (variant == 1) (*found)["payload"]["parentOperationRef"] = (*found)["payload"]["adoptionEventRef"];
        if (variant == 2) (*found)["payload"]["registeredEventRef"] = (*found)["payload"]["preparedPendingEventRef"];
        if (variant == 3) (*found)["payload"]["jobId"] = "foreign-job";
        if (variant == 4) (*found)["payload"]["adoptionEventRef"]["hash"] = std::string(64, 'f');
        const auto path = directory.root / ("relation-" + std::to_string(variant) + ".jsonl"); Write(path, Dump(changed, true));
        const auto verified = v3::VerifyV3File(path); REQUIRE_MESSAGE(verified.ok, verified.message);
        const auto read = v3::ReadV3Ledger(path); REQUIRE_FALSE(read); CHECK(read.error() == "v3reader.job_operation_invalid");
    }
    const auto ledger = Ledger(rig.Path()); auto operations = Rows(Bytes(rig.Dir() / "operations.jsonl"));
    REQUIRE(operations.size() == 2); operations[0]["payloadHash"] = std::string(64, 'f');
    Write(rig.Dir() / "operations.jsonl", Dump(operations));
    const auto bad_source = sdk::ReadJobOperations(rig.Dir(), ledger); CHECK(bad_source.state == Held::Rejected);
    CHECK(bad_source.error.code == "sdk.resume.operation_turn_invalid"); CHECK(Bytes(rig.Path()) == original);
    Rig final_rig(directory, directory.root / "b", directory.root / "final-store"); const auto final_job = final_rig.Adopt();
    REQUIRE(final_rig.table.Bind(*final_rig.service, *final_rig.coordinator, final_job.facts->job_id).bound());
    REQUIRE(final_rig.service->RecordTurnFinal({final_rig.parent.facts->operation_id, final_rig.parent.turn_id, "cancelled", {}, false}));
    const auto final_ledger = Ledger(final_rig.Path());
    REQUIRE(sdk::CheckMainOperationTurnBindings(final_rig.Dir(), final_ledger).state == sdk::OperationTurnMaterialState::Validated);
    REQUIRE(sdk::ReadJobOperations(final_rig.Dir(), final_ledger).state == Held::PassiveHold);
    auto final_rows = Rows(Bytes(final_rig.Dir() / "operations.jsonl")); REQUIRE(final_rows.size() == 3);
    REQUIRE(final_rows.back().at("kind") == "operation.final"); final_rows.back()["turnId"] = "foreign-turn";
    const auto final_bytes = Bytes(final_rig.Path()); Write(final_rig.Dir() / "operations.jsonl", Dump(final_rows));
    const auto wrong_final = sdk::ReadJobOperations(final_rig.Dir(), final_ledger); CHECK(wrong_final.state == Held::Rejected);
    CHECK(wrong_final.error.code == "sdk.resume.operation_turn_invalid"); CHECK(Bytes(final_rig.Path()) == final_bytes); final_rig.Idle();
    rig.Idle(); Mark("relation");
}

TEST_CASE("private Job Operation four real scenes keep independent ownership" * doctest::test_suite("sdk-job-operations")) {
    Directory directory;
    std::array<std::unique_ptr<Rig>, 4> rigs;
    rigs[0] = std::make_unique<Rig>(directory); rigs[1] = std::make_unique<Rig>(directory);
    rigs[2] = std::make_unique<Rig>(directory, directory.root / "b");
    rigs[3] = std::make_unique<Rig>(directory, directory.root / "a", directory.root / "other-store");
    std::array<OwnedJobAdoption, 4> adopted;
    std::array<sdk::JobOperationRegistration, 4> values;
    for (std::size_t i = 0; i < rigs.size(); ++i) {
        adopted[i] = rigs[i]->Adopt();
        values[i] = rigs[i]->table.Bind(*rigs[i]->service, *rigs[i]->coordinator, adopted[i].facts->job_id);
        Actual(Ledger(rigs[i]->Path()), values[i]); rigs[i]->Idle();
    }
    for (std::size_t i = 0; i < rigs.size(); ++i) for (std::size_t j = 0; j < rigs.size(); ++j) if (i != j) {
        const auto bytes = Bytes(rigs[i]->Path()); sdk::JobOperations foreign;
        const auto result = foreign.Bind(*rigs[i]->service, *rigs[j]->coordinator, adopted[j].facts->job_id);
        CHECK(result.knowledge == Knowledge::RejectedBeforeWrite); CHECK_FALSE(result.receipt); CHECK(Bytes(rigs[i]->Path()) == bytes);
        const auto held = sdk::ReadJobOperations(rigs[j]->Dir(), Ledger(rigs[i]->Path())); CHECK(held.state == Held::Rejected);
    }
    Mark("isolation");
}

TEST_CASE("private Job Operation table drains real publication without retaining borrows" * doctest::test_suite("sdk-job-operations")) {
    Directory directory; Rig rig(directory); const auto adopted = rig.Adopt(); auto gate = std::make_shared<Gate>();
    const auto nested_job = rig.Adopt(); sdk::JobOperations nested;
    std::future<sdk::JobOperationRegistration> binder;
    std::future<lubancore::Result<void>> closer;
    struct Release { std::shared_ptr<Gate> gate; ~Release() { gate->Release(); } } release{gate};
    binder = std::async(std::launch::async, [&rig, &nested, gate, id = adopted.facts->job_id, nested_id = nested_job.facts->job_id] {
        return rig.table.Bind(*rig.service, *rig.coordinator, id, [&](const auto& facts) {
            CHECK_FALSE(rig.table.Close()); CHECK_FALSE(facts.operation_id.empty());
            const auto before = Bytes(rig.Path());
            const auto recursion = nested.Bind(*rig.service, *rig.coordinator, nested_id);
            CHECK(recursion.knowledge == Knowledge::RejectedBeforeWrite); CHECK(recursion.error.code == "sdk.job_operation.reentrant");
            CHECK_FALSE(recursion.receipt); CHECK_FALSE(nested.Snapshot(nested_id)); CHECK(Bytes(rig.Path()) == before);
            const auto recursive_close = nested.Close(); REQUIRE_FALSE(recursive_close);
            CHECK(recursive_close.error().code == "sdk.job_operation.reentrant"); rig.Idle();
            REQUIRE(gate->Enter());
        });
    });
    { std::unique_lock lock(gate->mutex); REQUIRE(gate->cv.wait_for(lock, 20s, [&] { return gate->entered; })); }
    auto snapshot = rig.table.Snapshot(adopted.facts->job_id); REQUIRE(snapshot); REQUIRE(snapshot->facts);
    CHECK(snapshot->knowledge == Knowledge::CommittedPublicationGap); rig.Idle();
    closer = std::async(std::launch::async, [&rig] { return rig.table.Close(); });
    CHECK(closer.wait_for(50ms) == std::future_status::timeout);
    gate->Release(); REQUIRE(binder.wait_for(20s) == std::future_status::ready); const auto bound = binder.get(); REQUIRE(bound.bound());
    REQUIRE(closer.wait_for(20s) == std::future_status::ready); CHECK(closer.get());
    CHECK(nested.Close());
    const auto bytes = Bytes(rig.Path()); const auto late = rig.table.Bind(*rig.service, *rig.coordinator, adopted.facts->job_id);
    CHECK(late.knowledge == Knowledge::RejectedBeforeWrite); CHECK(late.error.code == "sdk.job_operation.closed"); CHECK_FALSE(late.receipt);
    REQUIRE(rig.table.Snapshot(adopted.facts->job_id)); CHECK(rig.table.Snapshot(adopted.facts->job_id)->bound());
    CHECK(Bytes(rig.Path()) == bytes); rig.Idle(); Mark("lifetime");
}
